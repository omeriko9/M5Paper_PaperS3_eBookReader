#include "book_index.h"
#include "epub_loader.h"
#include "esp_log.h"
#include <stdio.h>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <M5Unified.hpp>

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
        foundNewBooks = scanDirectory("/spiffs");
        // scanDirectory("/sd");
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

bool BookIndex::saveBookMetrics(int id, size_t totalChars, const std::vector<size_t>& chapterOffsets) {
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    char path[64];
    snprintf(path, sizeof(path), "/spiffs/m_%d.bin", id);
    
    FILE* f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open metrics file for writing: %s", path);
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
    
    ESP_LOGI(TAG, "Saved metrics for book %d: %zu chars, %u chapters", id, totalChars, count);
    xSemaphoreGiveRecursive(mutex);
    return true;
}

bool BookIndex::loadBookMetrics(int id, size_t& totalChars, std::vector<size_t>& chapterOffsets) {
    // File access doesn't strictly need mutex if it doesn't touch 'books', 
    // but good practice if we change implementation later.
    // However, this method doesn't touch 'books' vector, only reads a file.
    // But let's lock to be safe if we add caching later.
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    char path[64];
    snprintf(path, sizeof(path), "/spiffs/m_%d.bin", id);
    
    FILE* f = fopen(path, "rb");
    if (!f) {
        xSemaphoreGiveRecursive(mutex);
        return false;
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
                            // Reload title (metadata only, avoids chapter parsing)
                            EpubLoader loader;
                            if (loader.loadMetadataOnly(fullPath.c_str()))
                            {
                                std::string metaTitle = loader.getTitle();
                                if (!metaTitle.empty() && metaTitle != "Unknown Title")
                                {
                                    book.title = metaTitle;
                                }
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

                    // Try to load title from metadata
                    EpubLoader loader;
                    if (loader.loadMetadataOnly(fullPath.c_str()))
                    {
                        std::string metaTitle = loader.getTitle();
                        if (!metaTitle.empty() && metaTitle != "Unknown Title")
                        {
                            title = metaTitle;
                        }
                        loader.close();
                    }

                    books.push_back({id, title, fullPath, 0, 0, currentSize, false});
                    ESP_LOGI(TAG, "Found new book: %s", title.c_str());
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

            // Verify file exists
            struct stat st;
            if (stat(path.c_str(), &st) == 0)
            {
                books.push_back({id, title, path, chapter, offset, fsize, hasMetrics});
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
        fprintf(f, "%d|%s|%d|%u|%s|%u|%d\n", book.id, book.title.c_str(), book.currentChapter, (unsigned int)book.currentOffset, book.path.c_str(), (unsigned int)book.fileSize, book.hasMetrics ? 1 : 0);
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
    return {0, "", "", 0, 0, 0};
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
    char path[32];
    snprintf(path, sizeof(path), "/spiffs/%d.epub", id);

    books.push_back({id, title, std::string(path), 0, 0, 0, false});
    save();

    xSemaphoreGiveRecursive(mutex);
    return std::string(path);
}

void BookIndex::removeBook(int id)
{
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    auto it = std::remove_if(books.begin(), books.end(), [id](const BookEntry &b)
                             { return b.id == id; });

    if (it != books.end())
    {
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
