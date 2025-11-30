#pragma once
#include <string>
#include <vector>

struct BookEntry {
    int id;
    std::string title; // The original filename/title
    std::string path;  // The filesystem path (e.g., /spiffs/1.epub)
    int currentChapter = 0;
    size_t currentOffset = 0;
    size_t fileSize = 0; // For cache validation
    bool hasMetrics = false;
};

class BookIndex {
public:
    bool init(bool fastMode = false);
    // Returns the new file path to save to (e.g. "/spiffs/5.epub")
    std::string addBook(const std::string& title);
    void removeBook(int id);
    void updateProgress(int id, int chapter, size_t offset);
    std::vector<BookEntry> getBooks();
    BookEntry getBook(int id);
    bool scanDirectory(const char* basePath);

    // Metrics persistence
    bool saveBookMetrics(int id, size_t totalChars, const std::vector<size_t>& chapterOffsets);
    bool loadBookMetrics(int id, size_t& totalChars, std::vector<size_t>& chapterOffsets);

private:
    std::vector<BookEntry> books;
    void load();
    void save();
    int getNextId();
    void checkMetricsExistence();
};
