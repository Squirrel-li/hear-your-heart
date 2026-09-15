#ifndef AUDIO_TASK_H
#define AUDIO_TASK_H

#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>

#include "debug.h"
#include "globalData.h"

#include "AmebaFatFS.h"
#include <MAX98357.h>



void audioTask(void* parameter);

#endif // AUDIO_TASK_H