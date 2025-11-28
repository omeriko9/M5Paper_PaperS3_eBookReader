# Boot Time Optimization Plan: From 17s to 2s

## Current Analysis (from serial log)

### Timeline Breakdown:
- **0-538ms**: ESP32 Bootloader + Flash read
- **538-2532ms (~2s)**: M5GFX Autodetect + M5Paper detection
- **2532-5192ms (~2.6s)**: Display initialization + SPIFFS mounting
- **5192-5582ms (390ms)**: Hebrew font load (74KB)
- **5582-6622ms (~1s)**: Book index loading + canvas allocation failures
- **6622-10922ms (~4.3s)**: EPUB loading + chapter parsing
- **10922-11022ms (100ms)**: WiFi driver init
- **11022-13042ms (~2s)**: WiFi connection + NTP sync
- **13042-17312ms (~4.3s)**: Page rendering (measurement + drawing)

**TOTAL: ~17 seconds**

---

## Optimization Strategy

### Phase 1: ELIMINATE Unnecessary Operations on Deep Sleep Wake (Target: Save 5-7s)

#### 1.1 Skip WiFi Initialization on Wake from Deep Sleep ⚡ HIGH IMPACT
**Current Cost**: ~3 seconds (WiFi init + connect + NTP sync)
**Target**: 0 seconds on wake
**Implementation**:
- Detect deep sleep wake using `esp_sleep_get_wakeup_cause()`
- Only initialize WiFi on cold boot or if user explicitly requests it
- Save WiFi state in RTC memory before sleep
- Skip NTP sync (RTC retains time during deep sleep)

```cpp
// In main.cpp app_main():
auto wakeup_reason = esp_sleep_get_wakeup_cause();
bool is_wake_from_sleep = (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0);

if (!is_wake_from_sleep) {
    // Cold boot: Initialize WiFi normally
    wifiManager.init();
    bool wifiOk = wifiManager.connect();
    if (wifiOk) {
        syncRtcFromNtp();
    }
} else {
    // Wake from sleep: Skip WiFi entirely
    ESP_LOGI(TAG, "Woke from deep sleep - skipping WiFi init");
}
```

#### 1.2 Skip M5Paper Auto-Detection on Wake ⚡ MEDIUM IMPACT
**Current Cost**: ~2 seconds
**Target**: ~200ms (direct init without detection)
**Implementation**:
- Save board type to NVS on first boot
- Use direct initialization instead of autodetect on subsequent boots

```cpp
// Cache board detection result
nvs_handle_t handle;
uint8_t board_type = 0;
if (nvs_open("storage", NVS_READONLY, &handle) == ESP_OK) {
    nvs_get_u8(handle, "board_type", &board_type);
    nvs_close(handle);
}

auto cfg = M5.config();
if (board_type != 0) {
    cfg.internal_imu = false;  // Skip IMU detection
    cfg.internal_rtc = true;   // We know M5Paper has RTC
}
```

#### 1.3 Defer SD Card Mounting ⚡ LOW IMPACT (already commented out)
**Current Cost**: Previously ~1.5 seconds (now commented out)
**Status**: ✅ Already optimized

---

### Phase 2: OPTIMIZE EPUB Loading (Target: Save 3-4s)

#### 2.1 Direct Chapter Jump ⚡ HIGH IMPACT
**Current Cost**: ~2-3 seconds for iterating through chapters
**Target**: ~200ms for direct chapter load
**Status**: ✅ **IMPLEMENTED** - Added `restoreChapterIndex` parameter to `load()`

#### 2.2 Skip Chapter Content Heuristics on Restore ⚡ MEDIUM IMPACT
**Current Cost**: ~500ms for skipping intro chapters
**Target**: 0ms on restore
**Status**: ✅ **IMPLEMENTED** - Skip heuristics when restoring

#### 2.3 Cache EPUB Metadata in NVS (Future Optimization)
**Current Cost**: ~1 second for parsing container.xml + OPF
**Target**: ~50ms for metadata read from cache
**Implementation**: Store spine, title, language in NVS on first open

---

### Phase 3: OPTIMIZE Font Loading (Target: Save 200-300ms)

#### 3.1 Lazy Font Loading ⚡ MEDIUM IMPACT
**Current Cost**: 390ms for Hebrew font (74KB)
**Target**: Load only when needed for first Hebrew word
**Implementation**:
- Don't load Hebrew font in `init()` if restoring
- Load on first `drawStringMixed()` call that needs it
**Status**: Already partially implemented with `ensureHebrewFontLoaded()`

#### 3.2 Optimize Font File Format (Future)
- Consider using a binary format optimized for ESP32
- Split font into base + Hebrew glyphs

---

### Phase 4: OPTIMIZE Page Rendering (Target: Save 2-3s)

#### 4.1 Skip Redundant Measurements ⚡ HIGH IMPACT
**Current Cost**: ~3 seconds for double measurement (draw=false, then draw=true)
**Target**: ~1.5 seconds (single pass)
**Implementation**:
```cpp
// In drawReader():
// Remove redundant calls to drawPageContentAt()
// Measure only once, draw immediately
size_t charsDrawn = drawPageContentAt(currentTextOffset, true, nullptr);
// Remove the second call that just calculates without drawing
```

#### 4.2 Pre-render Next/Prev Pages in Background (Future)
**Current Cost**: N/A (already implemented but not used on wake)
**Target**: 0ms perceived latency for page turns
**Implementation**: Ensure canvases are valid after wake

---

### Phase 5: OPTIMIZE Display Initialization (Target: Save 500ms)

#### 5.1 Skip Redundant Display Clear ⚡ LOW IMPACT
**Current Cost**: ~200ms
**Target**: 0ms on wake (display retains state during deep sleep)
**Implementation**:
```cpp
auto cfg = M5.config();
if (is_wake_from_sleep) {
    cfg.clear_display = false;  // Don't clear on wake
}
```

#### 5.2 Use Partial Updates (Future)
- EPD supports partial refresh which is faster than full refresh
- Requires M5GFX API support

---

## Implementation Checklist

### ✅ Already Implemented:
1. ✅ SD Card init commented out
2. ✅ Direct chapter restore in `EpubLoader::load()`
3. ✅ Skip chapter heuristics on restore
4. ✅ Lazy Hebrew font loading

### 🔧 Ready to Implement (Quick Wins):
5. ⏳ Skip WiFi on deep sleep wake
6. ⏳ Skip M5Paper auto-detect on wake
7. ⏳ Remove redundant page measurement calls
8. ⏳ Skip display clear on wake

### 📋 Future Optimizations:
9. Cache EPUB metadata in NVS
10. Background page pre-rendering on wake
11. Optimize font file format
12. Use EPD partial refresh

---

## Expected Results

### Current Boot Time: ~17 seconds
| Phase | Operation | Current | Target | Savings |
|-------|-----------|---------|--------|---------|
| 1 | WiFi Init + NTP | 3.0s | 0s | **3.0s** |
| 2 | M5GFX Autodetect | 2.0s | 0.2s | **1.8s** |
| 3 | EPUB Load + Parse | 4.3s | 0.5s | **3.8s** |
| 4 | Page Rendering | 4.3s | 1.5s | **2.8s** |
| 5 | Display Init | 2.6s | 2.0s | **0.6s** |
| | **TOTAL SAVINGS** | | | **~12s** |

### **Target Boot Time: ~2-3 seconds** ✅

---

## Next Steps

1. Implement WiFi skip on wake (highest impact)
2. Remove redundant rendering calls
3. Skip M5GFX autodetect on wake
4. Test and measure each change
5. Fine-tune based on actual measurements

---

## Testing Strategy

1. Add timestamps to each initialization phase
2. Compare cold boot vs wake-from-sleep times
3. Ensure all functionality works correctly after optimizations
4. Verify no memory leaks or stability issues
