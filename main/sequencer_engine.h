/**
 * @file sequencer_engine.h
 * @brief 2-channel MIDI-style step sequencer engine for buzzer playback
 * 
 * This engine manages:
 * - Two channels of note events with start step, duration, pitch, velocity
 * - Timer-based playback independent of UI refresh rate
 * - Fast interpolation (rapid alternation) for overlapping notes on single buzzer
 * - Tempo, time signature, and step division control
 * 
 * Timing Strategy:
 * - Uses ESP high-resolution timer for accurate step timing
 * - Steps are quantized to current division (1, 1/2, 1/4, 1/8, 1/16, 1/32)
 * - Each bar has a fixed number of steps based on division
 * 
 * Buzzer Mixing Strategy:
 * - When two notes overlap, alternate between frequencies rapidly (every ~20ms)
 * - This creates a perceptible "chord" effect on a single buzzer
 * - Velocity controls duty cycle (PWM amplitude approximation)
 */

#pragma once

#include <stdint.h>
#include <vector>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

/**
 * @brief Step division options (note values relative to whole note)
 */
enum class StepDivision {
    WHOLE = 1,       // 1 step per bar
    HALF = 2,        // 2 steps per bar  
    QUARTER = 4,     // 4 steps per bar (common)
    EIGHTH = 8,      // 8 steps per bar
    SIXTEENTH = 16,  // 16 steps per bar
    THIRTY_SECOND = 32 // 32 steps per bar
};

/**
 * @brief Represents a single note event in the sequencer
 */
struct NoteEvent {
    uint16_t startStep;    // Step index where note starts (0-based)
    uint16_t duration;     // Duration in steps (minimum 1)
    uint8_t pitch;         // MIDI note number (0-127, middle C = 60)
    uint8_t velocity;      // Velocity/volume (0-127)
    bool enabled;          // Whether the note is active
    
    NoteEvent() : startStep(0), duration(1), pitch(60), velocity(100), enabled(true) {}
    NoteEvent(uint16_t start, uint16_t dur, uint8_t p, uint8_t vel)
        : startStep(start), duration(dur), pitch(p), velocity(vel), enabled(true) {}
    
    // Calculate end step (exclusive)
    uint16_t endStep() const { return startStep + duration; }
    
    // Check if note is active at a given step
    bool isActiveAt(uint16_t step) const {
        return enabled && step >= startStep && step < endStep();
    }
};

/**
 * @brief Composition data structure (serializable)
 */
struct Composition {
    uint16_t tempo;              // BPM (30-300)
    uint8_t timeSignatureNum;    // Numerator (default 4)
    uint8_t timeSignatureDen;    // Denominator (default 4)
    StepDivision division;       // Current step division
    uint16_t lengthBars;         // Length in bars (1-64)
    uint8_t channel1Instrument;  // MIDI program (0-127)
    uint8_t channel2Instrument;  // MIDI program (0-127)
    std::vector<NoteEvent> channel1;
    std::vector<NoteEvent> channel2;
    bool loopEnabled;            // Loop playback
    
    Composition() 
        : tempo(120), timeSignatureNum(4), timeSignatureDen(4),
          division(StepDivision::SIXTEENTH), lengthBars(4),
          channel1Instrument(0), channel2Instrument(0),
          loopEnabled(true) {}
    
    // Calculate total steps in composition
    uint16_t totalSteps() const {
        return lengthBars * static_cast<uint16_t>(division) * timeSignatureNum / timeSignatureDen;
    }
    
    // Calculate steps per bar
    uint16_t stepsPerBar() const {
        return static_cast<uint16_t>(division) * timeSignatureNum / timeSignatureDen;
    }
    
    // Calculate step duration in microseconds
    uint64_t stepDurationUs() const {
        // Whole note duration = 4 beats at current tempo
        // step duration = whole_note / division
        uint64_t wholeNoteUs = (4ULL * 60ULL * 1000000ULL) / tempo;
        return wholeNoteUs / static_cast<uint64_t>(division);
    }
    
    // Clear all notes
    void clear() {
        channel1.clear();
        channel2.clear();
    }
};

/**
 * @brief Playback state tracking
 */
struct PlaybackState {
    bool isPlaying;
    uint16_t currentStep;
    uint16_t loopStart;
    uint16_t loopEnd;
    bool channel1Muted;
    bool channel2Muted;
    bool metronomeEnabled;
    
    PlaybackState() 
        : isPlaying(false), currentStep(0), loopStart(0), loopEnd(0),
          channel1Muted(false), channel2Muted(false), metronomeEnabled(false) {}
};

/**
 * @brief Callback types for engine events
 */
using StepCallback = std::function<void(uint16_t step)>;
using PlayStateCallback = std::function<void(bool playing)>;

/**
 * @brief Main sequencer engine class
 */
class SequencerEngine {
public:
    static SequencerEngine& getInstance();
    
    // Delete copy/move
    SequencerEngine(const SequencerEngine&) = delete;
    SequencerEngine& operator=(const SequencerEngine&) = delete;
    
    /**
     * @brief Initialize the engine
     */
    void init();
    
    /**
     * @brief Cleanup resources
     */
    void deinit();
    
    // ==================== Playback Control ====================
    
    /**
     * @brief Start playback from current position
     */
    void play();
    
    /**
     * @brief Stop playback
     */
    void stop();
    
    /**
     * @brief Toggle play/stop
     * @return true if now playing
     */
    bool togglePlayback();
    
    /**
     * @brief Check if currently playing
     */
    bool isPlaying() const { return m_playback.isPlaying; }
    
    /**
     * @brief Set playback position
     * @param step Step index to jump to
     */
    void setPosition(uint16_t step);
    
    /**
     * @brief Get current playback position
     */
    uint16_t getPosition() const { return m_playback.currentStep; }
    
    /**
     * @brief Set loop region
     * @param start Start step (inclusive)
     * @param end End step (exclusive), 0 = loop full composition
     */
    void setLoopRegion(uint16_t start, uint16_t end);
    
    // ==================== Composition Access ====================
    
    /**
     * @brief Get reference to composition (for reading)
     */
    const Composition& getComposition() const { return m_composition; }
    
    /**
     * @brief Get mutable reference to composition
     * @note Lock mutex if modifying during playback
     */
    Composition& getCompositionMut() { return m_composition; }
    
    /**
     * @brief Set tempo
     * @param bpm Beats per minute (30-300)
     */
    void setTempo(uint16_t bpm);
    
    /**
     * @brief Get tempo
     */
    uint16_t getTempo() const { return m_composition.tempo; }
    
    /**
     * @brief Set step division
     */
    void setDivision(StepDivision div);
    
    /**
     * @brief Get step division
     */
    StepDivision getDivision() const { return m_composition.division; }
    
    /**
     * @brief Set composition length in bars
     */
    void setLengthBars(uint16_t bars);
    
    /**
     * @brief Get composition length in bars
     */
    uint16_t getLengthBars() const { return m_composition.lengthBars; }
    
    // ==================== Note Editing ====================
    
    /**
     * @brief Add a note to a channel
     * @param channel 0 or 1
     * @param note The note event to add
     * @return Index of added note, or -1 on failure
     */
    int addNote(int channel, const NoteEvent& note);
    
    /**
     * @brief Remove a note from a channel
     * @param channel 0 or 1
     * @param index Note index to remove
     */
    void removeNote(int channel, int index);
    
    /**
     * @brief Find note at specific step and channel
     * @param channel 0 or 1
     * @param step Step index
     * @return Pointer to note or nullptr if none found
     */
    NoteEvent* findNoteAt(int channel, uint16_t step);
    
    /**
     * @brief Get note index at step
     * @param channel 0 or 1
     * @param step Step index
     * @return Note index or -1 if not found
     */
    int findNoteIndexAt(int channel, uint16_t step);
    
    /**
     * @brief Toggle note at step (add if missing, remove if present)
     * @param channel 0 or 1
     * @param step Step index
     * @param defaultPitch Default pitch for new notes
     * @param defaultVelocity Default velocity for new notes
     * @return true if note was added, false if removed
     */
    bool toggleNoteAt(int channel, uint16_t step, uint8_t defaultPitch = 60, uint8_t defaultVelocity = 100);
    
    /**
     * @brief Update note pitch
     * @param channel 0 or 1
     * @param index Note index
     * @param pitch New pitch
     */
    void setNotePitch(int channel, int index, uint8_t pitch);
    
    /**
     * @brief Update note duration
     * @param channel 0 or 1
     * @param index Note index
     * @param duration New duration in steps
     */
    void setNoteDuration(int channel, int index, uint16_t duration);
    
    /**
     * @brief Update note velocity
     * @param channel 0 or 1
     * @param index Note index
     * @param velocity New velocity
     */
    void setNoteVelocity(int channel, int index, uint8_t velocity);
    
    /**
     * @brief Clear all notes from a channel
     * @param channel 0 or 1
     */
    void clearChannel(int channel);
    
    /**
     * @brief Clear all notes from both channels
     */
    void clearAll();
    
    // ==================== Channel Control ====================
    
    /**
     * @brief Set channel mute state
     */
    void setChannelMuted(int channel, bool muted);
    
    /**
     * @brief Get channel mute state
     */
    bool isChannelMuted(int channel) const;
    
    /**
     * @brief Toggle metronome
     */
    void setMetronome(bool enabled) { m_playback.metronomeEnabled = enabled; }
    bool isMetronomeEnabled() const { return m_playback.metronomeEnabled; }
    
    /**
     * @brief Toggle loop mode
     */
    void setLoopEnabled(bool enabled) { m_composition.loopEnabled = enabled; }
    bool isLoopEnabled() const { return m_composition.loopEnabled; }
    
    // ==================== Callbacks ====================
    
    /**
     * @brief Set callback for step changes (for UI updates)
     */
    void setStepCallback(StepCallback cb) { m_stepCallback = cb; }
    
    /**
     * @brief Set callback for play state changes
     */
    void setPlayStateCallback(PlayStateCallback cb) { m_playStateCallback = cb; }
    
    // ==================== MIDI Pitch Helpers ====================
    
    /**
     * @brief Convert MIDI note number to frequency in Hz
     */
    static int midiToFrequency(uint8_t midiNote);
    
    /**
     * @brief Get note name from MIDI note number
     */
    static const char* midiToNoteName(uint8_t midiNote);
    
    // ==================== Undo/Redo ====================
    
    /**
     * @brief Save current state for undo
     */
    void saveUndoState();
    
    /**
     * @brief Undo last operation
     * @return true if undo was successful
     */
    bool undo();
    
    /**
     * @brief Redo previously undone operation
     * @return true if redo was successful
     */
    bool redo();
    
    /**
     * @brief Check if undo is available
     */
    bool canUndo() const { return m_undoIndex > 0; }
    
    /**
     * @brief Check if redo is available
     */
    bool canRedo() const { return m_undoIndex < m_undoStack.size(); }

private:
    SequencerEngine();
    ~SequencerEngine() = default;
    
    // Timer callback (called at each step interval)
    static void timerCallback(void* arg);
    
    // Process current step (play notes, advance)
    void processStep();
    
    // Play notes for current step on buzzer
    void playCurrentNotes();
    
    // Stop all sound
    void stopSound();
    void stopMixing();
    
    // Buzzer control with velocity-based PWM
    void setBuzzerFrequency(int freq, uint8_t velocity);
    void setBuzzerOff();
    
    Composition m_composition;
    PlaybackState m_playback;
    
    esp_timer_handle_t m_timer;
    SemaphoreHandle_t m_mutex;
    bool m_initialized;
    
    // For two-note mixing (fast interpolation)
    int m_note1Freq;
    int m_note2Freq;
    uint8_t m_note1Velocity;
    uint8_t m_note2Velocity;
    bool m_alternateState;  // Toggle between note1 and note2
    esp_timer_handle_t m_mixTimer;  // Timer for alternation
    static void mixTimerCallback(void* arg);
    
    // Callbacks
    StepCallback m_stepCallback;
    PlayStateCallback m_playStateCallback;
    
    // Undo/Redo stack (stores composition states)
    static constexpr size_t MAX_UNDO_LEVELS = 10;
    std::vector<Composition> m_undoStack;
    size_t m_undoIndex;
};
