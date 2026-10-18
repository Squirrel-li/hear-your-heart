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



// One EN pin drives both MAX98357 SD_MODE pins (left direct, right via 200k; see pinset.md).
// setup() drives it LOW at boot so the amps stay in shutdown until audioTask enables them.
constexpr uint8_t AMP_SD_PIN = 15;

void audioTask(void* parameter);

#endif // AUDIO_TASK_H