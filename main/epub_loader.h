#pragma once
#include <string>
#include <vector>
#include "zip_reader.h"

class EpubLoader {
public:
    bool load(const char* path, int restoreChapterIndex = -1);
    // Lightweight metadata-only load (title/lang), no chapter parsing or heuristics.
    bool loadMetadataOnly(const char* path);
    std::string getTitle();
    
    // Navigation
    std::string getText(size_t offset, size_t length);
    size_t getChapterSize() const { return currentChapterSize; }
    
    bool nextChapter();
    bool prevChapter();
    size_t getChapterTextLength(int index);
    
    int getCurrentChapterIndex() const { return currentChapterIndex; }
    int getTotalChapters() const { return spine.size(); }
    
    bool jumpToChapter(int index);

    void close();
    
    std::string getLanguage() const { return language; }
    
private:
    std::string currentPath;
    std::string language;
    std::string title; // Added title
    ZipReader zip;
    bool isOpen = false;
    
    std::vector<std::string> spine; // List of HTML files in order
    std::string rootDir; // Directory where OPF is located
    
    int currentChapterIndex = 0;
    int currentTextOffset = 0;
    size_t currentChapterSize = 0;
    std::vector<size_t> chapterTextLengths;
    
    // Memory cache for current chapter
    std::string currentChapterContent;

    // Helpers
    bool parseContainer();
    bool parseOPF(const std::string& opfPath);
    void loadChapter(int index);
    std::string readFileFromZip(const std::string& path);
    bool isChapterSkippable(int index);
};
