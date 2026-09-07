#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>

struct SensorSample {
    uint32_t timestampMs;
    int heartRate;
    int activity;
};

static QueueHandle_t sampleQueue = nullptr;

static TaskHandle_t sensorTaskHandle = nullptr;
static TaskHandle_t analysisTaskHandle = nullptr;

static void sensorTask(void *argument)
{
    (void)argument;

    const TickType_t period = pdMS_TO_TICKS(100);
    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
        SensorSample sample = {
            millis(),
            75,  // TODO: 替換成實際心率感測值
            0    // TODO: 替換成實際活動狀態
        };

        // Queue 會複製結構內容；不要傳送區域變數的指標。
        if (xQueueSend(sampleQueue, &sample, pdMS_TO_TICKS(10)) != pdPASS) {
            // Queue 滿時可在此記錄遺失資料或覆蓋舊資料。
        }

        vTaskDelayUntil(&lastWake, period);
    }
}

static void analysisTask(void *argument)
{
    (void)argument;

    for (;;) {
        SensorSample sample;

        // 沒有資料時阻塞，不會佔用 CPU。
        if (xQueueReceive(sampleQueue, &sample, portMAX_DELAY) == pdPASS) {
            // TODO: 依心率、活動狀態選擇音樂或更新狀態。
            Serial.println("timestampMs:" + String(sample.timestampMs) + ", heartRate:" + String(sample.heartRate) + ", activity:" + String(sample.activity));
        }
    }
}

void setup()
{
    Serial.begin(115200);

    sampleQueue = xQueueCreate(4, sizeof(SensorSample));
    if (sampleQueue == nullptr) {
        Serial.println("sampleQueue create failed");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // xTaskCreate 的 stack depth 是 StackType_t 數量；目前 ARMv8-M port 為 4 bytes。
    if (xTaskCreate(sensorTask, "sensor", 1024, nullptr, 2, &sensorTaskHandle) != pdPASS) {
        Serial.println("sensor task create failed");
    }

    if (xTaskCreate(analysisTask, "analysis", 1024, nullptr, 2, &analysisTaskHandle) != pdPASS) {
        Serial.println("analysis task create failed");
    }
}

void loop()
{
    // loop() 本身也是 FreeRTOS task；沒有工作時要阻塞或延遲。
    vTaskDelay(pdMS_TO_TICKS(1000));
}