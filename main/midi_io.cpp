/**
 * @file midi_io.cpp
 * @brief MIDI file import/export implementation
 */

#include "midi_io.h"
#include "device_hal.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

static const char* TAG = "MidiIO";

// MIDI file constants
static constexpr uint32_t MIDI_HEADER_MAGIC = 0x4D546864;  // "MThd"
static constexpr uint32_t MIDI_TRACK_MAGIC = 0x4D54726B;   // "MTrk"
static constexpr uint16_t DEFAULT_PPQN = 480;              // Pulses per quarter note

// MIDI event types
static constexpr uint8_t MIDI_NOTE_OFF = 0x80;
static constexpr uint8_t MIDI_NOTE_ON = 0x90;
static constexpr uint8_t MIDI_META_EVENT = 0xFF;
static constexpr uint8_t META_SET_TEMPO = 0x51;
static constexpr uint8_t META_END_OF_TRACK = 0x2F;
static constexpr uint8_t META_TIME_SIGNATURE = 0x58;

const char* midiResultToString(MidiResult result) {
    switch (result) {
        case MidiResult::OK: return "OK";
        case MidiResult::FILE_NOT_FOUND: return "File not found";
        case MidiResult::FILE_READ_ERROR: return "File read error";
        case MidiResult::FILE_WRITE_ERROR: return "File write error";
        case MidiResult::INVALID_FORMAT: return "Invalid MIDI format";
        case MidiResult::UNSUPPORTED_FORMAT: return "Unsupported MIDI format";
        case MidiResult::OUT_OF_MEMORY: return "Out of memory";
        case MidiResult::SD_NOT_MOUNTED: return "SD card not mounted";
        default: return "Unknown error";
    }
}

// Variable-length quantity reading (MIDI format)
uint32_t MidiIO::readVarLen(const uint8_t* data, size_t& pos, size_t maxLen) {
    uint32_t value = 0;
    uint8_t c;
    
    do {
        if (pos >= maxLen) return value;
        c = data[pos++];
        value = (value << 7) | (c & 0x7F);
    } while (c & 0x80);
    
    return value;
}

// Variable-length quantity writing
void MidiIO::writeVarLen(std::vector<uint8_t>& data, uint32_t value) {
    uint8_t buffer[5];
    int count = 0;
    
    buffer[count++] = value & 0x7F;
    value >>= 7;
    
    while (value > 0) {
        buffer[count++] = (value & 0x7F) | 0x80;
        value >>= 7;
    }
    
    // Write in reverse order
    while (count > 0) {
        data.push_back(buffer[--count]);
    }
}

uint16_t MidiIO::readBE16(const uint8_t* data) {
    return (data[0] << 8) | data[1];
}

uint32_t MidiIO::readBE32(const uint8_t* data) {
    return (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
}

void MidiIO::writeBE16(std::vector<uint8_t>& data, uint16_t value) {
    data.push_back((value >> 8) & 0xFF);
    data.push_back(value & 0xFF);
}

void MidiIO::writeBE32(std::vector<uint8_t>& data, uint32_t value) {
    data.push_back((value >> 24) & 0xFF);
    data.push_back((value >> 16) & 0xFF);
    data.push_back((value >> 8) & 0xFF);
    data.push_back(value & 0xFF);
}

uint16_t MidiIO::ticksToSteps(uint32_t ticks, uint16_t ppqn, 
                               uint16_t tempo, StepDivision division) {
    // Convert MIDI ticks to sequencer steps
    // ppqn = ticks per quarter note
    // division = steps per whole note (which is 4 quarter notes)
    // So: ticks_per_whole_note = ppqn * 4
    //     ticks_per_step = ppqn * 4 / division
    
    uint32_t ticksPerStep = (ppqn * 4) / static_cast<uint32_t>(division);
    if (ticksPerStep == 0) ticksPerStep = 1;
    
    return (uint16_t)((ticks + ticksPerStep / 2) / ticksPerStep);  // Round
}

uint32_t MidiIO::stepsToTicks(uint16_t steps, uint16_t ppqn, StepDivision division) {
    uint32_t ticksPerStep = (ppqn * 4) / static_cast<uint32_t>(division);
    return steps * ticksPerStep;
}

MidiResult MidiIO::importFile(const std::string& filepath, 
                               Composition& composition,
                               StepDivision targetDivision) {
    DeviceHAL& hal = DeviceHAL::getInstance();
    
    // Check SD card
    if (!hal.isSDCardMounted()) {
        ESP_LOGW(TAG, "SD card not mounted");
        return MidiResult::SD_NOT_MOUNTED;
    }
    
    // Open file
    FILE* f = fopen(filepath.c_str(), "rb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open file: %s", filepath.c_str());
        return MidiResult::FILE_NOT_FOUND;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    size_t fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (fileSize < 14) {  // Minimum MIDI file size
        fclose(f);
        return MidiResult::INVALID_FORMAT;
    }
    
    // Read entire file
    std::vector<uint8_t> data(fileSize);
    if (fread(data.data(), 1, fileSize, f) != fileSize) {
        fclose(f);
        return MidiResult::FILE_READ_ERROR;
    }
    fclose(f);
    
    ESP_LOGI(TAG, "Read MIDI file: %s (%d bytes)", filepath.c_str(), fileSize);
    
    // Parse header chunk
    size_t pos = 0;
    
    if (readBE32(&data[pos]) != MIDI_HEADER_MAGIC) {
        ESP_LOGW(TAG, "Invalid MIDI header magic");
        return MidiResult::INVALID_FORMAT;
    }
    pos += 4;
    
    uint32_t headerLen = readBE32(&data[pos]);
    pos += 4;
    
    if (headerLen < 6) {
        return MidiResult::INVALID_FORMAT;
    }
    
    uint16_t format = readBE16(&data[pos]);
    pos += 2;
    
    uint16_t numTracks = readBE16(&data[pos]);
    pos += 2;
    
    uint16_t division = readBE16(&data[pos]);
    pos += 2;
    
    ESP_LOGI(TAG, "MIDI Format: %d, Tracks: %d, Division: %d", 
             format, numTracks, division);
    
    // We support Type 0 (single track) and Type 1 (multiple tracks)
    if (format > 1) {
        ESP_LOGW(TAG, "Unsupported MIDI format: %d", format);
        return MidiResult::UNSUPPORTED_FORMAT;
    }
    
    // Check if SMPTE timing (we only support PPQN)
    if (division & 0x8000) {
        ESP_LOGW(TAG, "SMPTE timing not supported");
        return MidiResult::UNSUPPORTED_FORMAT;
    }
    
    uint16_t ppqn = division;
    
    // Skip to end of header
    pos = 8 + headerLen;
    
    // Clear composition and set parameters
    composition.clear();
    composition.division = targetDivision;
    composition.tempo = 120;  // Default, may be overwritten by tempo event
    
    // Track active notes for note-off matching
    struct ActiveNote {
        uint8_t pitch;
        uint8_t channel;
        uint32_t startTick;
        uint8_t velocity;
    };
    std::vector<ActiveNote> activeNotes;
    
    uint32_t absoluteTick = 0;
    uint16_t maxStep = 0;
    
    // Parse track chunks
    for (int track = 0; track < numTracks && pos < fileSize; track++) {
        if (pos + 8 > fileSize) break;
        
        if (readBE32(&data[pos]) != MIDI_TRACK_MAGIC) {
            ESP_LOGW(TAG, "Invalid track magic at pos %d", pos);
            pos += 4;
            continue;
        }
        pos += 4;
        
        uint32_t trackLen = readBE32(&data[pos]);
        pos += 4;
        
        size_t trackEnd = pos + trackLen;
        if (trackEnd > fileSize) trackEnd = fileSize;
        
        ESP_LOGI(TAG, "Track %d: %d bytes", track, trackLen);
        
        absoluteTick = 0;
        uint8_t runningStatus = 0;
        
        while (pos < trackEnd) {
            // Read delta time
            uint32_t deltaTime = readVarLen(data.data(), pos, trackEnd);
            absoluteTick += deltaTime;
            
            if (pos >= trackEnd) break;
            
            // Read event
            uint8_t eventByte = data[pos];
            
            if (eventByte == MIDI_META_EVENT) {
                // Meta event
                pos++;
                if (pos >= trackEnd) break;
                
                uint8_t metaType = data[pos++];
                uint32_t metaLen = readVarLen(data.data(), pos, trackEnd);
                
                if (metaType == META_SET_TEMPO && metaLen == 3 && pos + 3 <= trackEnd) {
                    // Tempo: microseconds per quarter note
                    uint32_t uspqn = (data[pos] << 16) | (data[pos+1] << 8) | data[pos+2];
                    uint16_t bpm = 60000000 / uspqn;
                    composition.tempo = std::max((uint16_t)30, std::min((uint16_t)300, bpm));
                    ESP_LOGI(TAG, "Tempo: %d BPM", composition.tempo);
                }
                else if (metaType == META_TIME_SIGNATURE && metaLen >= 2 && pos + 2 <= trackEnd) {
                    composition.timeSignatureNum = data[pos];
                    composition.timeSignatureDen = 1 << data[pos+1];
                    ESP_LOGI(TAG, "Time signature: %d/%d", 
                             composition.timeSignatureNum, composition.timeSignatureDen);
                }
                else if (metaType == META_END_OF_TRACK) {
                    ESP_LOGI(TAG, "End of track %d at tick %d", track, absoluteTick);
                }
                
                pos += metaLen;
            }
            else if (eventByte >= 0xF0) {
                // System event (skip)
                pos++;
                if (eventByte == 0xF0 || eventByte == 0xF7) {
                    // SysEx - read length and skip
                    uint32_t sysexLen = readVarLen(data.data(), pos, trackEnd);
                    pos += sysexLen;
                }
            }
            else {
                // Channel event
                uint8_t status;
                if (eventByte & 0x80) {
                    status = eventByte;
                    runningStatus = status;
                    pos++;
                } else {
                    // Running status
                    status = runningStatus;
                }
                
                uint8_t type = status & 0xF0;
                uint8_t channel = status & 0x0F;
                
                if (type == MIDI_NOTE_ON || type == MIDI_NOTE_OFF) {
                    if (pos + 2 > trackEnd) break;
                    
                    uint8_t pitch = data[pos++];
                    uint8_t velocity = data[pos++];
                    
                    // Note on with velocity 0 is actually note off
                    bool isNoteOn = (type == MIDI_NOTE_ON && velocity > 0);
                    
                    if (isNoteOn) {
                        // Only process channels 0 and 1
                        if (channel <= 1) {
                            ActiveNote an;
                            an.pitch = pitch;
                            an.channel = channel;
                            an.startTick = absoluteTick;
                            an.velocity = velocity;
                            activeNotes.push_back(an);
                        }
                    } else {
                        // Note off - find matching note
                        for (auto it = activeNotes.begin(); it != activeNotes.end(); ++it) {
                            if (it->pitch == pitch && it->channel == channel) {
                                // Create note event
                                uint16_t startStep = ticksToSteps(it->startTick, ppqn, 
                                                                   composition.tempo, targetDivision);
                                uint16_t endStep = ticksToSteps(absoluteTick, ppqn,
                                                                 composition.tempo, targetDivision);
                                uint16_t duration = (endStep > startStep) ? (endStep - startStep) : 1;
                                
                                NoteEvent note(startStep, duration, it->pitch, it->velocity);
                                
                                if (channel == 0) {
                                    composition.channel1.push_back(note);
                                } else {
                                    composition.channel2.push_back(note);
                                }
                                
                                if (startStep + duration > maxStep) {
                                    maxStep = startStep + duration;
                                }
                                
                                activeNotes.erase(it);
                                break;
                            }
                        }
                    }
                }
                else if (type == 0xA0 || type == 0xB0 || type == 0xE0) {
                    // Aftertouch, Control Change, Pitch Bend - 2 data bytes
                    pos += 2;
                }
                else if (type == 0xC0 || type == 0xD0) {
                    // Program Change, Channel Pressure - 1 data byte
                    if (pos + 1 > trackEnd) break;
                    uint8_t program = data[pos++];
                    if (type == 0xC0 && channel <= 1) {
                        if (channel == 0) {
                            composition.channel1Instrument = program;
                        } else {
                            composition.channel2Instrument = program;
                        }
                    }
                }
            }
        }
    }
    
    // Calculate required bars
    uint16_t stepsPerBar = composition.stepsPerBar();
    if (stepsPerBar == 0) stepsPerBar = 16;
    
    composition.lengthBars = (maxStep + stepsPerBar - 1) / stepsPerBar;
    if (composition.lengthBars < 1) composition.lengthBars = 1;
    if (composition.lengthBars > 64) composition.lengthBars = 64;
    
    ESP_LOGI(TAG, "Imported %d notes on channel 1, %d on channel 2, %d bars",
             composition.channel1.size(), composition.channel2.size(), 
             composition.lengthBars);
    
    return MidiResult::OK;
}

MidiResult MidiIO::exportFile(const std::string& filepath,
                               const Composition& composition) {
    DeviceHAL& hal = DeviceHAL::getInstance();
    
    // Check SD card
    if (!hal.isSDCardMounted()) {
        ESP_LOGW(TAG, "SD card not mounted");
        return MidiResult::SD_NOT_MOUNTED;
    }
    
    std::vector<uint8_t> fileData;
    
    // Build track data first (we need its length for header)
    std::vector<uint8_t> trackData;
    
    // Tempo meta event at time 0
    writeVarLen(trackData, 0);  // Delta time = 0
    trackData.push_back(MIDI_META_EVENT);
    trackData.push_back(META_SET_TEMPO);
    trackData.push_back(3);  // Length
    uint32_t uspqn = 60000000 / composition.tempo;
    trackData.push_back((uspqn >> 16) & 0xFF);
    trackData.push_back((uspqn >> 8) & 0xFF);
    trackData.push_back(uspqn & 0xFF);
    
    // Time signature
    writeVarLen(trackData, 0);
    trackData.push_back(MIDI_META_EVENT);
    trackData.push_back(META_TIME_SIGNATURE);
    trackData.push_back(4);  // Length
    trackData.push_back(composition.timeSignatureNum);
    // Denominator as power of 2
    uint8_t denomPow = 0;
    for (int d = composition.timeSignatureDen; d > 1; d >>= 1) denomPow++;
    trackData.push_back(denomPow);
    trackData.push_back(24);  // MIDI clocks per metronome click
    trackData.push_back(8);   // 32nd notes per quarter note
    
    // Program changes at time 0 (if any)
    uint8_t program1 = composition.channel1Instrument;
    uint8_t program2 = composition.channel2Instrument;
    if (program1 > 127) program1 = 127;
    if (program2 > 127) program2 = 127;
    
    writeVarLen(trackData, 0);
    trackData.push_back(0xC0 | 0);  // Channel 0 program change
    trackData.push_back(program1);
    
    writeVarLen(trackData, 0);
    trackData.push_back(0xC0 | 1);  // Channel 1 program change
    trackData.push_back(program2);
    
    // Collect all note events and sort by time
    struct MidiNoteEvent {
        uint32_t tick;
        uint8_t channel;
        uint8_t type;  // 0x90 = note on, 0x80 = note off
        uint8_t pitch;
        uint8_t velocity;
    };
    std::vector<MidiNoteEvent> events;
    
    auto addNotes = [&](const std::vector<NoteEvent>& notes, uint8_t midiChannel) {
        for (const auto& note : notes) {
            if (!note.enabled) continue;
            
            uint32_t startTick = stepsToTicks(note.startStep, DEFAULT_PPQN, composition.division);
            uint32_t endTick = stepsToTicks(note.startStep + note.duration, DEFAULT_PPQN, composition.division);
            
            MidiNoteEvent noteOn;
            noteOn.tick = startTick;
            noteOn.channel = midiChannel;
            noteOn.type = MIDI_NOTE_ON;
            noteOn.pitch = note.pitch;
            noteOn.velocity = note.velocity;
            events.push_back(noteOn);
            
            MidiNoteEvent noteOff;
            noteOff.tick = endTick;
            noteOff.channel = midiChannel;
            noteOff.type = MIDI_NOTE_OFF;
            noteOff.pitch = note.pitch;
            noteOff.velocity = 0;
            events.push_back(noteOff);
        }
    };
    
    addNotes(composition.channel1, 0);
    addNotes(composition.channel2, 1);
    
    // Sort by tick, with note-off before note-on at same tick
    std::sort(events.begin(), events.end(), [](const MidiNoteEvent& a, const MidiNoteEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.type < b.type;  // Note-off (0x80) before note-on (0x90)
    });
    
    // Write events
    uint32_t lastTick = 0;
    for (const auto& evt : events) {
        uint32_t delta = evt.tick - lastTick;
        lastTick = evt.tick;
        
        writeVarLen(trackData, delta);
        trackData.push_back(evt.type | evt.channel);
        trackData.push_back(evt.pitch);
        trackData.push_back(evt.velocity);
    }
    
    // End of track
    writeVarLen(trackData, 0);
    trackData.push_back(MIDI_META_EVENT);
    trackData.push_back(META_END_OF_TRACK);
    trackData.push_back(0);  // Length = 0
    
    // Now build file
    // Header chunk
    writeBE32(fileData, MIDI_HEADER_MAGIC);
    writeBE32(fileData, 6);  // Header length
    writeBE16(fileData, 0);  // Format 0
    writeBE16(fileData, 1);  // 1 track
    writeBE16(fileData, DEFAULT_PPQN);
    
    // Track chunk
    writeBE32(fileData, MIDI_TRACK_MAGIC);
    writeBE32(fileData, trackData.size());
    fileData.insert(fileData.end(), trackData.begin(), trackData.end());
    
    // Write to file
    FILE* f = fopen(filepath.c_str(), "wb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to create file: %s", filepath.c_str());
        return MidiResult::FILE_WRITE_ERROR;
    }
    
    size_t written = fwrite(fileData.data(), 1, fileData.size(), f);
    fclose(f);
    
    if (written != fileData.size()) {
        ESP_LOGW(TAG, "Write incomplete: %d/%d bytes", written, fileData.size());
        return MidiResult::FILE_WRITE_ERROR;
    }
    
    ESP_LOGI(TAG, "Exported MIDI file: %s (%d bytes, %d events)",
             filepath.c_str(), fileData.size(), events.size());
    
    return MidiResult::OK;
}

MidiResult MidiIO::listMidiFiles(const std::string& directory,
                                  std::vector<std::string>& files) {
    DeviceHAL& hal = DeviceHAL::getInstance();
    
    if (!hal.isSDCardMounted()) {
        return MidiResult::SD_NOT_MOUNTED;
    }
    
    files.clear();
    
    DIR* dir = opendir(directory.c_str());
    if (!dir) {
        ESP_LOGW(TAG, "Failed to open directory: %s", directory.c_str());
        return MidiResult::FILE_NOT_FOUND;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_REG) {  // Regular file
            std::string name = entry->d_name;
            // Check for .mid or .midi extension (case insensitive)
            size_t len = name.length();
            if (len > 4) {
                std::string ext = name.substr(len - 4);
                // Convert to lowercase
                for (char& c : ext) c = tolower(c);
                
                if (ext == ".mid") {
                    files.push_back(name);
                } else if (len > 5) {
                    ext = name.substr(len - 5);
                    for (char& c : ext) c = tolower(c);
                    if (ext == ".midi") {
                        files.push_back(name);
                    }
                }
            }
        }
    }
    
    closedir(dir);
    
    // Sort alphabetically
    std::sort(files.begin(), files.end());
    
    ESP_LOGI(TAG, "Found %d MIDI files in %s", files.size(), directory.c_str());
    
    return MidiResult::OK;
}
