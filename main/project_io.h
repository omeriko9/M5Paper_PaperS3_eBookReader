/**
 * @file project_io.h
 * @brief Project save/load for the Music Composer
 * 
 * Saves compositions in a lossless binary format (.mcs - Music Composer S3)
 * that preserves all sequencer data exactly.
 * 
 * File Format (.mcs):
 * - Header: Magic (4), Version (1), Flags (1)
 * - Composition: Tempo (2), TimeSignature (2), Division (1), LengthBars (2), LoopEnabled (1),
 *               Channel1Instrument (1), Channel2Instrument (1)
 * - Channel 1: NoteCount (2), Notes[]
 * - Channel 2: NoteCount (2), Notes[]
 * - Each Note: StartStep (2), Duration (2), Pitch (1), Velocity (1), Enabled (1)
 * 
 * All multi-byte values are little-endian for ESP32 compatibility.
 */

#pragma once

#include <string>
#include <vector>
#include <ctime>
#include "sequencer_engine.h"

/**
 * @brief Result of project I/O operations
 */
enum class ProjectResult {
    OK,
    FILE_NOT_FOUND,
    FILE_READ_ERROR,
    FILE_WRITE_ERROR,
    INVALID_FORMAT,
    VERSION_MISMATCH,
    OUT_OF_MEMORY,
    SD_NOT_MOUNTED
};

/**
 * @brief Get human-readable error message
 */
const char* projectResultToString(ProjectResult result);

/**
 * @brief Project metadata (for file browser display)
 */
struct ProjectInfo {
    std::string filename;
    uint16_t tempo;
    uint16_t lengthBars;
    uint8_t division;
    uint16_t noteCount;
    time_t modifiedTime;
};

/**
 * @brief Project file I/O class
 */
class ProjectIO {
public:
    // File extension
    static constexpr const char* FILE_EXTENSION = ".mcs";
    
    /**
     * @brief Save composition to a project file
     * @param filepath Full path to the output file
     * @param composition Source composition
     * @return Result of the operation
     */
    static ProjectResult saveProject(const std::string& filepath,
                                      const Composition& composition);
    
    /**
     * @brief Load composition from a project file
     * @param filepath Full path to the project file
     * @param composition Target composition to populate
     * @return Result of the operation
     */
    static ProjectResult loadProject(const std::string& filepath,
                                      Composition& composition);
    
    /**
     * @brief List project files in a directory
     * @param directory Directory path to scan
     * @param files Output vector of filenames (not full paths)
     * @return Result (OK or error)
     */
    static ProjectResult listProjectFiles(const std::string& directory,
                                           std::vector<std::string>& files);
    
    /**
     * @brief Get project info without fully loading
     * @param filepath Full path to the project file
     * @param info Output project info
     * @return Result of the operation
     */
    static ProjectResult getProjectInfo(const std::string& filepath,
                                         ProjectInfo& info);
    
    /**
     * @brief Create default project directory on SD card
     * @return Result (OK if created or already exists)
     */
    static ProjectResult ensureProjectDirectory();
    
    /**
     * @brief Get the default project directory path
     */
    static std::string getProjectDirectory();

private:
    // File format constants
    static constexpr uint32_t FILE_MAGIC = 0x5343534D;  // "MSCS" (little-endian)
    static constexpr uint8_t FILE_VERSION = 2;
    
    // Helper functions
    static void writeLE16(std::vector<uint8_t>& data, uint16_t value);
    static void writeLE32(std::vector<uint8_t>& data, uint32_t value);
    static uint16_t readLE16(const uint8_t* data);
    static uint32_t readLE32(const uint8_t* data);
};
