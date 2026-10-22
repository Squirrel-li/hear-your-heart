#ifndef MOTION_LEVEL_H
#define MOTION_LEVEL_H

#include <math.h>
#include <stdint.h>

// 運動強度：LSM6DS3 ±8 g、加速度計已用高通去掉重力，所以三軸合力 ≈ 動態加速度。
// 每 MOTION_BUCKET_MS 取該段樣本的平均合力，再取最近 MOTION_WINDOW_BUCKETS 段平均分級；
// 新等級需連續維持 MOTION_DWELL_MS 才確認（單段突波不切換）。1 Hz（BLE）與 12.5 Hz（本地）來源都適用。
// shortcut: thresholds are estimates for a body-worn sensor; recalibrate from the logged mean mg.
constexpr uint8_t MOTION_LEVELS = 5;  // 0 靜止, 1 走路, 2 快走, 3 跑步, 4 跳躍
constexpr uint16_t MOTION_LEVEL_MG[MOTION_LEVELS - 1] = {50, 250, 600, 1200};
constexpr float ACC_MG_PER_LSB = 0.244f;
constexpr uint32_t MOTION_BUCKET_MS = 500;
constexpr uint8_t MOTION_WINDOW_BUCKETS = 1;
constexpr uint32_t MOTION_DWELL_MS = 500;
constexpr uint8_t MOTION_LEVEL_NONE = 0xFF;

struct MotionLevelState {
    float bucketSum = 0.0f;
    uint16_t bucketCount = 0;
    uint32_t bucketStartMs = 0;
    float window[MOTION_WINDOW_BUCKETS] = {};
    uint8_t windowCount = 0;
    uint8_t windowIdx = 0;
    uint8_t active = MOTION_LEVEL_NONE;
    uint8_t pending = MOTION_LEVEL_NONE;
    uint32_t pendingSinceMs = 0;
    uint16_t lastMeanMg = 0;  // for logging / calibration
};

inline void motion_level_add(MotionLevelState& s, int16_t x, int16_t y, int16_t z)
{
    const float fx = x, fy = y, fz = z;
    s.bucketSum += sqrtf(fx * fx + fy * fy + fz * fz) * ACC_MG_PER_LSB;
    ++s.bucketCount;
}

// New source: drop old samples, keep the confirmed level (sensor loss keeps the last level).
inline void motion_level_reset_samples(MotionLevelState& s)
{
    s.bucketSum = 0.0f;
    s.bucketCount = 0;
    s.windowCount = 0;
    s.windowIdx = 0;
    s.pending = MOTION_LEVEL_NONE;
}

inline uint8_t motion_level_for(float meanMg)
{
    uint8_t level = 0;
    while (level < MOTION_LEVELS - 1 && meanMg >= MOTION_LEVEL_MG[level]) ++level;
    return level;
}

// Call often; evaluates once per MOTION_BUCKET_MS. Returns the newly confirmed level, or -1 if unchanged.
inline int motion_level_update(MotionLevelState& s, uint32_t nowMs)
{
    if ((uint32_t)(nowMs - s.bucketStartMs) < MOTION_BUCKET_MS) return -1;
    s.bucketStartMs = nowMs;
    if (s.bucketCount == 0) return -1;  // no samples in this bucket

    s.window[s.windowIdx] = s.bucketSum / s.bucketCount;
    s.windowIdx = (uint8_t)((s.windowIdx + 1) % MOTION_WINDOW_BUCKETS);
    if (s.windowCount < MOTION_WINDOW_BUCKETS) ++s.windowCount;
    s.bucketSum = 0.0f;
    s.bucketCount = 0;

    float sum = 0.0f;
    for (uint8_t i = 0; i < s.windowCount; ++i) sum += s.window[i];
    const float mean = sum / s.windowCount;
    s.lastMeanMg = (uint16_t)(mean > 65535.0f ? 65535.0f : mean + 0.5f);
    const uint8_t observed = motion_level_for(mean);

    if (s.active == MOTION_LEVEL_NONE) {
        s.active = observed;
        return observed;
    }
    if (observed == s.active) {
        s.pending = MOTION_LEVEL_NONE;
        return -1;
    }
    if (observed != s.pending) {
        s.pending = observed;
        s.pendingSinceMs = nowMs;
        return -1;
    }
    if ((uint32_t)(nowMs - s.pendingSinceMs) < MOTION_DWELL_MS) return -1;
    s.active = observed;
    s.pending = MOTION_LEVEL_NONE;
    return observed;
}

#endif // MOTION_LEVEL_H
