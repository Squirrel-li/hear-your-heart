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
QueueHandle_t audioCmdQueue = nullptr;
QueueHandle_t audioPlaybackEventQueue = nullptr;
QueueHandle_t uiUpdateQueue = nullptr;
QueueHandle_t bleTxQueue = nullptr;
QueueHandle_t uiCmdQueue = nullptr;

UserInfo userInfo;
volatile bool musicLibraryReady = false;

void setup()
{
    // 初始化串口
    #ifdef DEBUG_MODE
    Serial.begin(2000000);
    DEBUG_PRINTLN("Initializing USB type-C DEBUG serial port");
    #else
    Serial2.begin(115200);
    DEBUG_PRINTLN("Initializing DEBUG serial port");
    #endif
    DEBUG_PRINTLN("Starting embedded loudspeaker application");
    
    // Serial2 TX is pin 8, which UITask uses as a menu button; PPG data now comes over BLE.

    userInfo.age = 25;
    userInfo.weight = 70.5;
    userInfo.height = 175.0;
    
    // 初始化應用程式佇列
    if (!appQueuesInit()) {
        DEBUG_PRINTLN("Failed to initialize app queues");
        while(1);
    }

    // 建立任務
    if (xTaskCreate(BLETask, "BLE", 1024, nullptr, 2, &BLETaskHandle) != pdPASS) {
        DEBUG_PRINTLN("BLE task create failed");
        while(1);
    }
    if (xTaskCreate(analysisTask, "analysis", 1024, nullptr, 2, &analysisTaskHandle) != pdPASS) {
        DEBUG_PRINTLN("analysis task create failed");
        while(1);
    }
    if (xTaskCreate(controlTask, "control", 1024, nullptr, 2, &controlTaskHandle) != pdPASS) {
        DEBUG_PRINTLN("control task create failed");
        while(1);
    }
    if (xTaskCreate(audioTask, "audio", 1024, nullptr, 2, &audioTaskHandle) != pdPASS) {
        DEBUG_PRINTLN("audio task create failed");
        while(1);
    }
    if (xTaskCreate(UITask, "UI", 2048, nullptr, 2, &UITaskHandle) != pdPASS) {
        DEBUG_PRINTLN("UI task create failed");
        while(1);
    }
    DEBUG_PRINTLN("All tasks created successfully");

}

void loop() 
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}
