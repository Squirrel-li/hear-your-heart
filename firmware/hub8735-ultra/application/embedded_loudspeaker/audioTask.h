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



// SD_MODE pins of the two MAX98357 amps (borrowed SPI1 MISO/CS pins). setup() drives them LOW
// at boot so the amps stay in shutdown until audioTask releases them for playback.
constexpr uint8_t AMP_SD_PIN_RIGHT = 18;
constexpr uint8_t AMP_SD_PIN_LEFT = 21;

void audioTask(void* parameter);

#endif // AUDIO_TASK_H