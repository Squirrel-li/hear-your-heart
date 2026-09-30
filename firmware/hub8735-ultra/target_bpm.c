#include "target_bpm.h"

#include <stddef.h>

uint8_t heart_zone_observed(uint16_t hrmax, uint16_t heart_rate)
{
    if (hrmax == 0 || heart_rate == 0) return 0;
    const uint32_t percentage = (uint32_t)heart_rate * 100u;
    if (percentage < (uint32_t)hrmax * 60u) return 1;
    if (percentage < (uint32_t)hrmax * 70u) return 2;
    if (percentage < (uint32_t)hrmax * 80u) return 3;
    if (percentage < (uint32_t)hrmax * 90u) return 4;
    return 5;
}

uint8_t heart_zone_update(HeartZoneState *state, uint16_t hrmax,
                          uint16_t heart_rate, uint32_t now_ms,
                          uint32_t dwell_ms, uint8_t hysteresis_percent)
{
    const uint8_t observed = heart_zone_observed(hrmax, heart_rate);
    if (state == NULL || observed == 0 || hysteresis_percent >= 10u) return 0;
    if (state->active == 0) {
        state->active = observed;
        state->pending = 0;
        return observed;
    }
    if (observed == state->active) {
        state->pending = 0;
        return state->active;
    }

    const uint8_t boundary = observed > state->active ? (uint8_t)(50u + state->active * 10u) : (uint8_t)(40u + state->active * 10u);
    const uint32_t percentage = (uint32_t)heart_rate * 100u;
    const uint32_t threshold = (uint32_t)hrmax * (observed > state->active ? boundary + hysteresis_percent : boundary - hysteresis_percent);
    const int outside = observed > state->active ? percentage >= threshold : percentage < threshold;
    if (!outside) {
        state->pending = 0;
        return state->active;
    }
    if (state->pending != observed) {
        state->pending = observed;
        state->pending_since_ms = now_ms;
    } else if ((uint32_t)(now_ms - state->pending_since_ms) >= dwell_ms) {
        state->active = observed;
        state->pending = 0;
    }
    return state->active;
}

int target_bpm_update_for_zone(TargetBpmState *state, uint16_t heart_rate, uint8_t active_zone)
{
    uint32_t target;
    if (state == NULL || state->hrmax == 0 || heart_rate == 0 ||
        active_zone < 1 || active_zone > 5) return 0;

    if (active_zone == 1) {
        const uint32_t denominator = 3u * state->hrmax;
        target = 60u + ((uint32_t)heart_rate * 100u + denominator / 2u) / denominator;
        if (target > 80u) target = 80u;
    } else if (active_zone <= 3) {
        target = heart_rate;
    } else {
        target = ((uint32_t)heart_rate * 3u + 2u) / 4u;
    }

    // ponytail: Upward change assumes 1 Hz calls; use elapsed time if cadence varies.
    if (state->bpm != 0 && target > (uint32_t)state->bpm + 5u) {
        target = (uint32_t)state->bpm + 5u;
    }
    state->bpm = (uint16_t)target;
    return (int)target;
}

int target_bpm_update(TargetBpmState *state, uint16_t heart_rate)
{
    return target_bpm_update_for_zone(
        state, heart_rate,
        state == NULL ? 0 : heart_zone_observed(state->hrmax, heart_rate));
}

#ifdef TARGET_BPM_SELF_TEST
#include <assert.h>
#include <stdio.h>

int main(void)
{
    TargetBpmState bpm = {200, 0};
    HeartZoneState zone = {0, 0, 0};

    assert(target_bpm_update(NULL, 60) == 0);
    assert(target_bpm_update(&bpm, 0) == 0 && bpm.bpm == 0);
    assert(target_bpm_update(&bpm, 60) == 70);
    assert(target_bpm_update(&bpm, 119) == 75);
    assert(target_bpm_update(&bpm, 120) == 80);
    bpm.bpm = 0;
    assert(target_bpm_update(&bpm, 120) == 120);
    assert(target_bpm_update(&bpm, 160) == 120);
    bpm.bpm = 0;
    assert(target_bpm_update(&bpm, 180) == 135);
    assert(target_bpm_update_for_zone(&bpm, 126, 1) == 80);

    assert(heart_zone_update(&zone, 200, 118, 0, 10000, 2) == 1);
    assert(heart_zone_update(&zone, 200, 121, 1000, 10000, 2) == 1);
    assert(heart_zone_update(&zone, 200, 126, 2000, 10000, 2) == 1);
    assert(heart_zone_update(&zone, 200, 126, 11999, 10000, 2) == 1);
    assert(heart_zone_update(&zone, 200, 126, 12000, 10000, 2) == 2);
    assert(heart_zone_update(&zone, 200, 119, 13000, 10000, 2) == 2);
    assert(heart_zone_update(&zone, 200, 114, 14000, 10000, 2) == 2);
    assert(heart_zone_update(&zone, 200, 114, 24000, 10000, 2) == 1);

    puts("target_bpm: OK");
    return 0;
}
#endif
