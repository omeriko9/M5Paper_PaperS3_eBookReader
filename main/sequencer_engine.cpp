/**
 * @file sequencer_engine.cpp
 * @brief Implementation of the 2-channel step sequencer engine
 */

#include "sequencer_engine.h"
#include "device_hal.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include <cmath>
#include <algorithm>

#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
#include "driver/ledc.h"
// Use same LEDC config as DeviceHAL
static constexpr ledc_timer_t SEQ_BUZZER_TIMER = LEDC_TIMER_0;
static constexpr ledc_channel_t SEQ_BUZZER_CHANNEL = LEDC_CHANNEL_0;
static constexpr ledc_mode_t SEQ_BUZZER_MODE = LEDC_LOW_SPEED_MODE;
#endif

static const char* TAG = "SeqEngine";

// Mixing timer interval for two-note alternation (microseconds)
// 20ms gives ~50Hz alternation, perceptible as both notes
static constexpr uint64_t MIX_INTERVAL_US = 20000;

// MIDI note frequency table (A4 = 440Hz)
// Formula: freq = 440 * 2^((note-69)/12)
static const uint16_t MIDI_FREQ_TABLE[] = {
    // Octave 0 (notes 0-11): Very low, may not be audible on buzzer
    16, 17, 18, 19, 21, 22, 23, 25, 26, 28, 29, 31,
    // Octave 1 (notes 12-23)
    33, 35, 37, 39, 41, 44, 46, 49, 52, 55, 58, 62,
    // Octave 2 (notes 24-35)
    65, 69, 73, 78, 82, 87, 93, 98, 104, 110, 117, 123,
    // Octave 3 (notes 36-47)
    131, 139, 147, 156, 165, 175, 185, 196, 208, 220, 233, 247,
    // Octave 4 (notes 48-59) - Middle octave, C4=262Hz
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494,
    // Octave 5 (notes 60-71) - C5=523Hz, good buzzer range
    523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988,
    // Octave 6 (notes 72-83)
    1047, 1109, 1175, 1245, 1319, 1397, 1480, 1568, 1661, 1760, 1865, 1976,
    // Octave 7 (notes 84-95)
    2093, 2217, 2349, 2489, 2637, 2794, 2960, 3136, 3322, 3520, 3729, 3951,
    // Octave 8 (notes 96-107) - High, some buzzers struggle here
    4186, 4435, 4699, 4978, 5274, 5588, 5920, 6272, 6645, 7040, 7459, 7902,
    // Octave 9 (notes 108-119) - Very high
    8372, 8870, 9397, 9956, 10548, 11175, 11840, 12544, 13290, 14080, 14917, 15804,
    // Octave 10 (notes 120-127) - Extremely high
    16744, 17740, 18795, 19912, 21096, 22351, 23680, 25088
};

// Note names for display
static const char* NOTE_NAMES[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

SequencerEngine& SequencerEngine::getInstance() {
    static SequencerEngine instance;
    return instance;
}

SequencerEngine::SequencerEngine()
    : m_timer(nullptr)
    , m_mutex(nullptr)
    , m_initialized(false)
    , m_note1Freq(0)
    , m_note2Freq(0)
    , m_note1Velocity(0)
    , m_note2Velocity(0)
    , m_alternateState(false)
    , m_mixTimer(nullptr)
    , m_stepCallback(nullptr)
    , m_playStateCallback(nullptr)
    , m_undoIndex(0)
{
}

void SequencerEngine::init() {
    if (m_initialized) return;
    
    ESP_LOGI(TAG, "Initializing sequencer engine");
    
    // Create mutex
    m_mutex = xSemaphoreCreateMutex();
    if (!m_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }
    
    // Create step timer (will be started/stopped with play/stop)
    const esp_timer_create_args_t timer_args = {
        .callback = &SequencerEngine::timerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "seq_step",
        .skip_unhandled_events = true
    };
    
    esp_err_t err = esp_timer_create(&timer_args, &m_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create step timer: %s", esp_err_to_name(err));
        return;
    }
    
    // Create mix timer for two-note alternation
    const esp_timer_create_args_t mix_timer_args = {
        .callback = &SequencerEngine::mixTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "seq_mix",
        .skip_unhandled_events = true
    };
    
    err = esp_timer_create(&mix_timer_args, &m_mixTimer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create mix timer: %s", esp_err_to_name(err));
        // Continue without mixing - single note will still work
    }
    
    m_initialized = true;
    ESP_LOGI(TAG, "Sequencer engine initialized");
}

void SequencerEngine::deinit() {
    if (!m_initialized) return;
    
    stop();
    
    if (m_timer) {
        esp_timer_delete(m_timer);
        m_timer = nullptr;
    }
    
    if (m_mixTimer) {
        esp_timer_delete(m_mixTimer);
        m_mixTimer = nullptr;
    }
    
    if (m_mutex) {
        vSemaphoreDelete(m_mutex);
        m_mutex = nullptr;
    }
    
    m_initialized = false;
    ESP_LOGI(TAG, "Sequencer engine deinitialized");
}

void SequencerEngine::play() {
    if (!m_initialized || m_playback.isPlaying) return;
    
    ESP_LOGI(TAG, "Starting playback at step %d, tempo %d BPM", 
             m_playback.currentStep, m_composition.tempo);
    
    m_playback.isPlaying = true;
    
    // Calculate step interval
    uint64_t stepUs = m_composition.stepDurationUs();
    ESP_LOGI(TAG, "Step duration: %llu us", stepUs);
    
    // Start periodic timer
    esp_err_t err = esp_timer_start_periodic(m_timer, stepUs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(err));
        m_playback.isPlaying = false;
        return;
    }
    
    // Notify callback
    if (m_playStateCallback) {
        m_playStateCallback(true);
    }
    
    // Play first step immediately
    processStep();
}

void SequencerEngine::stop() {
    if (!m_initialized) return;
    
    if (m_playback.isPlaying) {
        ESP_LOGI(TAG, "Stopping playback at step %d", m_playback.currentStep);
        
        // Stop timers
        esp_timer_stop(m_timer);
        if (m_mixTimer) {
            esp_timer_stop(m_mixTimer);
        }
        
        // Stop any playing sound
        stopSound();
        
        m_playback.isPlaying = false;
        
        // Notify callback
        if (m_playStateCallback) {
            m_playStateCallback(false);
        }
    }
}

bool SequencerEngine::togglePlayback() {
    if (m_playback.isPlaying) {
        stop();
        return false;
    } else {
        play();
        return true;
    }
}

void SequencerEngine::setPosition(uint16_t step) {
    if (xSemaphoreTake(m_mutex, pdMS_TO_TICKS(100))) {
        uint16_t maxStep = m_composition.totalSteps();
        m_playback.currentStep = (step >= maxStep) ? 0 : step;
        xSemaphoreGive(m_mutex);
        
        if (m_stepCallback) {
            m_stepCallback(m_playback.currentStep);
        }
    }
}

void SequencerEngine::setLoopRegion(uint16_t start, uint16_t end) {
    m_playback.loopStart = start;
    m_playback.loopEnd = (end == 0) ? m_composition.totalSteps() : end;
}

void SequencerEngine::setTempo(uint16_t bpm) {
    bpm = std::max((uint16_t)30, std::min((uint16_t)300, bpm));
    
    bool wasPlaying = m_playback.isPlaying;
    if (wasPlaying) {
        stop();
    }
    
    m_composition.tempo = bpm;
    ESP_LOGI(TAG, "Tempo set to %d BPM", bpm);
    
    if (wasPlaying) {
        play();
    }
}

void SequencerEngine::setDivision(StepDivision div) {
    bool wasPlaying = m_playback.isPlaying;
    if (wasPlaying) {
        stop();
    }
    
    m_composition.division = div;
    
    // Clamp current position to valid range
    uint16_t maxStep = m_composition.totalSteps();
    if (m_playback.currentStep >= maxStep) {
        m_playback.currentStep = 0;
    }
    
    ESP_LOGI(TAG, "Division set to 1/%d, total steps: %d", 
             static_cast<int>(div), maxStep);
    
    if (wasPlaying) {
        play();
    }
}

void SequencerEngine::setLengthBars(uint16_t bars) {
    bars = std::max((uint16_t)1, std::min((uint16_t)64, bars));
    m_composition.lengthBars = bars;
    
    // Update loop end if needed
    uint16_t maxStep = m_composition.totalSteps();
    if (m_playback.loopEnd == 0 || m_playback.loopEnd > maxStep) {
        m_playback.loopEnd = maxStep;
    }
    if (m_playback.currentStep >= maxStep) {
        m_playback.currentStep = 0;
    }
}

int SequencerEngine::addNote(int channel, const NoteEvent& note) {
    if (channel < 0 || channel > 1) return -1;
    
    saveUndoState();
    
    if (xSemaphoreTake(m_mutex, pdMS_TO_TICKS(100))) {
        auto& ch = (channel == 0) ? m_composition.channel1 : m_composition.channel2;
        ch.push_back(note);
        int idx = ch.size() - 1;
        xSemaphoreGive(m_mutex);
        return idx;
    }
    return -1;
}

void SequencerEngine::removeNote(int channel, int index) {
    if (channel < 0 || channel > 1) return;
    
    saveUndoState();
    
    if (xSemaphoreTake(m_mutex, pdMS_TO_TICKS(100))) {
        auto& ch = (channel == 0) ? m_composition.channel1 : m_composition.channel2;
        if (index >= 0 && index < (int)ch.size()) {
            ch.erase(ch.begin() + index);
        }
        xSemaphoreGive(m_mutex);
    }
}

NoteEvent* SequencerEngine::findNoteAt(int channel, uint16_t step) {
    if (channel < 0 || channel > 1) return nullptr;
    
    auto& ch = (channel == 0) ? m_composition.channel1 : m_composition.channel2;
    for (auto& note : ch) {
        if (note.isActiveAt(step)) {
            return &note;
        }
    }
    return nullptr;
}

int SequencerEngine::findNoteIndexAt(int channel, uint16_t step) {
    if (channel < 0 || channel > 1) return -1;
    
    auto& ch = (channel == 0) ? m_composition.channel1 : m_composition.channel2;
    for (size_t i = 0; i < ch.size(); i++) {
        if (ch[i].isActiveAt(step)) {
            return (int)i;
        }
    }
    return -1;
}

bool SequencerEngine::toggleNoteAt(int channel, uint16_t step, uint8_t defaultPitch, uint8_t defaultVelocity) {
    int idx = findNoteIndexAt(channel, step);
    if (idx >= 0) {
        // Note exists, remove it
        removeNote(channel, idx);
        return false;
    } else {
        // No note, add one
        NoteEvent note(step, 1, defaultPitch, defaultVelocity);
        addNote(channel, note);
        return true;
    }
}

void SequencerEngine::setNotePitch(int channel, int index, uint8_t pitch) {
    if (channel < 0 || channel > 1) return;
    pitch = std::min(pitch, (uint8_t)127);
    
    saveUndoState();
    
    if (xSemaphoreTake(m_mutex, pdMS_TO_TICKS(100))) {
        auto& ch = (channel == 0) ? m_composition.channel1 : m_composition.channel2;
        if (index >= 0 && index < (int)ch.size()) {
            ch[index].pitch = pitch;
        }
        xSemaphoreGive(m_mutex);
    }
}

void SequencerEngine::setNoteDuration(int channel, int index, uint16_t duration) {
    if (channel < 0 || channel > 1) return;
    duration = std::max((uint16_t)1, duration);
    
    saveUndoState();
    
    if (xSemaphoreTake(m_mutex, pdMS_TO_TICKS(100))) {
        auto& ch = (channel == 0) ? m_composition.channel1 : m_composition.channel2;
        if (index >= 0 && index < (int)ch.size()) {
            ch[index].duration = duration;
        }
        xSemaphoreGive(m_mutex);
    }
}

void SequencerEngine::setNoteVelocity(int channel, int index, uint8_t velocity) {
    if (channel < 0 || channel > 1) return;
    velocity = std::min(velocity, (uint8_t)127);
    
    saveUndoState();
    
    if (xSemaphoreTake(m_mutex, pdMS_TO_TICKS(100))) {
        auto& ch = (channel == 0) ? m_composition.channel1 : m_composition.channel2;
        if (index >= 0 && index < (int)ch.size()) {
            ch[index].velocity = velocity;
        }
        xSemaphoreGive(m_mutex);
    }
}

void SequencerEngine::clearChannel(int channel) {
    if (channel < 0 || channel > 1) return;
    
    saveUndoState();
    
    if (xSemaphoreTake(m_mutex, pdMS_TO_TICKS(100))) {
        if (channel == 0) {
            m_composition.channel1.clear();
        } else {
            m_composition.channel2.clear();
        }
        xSemaphoreGive(m_mutex);
    }
}

void SequencerEngine::clearAll() {
    saveUndoState();
    
    if (xSemaphoreTake(m_mutex, pdMS_TO_TICKS(100))) {
        m_composition.clear();
        xSemaphoreGive(m_mutex);
    }
}

void SequencerEngine::setChannelMuted(int channel, bool muted) {
    if (channel == 0) {
        m_playback.channel1Muted = muted;
    } else if (channel == 1) {
        m_playback.channel2Muted = muted;
    }
}

bool SequencerEngine::isChannelMuted(int channel) const {
    if (channel == 0) return m_playback.channel1Muted;
    if (channel == 1) return m_playback.channel2Muted;
    return true;
}

// Static helper functions
int SequencerEngine::midiToFrequency(uint8_t midiNote) {
    if (midiNote > 127) midiNote = 127;
    return MIDI_FREQ_TABLE[midiNote];
}

const char* SequencerEngine::midiToNoteName(uint8_t midiNote) {
    static char buf[8];
    int octave = (midiNote / 12) - 1;
    int note = midiNote % 12;
    snprintf(buf, sizeof(buf), "%s%d", NOTE_NAMES[note], octave);
    return buf;
}

// Timer callback - called at each step
void SequencerEngine::timerCallback(void* arg) {
    auto* self = static_cast<SequencerEngine*>(arg);
    if (self) {
        self->processStep();
    }
}

// Mix timer callback - alternates between two notes
void SequencerEngine::mixTimerCallback(void* arg) {
    auto* self = static_cast<SequencerEngine*>(arg);
    if (self && self->m_note1Freq > 0 && self->m_note2Freq > 0) {
        // Alternate between the two notes
        self->m_alternateState = !self->m_alternateState;
        if (self->m_alternateState) {
            self->setBuzzerFrequency(self->m_note1Freq, self->m_note1Velocity);
        } else {
            self->setBuzzerFrequency(self->m_note2Freq, self->m_note2Velocity);
        }
    }
}

void SequencerEngine::processStep() {
    if (!m_playback.isPlaying) return;
    
    // Take mutex briefly to read notes
    bool gotMutex = (xSemaphoreTake(m_mutex, 0) == pdTRUE);
    
    // Play notes for current step
    playCurrentNotes();
    
    // Advance to next step
    uint16_t nextStep = m_playback.currentStep + 1;
    uint16_t maxStep = m_composition.totalSteps();
    uint16_t loopEnd = (m_playback.loopEnd == 0) ? maxStep : m_playback.loopEnd;
    
    if (nextStep >= loopEnd) {
        if (m_composition.loopEnabled) {
            nextStep = m_playback.loopStart;
        } else {
            // Stop at end
            if (gotMutex) xSemaphoreGive(m_mutex);
            stop();
            return;
        }
    }
    
    m_playback.currentStep = nextStep;
    
    if (gotMutex) xSemaphoreGive(m_mutex);
    
    // Notify UI of step change
    if (m_stepCallback) {
        m_stepCallback(m_playback.currentStep);
    }
}

void SequencerEngine::playCurrentNotes() {
    uint16_t step = m_playback.currentStep;
    
    // Find active notes on each channel (notes that are currently sounding)
    NoteEvent* note1 = nullptr;
    NoteEvent* note2 = nullptr;
    
    if (!m_playback.channel1Muted) {
        for (auto& n : m_composition.channel1) {
            if (n.isActiveAt(step) && n.enabled) {  // Check if note is active at this step
                note1 = &n;
                break;
            }
        }
    }
    
    if (!m_playback.channel2Muted) {
        for (auto& n : m_composition.channel2) {
            if (n.isActiveAt(step) && n.enabled) {  // Check if note is active at this step
                note2 = &n;
                break;
            }
        }
    }
    
    // Play metronome click on beat (only at the beat moment)
    bool metronomeTriggered = false;
    if (m_playback.metronomeEnabled) {
        uint16_t stepsPerBar = m_composition.stepsPerBar();
        uint16_t stepsPerBeat = stepsPerBar / m_composition.timeSignatureNum;
        if (stepsPerBeat == 0) stepsPerBeat = 1;
        
        if (step % stepsPerBar == 0) {
            // Downbeat - higher pitch, brief click
            m_note1Freq = 1200;
            m_note1Velocity = 80;
            setBuzzerFrequency(m_note1Freq, m_note1Velocity);
            metronomeTriggered = true;
        } else if (step % stepsPerBeat == 0) {
            // Beat
            m_note1Freq = 800;
            m_note1Velocity = 60;
            setBuzzerFrequency(m_note1Freq, m_note1Velocity);
            metronomeTriggered = true;
        }
    }
    
    // If metronome just played, don't override it with notes
    if (metronomeTriggered) {
        return;
    }
    
    // Determine what to play
    if (note1 && note2) {
        // Two notes - use fast alternation
        m_note1Freq = midiToFrequency(note1->pitch);
        m_note2Freq = midiToFrequency(note2->pitch);
        m_note1Velocity = note1->velocity;
        m_note2Velocity = note2->velocity;
        
        // Start alternation timer
        if (m_mixTimer) {
            esp_timer_stop(m_mixTimer);
            esp_timer_start_periodic(m_mixTimer, MIX_INTERVAL_US);
        }
        
        // Start with first note
        m_alternateState = true;
        setBuzzerFrequency(m_note1Freq, m_note1Velocity);
        
    } else if (note1) {
        // Single note on channel 1
        stopMixing();
        m_note1Freq = midiToFrequency(note1->pitch);
        m_note1Velocity = note1->velocity;
        m_note2Freq = 0;
        setBuzzerFrequency(m_note1Freq, m_note1Velocity);
        
    } else if (note2) {
        // Single note on channel 2
        stopMixing();
        m_note1Freq = midiToFrequency(note2->pitch);
        m_note1Velocity = note2->velocity;
        m_note2Freq = 0;
        setBuzzerFrequency(m_note1Freq, m_note1Velocity);
        
    } else {
        // No notes playing at this step - silence
        stopMixing();
        setBuzzerOff();
    }
}

void SequencerEngine::stopMixing() {
    if (m_mixTimer) {
        esp_timer_stop(m_mixTimer);
    }
    m_note2Freq = 0;
}

void SequencerEngine::stopSound() {
    stopMixing();
    m_note1Freq = 0;
    m_note2Freq = 0;
    setBuzzerOff();
}

void SequencerEngine::setBuzzerFrequency(int freq, uint8_t velocity) {
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
#ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
    if (freq <= 0) {
        setBuzzerOff();
        return;
    }
    
    // Set frequency
    ledc_set_freq(SEQ_BUZZER_MODE, SEQ_BUZZER_TIMER, freq);
    
    // Map velocity (0-127) to duty cycle (0-1023)
    // Use a curve to make velocity more perceptible
    // Lower velocities use lower duty cycles
    uint32_t duty = (velocity * velocity * 1023) / (127 * 127);
    duty = std::max((uint32_t)64, std::min((uint32_t)512, duty));  // Clamp to audible range
    
    ledc_set_duty(SEQ_BUZZER_MODE, SEQ_BUZZER_CHANNEL, duty);
    ledc_update_duty(SEQ_BUZZER_MODE, SEQ_BUZZER_CHANNEL);
#endif
#endif
}

void SequencerEngine::setBuzzerOff() {
#ifdef CONFIG_EBOOK_DEVICE_M5PAPERS3
#ifdef CONFIG_EBOOK_S3_ENABLE_BUZZER
    ledc_set_duty(SEQ_BUZZER_MODE, SEQ_BUZZER_CHANNEL, 0);
    ledc_update_duty(SEQ_BUZZER_MODE, SEQ_BUZZER_CHANNEL);
#endif
#endif
}

// Undo/Redo implementation
void SequencerEngine::saveUndoState() {
    // Remove any redo states
    if (m_undoIndex < m_undoStack.size()) {
        m_undoStack.resize(m_undoIndex);
    }
    
    // Add current state
    m_undoStack.push_back(m_composition);
    m_undoIndex = m_undoStack.size();
    
    // Limit stack size
    while (m_undoStack.size() > MAX_UNDO_LEVELS) {
        m_undoStack.erase(m_undoStack.begin());
        m_undoIndex--;
    }
}

bool SequencerEngine::undo() {
    if (m_undoIndex == 0) return false;
    
    // Save current state for redo if at the end
    if (m_undoIndex == m_undoStack.size()) {
        m_undoStack.push_back(m_composition);
    }
    
    m_undoIndex--;
    
    if (xSemaphoreTake(m_mutex, pdMS_TO_TICKS(100))) {
        m_composition = m_undoStack[m_undoIndex];
        xSemaphoreGive(m_mutex);
    }
    
    return true;
}

bool SequencerEngine::redo() {
    if (m_undoIndex >= m_undoStack.size() - 1) return false;
    
    m_undoIndex++;
    
    if (xSemaphoreTake(m_mutex, pdMS_TO_TICKS(100))) {
        m_composition = m_undoStack[m_undoIndex];
        xSemaphoreGive(m_mutex);
    }
    
    return true;
}
