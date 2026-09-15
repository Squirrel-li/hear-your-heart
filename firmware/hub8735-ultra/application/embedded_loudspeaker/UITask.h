#ifndef UI_TASK_H
#define UI_TASK_H

#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>

void UITask(void* parameter);

#endif // UI_TASK_H