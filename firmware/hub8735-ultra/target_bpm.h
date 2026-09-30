#ifndef TARGET_BPM_H
#define TARGET_BPM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t hrmax;
    uint16_t bpm;
} TargetBpmState;

typedef struct {
    uint8_t active;
    uint8_t pending;
    uint32_t pending_since_ms;
} HeartZoneState;

/* Return 1..5, or 0 for invalid input. */
uint8_t heart_zone_observed(uint16_t hrmax, uint16_t heart_rate);

/* Hold a candidate zone until it clears hysteresis for dwell_ms. */
uint8_t heart_zone_update(HeartZoneState *state, uint16_t hrmax,
                          uint16_t heart_rate, uint32_t now_ms,
                          uint32_t dwell_ms, uint8_t hysteresis_percent);

/* Call once per second; invalid input leaves state unchanged. */
int target_bpm_update_for_zone(TargetBpmState *state, uint16_t heart_rate,
                               uint8_t active_zone);
int target_bpm_update(TargetBpmState *state, uint16_t heart_rate);

#ifdef __cplusplus
}
#endif

#endif
