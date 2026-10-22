#include "analysisTask.h"

#include "localSensor.h"
#include "motion_level.h"
#include "ppg_raw_hr.h"

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

const char* sourceName(SensorSource source)
{
    return source == SensorSource::Chest ? "chest" : source == SensorSource::Watch ? "watch" : "local";
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
    LOGI("ANALYSIS", "started: age=%d HRmax=%u bpm (zones: 60/70/80/90%%)", userInfo.age,
         static_cast<unsigned>(hrmaxBpm));
    if (hrmaxBpm == 0) LOGE("ANALYSIS", "HRmax is 0 (invalid user info); target BPM will never be produced");
    TargetBpmState bpmState{hrmaxBpm, 0};
    HeartZoneState zoneState{};
    MotionLevelState motionState{};
    PpgSensorElement latest{};
    AccElement accElement{};
    GyroElement gyroElement{};
    bool haveValidPpg = false;
    bool haveUpdatedBpm = false;
    uint32_t lastBpmUpdateMs = 0;
    uint16_t lastQueuedBpm = 0;
    uint8_t lastQueuedZone = 0;
    bool wasFresh = false;
    uint8_t lastZoneSeen = 0;
    bool wasStandby = false;
    // 0x05 原始 PPG：HUB 自算心率。有效結果優先於 0x01 封包內的心率，結果失效或逾時才改用 0x01。
    static PpgRawHrState rawHr;  // static：不佔 4 KB task stack
    ppg_raw_hr_reset(rawHr);
    uint8_t rawBpm = 0;
    uint32_t rawBpmMs = 0;
    localSensorInit();  // 本地感測器先全部關閉，再只開預設選為 Local 的來源
    if (activePpgSource == SensorSource::Local && !localPpgSetEnabled(true)) {
        LOGE("ANALYSIS", "default local PPG init failed");
    }
    if (activeImuSource == SensorSource::Local && !localImuSetEnabled(true)) {
        LOGE("ANALYSIS", "default local IMU init failed");
    }

    for (;;) {
        AnalysisCmdElement cmd{};
        while (analysisCmdQueue != nullptr && xQueueReceive(analysisCmdQueue, &cmd, 0) == pdPASS) {
            const bool isPpg = cmd.command == AnalysisCmdType::SetPpgSource;
            volatile SensorSource& current = isPpg ? activePpgSource : activeImuSource;
            if (cmd.source == current) continue;
            // 只有被選為來源的本地感測器才供電；初始化失敗時維持原來源。
            const bool enable = cmd.source == SensorSource::Local;
            if (!(isPpg ? localPpgSetEnabled(enable) : localImuSetEnabled(enable))) {
                LOGE("ANALYSIS", "%s local sensor init failed; source stays %s", isPpg ? "PPG" : "motion",
                     sourceName(current));
                continue;
            }
            LOGI("ANALYSIS", "%s source %s -> %s", isPpg ? "PPG" : "motion", sourceName(current),
                 sourceName(cmd.source));
            current = cmd.source;
            // 清掉舊來源殘留的樣本，避免被當成新來源的資料。
            xQueueReset(isPpg ? ppgSensorQueue : accSensorQueue);
            if (!isPpg) {
                xQueueReset(gyroSensorQueue);
                motion_level_reset_samples(motionState);
            }
            if (isPpg) {
                // 新來源尚無有效心率；目標 BPM 沿用最後值，Zone 變更重新計時。
                haveValidPpg = false;
                zoneState.pending = 0;
                xQueueReset(ppgRawQueue);
                ppg_raw_hr_reset(rawHr);
                rawBpm = 0;
            }
        }

        // 待機時本地感測器斷電；喚醒後依原來源重新開啟（來源設定保留）。
        if (systemStandby != wasStandby) {
            wasStandby = systemStandby;
            const bool on = !wasStandby;
            LOGI("ANALYSIS", "%s: local sensors %s", on ? "wake" : "standby", on ? "restored" : "powered down");
            if ((!on || activePpgSource == SensorSource::Local) && !localPpgSetEnabled(on)) {
                LOGE("ANALYSIS", "local PPG re-enable failed after wake");
            }
            if ((!on || activeImuSource == SensorSource::Local) && !localImuSetEnabled(on)) {
                LOGE("ANALYSIS", "local IMU re-enable failed after wake");
            }
            haveValidPpg = false;  // 喚醒後等新的有效心率；目標 BPM 沿用最後值
            ppg_raw_hr_reset(rawHr);
            rawBpm = 0;
        }

        localSensorPoll();  // 選了本地來源才會有資料；樣本與 BLE 走同一組 queue

        PpgRawElement raw{};
        while (xQueueReceive(ppgRawQueue, &raw, 0) == pdPASS) {
            const int bpm = ppg_raw_hr_feed(rawHr, raw.sensor_time_ms, raw.ppg1, raw.ppg2);
            if (bpm < 0) continue;
            LOGI("ANALYSIS", "HUB raw PPG cycle: HR=%d (ch1=%u ch2=%u)%s", bpm, static_cast<unsigned>(rawHr.hr.last_b1),
                 static_cast<unsigned>(rawHr.hr.last_b2), bpm == 0 ? ", invalid -> 0x01 heart rate used" : "");
            rawBpm = static_cast<uint8_t>(bpm);
            if (bpm == 0) continue;
            rawBpmMs = raw.hub_rx_time_ms;
            latest = PpgSensorElement{};
            latest.hub_rx_time_ms = raw.hub_rx_time_ms;
            latest.sensor_time_ms = raw.sensor_time_ms;
            latest.sequence = raw.sequence;
            latest.status = 2;
            latest.heartRateBpm = rawBpm;
            if (!haveValidPpg) LOGI("ANALYSIS", "heart rate valid (HUB raw HR=%u)", static_cast<unsigned>(rawBpm));
            haveValidPpg = true;
        }
        const bool rawHrActive = rawBpm != 0 && (uint32_t)(millis() - rawBpmMs) <= 15000u;

        PpgSensorElement sample{};
        while (xQueueReceive(ppgSensorQueue, &sample, 0) == pdPASS) {
            if (rawHrActive) {
                LOGT("ANALYSIS", "PPG HR=%u ignored: HUB raw HR %u in use", static_cast<unsigned>(sample.heartRateBpm),
                     static_cast<unsigned>(rawBpm));
                continue;
            }
            const bool valid = sample.status == 2 && sample.heartRateBpm != 0;
            LOGT("ANALYSIS", "PPG recv: HR=%u status=%u sequence=%lu valid=%d", static_cast<unsigned>(sample.heartRateBpm),
                 static_cast<unsigned>(sample.status), static_cast<unsigned long>(sample.sequence), valid ? 1 : 0);
            if (valid != haveValidPpg) {
                LOGI("ANALYSIS", "heart rate %s (HR=%u status=%u)", valid ? "valid" : "invalid",
                     static_cast<unsigned>(sample.heartRateBpm), static_cast<unsigned>(sample.status));
            }
            haveValidPpg = valid;
            if (haveValidPpg) latest = sample;
        }

        const uint32_t now = millis();
        const bool fresh = haveValidPpg &&
            (uint32_t)(now - latest.hub_rx_time_ms) <= 15000u && bpmState.hrmax != 0;
        if (fresh != wasFresh) {
            if (fresh) {
                LOGI("ANALYSIS", "sensor data fresh again (HR=%u)", static_cast<unsigned>(latest.heartRateBpm));
            } else if (haveValidPpg && bpmState.hrmax != 0) {
                LOGW("ANALYSIS", "sensor data stale (> 15 s since last valid sample); keeping last target BPM=%u",
                     static_cast<unsigned>(lastQueuedBpm));
            }
            wasFresh = fresh;
        }
        if (fresh) {
            if (!haveUpdatedBpm ||
                (uint32_t)(now - lastBpmUpdateMs) >= 1000u) {
                haveUpdatedBpm = true;
                lastBpmUpdateMs = now;
                const uint8_t zone = heart_zone_update(
                    &zoneState, bpmState.hrmax, latest.heartRateBpm, now,
                    10000u, 2u);
                const uint16_t previousBpm = bpmState.bpm;
                const int target = target_bpm_update_for_zone(
                    &bpmState, latest.heartRateBpm, zone);
                if (zone != lastZoneSeen) {
                    LOGI("ANALYSIS", "zone %u -> %u (HR=%u, %u%% of HRmax)", static_cast<unsigned>(lastZoneSeen),
                         static_cast<unsigned>(zone), static_cast<unsigned>(latest.heartRateBpm),
                         static_cast<unsigned>(latest.heartRateBpm * 100u / bpmState.hrmax));
                    lastZoneSeen = zone;
                } else if (zoneState.pending != 0) {
                    LOGD("ANALYSIS", "zone %u change to %u pending (HR=%u, needs 10 s dwell)",
                         static_cast<unsigned>(zone), static_cast<unsigned>(zoneState.pending),
                         static_cast<unsigned>(latest.heartRateBpm));
                }
                LOGD("ANALYSIS", "update: HR=%u zone=%u target=%d (prev=%u%s)", static_cast<unsigned>(latest.heartRateBpm),
                     static_cast<unsigned>(zone), target, static_cast<unsigned>(previousBpm),
                     (previousBpm != 0 && target == static_cast<int>(previousBpm) + 5) ? ", ramp-limited +5" : "");
                if (target > 0 &&
                    (target != lastQueuedBpm || zone != lastQueuedZone) &&
                    targetBpmResultQueue != nullptr) {
                    TargetBpmResult result{};
                    result.targetBpm = static_cast<uint16_t>(target);
                    result.zone = zone;
                    if (xQueueOverwrite(targetBpmResultQueue, &result) == pdPASS) {
                        lastQueuedBpm = result.targetBpm;
                        lastQueuedZone = result.zone;
                        LOGI("ANALYSIS", "target BPM -> control: %u (zone %u)", static_cast<unsigned>(result.targetBpm),
                             static_cast<unsigned>(result.zone));
                    } else {
                        LOGE("ANALYSIS", "failed to queue target BPM %u", static_cast<unsigned>(result.targetBpm));
                    }
                }
            }
        } else {
            zoneState.pending = 0; // Stale sensor data cannot confirm a Zone change.
        }

        while (xQueueReceive(accSensorQueue, &accElement, 0) == pdPASS) {
            LOGT("ANALYSIS", "Acc recv: %d,%d,%d sequence=%lu", accElement.acc_x, accElement.acc_y,
                 accElement.acc_z, static_cast<unsigned long>(accElement.sequence));
            motion_level_add(motionState, accElement.acc_x, accElement.acc_y, accElement.acc_z);
        }
        const uint32_t bucketBefore = motionState.bucketStartMs;
        const bool hadSamples = motionState.bucketCount != 0;
        const int newLevel = motion_level_update(motionState, now);
        if (motionState.bucketStartMs != bucketBefore && hadSamples) {
            LOGD("ANALYSIS", "motion mean=%u mg level=%u pending=%d", static_cast<unsigned>(motionState.lastMeanMg),
                 static_cast<unsigned>(motionState.active),
                 motionState.pending == MOTION_LEVEL_NONE ? -1 : static_cast<int>(motionState.pending));
        }
        if (newLevel >= 0) {
            const uint8_t level = static_cast<uint8_t>(newLevel);
            LOGI("ANALYSIS", "motion level -> %u (mean %u mg)", static_cast<unsigned>(level),
                 static_cast<unsigned>(motionState.lastMeanMg));
            if (motionLevelQueue != nullptr) xQueueOverwrite(motionLevelQueue, &level);
        }
        if (xQueueReceive(gyroSensorQueue, &gyroElement, 0) == pdPASS) {
            LOGT("ANALYSIS", "Gyro recv: %d,%d,%d sequence=%lu", gyroElement.gyro_x, gyroElement.gyro_y,
                 gyroElement.gyro_z, static_cast<unsigned long>(gyroElement.sequence));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
