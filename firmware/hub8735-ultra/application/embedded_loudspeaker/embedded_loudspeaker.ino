#include "debug.h"

#include "globalData.h"

#include "BLETask.h"
#include "analysisTask.h"
#include "controlTask.h"
#include "audioTask.h"
#include "UITask.h"

static TaskHandle_t BLETaskHandle = nullptr;
static TaskHandle_t analysisTaskHandle = nullptr;
static TaskHandle_t controlTaskHandle = nullptr;
static TaskHandle_t audioTaskHandle = nullptr;
static TaskHandle_t UITaskHandle = nullptr;

QueueHandle_t bleRxFrameQueue = nullptr;
QueueHandle_t ppgSensorQueue = nullptr;
QueueHandle_t accSensorQueue = nullptr;
QueueHandle_t gyroSensorQueue = nullptr;
QueueHandle_t targetBpmResultQueue = nullptr;
QueueHandle_t motionLevelQueue = nullptr;
QueueHandle_t audioCmdQueue = nullptr;
QueueHandle_t audioPlaybackEventQueue = nullptr;
QueueHandle_t uiUpdateQueue = nullptr;
QueueHandle_t bleTxQueue = nullptr;
QueueHandle_t uiCmdQueue = nullptr;
QueueHandle_t analysisCmdQueue = nullptr;
QueueHandle_t bleCmdQueue = nullptr;
volatile SensorSource activePpgSource = SensorSource::Chest;
volatile SensorSource activeImuSource = SensorSource::Chest;

UserInfo userInfo;
volatile bool musicLibraryReady = false;
volatile bool musicStorageReady = false;
volatile bool musicBpmAnalysisBusy = false;
volatile bool systemStandby = false;

void setup()
{
    // Hold both amps in shutdown from the very start of boot.
    pinMode(AMP_SD_PIN, OUTPUT);
    digitalWrite(AMP_SD_PIN, LOW);

    // 初始化串口
    Serial.begin(2000000);
    logInit();
    LOGI("MAIN", "embedded_loudspeaker build %s %s, log level=%d", __DATE__, __TIME__, LOG_LEVEL);
    LOGI("MAIN", "log port: Serial (LOG UART, 2000000 baud), TX = D17 / RX = D16");

    userInfo.age = 25;
    userInfo.weight = 70.5;
    userInfo.height = 175.0;
    LOGI("MAIN", "user: age=%d weight=%d height=%d", static_cast<int>(userInfo.age),
         static_cast<int>(userInfo.weight), static_cast<int>(userInfo.height));

    // 初始化應用程式佇列
    if (!appQueuesInit()) {
        LOGE("MAIN", "app queue creation failed; halting");
        while(1);
    }
    LOGI("MAIN", "app queues created, free heap=%u", static_cast<unsigned>(xPortGetFreeHeapSize()));

    // 建立任務
    if (xTaskCreate(BLETask, "BLE", 1024, nullptr, 2, &BLETaskHandle) != pdPASS) {
        LOGE("MAIN", "BLE task create failed; halting");
        while(1);
    }
    if (xTaskCreate(analysisTask, "analysis", 1024, nullptr, 2, &analysisTaskHandle) != pdPASS) {
        LOGE("MAIN", "analysis task create failed; halting");
        while(1);
    }
    if (xTaskCreate(controlTask, "control", 1024, nullptr, 2, &controlTaskHandle) != pdPASS) {
        LOGE("MAIN", "control task create failed; halting");
        while(1);
    }
    if (xTaskCreate(audioTask, "audio", 1024, nullptr, 2, &audioTaskHandle) != pdPASS) {
        LOGE("MAIN", "audio task create failed; halting");
        while(1);
    }
    if (xTaskCreate(UITask, "UI", 2048, nullptr, 2, &UITaskHandle) != pdPASS) {
        LOGE("MAIN", "UI task create failed; halting");
        while(1);
    }
    LOGI("MAIN", "all tasks created (stack words: BLE/analysis/control/audio=1024, UI=2048), free heap=%u",
         static_cast<unsigned>(xPortGetFreeHeapSize()));
}

void loop() 
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}
