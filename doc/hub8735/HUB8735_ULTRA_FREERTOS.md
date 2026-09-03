# HUB 8735 Ultra 使用 FreeRTOS 指南

本文件說明本專案在 HUB 8735 Ultra（RTL8735B、AmebaPro2 Arduino SDK）中使用 FreeRTOS 的方式、目前 SDK 設定，以及常用 API。

本文件以專案內的 Arduino SDK 為主。專案中的 SDK 位於：

```text
external/HUB-8735/amebapro2_arduino/Arduino_package/
```

## 1. 先說結論

HUB 8735 Ultra 不需要另外移植 FreeRTOS。Arduino 套件已經內含 FreeRTOS、CMSIS-RTOS wrapper、task、queue、semaphore 與 software timer。

在目前架構中：

1. 不要在 `.ino` 中自行呼叫 `vTaskStartScheduler()` 或 `osKernelStart()`。
2. `setup()` 與 `loop()` 已經在 SDK 建立的 `main task` 中執行。
3. 需要並行處理時，在 `setup()` 建立自己的 task、queue 或 semaphore。
4. task 必須長時間存在，並以 `vTaskDelay()`、`vTaskDelayUntil()`、queue 或 notification 阻塞，不要用忙等消耗 CPU。
5. 直接使用 FreeRTOS API 時，`xTaskCreate()` 的 stack 參數是 `StackType_t` 數量；使用 `wiring_os.h` wrapper 時，文件定義為 bytes，兩者不要直接混用同一個數值。

## 2. 本專案目前的執行架構

Arduino SDK 的 `main.cpp` 會做以下事情：

```cpp
ameba_init();
xTaskCreate(main_task, "main task", MAIN_THREAD_STACK_SIZE, NULL, 1, NULL);
vTaskStartScheduler();
```

`main_task()` 之後呼叫 `setup()`，再反覆呼叫 `loop()`，每次迴圈最後執行 `vPortYield()`。因此，Arduino 程式不是裸機 `while(1)`，而是已經由 FreeRTOS 管理的 task。

目前 Ultra board variant 定義：

```cpp
#define MAIN_THREAD_STACK_SIZE (4096 * 4)
```

這是 SDK 的 `main task` 設定，不代表應該把所有應用程式 task 都設成同樣大小。直接呼叫 `xTaskCreate()` 時，依目前 ARMv8-M port 定義，`StackType_t` 是 `uint32_t`，因此 stack depth 應以 32-bit stack element 計算；請以 stack watermark 實測後再縮減或增加。

相關原始檔：

- `external/HUB-8735/amebapro2_arduino/Arduino_package/hardware/cores/ambpro2/main.cpp`
- `external/HUB-8735/amebapro2_arduino/Arduino_package/hardware/variants/ameba_hub8735_ultra/variant.h`
- `external/HUB-8735/amebapro2_arduino/Arduino_package/hardware/cores/ambpro2/wiring_os.h`

## 3. FreeRTOS API 與 Arduino wrapper 的選擇

### 3.1 直接使用 FreeRTOS API

適合需要 queue、task notification、ISR-safe API、精確控制 task priority 或 static allocation 的程式。

```cpp
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <semphr.h>
#include <timers.h>
```

常用 API：

| 用途 | API |
| --- | --- |
| 建立 task | `xTaskCreate()`、`xTaskCreateStatic()` |
| 刪除 task | `vTaskDelete()` |
| task 延遲 | `vTaskDelay()` |
| 固定週期執行 | `vTaskDelayUntil()` |
| 主動讓出 CPU | `taskYIELD()` |
| 取得 tick | `xTaskGetTickCount()` |
| 建立 queue | `xQueueCreate()`、`xQueueCreateStatic()` |
| 傳送/接收 queue | `xQueueSend()`、`xQueueReceive()` |
| ISR 傳送/接收 | `xQueueSendFromISR()`、`xQueueReceiveFromISR()` |
| 建立 mutex | `xSemaphoreCreateMutex()` |
| 建立 binary semaphore | `xSemaphoreCreateBinary()` |
| 取得/釋放 semaphore | `xSemaphoreTake()`、`xSemaphoreGive()` |
| ISR 釋放 semaphore | `xSemaphoreGiveFromISR()` |
| task notification | `xTaskNotifyGive()`、`ulTaskNotifyTake()` |
| 建立 software timer | `xTimerCreate()`、`xTimerCreateStatic()` |
| 取得剩餘 heap | `xPortGetFreeHeapSize()` |

### 3.2 使用 Arduino SDK wrapper

`Arduino.h` 已經包含 `wiring_os.h`。也可以明確加入：

```cpp
#include <Arduino.h>
#include <wiring_os.h>
```

wrapper 適合只需要 thread、software timer、semaphore 或 signal 的簡單程式：

| 用途 | SDK wrapper |
| --- | --- |
| 建立 thread | `os_thread_create_arduino()` |
| 結束 thread | `os_thread_terminate_arduino()` |
| 讓出 CPU | `os_thread_yield_arduino()` |
| 設定/取得 priority | `os_thread_set_priority_arduino()` / `os_thread_get_priority_arduino()` |
| 建立/啟動/停止 timer | `os_timer_create_arduino()` / `os_timer_start_arduino()` / `os_timer_stop_arduino()` |
| 刪除 timer | `os_timer_delete_arduino()` |
| 建立/取得/釋放 semaphore | `os_semaphore_create_arduino()` / `os_semaphore_wait_arduino()` / `os_semaphore_release_arduino()` |
| 取得剩餘 heap | `os_get_free_heap_size_arduino()` |

wrapper 的 priority 使用 `OS_PRIORITY_*`：

```cpp
OS_PRIORITY_IDLE          // -3
OS_PRIORITY_LOW           // -2
OS_PRIORITY_BELOW_NORMAL  // -1
OS_PRIORITY_NORMAL        //  0
OS_PRIORITY_ABOVENORMAL   // +1
OS_PRIORITY_HIGH          // +2
OS_PRIORITY_REALTIME      // +3
```

直接 FreeRTOS 的 priority 則是 `0` 到 `configMAX_PRIORITIES - 1`。目前設定為 `0` 到 `10`，不要把兩套 priority 數值混用。

## 4. 導入流程

### 4.1 Arduino 專案

1. 在 Arduino IDE 安裝本專案使用的 HUB 8735 board package。
2. 選取 `AmebaPro2 ARM (32-bits) Boards - ideasHatch` 下的 HUB 8735 Ultra 型號。
3. 在 `.ino` 中加入需要的 FreeRTOS header。
4. 在 `setup()` 建立 queue、semaphore、timer 與 application task。
5. 保留 `loop()`，但不要在其中長時間 busy-wait；若沒有工作，使用 `vTaskDelay()` 或 `delay()`。
6. 編譯、燒錄，並從 LOG UART 觀察 task 建立失敗、assert 或 stack overflow 訊息。

### 4.2 不需要做的事

不要在 sketch 中重新：

```cpp
vTaskStartScheduler();
osKernelStart();
```

因為目前 Arduino core 的 `main.cpp` 已經啟動 scheduler。重複啟動可能造成錯誤或讓程式無法正常回到 Arduino 流程。

也不要自行把整個 FreeRTOS source 複製進 `test/` 或 `scr/`。目前 SDK 已提供 kernel、port 與編譯設定。

## 5. 最小可用範例：感測資料送到分析 task

下面範例可放在一個 HUB 8735 Ultra Arduino sketch 中，示範：

- 建立兩個 task
- 用 queue 傳送結構資料
- 用 `pdMS_TO_TICKS()` 做時間轉換
- 使用 `vTaskDelayUntil()` 維持固定週期
- 檢查 queue 與 task 建立是否成功

```cpp
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
            (void)sample;
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
```

實際專案中，`sensorTask` 可負責讀取感測器，`analysisTask` 負責狀態判斷，音訊播放可再由獨立的 `audioTask` 處理。影像 frame、音訊 buffer 或 NN buffer 不適合直接複製進 queue，應傳送指標並明確管理 buffer 的所有權與生命週期。

## 6. Task 的建立與管理

### 6.1 `xTaskCreate()`

```cpp
BaseType_t xTaskCreate(
    TaskFunction_t taskFunction,
    const char *name,
    configSTACK_DEPTH_TYPE stackDepth,
    void *argument,
    UBaseType_t priority,
    TaskHandle_t *taskHandle
);
```

基本規則：

- task function 不應該 return；使用 `for (;;) { ... }`。
- `stackDepth` 是 stack element 數量，不是直接的 bytes。
- 建立後檢查回傳值是否為 `pdPASS`。
- priority 越大，越優先執行；不要讓一般應用 task 任意使用最高 priority。
- task 中的區域變數、函式呼叫鏈與暫存 buffer 都會消耗該 task 的 stack。
- 大型陣列不要放在 task 的區域變數中。

### 6.2 延遲與固定週期

```cpp
vTaskDelay(pdMS_TO_TICKS(100));
```

目前 `FreeRTOSConfig.h` 設定 `configTICK_RATE_HZ` 為 `1000`，通常為 1 ms 一個 tick，但程式仍應使用 `pdMS_TO_TICKS()`，不要依賴硬編碼的 tick 數值。

固定週期工作使用：

```cpp
const TickType_t period = pdMS_TO_TICKS(20);
TickType_t lastWake = xTaskGetTickCount();

for (;;) {
    readSensor();
    vTaskDelayUntil(&lastWake, period);
}
```

`vTaskDelayUntil()` 比在工作結束後直接 `vTaskDelay()` 更適合固定取樣週期，因為它會以原本的週期基準排程。

Arduino 的 `delay(ms)` 會透過 SDK 的 `osDelay()` 進入 RTOS 延遲，因此可在 task 中使用；FreeRTOS 程式碼建議使用 `vTaskDelay(pdMS_TO_TICKS(ms))`，意圖較清楚。`delayMicroseconds()` 是 busy-wait，長時間使用會阻塞目前 task，不能拿來取代 task delay。

### 6.3 Task notification

若只有一個 task 等待一個事件，task notification 通常比建立 binary semaphore 或 queue 更簡單：

```cpp
static TaskHandle_t workerTaskHandle = nullptr;

static void workerTask(void *)
{
    for (;;) {
        // 等待通知；portMAX_DELAY 表示無限期等待。
        uint32_t count = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (count > 0) {
            processOneEvent();
        }
    }
}

static void notifyWorker(void)
{
    xTaskNotifyGive(workerTaskHandle);
}
```

若事件來自 ISR，改用 `vTaskNotifyGiveFromISR()` 或 `xTaskNotifyFromISR()`，並依 ISR 規則呼叫 `portYIELD_FROM_ISR()`。

## 7. Queue、Semaphore 與 Mutex

### 7.1 Queue：傳送資料

```cpp
QueueHandle_t queue = xQueueCreate(8, sizeof(uint32_t));
uint32_t value = 123;

xQueueSend(queue, &value, pdMS_TO_TICKS(10));

uint32_t received;
if (xQueueReceive(queue, &received, portMAX_DELAY) == pdPASS) {
    // 使用 received
}
```

Queue 的 item 是以內容複製方式傳送。小型結構可直接傳送；影像、音訊或大型 NN buffer 建議傳送指標：

```cpp
struct AudioMessage {
    uint8_t *buffer;
    size_t length;
};
```

傳送指標時，必須定義清楚：

- 哪個 task 配置 buffer。
- 哪個 task 可以修改 buffer。
- 哪個 task 負責釋放或歸還 buffer。
- queue 滿時是否丟棄、覆蓋或阻塞。

### 7.2 Binary semaphore：事件同步

```cpp
SemaphoreHandle_t dataReady = xSemaphoreCreateBinary();

// Producer
xSemaphoreGive(dataReady);

// Consumer
if (xSemaphoreTake(dataReady, pdMS_TO_TICKS(100)) == pdTRUE) {
    consumeData();
}
```

binary semaphore 適合「資料準備好了」這類事件通知，不適合保護共享資源。

### 7.3 Mutex：保護共享資源

```cpp
SemaphoreHandle_t i2cMutex = xSemaphoreCreateMutex();

if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    readOledOrSensor();
    xSemaphoreGive(i2cMutex);
}
```

mutex 有 priority inheritance，適合保護 I2C、SPI、共享檔案或其他不可同時使用的資源。不要在取得 mutex 後進行長時間運算、網路等待或播放整段音訊。

### 7.4 ISR 與 queue/semaphore

ISR 只能使用 ISR-safe API，不能使用一般 task API：

```cpp
void sensorIrqHandler(void)
{
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    uint32_t event = 1;

    xQueueSendFromISR(eventQueue, &event, &higherPriorityTaskWoken);
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}
```

ISR 中不要：

- 呼叫 `vTaskDelay()`、`xQueueReceive()` 或會阻塞的 API。
- 取得 mutex。
- 配置/釋放 heap。
- 執行影像辨識、檔案 I/O、網路傳輸或大量 `Serial` 輸出。

ISR 應只清除硬體旗標、保存必要資料，然後透過 `FromISR` API 通知 task。

## 8. Software timer

Software timer 的 callback 是由 FreeRTOS timer service task 執行，不是硬體 ISR：

```cpp
static TimerHandle_t statusTimer = nullptr;
static TaskHandle_t analysisTaskHandle = nullptr;

static void statusTimerCallback(TimerHandle_t timer)
{
    (void)timer;

    // 只做很短的工作，或通知其他 task。
    xTaskNotifyGive(analysisTaskHandle);
}

void setup()
{
    statusTimer = xTimerCreate(
        "status",
        pdMS_TO_TICKS(1000),
        pdTRUE,
        nullptr,
        statusTimerCallback
    );

    if (statusTimer != nullptr) {
        xTimerStart(statusTimer, 0);
    }
}
```

Timer callback 不應：

- 呼叫長時間 blocking API。
- 進行 NN inference 或大型影像處理。
- 等待 queue 或 mutex。
- 執行可能卡住的檔案或網路操作。

正確做法是 callback 只送 queue 或 notification，再讓專用 task 處理真正工作。

## 9. SDK 的 wrapper 使用範例

如果只需要建立簡單背景 thread，可以使用 `wiring_os.h`：

```cpp
#include <Arduino.h>
#include <wiring_os.h>

static uint32_t workerId = 0;

static void worker(const void *argument)
{
    (void)argument;

    for (;;) {
        doSmallJob();
        os_thread_yield_arduino();
    }
}

void setup()
{
    // wiring_os.h 文件將 stack_size 定義為 bytes。
    workerId = os_thread_create_arduino(
        worker,
        nullptr,
        OS_PRIORITY_NORMAL,
        4096
    );

    if (workerId == 0) {
        Serial.println("worker create failed");
    }
}

void loop()
{
    delay(1000);
}
```

wrapper 的限制是沒有直接提供本專案最常用的 queue API；需要傳送結構資料、從 ISR 通知或精細管理 task 時，直接使用 FreeRTOS API。

## 10. 目前 FreeRTOSConfig.h 設定

目前參考設定檔：

```text
external/HUB-8735/amebapro2_arduino/Arduino_package/
  hardware/system/project/realtek_amebapro2_v0_example/inc/FreeRTOSConfig.h
```

主要設定如下：

| 設定 | 目前值 | 作用與建議 |
| --- | --- | --- |
| `configUSE_PREEMPTION` | `1` | 啟用搶佔式排程。一般應保持啟用。 |
| `configUSE_TIME_SLICING` | `1` | 同 priority task 可分時執行。 |
| `configMAX_PRIORITIES` | `11` | 直接 FreeRTOS priority 為 `0` 到 `10`。 |
| `configMINIMAL_STACK_SIZE` | `512` | Idle task 的基準 stack，不是應用 task 建議值。 |
| `configTOTAL_HEAP_SIZE` | `120 * 1024` | FreeRTOS heap 設定；task、queue、timer 的 dynamic allocation 會消耗它。 |
| `configTICK_RATE_HZ` | `1000` | 1 秒 1000 ticks；程式使用 `pdMS_TO_TICKS()`。 |
| `configUSE_TICKLESS_IDLE` | `0` | 目前未啟用 tickless idle；低功耗不能只靠此設定自動完成。 |
| `configUSE_MUTEXES` | `1` | 啟用 mutex。 |
| `configUSE_COUNTING_SEMAPHORES` | `1` | 啟用 counting semaphore。 |
| `configUSE_RECURSIVE_MUTEXES` | `1` | 啟用 recursive mutex。 |
| `configUSE_QUEUE_SETS` | `1` | 啟用 queue set。沒有使用時不必特別依賴它。 |
| `configUSE_TASK_NOTIFICATIONS` | `1` | 啟用 task notification，單一事件同步優先使用它。 |
| `configUSE_NEWLIB_REENTRANT` | `1` | 每個 task 保留 Newlib reentrancy 狀態，會增加 task 記憶體；除非確認 SDK 不需要，否則不要關閉。 |
| `configUSE_TRACE_FACILITY` | `1` | 啟用 task trace 與統計相關功能。 |
| `configUSE_STATS_FORMATTING_FUNCTIONS` | `1` | 可使用 `vTaskList()` 等統計格式化 API。 |
| `configUSE_TIMERS` | `1` | 啟用 software timer。 |
| `configTIMER_TASK_PRIORITY` | `10` | Timer service task priority。callback 必須保持短小。 |
| `configTIMER_QUEUE_LENGTH` | `44` | Timer command queue 長度。 |
| `configTIMER_TASK_STACK_DEPTH` | `1024` | Timer service task stack depth。 |
| `configCHECK_FOR_STACK_OVERFLOW` | `2` | 啟用較完整的 stack overflow 檢查。 |
| `configSUPPORT_STATIC_ALLOCATION` | `1` | 可使用 static task/queue/semaphore/timer API。 |
| `configUSE_MALLOC_FAILED_HOOK` | `0` | 目前沒有啟用 malloc failure hook。需要時要連同 callback 一起實作。 |
| `INCLUDE_uxTaskGetStackHighWaterMark` | `0` | 目前不能直接使用 watermark API；除錯時可改為 `1`。 |
| `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` | `4` | 會呼叫 `FromISR` API 的 ISR，優先權不可高於此限制。 |

### 10.1 不要直接修改外部 Arduino 套件作為長期方案

`Arduino_package` 是 board package 的內容，更新版本時可能被覆蓋。若只是暫時除錯，可以修改參考設定檔；若要長期客製：

1. 將修改記錄在專案文件或 patch。
2. 固定 board package 版本。
3. 或改用可控制 application project 的 AmebaPro2 Standard SDK。

只改 `configTOTAL_HEAP_SIZE` 並不能自動增加實際可用 RAM；還要確認 linker、SDK heap region、DDR 與影像/AI buffer 的配置。

## 11. Stack、Heap 與除錯

### 11.1 讀取 heap

Arduino SDK 提供：

```cpp
size_t freeHeap = os_get_free_heap_size_arduino();
Serial.println((unsigned long)freeHeap);
```

直接 FreeRTOS API 可使用：

```cpp
size_t currentFree = xPortGetFreeHeapSize();
size_t minimumEverFree = xPortGetMinimumEverFreeHeapSize();
```

要在 task 建立前、建立後，以及啟動 camera/AI/audio 功能後各量測一次，才能知道是哪一個模組消耗 heap。

### 11.2 讀取 task stack watermark

目前設定將 `INCLUDE_uxTaskGetStackHighWaterMark` 設為 `0`。若要使用，將它改為 `1`：

```cpp
#define INCLUDE_uxTaskGetStackHighWaterMark 1
```

然後：

```cpp
UBaseType_t unusedStack = uxTaskGetStackHighWaterMark(sensorTaskHandle);
Serial.println((unsigned long)unusedStack);
```

回傳值是 `StackType_t` 數量，不是 bytes。依目前 port，若要換算 bytes：

```cpp
size_t unusedBytes = unusedStack * sizeof(StackType_t);
```

建議在最壞情況下量測，例如 Wi-Fi 傳輸、音訊播放與 NN inference 同時執行時，再決定 stack 是否可縮小。

### 11.3 Stack overflow 與 assert

目前設定已啟用：

```cpp
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configASSERT(x) if ((x) == 0) vAssertCalled(__LINE__, __FILE__)
```

因此遇到 stack overflow 或 assert 時，系統可能停在 assert handler。看到程式突然停止時，先檢查：

- 最近新增的 task stack 是否太小。
- task 是否宣告大型區域陣列。
- 函式呼叫鏈是否變深。
- queue item 是否不小心設成大型影像或音訊結構。
- dynamic heap 是否已不足。

## 12. ISR 優先權設定

目前 `FreeRTOSConfig.h`：

```cpp
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY       15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY  4
```

ARM NVIC 的數值越小代表優先權越高。因此：

- priority `0` 到 `3` 的 ISR 不可以呼叫 FreeRTOS `FromISR` API。
- priority `4` 到 `15` 的 ISR 才能安全呼叫可用的 `FromISR` API。
- ISR 優先權設定要依實際使用的 NVIC API 與 SDK driver 文件確認，不要只看 task priority。

若某個硬體中斷必須使用很高的優先權，該 ISR 就只處理硬體暫存器，透過共享旗標或硬體 ring buffer 交給較低優先權的 task 處理。

## 13. 建議的本專案 task 分工

本專案是感測器、音訊與手機 App 的整合系統，建議採取少量、職責清楚的 task：

| Task | 工作 | 建議同步方式 |
| --- | --- | --- |
| Sensor task | 定期讀取心率、呼吸、活動資料 | `vTaskDelayUntil()`，送 `sensorQueue` |
| Analysis task | 分析狀態、決定音樂/節奏 | 阻塞等待 `sensorQueue` |
| Audio task | 播放或切換音樂 | 阻塞等待 `audioQueue`；保護 I2S/SPI 資源 |
| BLE/communication task | 傳送資料或接收手機命令 | queue 或 task notification |
| UI/OLED task | 更新顯示 | 低頻 timer 或 queue |

HUB 8735 Ultra 的 Wi-Fi、BLE、影像與底層 driver 也會使用 SDK 內部 task。應用程式不要大量建立高 priority task，避免影響系統 driver；除非已量測並確認有必要，應用 task 優先從低到中等 priority 開始。

## 14. 常見錯誤

### 錯誤一：在 `setup()` 呼叫 `vTaskStartScheduler()`

目前 Arduino core 已啟動 scheduler。刪除 sketch 中的 scheduler 啟動呼叫。

### 錯誤二：task function 執行到結尾

錯誤：

```cpp
static void worker(void *)
{
    doWork();
}
```

正確：

```cpp
static void worker(void *)
{
    for (;;) {
        doWork();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

若 task 只需要執行一次，完成後明確呼叫 `vTaskDelete(nullptr)`，不要直接 return。

### 錯誤三：用 busy-wait 做週期工作

錯誤：

```cpp
while (millis() - start < 100) {
}
```

正確：

```cpp
vTaskDelay(pdMS_TO_TICKS(100));
```

### 錯誤四：把 `xTaskCreate()` 和 wrapper 的 stack 數值混用

直接 API 的 stack depth 是 `StackType_t` 數量；`wiring_os.h` 的 `stack_size` 文件定義為 bytes。使用前先確認所呼叫的 API。

### 錯誤五：task priority 設太高

priority 越高不代表程式越快。高 priority task 若沒有阻塞，會壓住 Wi-Fi、BLE、audio 或其他 system task。一般 task 應使用阻塞式 queue、notification 或 delay。

### 錯誤六：從 ISR 呼叫一般 API

將 `xQueueSend()` 改成 `xQueueSendFromISR()`，將 `xSemaphoreGive()` 改成 `xSemaphoreGiveFromISR()`，並處理 `higherPriorityTaskWoken`。

### 錯誤七：queue 傳送區域變數指標

區域變數離開函式後就失效。小資料直接讓 queue 複製內容；大資料則使用有明確所有權規則的 buffer pool。

## 15. 效能與負擔判斷

FreeRTOS 本身的 scheduler 負擔通常不是 HUB 8735 Ultra 的主要瓶頸。更需要注意的是：

- task 數量過多。
- 每個 task 配置過大的 stack。
- 以高頻 tick 或 busy-wait 取代阻塞。
- queue 複製大型 frame 或 audio buffer。
- timer callback 執行長時間工作。
- 不必要地啟用大量 queue、trace 或 debug 功能。

對本專案，建議先使用 FreeRTOS，然後用以下順序量測：

1. 量測啟動後剩餘 heap。
2. 建立應用 task 後再次量測。
3. 啟動 BLE、音訊、攝影機與 AI 功能後再次量測。
4. 啟用 stack watermark，觀察每個 task 的最小剩餘 stack。
5. 只縮減實測有餘裕的 stack，不要憑感覺設定。

## 16. 參考檔案

- `external/HUB-8735/amebapro2_arduino/README.md`
- `external/HUB-8735/amebapro2_arduino/Arduino_package/hardware/cores/ambpro2/main.cpp`
- `external/HUB-8735/amebapro2_arduino/Arduino_package/hardware/cores/ambpro2/wiring_os.h`
- `external/HUB-8735/amebapro2_arduino/Arduino_package/hardware/cores/ambpro2/wiring_os.c`
- `external/HUB-8735/amebapro2_arduino/Arduino_package/hardware/system/project/realtek_amebapro2_v0_example/inc/FreeRTOSConfig.h`
- `test/libraries/DFRobot_MAX98357A/src/DFRobot_MAX98357A.cpp`

本專案已有音訊函式庫直接使用 `xTaskCreate()` 與 `vTaskDelay()`，可作為實際整合時的參考；新增程式時仍建議使用 `pdMS_TO_TICKS()`，讓時間單位清楚且不依賴 tick 設定。
