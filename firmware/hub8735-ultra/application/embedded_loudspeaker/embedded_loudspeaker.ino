#include "AmebaFatFS.h"
#include <MAX98357.h>

#include "SensorTask.h"
#include "ControlTask.h"
#include "DisplayTask.h"
#include "CommTask.h"
#include "LogTask.h"



static QueueHandle_t sampleQueue = nullptr;

static TaskHandle_t sensorTaskHandle = nullptr;
static TaskHandle_t analysisTaskHandle = nullptr;
static TaskHandle_t audioTaskHandle = nullptr;
static TaskHandle_t BLETaskHandle = nullptr;
static TaskHandle_t UITaskHandle = nullptr;

AmebaFatFS fs;
MAX98357 amp;

void setup() 
{
    fs.begin();                  // SD 卡
    amp.begin();                 // I2S 初始化(佔用 D11/D12/D22/D23/D24)
    amp.setShutdownPin(10);      // 選用:SD_MODE 接 D10,不播放時硬體關斷
    amp.setVolume(0.8);          // 軟體音量 0.0 ~ 1.0

    // if (!amp.playWav(fs, "test001.wav")) {   // 阻塞式播放 SD 卡根目錄的檔案
    //     Serial.println(amp.lastError());
    // }

    if (xTaskCreate(sensorTask, "sensor", 1024, nullptr, 2, &sensorTaskHandle) != pdPASS) {
        Serial.println("sensor task create failed");
    }
    if (xTaskCreate(analysisTask, "analysis", 1024, nullptr, 2, &analysisTaskHandle) != pdPASS) {
        Serial.println("analysis task create failed");
    }
    if (xTaskCreate(audioTask, "audio", 1024, nullptr, 2, &audioTaskHandle) != pdPASS) {
        Serial.println("audio task create failed");
    }
    if (xTaskCreate(BLETask, "BLE", 1024, nullptr, 2, &BLETaskHandle) != pdPASS) {
        Serial.println("BLE task create failed");
    }
    if (xTaskCreate(UITask, "UI", 1024, nullptr, 2, &UITaskHandle) != pdPASS) {
        Serial.println("UI task create failed");
    }
}

void loop() 
{
    int test_num = 0x2A;
}
