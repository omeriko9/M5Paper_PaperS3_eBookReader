#pragma once
#include <string>
#include <vector>
#include <map>
#include "zip_reader.h"

/**
 * @brief Represents an image reference embedded in chapter content
 */
struct EpubImage {
    size_t textOffset;       // Position in text content where image appears
    std::string path;        // Path to image within EPUB (relative to rootDir)
    std::string alt;         // Alt text if provided
    bool isBlock;            // True if image is block-level (full width)
    int width;               // Specified width (-1 if not specified)
    int height;              // Specified height (-1 if not specified)
};

class EpubLoader {
public:
    bool load(const char* path, int restoreChapterIndex = -1);
    // Lightweight metadata-only load (title/lang), no chapter parsing or heuristics.
    bool loadMetadataOnly(const char* path);
    std::string getTitle();
    std::string getAuthor();
    
    // Navigation
    std::string getText(size_t offset, size_t length);
    size_t getChapterSize() const { return currentChapterSize; }
    
    bool nextChapter();
    bool prevChapter();
    size_t getChapterTextLength(int index);
    
    int getCurrentChapterIndex() const { return currentChapterIndex; }
    int getTotalChapters() const { return spine.size(); }
    const std::vector<std::string>& getSpine() const { return spine; }
    
    // Get chapter titles from TOC (navMap)
    const std::vector<std::string>& getChapterTitles() const { return chapterTitles; }
    
    bool jumpToChapter(int index);

    void close();
    
    std::string getLanguage() const { return language; }
    
    // Image support
    /**
     * @brief Get all images in the current chapter
     * @return Vector of image references
     */
    const std::vector<EpubImage>& getChapterImages() const { return currentChapterImages; }
    
    /**
     * @brief Extract image data from EPUB
     * @param imagePath Path to image within EPUB
     * @param outData Output buffer for binary image data
     * @return true if successful
     */
    bool extractImage(const std::string& imagePath, std::vector<uint8_t>& outData);
    
    /**
     * @brief Find image at or near text offset
     * @param textOffset Text offset to search near
     * @param tolerance How many characters to search around offset
     * @return Pointer to image info, or nullptr if not found
     */
    const EpubImage* findImageAtOffset(size_t textOffset, size_t tolerance = 10) const;
    
    /**
     * @brief Check if there's an image placeholder at exact offset
     * @param textOffset Text offset to check
     * @return true if there's an image placeholder at this offset
     */
    bool hasImageAtOffset(size_t textOffset) const;
    
private:
    std::string currentPath;
    std::string language;
    std::string title;
    std::string author;
    ZipReader zip;
    bool isOpen = false;
    
    std::vector<std::string> spine; // List of HTML files in order
    std::vector<std::string> chapterTitles; // Chapter titles from NCX/NAV
    std::string rootDir; // Directory where OPF is located
    std::string currentChapterDir; // Directory of current chapter file
    
    int currentChapterIndex = 0;
    int currentTextOffset = 0;
    size_t currentChapterSize = 0;
    std::vector<size_t> chapterTextLengths;
    
    // Memory cache for current chapter
    std::string currentChapterContent;
    
    // Image references in current chapter
    std::vector<EpubImage> currentChapterImages;

    // Helpers
    bool parseContainer();
    bool parseOPF(const std::string& opfPath);
    void parseTOC(const std::string& tocPath, bool isNcx);  // Parse NCX or NAV TOC
    void loadChapter(int index);
    std::string readFileFromZip(const std::string& path);
    bool isChapterSkippable(int index);
    
    // Image path resolution
    std::string resolveImagePath(const std::string& src);
};
