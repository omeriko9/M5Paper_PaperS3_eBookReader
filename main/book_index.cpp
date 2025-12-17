#include "book_index.h"
#include "epub_loader.h"
#include "esp_log.h"
#include <stdio.h>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <M5Unified.hpp>
#include "device_hal.h"

static const char *TAG = "INDEX";
static const char *INDEX_FILE = "/spiffs/index.txt";

BookIndex::BookIndex() {
    mutex = xSemaphoreCreateRecursiveMutex();
}

bool BookIndex::init(bool fastMode)
{
    if (!mutex) mutex = xSemaphoreCreateRecursiveMutex();
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    
    bool foundNewBooks = false;
    load();
    if (!fastMode)
    {
        foundNewBooks |= scanDirectory("/spiffs");
        
        DeviceHAL& hal = DeviceHAL::getInstance();
        if (hal.isSDCardMounted()) {
            const char* sdPath = hal.getSDCardMountPoint();
            if (sdPath) {
                foundNewBooks |= scanDirectory(sdPath);
            }
        }
        save();
    }
    
    // checkMetricsExistence(); // Removed to avoid WDT timeout on wake

    xSemaphoreGiveRecursive(mutex);
    return foundNewBooks;
}

void BookIndex::checkMetricsExistence() {
    // Private method, assumes mutex held
    for (auto& book : books) {
        char path[64];
        snprintf(path, sizeof(path), "/spiffs/m_%d.bin", book.id);
        struct stat st;
        book.hasMetrics = (stat(path, &st) == 0);
    }
}

static std::string getMetricsPath(const std::string& bookPath) {
    if (bookPath.empty()) return "";
    
    std::string metricsPath = bookPath;
    size_t lastSlash = metricsPath.find_last_of('/');
    if (lastSlash != std::string::npos) {
        metricsPath.insert(lastSlash + 1, "m_");
    } else {
        metricsPath = "m_" + metricsPath;
    }
    
    size_t dot = metricsPath.find_last_of('.');
    if (dot != std::string::npos) {
        metricsPath = metricsPath.substr(0, dot) + ".bin";
    } else {
        metricsPath += ".bin";
    }
    return metricsPath;
}

bool BookIndex::saveBookMetrics(int id, size_t totalChars, const std::vector<size_t>& chapterOffsets) {
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    
    std::string bookPath;
    for (const auto& book : books) {
        if (book.id == id) {
            bookPath = book.path;
            break;
        }
    }
    
    if (bookPath.empty()) {
        xSemaphoreGiveRecursive(mutex);
        return false;
    }
    
    std::string path = getMetricsPath(bookPath);
    
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open metrics file for writing: %s", path.c_str());
        xSemaphoreGiveRecursive(mutex);
        return false;
    }
    
    // Header: Version (1), TotalChars (8), Count (4)
    uint8_t version = 1;
    fwrite(&version, 1, 1, f);
    fwrite(&totalChars, sizeof(size_t), 1, f);
    uint32_t count = chapterOffsets.size();
    fwrite(&count, sizeof(uint32_t), 1, f);
    
    if (count > 0) {
        fwrite(chapterOffsets.data(), sizeof(size_t), count, f);
    }
    
    fclose(f);
    
    // Update in-memory flag
    for (auto& book : books) {
        if (book.id == id) {
            book.hasMetrics = true;
            break;
        }
    }
    
    // Persist the flag to index
    save();
    
    ESP_LOGI(TAG, "Saved metrics for book %d: %zu chars, %u chapters at %s", id, totalChars, count, path.c_str());
    xSemaphoreGiveRecursive(mutex);
    return true;
}

bool BookIndex::loadBookMetrics(int id, size_t& totalChars, std::vector<size_t>& chapterOffsets) {
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    
    std::string bookPath;
    for (const auto& book : books) {
        if (book.id == id) {
            bookPath = book.path;
            break;
        }
    }
    
    if (bookPath.empty()) {
        xSemaphoreGiveRecursive(mutex);
        return false;
    }
    
    std::string path = getMetricsPath(bookPath);
    
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        // Fallback to legacy path if not found
        char legacyPath[64];
        snprintf(legacyPath, sizeof(legacyPath), "/spiffs/m_%d.bin", id);
        f = fopen(legacyPath, "rb");
        if (!f) {
            xSemaphoreGiveRecursive(mutex);
            return false;
        }
        ESP_LOGI(TAG, "Loaded metrics from legacy path for book %d", id);
    }
    
    uint8_t version = 0;
    if (fread(&version, 1, 1, f) != 1 || version != 1) {
        fclose(f);
        xSemaphoreGiveRecursive(mutex);
        return false;
    }
    
    if (fread(&totalChars, sizeof(size_t), 1, f) != 1) {
        fclose(f);
        xSemaphoreGiveRecursive(mutex);
        return false;
    }
    
    uint32_t count = 0;
    if (fread(&count, sizeof(uint32_t), 1, f) != 1) {
        fclose(f);
        xSemaphoreGiveRecursive(mutex);
        return false;
    }
    
    chapterOffsets.resize(count);
    if (count > 0) {
        if (fread(chapterOffsets.data(), sizeof(size_t), count, f) != count) {
            fclose(f);
            xSemaphoreGiveRecursive(mutex);
            return false;
        }
    }
    
    fclose(f);
    xSemaphoreGiveRecursive(mutex);
    return true;
}

bool BookIndex::scanDirectory(const char *basePath)
{
    // Private helper or public? Public. Needs lock.
    // But init calls it. Recursive mutex handles this.
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    bool foundNewBooks = false;
    DIR *dir = opendir(basePath);
    if (!dir)
    {
        ESP_LOGW(TAG, "Failed to open directory: %s", basePath);
        xSemaphoreGiveRecursive(mutex);
        return false;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_type == DT_REG)
        { // Regular file
            std::string fname = entry->d_name;
            // Check extension .epub
            if (fname.length() > 5 &&
                (fname.substr(fname.length() - 5) == ".epub" || fname.substr(fname.length() - 5) == ".EPUB"))
            {

                std::string fullPath = std::string(basePath) + "/" + fname;

                // Get file stats
                struct stat st;
                size_t currentSize = 0;
                if (stat(fullPath.c_str(), &st) == 0)
                {
                    currentSize = st.st_size;
                }

                // Check if already exists in books
                bool found = false;
                for (auto &book : books)
                {
                    if (book.path == fullPath)
                    {
                        found = true;
                        // Check if file changed
                        if (book.fileSize != currentSize || book.fileSize == 0)
                        {
                            ESP_LOGI(TAG, "Book changed or no size cached: %s", fname.c_str());
                            // Reload title and author (metadata only, avoids chapter parsing)
                            EpubLoader loader;
                            if (loader.loadMetadataOnly(fullPath.c_str()))
                            {
                                std::string metaTitle = loader.getTitle();
                                if (!metaTitle.empty() && metaTitle != "Unknown Title")
                                {
                                    book.title = metaTitle;
                                }
                                std::string metaAuthor = loader.getAuthor();
                                book.author = metaAuthor;
                                loader.close();
                            }
                            book.fileSize = currentSize;
                            foundNewBooks = true; // Treat changed book as "new" for refresh
                        }
                        break;
                    }
                }

                if (!found)
                {
                    int id = getNextId();
                    // Use filename as title initially
                    std::string title = fname.substr(0, fname.length() - 5);
                    std::string bookAuthor;

                    // Try to load title and author from metadata
                    EpubLoader loader;
                    if (loader.loadMetadataOnly(fullPath.c_str()))
                    {
                        std::string metaTitle = loader.getTitle();
                        if (!metaTitle.empty() && metaTitle != "Unknown Title")
                        {
                            title = metaTitle;
                        }
                        bookAuthor = loader.getAuthor();
                        loader.close();
                    }

                    books.push_back({id, title, bookAuthor, fullPath, 0, 0, currentSize, false, false});
                    ESP_LOGI(TAG, "Found new book: %s by %s", title.c_str(), bookAuthor.c_str());
                    foundNewBooks = true;
                }
            }
        }
    }
    closedir(dir);

    xSemaphoreGiveRecursive(mutex);
    return foundNewBooks;
}

void BookIndex::load()
{
    // Private helper, assumes mutex held
    books.clear();
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f)
    {
        ESP_LOGW(TAG, "Index file not found, creating new");
        // Don't draw here, it's not thread safe if called from background task
        // M5.Display.fillScreen(WHITE);
        // ...
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;

        std::string s(line);
        std::vector<std::string> parts;
        size_t start = 0;
        size_t end = s.find('|');
        while (end != std::string::npos)
        {
            parts.push_back(s.substr(start, end - start));
            start = end + 1;
            end = s.find('|', start);
        }
        parts.push_back(s.substr(start));

        if (parts.size() >= 2)
        {
            int id = atoi(parts[0].c_str());
            std::string title = parts[1];
            int chapter = 0;
            size_t offset = 0;
            std::string path;

            if (parts.size() >= 3)
                chapter = atoi(parts[2].c_str());
            if (parts.size() >= 4)
                offset = atol(parts[3].c_str());
            if (parts.size() >= 5)
                path = parts[4];
            else
            {
                // Legacy path
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "/spiffs/%d.epub", id);
                path = tmp;
            }
            size_t fsize = 0;
            if (parts.size() >= 6)
                fsize = atol(parts[5].c_str());
            
            bool hasMetrics = false;
            if (parts.size() >= 7)
                hasMetrics = (atoi(parts[6].c_str()) == 1);
            
            std::string author;
            if (parts.size() >= 8)
                author = parts[7];
            
            bool isFavorite = false;
            if (parts.size() >= 9)
                isFavorite = (atoi(parts[8].c_str()) == 1);

            // Verify file exists
            struct stat st;
            if (stat(path.c_str(), &st) == 0)
            {
                books.push_back({id, title, author, path, chapter, offset, fsize, hasMetrics, isFavorite});
            }
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "Loaded %d books", books.size());
}

void BookIndex::save()
{
    // Private helper, assumes mutex held
    FILE *f = fopen(INDEX_FILE, "w");
    if (!f)
    {
        ESP_LOGE(TAG, "Failed to open index for writing");
        return;
    }

    for (const auto &book : books)
    {
        fprintf(f, "%d|%s|%d|%u|%s|%u|%d|%s|%d\n", book.id, book.title.c_str(), book.currentChapter, (unsigned int)book.currentOffset, book.path.c_str(), (unsigned int)book.fileSize, book.hasMetrics ? 1 : 0, book.author.c_str(), book.isFavorite ? 1 : 0);
    }
    fclose(f);
}

void BookIndex::updateProgress(int id, int chapter, size_t offset)
{
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    for (auto &book : books)
    {
        if (book.id == id)
        {
            book.currentChapter = chapter;
            book.currentOffset = offset;
            save();
            xSemaphoreGiveRecursive(mutex);
            return;
        }
    }
    xSemaphoreGiveRecursive(mutex);
}

BookEntry BookIndex::getBook(int id)
{
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    for (const auto &book : books)
    {
        if (book.id == id) {
            BookEntry b = book;
            xSemaphoreGiveRecursive(mutex);
            return b;
        }
    }
    xSemaphoreGiveRecursive(mutex);
    return {0, "", "", "", 0, 0, 0, false, false};
}

int BookIndex::getNextId()
{
    // Private helper, assumes mutex held
    int maxId = 0;
    for (const auto &book : books)
    {
        if (book.id > maxId)
            maxId = book.id;
    }
    return maxId + 1;
}

std::string BookIndex::addBook(const std::string &title)
{
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    int id = getNextId();
    char path[128];
    
    DeviceHAL& hal = DeviceHAL::getInstance();
    if (hal.isSDCardMounted() && hal.getSDCardMountPoint()) {
        snprintf(path, sizeof(path), "%s/%d.epub", hal.getSDCardMountPoint(), id);
    } else {
        snprintf(path, sizeof(path), "/spiffs/%d.epub", id);
    }

    books.push_back({id, title, "", std::string(path), 0, 0, 0, false, false});
    save();

    xSemaphoreGiveRecursive(mutex);
    return std::string(path);
}

void BookIndex::removeBook(int id)
{
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    
    std::string bookPath;
    for (const auto& book : books) {
        if (book.id == id) {
            bookPath = book.path;
            break;
        }
    }

    auto it = std::remove_if(books.begin(), books.end(), [id](const BookEntry &b)
                             { return b.id == id; });

    if (it != books.end())
    {
        // Delete metrics file if it exists
        if (!bookPath.empty()) {
            std::string mPath = getMetricsPath(bookPath);
            unlink(mPath.c_str());
            // Also try legacy path
            char legacyPath[64];
            snprintf(legacyPath, sizeof(legacyPath), "/spiffs/m_%d.bin", id);
            unlink(legacyPath);
        }
        
        books.erase(it, books.end());
        save();
    }
    xSemaphoreGiveRecursive(mutex);
}

std::vector<BookEntry> BookIndex::getBooks()
{
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    std::vector<BookEntry> copy = books;
    xSemaphoreGiveRecursive(mutex);
    return copy;
}

// Helper to convert string to lowercase for case-insensitive search
static std::string toLowerStr(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') {
            result += (c + 32);
        } else {
            result += c;
        }
    }
    return result;
}

std::vector<BookEntry> BookIndex::getFilteredBooks(const std::string& searchQuery, bool favoritesOnly)
{
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    std::vector<BookEntry> result;
    
    std::string queryLower = toLowerStr(searchQuery);
    
    for (const auto& book : books) {
        // Filter by favorites if enabled
        if (favoritesOnly && !book.isFavorite) {
            continue;
        }
        
        // If no search query, include all (that passed favorites filter)
        if (searchQuery.empty()) {
            result.push_back(book);
            continue;
        }
        
        // Search in title and author (case-insensitive)
        std::string titleLower = toLowerStr(book.title);
        std::string authorLower = toLowerStr(book.author);
        
        if (titleLower.find(queryLower) != std::string::npos ||
            authorLower.find(queryLower) != std::string::npos) {
            result.push_back(book);
        }
    }
    
    xSemaphoreGiveRecursive(mutex);
    return result;
}

void BookIndex::setFavorite(int id, bool favorite)
{
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    for (auto& book : books) {
        if (book.id == id) {
            book.isFavorite = favorite;
            save();
            break;
        }
    }
    xSemaphoreGiveRecursive(mutex);
}

bool BookIndex::isFavorite(int id)
{
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    for (const auto& book : books) {
        if (book.id == id) {
            bool fav = book.isFavorite;
            xSemaphoreGiveRecursive(mutex);
            return fav;
        }
    }
    xSemaphoreGiveRecursive(mutex);
    return false;
}
