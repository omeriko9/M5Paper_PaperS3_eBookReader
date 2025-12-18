#pragma once
#include <string>
#include <vector>
#include <map>
#include <stdint.h>
#include <functional>

struct ZipFileInfo {
    std::string name;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint32_t localHeaderOffset;
    uint16_t compressionMethod;
};

class ZipReader {
public:
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
     * @brief Get list of all files in the ZIP
     * @return Vector of file paths
     */
    std::vector<std::string> listFiles() const;
    
private:
    std::string filePath;
    std::map<std::string, ZipFileInfo> files;
    bool parseCentralDirectory();
};
