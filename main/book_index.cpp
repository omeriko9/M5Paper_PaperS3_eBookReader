#include "book_index.h"
#include "epub_loader.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
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

bool BookIndex::init(bool fastMode, ProgressCallback callback)
{
    if (!mutex) mutex = xSemaphoreCreateRecursiveMutex();
    
    // Only load the index from disk. Scanning is now done in background.
    // We pass the callback to load() to allow incremental updates
    load(callback);
    
    return false; // No new books found during init
}

bool BookIndex::validateBooks()
{
    // Get a copy of books to check without holding the lock for too long
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    std::vector<BookEntry> booksCopy = books;
    xSemaphoreGiveRecursive(mutex);

    std::vector<int> idsToRemove;
    for (const auto& book : booksCopy) {
        struct stat st;
        if (stat(book.path.c_str(), &st) != 0) {
            ESP_LOGW(TAG, "Book file missing: %s", book.path.c_str());
            idsToRemove.push_back(book.id);
        }
        // Yield to keep system responsive
        vTaskDelay(1);
    }

    if (!idsToRemove.empty()) {
        xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
        bool changed = false;
        for (int id : idsToRemove) {
            auto it = std::remove_if(books.begin(), books.end(), [id](const BookEntry &b) { return b.id == id; });
            if (it != books.end()) {
                books.erase(it, books.end());
                changed = true;
            }
        }
        if (changed) {
            saveInternal();
            xSemaphoreGiveRecursive(mutex);
            return true;
        }
        xSemaphoreGiveRecursive(mutex);
    }
    return false;
}

bool BookIndex::scanForNewBooks(ProgressCallback callback)
{
    bool foundNewBooks = validateBooks();
    foundNewBooks |= scanDirectory("/spiffs", callback);
    
    DeviceHAL& hal = DeviceHAL::getInstance();
    if (hal.isSDCardMounted()) {
        const char* sdPath = hal.getSDCardMountPoint();
        if (sdPath) {
            // Scan /sdcard/books instead of root
            std::string booksPath = std::string(sdPath) + "/books";
            foundNewBooks |= scanDirectory(booksPath.c_str(), callback);
        }
    }
    
    if (foundNewBooks) {
        xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
        saveInternal();
        xSemaphoreGiveRecursive(mutex);
    }
    
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

void BookIndex::updateBookMetricsFlag(int id, bool hasMetrics) {
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    for (auto& book : books) {
        if (book.id == id) {
            book.hasMetrics = hasMetrics;
            break;
        }
    }
    xSemaphoreGiveRecursive(mutex);
}

bool BookIndex::saveBookMetrics(int id, size_t totalChars, const std::vector<size_t>& chapterOffsets, bool saveToDisk) {
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
    
    // Persist the flag to index if requested
    if (saveToDisk) {
        saveInternal();
    }
    
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

bool BookIndex::scanDirectory(const char *basePath, ProgressCallback callback)
{
    // We don't take the mutex for the whole function anymore to avoid freezing the UI.
    // Instead, we take it only when accessing shared data.
    
    // Pre-process existing books for faster lookup
    // Take lock briefly to build the map
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    std::vector<std::pair<std::string, size_t>> bookMap;
    bookMap.reserve(books.size());
    for (size_t i = 0; i < books.size(); ++i) {
        std::string p = books[i].path;
        size_t lastSlash = p.find_last_of('/');
        std::string fname = (lastSlash != std::string::npos) ? p.substr(lastSlash + 1) : p;
        bookMap.push_back({fname, i});
    }
    xSemaphoreGiveRecursive(mutex);

    DIR *dir = opendir(basePath);
    if (!dir)
    {
        ESP_LOGW(TAG, "Failed to open directory: %s", basePath);
        return false;
    }

    bool foundNewBooks = false;
    int processedFiles = 0;
    struct dirent *entry;
    
    while ((entry = readdir(dir)) != NULL)
    {
        esp_task_wdt_reset();
        // Yield to let UI task run - but not too much
        if (processedFiles % 10 == 0) vTaskDelay(1);
        
        if (entry->d_type == DT_REG)
        { // Regular file
            std::string fname = entry->d_name;
            // Check extension .epub
            if (fname.length() > 5 &&
                (fname.substr(fname.length() - 5) == ".epub" || fname.substr(fname.length() - 5) == ".EPUB"))
            {
                processedFiles++;
                
                // Update progress every 20 books
                if (callback && (processedFiles % 20 == 0)) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Scanning %d books...", processedFiles);
                    callback(processedFiles, -1, msg);
                }

                std::string fullPath = std::string(basePath) + "/" + fname;

                // Check if already exists in books
                bool found = false;
                
                // Try to find by filename using our map
                for (const auto& pair : bookMap) {
                    if (pair.first == fname) {
                        found = true;
                        
                        // Lock only for checking/updating the specific book
                        xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
                        // Re-verify index validity (though we are the only writer)
                        if (pair.second < books.size()) {
                            BookEntry& book = books[pair.second];
                            
                            // Update path if it changed (e.g. mount point change)
                            if (book.path != fullPath) {
                                ESP_LOGI(TAG, "Updating book path from %s to %s", book.path.c_str(), fullPath.c_str());
                                book.path = fullPath;
                                foundNewBooks = true;
                            }
                            
                            // Check size only if we suspect change, but stat is slow.
                            // Let's skip size check for now to speed up scanning.
                            // If the file changed, the user should probably delete the index or we need a better way.
                        }
                        xSemaphoreGiveRecursive(mutex);
                        break;
                    }
                }

                if (!found)
                {
                    vTaskDelay(5);
                    // New book found. Load metadata first (slow, no lock)
                    int id = getNextId(); 
                    
                    std::string title = fname.substr(0, fname.length() - 5);
                    std::string author = "Unknown";
                    
                    EpubLoader loader;
                    if (loader.loadMetadataOnly(fullPath.c_str()))
                    {
                        std::string metaTitle = loader.getTitle();
                        if (!metaTitle.empty()) title = metaTitle;
                        author = loader.getAuthor();
                        loader.close();
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Failed to load metadata for %s", fullPath.c_str());
                        // Still add it with filename as title
                    }
                    
                    struct stat st;
                    size_t currentSize = 0;
                    if (stat(fullPath.c_str(), &st) == 0)
                    {
                        currentSize = st.st_size;
                    }

                    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
                    BookEntry newBook;
                    newBook.id = id;
                    newBook.title = title;
                    newBook.author = author;
                    newBook.path = fullPath;
                    newBook.fileSize = currentSize;
                    newBook.hasMetrics = false; // Will be checked later
                    books.push_back(newBook);
                    // Add to map to avoid duplicates if file appears twice (unlikely)
                    bookMap.push_back({fname, books.size() - 1});
                    foundNewBooks = true;
                    xSemaphoreGiveRecursive(mutex);
                    
                    ESP_LOGI(TAG, "Added new book: %s", title.c_str());
                }
            }
        }
    }
    closedir(dir);
    
    return foundNewBooks;
}

void BookIndex::load(ProgressCallback callback)
{
    // Clear existing books safely
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    books.clear();
    xSemaphoreGiveRecursive(mutex);

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f)
    {
        ESP_LOGW(TAG, "Index file not found, creating new");
        return;
    }

    char line[512]; // Increased buffer size
    std::vector<BookEntry> batch;
    int count = 0;

    while (fgets(line, sizeof(line), f))
    {
        esp_task_wdt_reset();
        // Strip newline and carriage return
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[len-1] = 0;
            len--;
        }

        std::string s(line);
        if (s.empty()) continue;

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
            
            std::string lastFont;
            if (parts.size() >= 10)
                lastFont = parts[9];
            
            float lastFontSize = 1.0f;
            if (parts.size() >= 11)
                lastFontSize = atof(parts[10].c_str());

            bool isFailed = false;
            if (parts.size() >= 12)
                isFailed = (atoi(parts[11].c_str()) == 1);

            // Verify file exists - REMOVED for speed. Cleanup happens in background scan.
            // struct stat st;
            // if (stat(path.c_str(), &st) == 0)
            {
                batch.push_back({id, title, author, path, chapter, offset, fsize, hasMetrics, isFavorite, lastFont, lastFontSize, isFailed});
                count++;
                // log the book - REMOVED for speed
                // ESP_LOGI(TAG, "Loaded book: %s by %s", title.c_str(), author.c_str());
            }
        }

        // Batch update
        if (batch.size() >= 50) {
            xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
            books.insert(books.end(), batch.begin(), batch.end());
            xSemaphoreGiveRecursive(mutex);
            batch.clear();
            
            if (callback) {
                callback(count, -1, "Loading books...");
            }
            
            // Yield to let UI task run and avoid WDT on other tasks waiting for mutex
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    // Add remaining
    if (!batch.empty()) {
        xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
        books.insert(books.end(), batch.begin(), batch.end());
        xSemaphoreGiveRecursive(mutex);
        if (callback) {
            callback(count, -1, "Loading books...");
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "Loaded %d books", count);
}

static std::string sanitize(const std::string& s) {
    std::string res = s;
    for (char& c : res) {
        if (c == '|') c = '-';
        if (c == '\n') c = ' ';
        if (c == '\r') c = ' ';
    }
    return res;
}

void BookIndex::saveInternal()
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
        esp_task_wdt_reset();
        fprintf(f, "%d|%s|%d|%u|%s|%u|%d|%s|%d|%s|%.2f|%d\n", 
            book.id, 
            sanitize(book.title).c_str(), 
            book.currentChapter, 
            (unsigned int)book.currentOffset, 
            book.path.c_str(), 
            (unsigned int)book.fileSize, 
            book.hasMetrics ? 1 : 0, 
            sanitize(book.author).c_str(), 
            book.isFavorite ? 1 : 0,
            sanitize(book.lastFont).c_str(),
            book.lastFontSize,
            book.isFailed ? 1 : 0);
    }
    fclose(f);
}

void BookIndex::markAsFailed(int id)
{
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    for (auto &book : books)
    {
        if (book.id == id)
        {
            book.isFailed = true;
            saveInternal();
            break;
        }
    }
    xSemaphoreGiveRecursive(mutex);
}

void BookIndex::save()
{
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    saveInternal();
    xSemaphoreGiveRecursive(mutex);
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
            // saveInternal(); // Don't save on every progress update to avoid lag
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
    return {0, "", "", "", 0, 0, 0, false, false, "", 1.0f};
}

int BookIndex::getBookIdByPath(const std::string& path)
{
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    for (const auto &book : books)
    {
        if (book.path == path) {
            int id = book.id;
            xSemaphoreGiveRecursive(mutex);
            return id;
        }
    }
    xSemaphoreGiveRecursive(mutex);
    return 0;
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

    books.push_back({id, title, "", std::string(path), 0, 0, 0, false, false, "", 1.0f});
    saveInternal();

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
        saveInternal();
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
            saveInternal();
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

void BookIndex::setBookFont(int id, const std::string& fontName, float fontSize)
{
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    for (auto& book : books) {
        if (book.id == id) {
            book.lastFont = fontName;
            book.lastFontSize = fontSize;
            saveInternal();
            break;
        }
    }
    xSemaphoreGiveRecursive(mutex);
}

bool BookIndex::getBookFont(int id, std::string& fontName, float& fontSize)
{
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    for (const auto& book : books) {
        if (book.id == id) {
            if (!book.lastFont.empty()) {
                fontName = book.lastFont;
                fontSize = book.lastFontSize;
                xSemaphoreGiveRecursive(mutex);
                return true;
            }
            xSemaphoreGiveRecursive(mutex);
            return false;
        }
    }
    xSemaphoreGiveRecursive(mutex);
    return false;
}
