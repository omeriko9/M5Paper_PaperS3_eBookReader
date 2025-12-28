#pragma once
#include <string>
#include <vector>
#include <stdint.h>
#include <functional>
#include <memory>
#include "esp_heap_caps.h"
#include "task_coordinator.h"

struct ZipFileInfo {
    uint32_t nameOffset; // Offset in cdBuffer
    uint16_t nameLen;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint32_t localHeaderOffset;
    uint16_t compressionMethod;
};

class ZipReader {
public:
    void setPriority(TaskPriority priority);
    TaskPriority getPriority() const;
    bool wasAborted() const;
    void clearAbort();
    bool open(const char* path);
    void close();
    std::string extractFile(const std::string& filename);
    bool extractFile(const std::string& filename, bool (*callback)(const char*, size_t, void*), void* context);
    bool fileExists(const std::string& filename);
    uint32_t getUncompressedSize(const std::string& filename);
    
    /**
     * @brief Extract file as binary data (for images)
     * @param filename Path to file within ZIP
     * @param outData Output vector to receive binary data
     * @return true if successful
     */
    bool extractBinary(const std::string& filename, std::vector<uint8_t>& outData);
    
    /**
     * @brief Read the first N bytes of a file without extracting the whole file
     * @param filename File to peek
     * @param outData Buffer to store data
     * @param size Number of bytes to read
     * @return Number of bytes read
     */
    size_t peekFile(const std::string& filename, uint8_t* outData, size_t size);

    /**
     * @brief Get list of all files in the ZIP
     * @return Vector of file paths
     */
    std::vector<std::string> listFiles() const;
    
private:
    std::string filePath;
    std::vector<ZipFileInfo> files;
    std::unique_ptr<uint8_t, void(*)(void*)> cdBuffer{nullptr, heap_caps_free};
    TaskPriority ioPriority = TaskPriority::NORMAL;
    bool aborted = false;
    bool parseCentralDirectory();
    const ZipFileInfo* findFile(const std::string& filename) const;
    bool shouldAbort();
};
