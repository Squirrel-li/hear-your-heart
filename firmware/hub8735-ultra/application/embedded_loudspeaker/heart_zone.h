#ifndef HEART_ZONE_H
#define HEART_ZONE_H

#include <stdint.h>

// Enjoy 模式：心率 → 穩定 Zone → 目標歌曲 BPM。Zone 與各區音樂對應依專題規劃書「目標規格說明」（ACSM 五區）：
//   Z1 <60%  暖身/休息  沉浸與放鬆      → 低 BPM 區間 60–80（心率線性映射）
//   Z2 60–70 有氧燃脂  穩定節奏維持    → 心率（節奏同步）
//   Z3 70–80 耐力心肺  節奏同步、適度激勵 → 心率 +5%
//   Z4 80–90 無氧極限  引導降速        → 心率 -20%
//   Z5 >=90  疲勞危險  立即音樂降速    → 心率 -30%，進入 Z5 不等待駐留時間
struct TargetBpmState {
    uint16_t hrmax;
    uint16_t bpm;
};

struct HeartZoneState {
    uint8_t active;
    uint8_t pending;
    uint32_t pending_since_ms;
};

inline uint8_t heart_zone_observed(uint16_t hrmax, uint16_t heart_rate)
{
    if (hrmax == 0 || heart_rate == 0) return 0;
    const uint32_t percentage = (uint32_t)heart_rate * 100u;
    if (percentage < (uint32_t)hrmax * 60u) return 1;
    if (percentage < (uint32_t)hrmax * 70u) return 2;
    if (percentage < (uint32_t)hrmax * 80u) return 3;
    if (percentage < (uint32_t)hrmax * 90u) return 4;
    return 5;
}

// A change must cross the old zone edge by hysteresis_percent of HRmax and hold for dwell_ms;
// rising into Z5 (danger) takes effect at once.
inline uint8_t heart_zone_update(HeartZoneState* state, uint16_t hrmax, uint16_t heart_rate, uint32_t now_ms,
                                 uint32_t dwell_ms, uint8_t hysteresis_percent)
{
    const uint8_t observed = heart_zone_observed(hrmax, heart_rate);
    if (state == nullptr || observed == 0 || hysteresis_percent >= 10u) return 0;
    if (state->active == 0) {
        state->active = observed;
        state->pending = 0;
        return observed;
    }
    if (observed == state->active) {
        state->pending = 0;
        return state->active;
    }

    const bool movingUp = observed > state->active;
    const uint8_t boundary = movingUp ? static_cast<uint8_t>(50u + state->active * 10u)
                                      : static_cast<uint8_t>(40u + state->active * 10u);
    const uint32_t percentage = (uint32_t)heart_rate * 100u;
    const uint8_t thresholdPercent = movingUp ? static_cast<uint8_t>(boundary + hysteresis_percent)
                                              : static_cast<uint8_t>(boundary - hysteresis_percent);
    const uint32_t threshold = (uint32_t)hrmax * thresholdPercent;
    const bool outside = movingUp ? percentage >= threshold : percentage < threshold;
    if (!outside) {
        state->pending = 0;
        return state->active;
    }
    if (observed == 5) {
        state->active = 5;
        state->pending = 0;
    } else if (state->pending != observed) {
        state->pending = observed;
        state->pending_since_ms = now_ms;
    } else if ((uint32_t)(now_ms - state->pending_since_ms) >= dwell_ms) {
        state->active = observed;
        state->pending = 0;
    }
    return state->active;
}

// Steady target for the active zone; the caller rate-limits rises (see target_bpm_update_for_zone).
inline uint32_t target_bpm_for_zone(uint16_t hrmax, uint16_t heart_rate, uint8_t zone)
{
    switch (zone) {
    case 1: {
        const uint32_t denominator = 3u * hrmax;
        const uint32_t target = 60u + ((uint32_t)heart_rate * 100u + denominator / 2u) / denominator;
        return target > 80u ? 80u : target;
    }
    case 2: return heart_rate;
    case 3: return ((uint32_t)heart_rate * 105u + 50u) / 100u;
    case 4: return ((uint32_t)heart_rate * 80u + 50u) / 100u;
    default: return ((uint32_t)heart_rate * 70u + 50u) / 100u;
    }
}

inline int target_bpm_update_for_zone(TargetBpmState* state, uint16_t heart_rate, uint8_t active_zone)
{
    if (state == nullptr || state->hrmax == 0 || heart_rate == 0 || active_zone < 1 || active_zone > 5) return 0;
    uint32_t target = target_bpm_for_zone(state->hrmax, heart_rate, active_zone);
    // ponytail: The upward ramp assumes 1 Hz calls; use elapsed time if cadence varies.
    if (state->bpm != 0 && target > (uint32_t)state->bpm + 5u) target = (uint32_t)state->bpm + 5u;
    state->bpm = static_cast<uint16_t>(target);
    return static_cast<int>(target);
}

#endif  // HEART_ZONE_H
