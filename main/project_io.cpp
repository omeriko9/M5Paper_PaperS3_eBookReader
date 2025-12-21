/**
 * @file project_io.cpp
 * @brief Project save/load implementation
 */

#include "project_io.h"
#include "device_hal.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

static const char* TAG = "ProjectIO";

// Default directory on SD card
static constexpr const char* PROJECT_DIR = "/sdcard/music";

const char* projectResultToString(ProjectResult result) {
    switch (result) {
        case ProjectResult::OK: return "OK";
        case ProjectResult::FILE_NOT_FOUND: return "File not found";
        case ProjectResult::FILE_READ_ERROR: return "File read error";
        case ProjectResult::FILE_WRITE_ERROR: return "File write error";
        case ProjectResult::INVALID_FORMAT: return "Invalid project format";
        case ProjectResult::VERSION_MISMATCH: return "Incompatible version";
        case ProjectResult::OUT_OF_MEMORY: return "Out of memory";
        case ProjectResult::SD_NOT_MOUNTED: return "SD card not mounted";
        default: return "Unknown error";
    }
}

void ProjectIO::writeLE16(std::vector<uint8_t>& data, uint16_t value) {
    data.push_back(value & 0xFF);
    data.push_back((value >> 8) & 0xFF);
}

void ProjectIO::writeLE32(std::vector<uint8_t>& data, uint32_t value) {
    data.push_back(value & 0xFF);
    data.push_back((value >> 8) & 0xFF);
    data.push_back((value >> 16) & 0xFF);
    data.push_back((value >> 24) & 0xFF);
}

uint16_t ProjectIO::readLE16(const uint8_t* data) {
    return data[0] | (data[1] << 8);
}

uint32_t ProjectIO::readLE32(const uint8_t* data) {
    return data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
}

std::string ProjectIO::getProjectDirectory() {
    return PROJECT_DIR;
}

ProjectResult ProjectIO::ensureProjectDirectory() {
    DeviceHAL& hal = DeviceHAL::getInstance();
    
    if (!hal.isSDCardMounted()) {
        return ProjectResult::SD_NOT_MOUNTED;
    }
    
    struct stat st;
    if (stat(PROJECT_DIR, &st) == 0) {
        // Directory exists
        return ProjectResult::OK;
    }
    
    // Create directory
    if (mkdir(PROJECT_DIR, 0755) != 0) {
        ESP_LOGW(TAG, "Failed to create directory: %s", PROJECT_DIR);
        return ProjectResult::FILE_WRITE_ERROR;
    }
    
    ESP_LOGI(TAG, "Created project directory: %s", PROJECT_DIR);
    return ProjectResult::OK;
}

ProjectResult ProjectIO::saveProject(const std::string& filepath,
                                      const Composition& composition) {
    DeviceHAL& hal = DeviceHAL::getInstance();
    
    if (!hal.isSDCardMounted()) {
        return ProjectResult::SD_NOT_MOUNTED;
    }
    
    std::vector<uint8_t> data;
    
    // Header
    writeLE32(data, FILE_MAGIC);
    data.push_back(FILE_VERSION);
    data.push_back(0);  // Flags (reserved)
    
    // Composition metadata
    writeLE16(data, composition.tempo);
    data.push_back(composition.timeSignatureNum);
    data.push_back(composition.timeSignatureDen);
    data.push_back(static_cast<uint8_t>(composition.division));
    writeLE16(data, composition.lengthBars);
    data.push_back(composition.loopEnabled ? 1 : 0);
    data.push_back(composition.channel1Instrument);
    data.push_back(composition.channel2Instrument);
    
    // Channel 1
    writeLE16(data, composition.channel1.size());
    for (const auto& note : composition.channel1) {
        writeLE16(data, note.startStep);
        writeLE16(data, note.duration);
        data.push_back(note.pitch);
        data.push_back(note.velocity);
        data.push_back(note.enabled ? 1 : 0);
    }
    
    // Channel 2
    writeLE16(data, composition.channel2.size());
    for (const auto& note : composition.channel2) {
        writeLE16(data, note.startStep);
        writeLE16(data, note.duration);
        data.push_back(note.pitch);
        data.push_back(note.velocity);
        data.push_back(note.enabled ? 1 : 0);
    }
    
    // Write to file
    FILE* f = fopen(filepath.c_str(), "wb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to create file: %s", filepath.c_str());
        return ProjectResult::FILE_WRITE_ERROR;
    }
    
    size_t written = fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    
    if (written != data.size()) {
        ESP_LOGW(TAG, "Write incomplete: %d/%d bytes", written, data.size());
        return ProjectResult::FILE_WRITE_ERROR;
    }
    
    ESP_LOGI(TAG, "Saved project: %s (%d bytes)", filepath.c_str(), data.size());
    return ProjectResult::OK;
}

ProjectResult ProjectIO::loadProject(const std::string& filepath,
                                      Composition& composition) {
    DeviceHAL& hal = DeviceHAL::getInstance();
    
    if (!hal.isSDCardMounted()) {
        return ProjectResult::SD_NOT_MOUNTED;
    }
    
    FILE* f = fopen(filepath.c_str(), "rb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open file: %s", filepath.c_str());
        return ProjectResult::FILE_NOT_FOUND;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    size_t fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    // Minimum size: header (6) + metadata (8) + channel counts (4)
    // Version 2 adds 2 bytes for instruments.
    if (fileSize < 18) {
        fclose(f);
        return ProjectResult::INVALID_FORMAT;
    }
    
    // Read entire file
    std::vector<uint8_t> data(fileSize);
    if (fread(data.data(), 1, fileSize, f) != fileSize) {
        fclose(f);
        return ProjectResult::FILE_READ_ERROR;
    }
    fclose(f);
    
    size_t pos = 0;
    
    // Check magic
    if (readLE32(&data[pos]) != FILE_MAGIC) {
        ESP_LOGW(TAG, "Invalid file magic");
        return ProjectResult::INVALID_FORMAT;
    }
    pos += 4;
    
    // Check version
    uint8_t version = data[pos++];
    if (version > FILE_VERSION) {
        ESP_LOGW(TAG, "Unsupported version: %d (max: %d)", version, FILE_VERSION);
        return ProjectResult::VERSION_MISMATCH;
    }
    
    pos++;  // Skip flags
    
    // Read metadata
    composition.clear();
    composition.tempo = readLE16(&data[pos]); pos += 2;
    composition.timeSignatureNum = data[pos++];
    composition.timeSignatureDen = data[pos++];
    composition.division = static_cast<StepDivision>(data[pos++]);
    composition.lengthBars = readLE16(&data[pos]); pos += 2;
    composition.loopEnabled = (data[pos++] != 0);
    if (version >= 2) {
        if (pos + 2 > fileSize) return ProjectResult::INVALID_FORMAT;
        composition.channel1Instrument = data[pos++];
        composition.channel2Instrument = data[pos++];
        if (composition.channel1Instrument > 127) composition.channel1Instrument = 127;
        if (composition.channel2Instrument > 127) composition.channel2Instrument = 127;
    } else {
        composition.channel1Instrument = 0;
        composition.channel2Instrument = 0;
    }
    
    // Validate values
    composition.tempo = std::max((uint16_t)30, std::min((uint16_t)300, composition.tempo));
    composition.lengthBars = std::max((uint16_t)1, std::min((uint16_t)64, composition.lengthBars));
    
    // Read channel 1
    if (pos + 2 > fileSize) return ProjectResult::INVALID_FORMAT;
    uint16_t ch1Count = readLE16(&data[pos]); pos += 2;
    
    for (uint16_t i = 0; i < ch1Count && pos + 7 <= fileSize; i++) {
        NoteEvent note;
        note.startStep = readLE16(&data[pos]); pos += 2;
        note.duration = readLE16(&data[pos]); pos += 2;
        note.pitch = data[pos++];
        note.velocity = data[pos++];
        note.enabled = (data[pos++] != 0);
        composition.channel1.push_back(note);
    }
    
    // Read channel 2
    if (pos + 2 > fileSize) return ProjectResult::INVALID_FORMAT;
    uint16_t ch2Count = readLE16(&data[pos]); pos += 2;
    
    for (uint16_t i = 0; i < ch2Count && pos + 7 <= fileSize; i++) {
        NoteEvent note;
        note.startStep = readLE16(&data[pos]); pos += 2;
        note.duration = readLE16(&data[pos]); pos += 2;
        note.pitch = data[pos++];
        note.velocity = data[pos++];
        note.enabled = (data[pos++] != 0);
        composition.channel2.push_back(note);
    }
    
    ESP_LOGI(TAG, "Loaded project: %s (tempo=%d, %d bars, %d+%d notes)",
             filepath.c_str(), composition.tempo, composition.lengthBars,
             composition.channel1.size(), composition.channel2.size());
    
    return ProjectResult::OK;
}

ProjectResult ProjectIO::listProjectFiles(const std::string& directory,
                                           std::vector<std::string>& files) {
    DeviceHAL& hal = DeviceHAL::getInstance();
    
    if (!hal.isSDCardMounted()) {
        return ProjectResult::SD_NOT_MOUNTED;
    }
    
    files.clear();
    
    DIR* dir = opendir(directory.c_str());
    if (!dir) {
        // Try to create the directory
        if (ensureProjectDirectory() == ProjectResult::OK) {
            dir = opendir(directory.c_str());
        }
        if (!dir) {
            ESP_LOGW(TAG, "Failed to open directory: %s", directory.c_str());
            return ProjectResult::FILE_NOT_FOUND;
        }
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_REG) {
            std::string name = entry->d_name;
            // Check for .mcs extension
            if (name.length() > 4) {
                std::string ext = name.substr(name.length() - 4);
                for (char& c : ext) c = tolower(c);
                if (ext == ".mcs") {
                    files.push_back(name);
                }
            }
        }
    }
    
    closedir(dir);
    std::sort(files.begin(), files.end());
    
    ESP_LOGI(TAG, "Found %d project files in %s", files.size(), directory.c_str());
    return ProjectResult::OK;
}

ProjectResult ProjectIO::getProjectInfo(const std::string& filepath, ProjectInfo& info) {
    DeviceHAL& hal = DeviceHAL::getInstance();
    
    if (!hal.isSDCardMounted()) {
        return ProjectResult::SD_NOT_MOUNTED;
    }
    
    FILE* f = fopen(filepath.c_str(), "rb");
    if (!f) {
        return ProjectResult::FILE_NOT_FOUND;
    }
    
    // Read header and metadata only (version 2 is 20 bytes minimum)
    uint8_t header[20];
    size_t readBytes = fread(header, 1, sizeof(header), f);
    if (readBytes < 18) {
        fclose(f);
        return ProjectResult::INVALID_FORMAT;
    }
    
    // Check magic
    if (readLE32(header) != FILE_MAGIC) {
        fclose(f);
        return ProjectResult::INVALID_FORMAT;
    }
    
    // Extract info
    uint8_t version = header[4];
    info.tempo = readLE16(&header[6]);
    info.division = header[10];
    info.lengthBars = readLE16(&header[11]);
    size_t ch1Offset = (version >= 2) ? 16 : 14;
    if (readBytes < ch1Offset + 2) {
        fclose(f);
        return ProjectResult::INVALID_FORMAT;
    }
    info.noteCount = readLE16(&header[ch1Offset]);  // Channel 1 count
    
    // Get file modification time
    struct stat st;
    if (fstat(fileno(f), &st) == 0) {
        info.modifiedTime = st.st_mtime;
    } else {
        info.modifiedTime = 0;
    }
    
    fclose(f);
    
    // Extract filename from path
    size_t lastSlash = filepath.rfind('/');
    if (lastSlash != std::string::npos) {
        info.filename = filepath.substr(lastSlash + 1);
    } else {
        info.filename = filepath;
    }
    
    return ProjectResult::OK;
}
