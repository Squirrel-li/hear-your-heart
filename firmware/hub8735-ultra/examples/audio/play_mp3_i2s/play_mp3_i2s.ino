/*
 * PlayMp3 - play an MP3 file from the SD card through MAX98357.
 *
 * Wiring (I2S0 fixed pin group):
 *   MAX98357 BCLK -> D24, LRC -> D12, DIN -> D11, VIN -> 5V, GND -> GND
 *
 * Put an MP3 named test001.mp3 in the SD card root.
 */

#include "AmebaFatFS.h"
#include <MAX98357.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>

AmebaFatFS fs;
MAX98357 amp;

static TaskHandle_t sensorTaskHandle = nullptr;
static TaskHandle_t audioTaskHandle = nullptr;

void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    Serial.begin(115200);
    delay(1000);

    fs.begin();
    amp.begin();
    amp.setVolume(0.8);

    if (xTaskCreate(sensorTask, "sensor", 1024, nullptr, 2, &sensorTaskHandle) != pdPASS) {
        Serial.println("sensor task create failed");
    }
    if (xTaskCreate(audioTask, "audio", 1024, nullptr, 2, &audioTaskHandle) != pdPASS) {
        Serial.println("audio task create failed");
    }

}

void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}


void audioTask(void *argument)
{
    (void)argument;

    for (;;) {
        Serial.println("Playing music2.mp3 ...");
        if (amp.playMp3(fs, "music2.mp3")) {
            Serial.println("Done.");
        } else {
            Serial.print("Playback failed: ");
            Serial.println(amp.lastError());
        }
    }
}

void sensorTask(void *argument)
{
    (void)argument;

    for (;;) {
        digitalWrite(LED_BUILTIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(500));
        digitalWrite(LED_BUILTIN, LOW);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}