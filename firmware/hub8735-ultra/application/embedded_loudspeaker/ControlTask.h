#ifndef CONTROL_TASK_H
#define CONTROL_TASK_H

#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>

#include "debug.h"
#include "globalData.h"

void controlTask(void* parameter);

#endif // CONTROL_TASK_H
