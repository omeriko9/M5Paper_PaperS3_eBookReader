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

bool BookIndex::init(bool fastMode)
{
    bool foundNewBooks = false;
    load();
    if (!fastMode)
    {
        foundNewBooks = scanDirectory("/spiffs");
        // scanDirectory("/sd");
        save();
    }
    
    // checkMetricsExistence(); // Removed to avoid WDT timeout on wake

    return foundNewBooks;
}

void BookIndex::checkMetricsExistence() {
    for (auto& book : books) {
        char path[64];
        snprintf(path, sizeof(path), "/spiffs/m_%d.bin", book.id);
        struct stat st;
        book.hasMetrics = (stat(path, &st) == 0);
    }
}

bool BookIndex::saveBookMetrics(int id, size_t totalChars, const std::vector<size_t>& chapterOffsets) {
    char path[64];
    snprintf(path, sizeof(path), "/spiffs/m_%d.bin", id);
    
    FILE* f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open metrics file for writing: %s", path);
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
    return true;
}

bool BookIndex::loadBookMetrics(int id, size_t& totalChars, std::vector<size_t>& chapterOffsets) {
    char path[64];
    snprintf(path, sizeof(path), "/spiffs/m_%d.bin", id);
    
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    
    uint8_t version = 0;
    if (fread(&version, 1, 1, f) != 1 || version != 1) {
        fclose(f);
        return false;
    }
    
    if (fread(&totalChars, sizeof(size_t), 1, f) != 1) {
        fclose(f);
        return false;
    }
    
    uint32_t count = 0;
    if (fread(&count, sizeof(uint32_t), 1, f) != 1) {
        fclose(f);
        return false;
    }
    
    chapterOffsets.resize(count);
    if (count > 0) {
        if (fread(chapterOffsets.data(), sizeof(size_t), count, f) != count) {
            fclose(f);
            return false;
        }
    }
    
    fclose(f);
    return true;
}

bool BookIndex::scanDirectory(const char *basePath)
{
    bool foundNewBooks = false;
    DIR *dir = opendir(basePath);
    if (!dir)
    {
        ESP_LOGW(TAG, "Failed to open directory: %s", basePath);
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

    return foundNewBooks;
}

void BookIndex::load()
{
    books.clear();
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f)
    {
        ESP_LOGW(TAG, "Index file not found, creating new");
        M5.Display.fillScreen(WHITE);
        M5.Display.setTextColor(BLACK);
        M5.Display.setTextSize(1.5);
        M5.Display.setTextDatum(textdatum_t::middle_center);
        M5.Display.drawString("Indexing Books...", M5.Display.width() / 2, M5.Display.height() / 2);

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
    for (auto &book : books)
    {
        if (book.id == id)
        {
            book.currentChapter = chapter;
            book.currentOffset = offset;
            save();
            return;
        }
    }
}

BookEntry BookIndex::getBook(int id)
{
    for (const auto &book : books)
    {
        if (book.id == id)
            return book;
    }
    return {0, "", "", 0, 0, 0};
}

int BookIndex::getNextId()
{
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
    // Check if already exists, if so, delete old entry first to avoid duplicates
    // Or just allow duplicates? Let's allow duplicates but with different IDs for simplicity,
    // or we could update the existing one.
    // For this POC, let's just add new.

    int id = getNextId();
    char path[32];
    snprintf(path, sizeof(path), "/spiffs/%d.epub", id);

    books.push_back({id, title, std::string(path), 0, 0, 0, false});
    save();

    return std::string(path);
}

void BookIndex::removeBook(int id)
{
    auto it = std::remove_if(books.begin(), books.end(), [id](const BookEntry &b)
                             { return b.id == id; });

    if (it != books.end())
    {
        books.erase(it, books.end());
        save();
    }
}

std::vector<BookEntry> BookIndex::getBooks()
{
    return books;
}
