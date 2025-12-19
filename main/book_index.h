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
};

class BookIndex {
public:
    BookIndex();
    // Callback for progress updates: (current, total, message)
    using ProgressCallback = std::function<void(int, int, const char*)>;
    
    bool init(bool fastMode = false, ProgressCallback callback = nullptr);
    
    // Background scanning
    bool scanForNewBooks(ProgressCallback callback = nullptr);

    // Returns the new file path to save to (e.g. "/spiffs/5.epub")
    std::string addBook(const std::string& title);
    void removeBook(int id);
    void updateProgress(int id, int chapter, size_t offset);
    std::vector<BookEntry> getBooks();
    std::vector<BookEntry> getFilteredBooks(const std::string& searchQuery, bool favoritesOnly);
    BookEntry getBook(int id);
    bool scanDirectory(const char* basePath, ProgressCallback callback = nullptr);
    
    // Favorites management
    void setFavorite(int id, bool favorite);
    bool isFavorite(int id);
    
    // Book font settings
    void setBookFont(int id, const std::string& fontName, float fontSize);
    bool getBookFont(int id, std::string& fontName, float& fontSize);

    // Metrics persistence
    bool saveBookMetrics(int id, size_t totalChars, const std::vector<size_t>& chapterOffsets);
    bool loadBookMetrics(int id, size_t& totalChars, std::vector<size_t>& chapterOffsets);

private:
    std::vector<BookEntry> books;
    SemaphoreHandle_t mutex = NULL;
    void load();
    void save();
    int getNextId();
    void checkMetricsExistence();
};
