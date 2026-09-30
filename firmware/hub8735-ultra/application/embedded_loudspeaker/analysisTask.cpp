#include "analysisTask.h"

namespace {

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

uint8_t heart_zone_update(HeartZoneState* state, uint16_t hrmax,
                          uint16_t heart_rate, uint32_t now_ms,
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
    const uint8_t boundary = movingUp
        ? static_cast<uint8_t>(50u + state->active * 10u)
        : static_cast<uint8_t>(40u + state->active * 10u);
    const uint32_t percentage = (uint32_t)heart_rate * 100u;
    const uint8_t thresholdPercent = movingUp
        ? static_cast<uint8_t>(boundary + hysteresis_percent)
        : static_cast<uint8_t>(boundary - hysteresis_percent);
    const uint32_t threshold = (uint32_t)hrmax * thresholdPercent;
    const bool outside = movingUp ? percentage >= threshold : percentage < threshold;
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

int target_bpm_update_for_zone(TargetBpmState* state, uint16_t heart_rate,
                               uint8_t active_zone)
{
    uint32_t target;
    if (state == nullptr || state->hrmax == 0 || heart_rate == 0 ||
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

    // ponytail: The upward ramp assumes 1 Hz calls; use elapsed time if cadence varies.
    if (state->bpm != 0 && target > (uint32_t)state->bpm + 5u) {
        target = (uint32_t)state->bpm + 5u;
    }
    state->bpm = static_cast<uint16_t>(target);
    return static_cast<int>(target);
}

} // namespace

void analysisTask(void* parameter)
{
    (void)parameter;
    const float maxHeartRate = userInfo.HRmax();
    const uint16_t hrmaxBpm =
        userInfo.age > 0 && maxHeartRate > 0.0f
            ? static_cast<uint16_t>(maxHeartRate + 0.5f)
            : 0;
    TargetBpmState bpmState{hrmaxBpm, 0};
    HeartZoneState zoneState{};
    PpgSensorElement latest{};
    GSensorElement gSensorElement{};
    bool haveValidPpg = false;
    bool haveUpdatedBpm = false;
    uint32_t lastBpmUpdateMs = 0;
    uint16_t lastQueuedBpm = 0;
    uint8_t lastQueuedZone = 0;

    for (;;) {
        PpgSensorElement sample{};
        while (xQueueReceive(ppgSensorQueue, &sample, 0) == pdPASS) {
            DEBUG_PRINT("BLETask -> analysisTask [recv]: PPG HR=");
            DEBUG_PRINT(sample.heartRateBpm);
            DEBUG_PRINT(" status=");
            DEBUG_PRINT(sample.status);
            DEBUG_PRINT(" sequence=");
            DEBUG_PRINTLN(sample.sequence);
            haveValidPpg = sample.status == 2 && sample.heartRateBpm != 0;
            if (haveValidPpg) latest = sample;
        }

        const uint32_t now = millis();
        if (haveValidPpg &&
            (uint32_t)(now - latest.receive_timestamp) <= 6000u &&
            bpmState.hrmax != 0) {
            if (!haveUpdatedBpm ||
                (uint32_t)(now - lastBpmUpdateMs) >= 1000u) {
                haveUpdatedBpm = true;
                lastBpmUpdateMs = now;
                const uint8_t zone = heart_zone_update(
                    &zoneState, bpmState.hrmax, latest.heartRateBpm, now,
                    10000u, 2u);
                const int target = target_bpm_update_for_zone(
                    &bpmState, latest.heartRateBpm, zone);
                if (target > 0 &&
                    (target != lastQueuedBpm || zone != lastQueuedZone) &&
                    targetBpmResultQueue != nullptr) {
                    TargetBpmResult result{};
                    result.targetBpm = static_cast<uint16_t>(target);
                    result.zone = zone;
                    if (xQueueOverwrite(targetBpmResultQueue, &result) == pdPASS) {
                        lastQueuedBpm = result.targetBpm;
                        lastQueuedZone = result.zone;
                        DEBUG_PRINT("analysisTask -> controlTask: target BPM=");
                        DEBUG_PRINT(result.targetBpm);
                        DEBUG_PRINT(" zone=");
                        DEBUG_PRINTLN(result.zone);
                    } else {
                        DEBUG_PRINTLN("analysisTask -> controlTask: failed to queue target BPM");
                    }
                }
            }
        } else {
            zoneState.pending = 0; // Stale sensor data cannot confirm a Zone change.
        }

        if (xQueueReceive(gSensorQueue, &gSensorElement, 0) == pdPASS) {
            DEBUG_PRINT("BLETask -> analysisTask [recv]: G sensor sequence=");
            DEBUG_PRINTLN(gSensorElement.sequence);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
