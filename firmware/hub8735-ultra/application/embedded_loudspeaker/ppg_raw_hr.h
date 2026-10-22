// HUB side of msgType 0x05 (raw PPG batch): heart rate with the same algorithm as ble_sensor_hr (ppg_hr.h).
// analysisTask feeds every sample; a 10 s cycle then gives a BPM (0 = invalid), the first cycle is a warm-up.
// Host check: tests/ppg_raw_hr_check.cpp
#pragma once

#include <stdint.h>

#include "ppg_hr.h"

constexpr uint32_t PPG_RAW_SAMPLE_MS = 28;  // module output period (ble-notify-data-format.md 4.1)
constexpr uint32_t PPG_RAW_LOST_MS = 2000;  // longer silence (or time going back = sensor restart) starts over

struct PpgRawHrState {
    hr_t hr;
    bool started;
    uint32_t lastTimeMs;
};

inline void ppg_raw_hr_reset(PpgRawHrState& s)
{
    hr_reset(&s.hr);
    s.started = false;
    s.lastTimeMs = 0;
}

// Sample i of an N-sample frame: the frame time stamps the last sample.
inline uint32_t ppg_raw_sample_time(uint32_t frameTimeMs, uint32_t n, uint32_t i)
{
    return frameTimeMs - (n - 1 - i) * PPG_RAW_SAMPLE_MS;
}

// Returns -1 while a cycle runs, otherwise the finished cycle's BPM (0 = no valid heart rate).
inline int ppg_raw_hr_feed(PpgRawHrState& s, uint32_t timeMs, uint16_t ppg1, uint16_t ppg2)
{
    if (s.started && (uint32_t)(timeMs - s.lastTimeMs) > PPG_RAW_LOST_MS) ppg_raw_hr_reset(s);
    s.started = true;
    s.lastTimeMs = timeMs;
    uint8_t bpm = 0;
    return hr_feed(&s.hr, ppg1, ppg2, static_cast<uint16_t>(timeMs), &bpm) ? bpm : -1;
}
