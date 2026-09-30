#ifndef ANALYSIS_TASK_H
#define ANALYSIS_TASK_H

#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>

#include "debug.h"
#include "globalData.h"

#include <stdint.h>
typedef struct {
    uint16_t hrmax;
    uint16_t bpm;
} TargetBpmState;

typedef struct {
    uint8_t active;
    uint8_t pending;
    uint32_t pending_since_ms;
} HeartZoneState;
void analysisTask(void* parameter);

#endif // ANALYSIS_TASK_H