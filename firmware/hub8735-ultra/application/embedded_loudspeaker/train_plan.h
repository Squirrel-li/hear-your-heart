#ifndef TRAIN_PLAN_H
#define TRAIN_PLAN_H

#include <stdint.h>

// Train 模式內建課表：每段指定目標心率 Zone 與固定時長，時間到立即換段換歌，全部結束後停止。
// 歌曲 BPM = 該 Zone 中點心率（%HRmax），段落越強歌越快；實際心率不影響 Train 選曲。
// 參考間歇訓練「慢、快、慢、更快、慢」的組合拳：暖身 Z2、衝刺 Z4、恢復 Z2、更強 Z5、緩和 Z1。
struct TrainStage {
    uint8_t zone;        // 1..5
    uint16_t seconds;
};

struct TrainPlan {
    const char* name;    // OLED label, <= 9 chars
    uint8_t stageCount;
    TrainStage stages[5];
};

constexpr TrainPlan TRAIN_PLANS[] = {
    {"Train 11m", 5, {{2, 180}, {4, 120}, {2, 120}, {5, 60}, {1, 180}}},
    {"Train 5m", 5, {{2, 60}, {4, 60}, {2, 60}, {5, 60}, {1, 60}}},  // short version for demos
};
constexpr uint8_t TRAIN_PLAN_COUNT = sizeof(TRAIN_PLANS) / sizeof(TRAIN_PLANS[0]);
constexpr uint8_t TRAIN_STAGE_DONE = 0xFF;

// Zone midpoints (%HRmax), same zone edges as analysisTask: <60 / 60-70 / 70-80 / 80-90 / >=90.
inline uint16_t train_stage_bpm(uint16_t hrmax, uint8_t zone)
{
    static const uint8_t MID_PERCENT[5] = {55, 65, 75, 85, 95};
    if (hrmax == 0 || zone < 1 || zone > 5) return 0;
    return (uint16_t)((hrmax * MID_PERCENT[zone - 1] + 50u) / 100u);
}

// Stage index at elapsed time, or TRAIN_STAGE_DONE once the plan is over; remainingS = seconds left in that stage.
inline uint8_t train_stage_at(const TrainPlan& plan, uint32_t elapsedMs, uint16_t* remainingS)
{
    uint32_t endMs = 0;
    for (uint8_t i = 0; i < plan.stageCount; ++i) {
        endMs += plan.stages[i].seconds * 1000u;
        if (elapsedMs < endMs) {
            if (remainingS != nullptr) *remainingS = (uint16_t)((endMs - elapsedMs + 999u) / 1000u);
            return i;
        }
    }
    if (remainingS != nullptr) *remainingS = 0;
    return TRAIN_STAGE_DONE;
}

#endif // TRAIN_PLAN_H
