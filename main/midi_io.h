/**
 * @file midi_io.h
 * @brief MIDI file import/export for the Music Composer
 * 
 * Supports:
 * - Standard MIDI File (SMF) Type 0 (single track) for simplicity
 * - Import: Reads notes and quantizes to current grid resolution
 * - Export: Writes composition as Type 0 MIDI with tempo, program changes, and notes
 * 
 * MIDI File Structure (Type 0):
 * - Header chunk (MThd): File type, track count, timing division
 * - Track chunk (MTrk): Contains all events (tempo, notes, end of track)
 * 
 * Quantization Strategy:
 * - On import, note times are rounded to nearest grid step
 * - Duration is preserved as closely as possible
 * - Channel 1 notes become MIDI channel 0, Channel 2 becomes MIDI channel 1
 */

#pragma once

#include <string>
#include <vector>
#include "sequencer_engine.h"

/**
 * @brief Result of MIDI operations
 */
enum class MidiResult {
    OK,
    FILE_NOT_FOUND,
    FILE_READ_ERROR,
    FILE_WRITE_ERROR,
    INVALID_FORMAT,
    UNSUPPORTED_FORMAT,
    OUT_OF_MEMORY,
    SD_NOT_MOUNTED
};

/**
 * @brief Get human-readable error message
 */
const char* midiResultToString(MidiResult result);

/**
 * @brief MIDI file I/O class
 */
class MidiIO {
public:
    /**
     * @brief Import a MIDI file into the composition
     * @param filepath Full path to the MIDI file
     * @param composition Target composition to populate
     * @param targetDivision Grid division to quantize to
     * @return Result of the operation
     * 
     * Notes:
     * - Clears existing notes in composition
     * - Reads tempo from MIDI file
     * - Quantizes all notes to the target grid
     * - MIDI channel 0 -> Composer channel 1
     * - MIDI channel 1 -> Composer channel 2
     * - Other channels are ignored
     */
    static MidiResult importFile(const std::string& filepath, 
                                  Composition& composition,
                                  StepDivision targetDivision = StepDivision::SIXTEENTH);
    
    /**
     * @brief Export composition to a MIDI file
     * @param filepath Full path for the output MIDI file
     * @param composition Source composition
     * @return Result of the operation
     * 
     * Notes:
     * - Creates a Type 0 MIDI file
     * - Channel 1 -> MIDI channel 0
     * - Channel 2 -> MIDI channel 1
     * - Includes tempo meta event
     */
    static MidiResult exportFile(const std::string& filepath,
                                  const Composition& composition);
    
    /**
     * @brief List MIDI files in a directory
     * @param directory Directory path to scan
     * @param files Output vector of filenames (not full paths)
     * @return Result (OK or error)
     */
    static MidiResult listMidiFiles(const std::string& directory,
                                    std::vector<std::string>& files);

private:
    // MIDI parsing helpers
    static uint32_t readVarLen(const uint8_t* data, size_t& pos, size_t maxLen);
    static void writeVarLen(std::vector<uint8_t>& data, uint32_t value);
    static uint16_t readBE16(const uint8_t* data);
    static uint32_t readBE32(const uint8_t* data);
    static void writeBE16(std::vector<uint8_t>& data, uint16_t value);
    static void writeBE32(std::vector<uint8_t>& data, uint32_t value);
    
    // Convert between MIDI ticks and composer steps
    static uint16_t ticksToSteps(uint32_t ticks, uint16_t ppqn, 
                                  uint16_t tempo, StepDivision division);
    static uint32_t stepsToTicks(uint16_t steps, uint16_t ppqn,
                                  StepDivision division);
};
