#ifndef ANALYSIS_TASK_H
#define ANALYSIS_TASK_H

#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>

#include "debug.h"
#include "globalData.h"

#include <stdint.h>
void analysisTask(void* parameter);

#endif // ANALYSIS_TASK_H