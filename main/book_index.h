#pragma once
#include <string>
#include <vector>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct BookEntry {
    int id;
    std::string title; // The original filename/title
    std::string author; // Book author from EPUB metadata
    std::string path;  // The filesystem path (e.g., /spiffs/1.epub)
    int currentChapter = 0;
    size_t currentOffset = 0;
    size_t fileSize = 0; // For cache validation
    bool hasMetrics = false;
    bool isFavorite = false; // User-marked favorite
    std::string lastFont;    // Last font used for this book
    float lastFontSize = 1.0f;  // Last font size used
    bool isFailed = false;   // Failed to index/load
};

class BookIndex {
public:
    BookIndex();
    // Callback for progress updates: (current, total, message)
    using ProgressCallback = std::function<void(int, int, const char*)>;
    
    bool init(bool fastMode = false, ProgressCallback callback = nullptr);
    
    // Background scanning
    bool scanForNewBooks(ProgressCallback callback = nullptr, std::function<bool()> shouldPause = nullptr);
    bool validateBooks(std::function<bool()> shouldPause = nullptr); // Check if books still exist

    // Returns the new file path to save to (e.g. "/spiffs/5.epub")
    std::string addBook(const std::string& title);
    void removeBook(int id);
    void updateProgress(int id, int chapter, size_t offset);
    void markAsFailed(int id); // Mark book as failed to index
    void save(); // Force save index to disk
    std::vector<BookEntry> getBooks();
    std::vector<BookEntry> getFilteredBooks(const std::string& searchQuery, bool favoritesOnly);
    BookEntry getBook(int id);
    int getBookIdByPath(const std::string& path);
    bool scanDirectory(const char* basePath, ProgressCallback callback = nullptr, std::function<bool()> shouldPause = nullptr);
    
    // Favorites management
    void setFavorite(int id, bool favorite);
    bool isFavorite(int id);
    
    // Book font settings
    void setBookFont(int id, const std::string& fontName, float fontSize);
    bool getBookFont(int id, std::string& fontName, float& fontSize);

    // Metrics persistence
    bool saveBookMetrics(int id, size_t totalChars, const std::vector<size_t>& chapterOffsets, bool saveToDisk = true);
    bool loadBookMetrics(int id, size_t& totalChars, std::vector<size_t>& chapterOffsets);
    void updateBookMetricsFlag(int id, bool hasMetrics);

private:
    std::vector<BookEntry> books;
    SemaphoreHandle_t mutex = NULL;
    void load(ProgressCallback callback = nullptr);
    void saveInternal();
    int getNextId();
    void checkMetricsExistence();
};
