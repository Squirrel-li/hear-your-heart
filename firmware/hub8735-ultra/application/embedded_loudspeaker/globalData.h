#ifndef GOLOBAL_DATA_H
#define GOLOBAL_DATA_H

#include <FreeRTOS.h>
#include <queue.h>

#include <Arduino.h>
#include <stdint.h>

constexpr uint16_t BLE_FRAME_MAX = 100;
constexpr uint16_t AUDIO_FILENAME_MAX = 64;
constexpr uint16_t LOG_MESSAGE_MAX = 96;

extern QueueHandle_t bleRxFrameQueue;	// BLE notification callback queue
extern QueueHandle_t ppgSensorQueue;	// PPG sensor data queue
extern QueueHandle_t ppgRawQueue;	// Raw PPG samples (msgType 0x05) for the HUB heart-rate algorithm
extern QueueHandle_t accSensorQueue;	// Accelerometer data queue
extern QueueHandle_t gyroSensorQueue;	// Gyroscope data queue
extern QueueHandle_t targetBpmResultQueue;	// Latest analysis result for controlTask
extern QueueHandle_t motionLevelQueue;	// Latest confirmed motion level (uint8_t, see motion_level.h) for controlTask
extern QueueHandle_t audioCmdQueue;		// Audio command queue
extern QueueHandle_t audioPlaybackEventQueue;	// Playback events for controlTask
extern QueueHandle_t uiUpdateQueue;		// UI update queue
extern QueueHandle_t uiCmdQueue;		// Menu commands from UITask to controlTask
extern QueueHandle_t analysisCmdQueue;	// Commands from controlTask to analysisTask
extern QueueHandle_t bleTxQueue;		// BLE transmission queue
extern QueueHandle_t bleCmdQueue;		// Connection commands from UITask to BLETask

bool appQueuesInit();

// 啟動流程：audioTask 先初始化 SD 並載入 musicLibrary，之後才設為 true；
// controlTask 等到 true 才開始選曲，避免讀到載入中的曲目表。
extern volatile bool musicLibraryReady;
extern volatile bool musicStorageReady;
extern volatile bool musicBpmAnalysisBusy;
// Software standby ("Power off"): written only by controlTask; BLETask and analysisTask
// read it to drop the link / power down local sensors.
extern volatile bool systemStandby;

struct UserInfo {
    int age;
    float weight;
    float height;
    float bmi() const {
        return weight / ((height / 100.0f) * (height / 100.0f));
    }
    float HRmax() const {
        return 206.9f - (0.67 * age);
    }
};

extern UserInfo userInfo;

// 感測資料來源。BLE frame 的 msgType byte：高 4 bit = source，低 4 bit = msgType。
//   Chest (0): 胸帶，廣播名稱 BLE_sensor_ITAP
//   Watch (1): 手錶（ble_sensor_hr），廣播名稱 BLE_watch_ITAP；兩台可同時連線
//   Local (2): 直接接在 Hub 上的 BMH08002-4 + LSM6DS3；不經 BLE，不會出現在 frame 裡
enum class SensorSource : uint8_t {
    Chest = 0,
    Watch = 1,
    Local = 2
};

// 目前選用的來源：analysisTask 寫入，BLETask 讀取（BLE 端只收與選擇相同 source 的 frame）。
extern volatile SensorSource activePpgSource;
extern volatile SensorSource activeImuSource;

// Big-endian format

// BLE frame (one FFF1 notify, no length prefix; the notify carries the length)
// | sensor_time_ms | msgType | payload |
// | 4B             | 1B      | nB      |
struct BleFrameElement {
    uint32_t hub_rx_time_ms;
    uint16_t length;
    uint8_t data[BLE_FRAME_MAX];
};

// PPG 感測資料 payload (msgType 0x01), frame = 20B
// | sequence | status | SpO2 | heartRateBpm | bpi | hrv |
// | 4B       | 1B     | 1B   | 1B           | 1B  | 1B  |
// | pulseAmplitude | systoleDuration | ppg1High | ppg1Low | ppg2High | ppg2Low |
// | 1B             | 1B              | 1B       | 1B      | 1B       | 1B      |
struct PpgSensorElement {
    uint32_t hub_rx_time_ms;
    uint32_t sensor_time_ms;
    uint32_t sequence;
    uint8_t status; // 0: finger off, 1: finger on, 2: get data
    uint8_t SpO2;   // Blood Oxygen Saturation in percentage
    uint8_t heartRateBpm;   // Heart Rate in BPM
    uint8_t bpi;   // Blood Perfusion Index
    uint8_t hrv;   // Heart Rate Variability
    uint8_t pulseAmplitude; // Pulse Amplitude
    uint8_t systoleDuration; // Signal Strength
    uint16_t ppg1;   // PPG1
    uint16_t ppg2;    // PPG2
};

// Acc 三軸資料
// msgType 0x03 (acc only, 15B) 或 0x02 的前半 (acc+gyro 合併, 21B)
// | sequence | acc_x | acc_y | acc_z |
// | 4B       | 2B    | 2B    | 2B    |
struct AccElement {
    uint32_t hub_rx_time_ms;
    uint32_t sensor_time_ms;
    uint32_t sequence;
    int16_t acc_x;
    int16_t acc_y;
    int16_t acc_z;
};

// Gyro 三軸資料
// msgType 0x04 (gyro only, 15B) 或 0x02 的後半 (acc+gyro 合併, 21B)
// | sequence | gyro_x | gyro_y | gyro_z |
// | 4B       | 2B     | 2B     | 2B     |
struct GyroElement {
    uint32_t hub_rx_time_ms;
    uint32_t sensor_time_ms;
    uint32_t sequence;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
};

// One sample of a PPG raw batch (msgType 0x05), sensor_time_ms already rebuilt for this sample.
struct PpgRawElement {
    uint32_t hub_rx_time_ms;
    uint32_t sensor_time_ms;
    uint32_t sequence;
    uint8_t status;
    uint16_t ppg1;
    uint16_t ppg2;
};

struct TargetBpmResult {
    uint16_t targetBpm;
    uint8_t zone;
};

// controlTask -> analysisTask 的指令
enum class AnalysisCmdType : uint8_t {
    SetPpgSource,
    SetMotionSource  // acc + gyro
};

struct AnalysisCmdElement {
    AnalysisCmdType command;
    SensorSource source;
};

enum class AudioPlaybackEventType : uint8_t {
    Finished,
    Failed
};

struct AudioPlaybackEvent {
    AudioPlaybackEventType type;
    int16_t songIndex;
};

enum class AudioCommandType : uint8_t {
    Play,
    Stop,
    Pause,
    Resume,
    SetVolume,
    AnalyzeNewOrUnknownBpm,
    ReanalyzeAllBpm,
    CancelBpmAnalysis
};

struct AudioCmdElement {
    AudioCommandType command;
    uint8_t volumePercent;
    uint8_t isAutomatic;
    int16_t songIndex;
    char filename[AUDIO_FILENAME_MAX];
};

// UI 更新資料
enum class UiUpdateType : uint8_t {
    Sensor,
    Analysis,
    Audio,
    Ble,
    Error,
    Acc,
    Gyro,
    Mode,
    BpmAnalysis,
    Train,
    Motion  // Demo motion level in activityLevel (MOTION_LEVEL_NONE = no data)
};

// Enjoy: automatic, heart-rate driven selection.
// Train: automatic, follows a built-in plan (train_plan.h) by time; heart rate is only shown.
// Demo is automatic too, but driven by the motion level and plays /demo/DEMO_nn.mp3 instead of the library.
enum class PlayMode : uint8_t {
    Enjoy,
    Train,
    Manual,
    Demo
};

enum class AudioState : uint8_t {
    Stopped,
    Playing,
    Paused
};

// Sensor: heartRateBpm/spo2/sensorStatus; Ble: bleLink/bleAuto; Acc: acc; Gyro: gyro;
// Audio: audioState and, when a new file starts, filename; Mode: mode;
// Train: trainPlan, trainStage (TRAIN_STAGE_DONE when finished), trainRemainingS;
// Analysis: targetBpm, zone (heart-rate target used by Enjoy); Motion: activityLevel
enum class BpmAnalysisMode : uint8_t {
    NewOrUnknown,
    All
};

enum class BpmAnalysisState : uint8_t {
    Idle,
    Queued,
    Running,
    Complete,
    Failed,
    Cancelled
};

struct UiUpdateElement {
    UiUpdateType type;
    uint32_t timestampMs;
    uint16_t heartRateBpm;
    uint8_t activityLevel;
    uint8_t volumePercent;
    uint8_t isPlaying;
    uint8_t spo2;
    uint8_t sensorStatus;
    uint8_t bleLink[2];  // BleLinkState, index = SensorSource (Chest, Watch)
    uint8_t bleAuto[2];  // 1 = Auto, 0 = Manual
    uint8_t mode;        // PlayMode
    uint8_t audioState;  // AudioState
    BpmAnalysisState bpmAnalysisState;
    BpmAnalysisMode bpmAnalysisMode;
    uint16_t bpmAnalysisCurrent;
    uint16_t bpmAnalysisTotal;
    uint16_t bpmAnalysisSucceeded;
    uint16_t bpmAnalysisFailed;
    int16_t acc[3];
    int16_t gyro[3];
    uint16_t targetBpm;
    uint8_t zone;
    uint8_t trainPlan;
    uint8_t trainStage;
    uint16_t trainRemainingS;
    char filename[AUDIO_FILENAME_MAX];
};

// BLE link per device. Auto retries until connected and switches to Manual when the link drops;
// Manual scans only after a Connect command.
enum class BleLinkState : uint8_t {
    Down,
    Connecting,
    Up
};

enum class BleCommandType : uint8_t {
    Connect,     // scan + connect, up to 3 tries
    Disconnect,  // also switches to Manual
    SetAuto,
    SetManual
};

struct BleCmdElement {
    BleCommandType command;
    SensorSource device;  // Chest or Watch
};

// Menu command from UITask; controlTask applies the manual-control rules.
enum class UiCommandType : uint8_t {
    SetMode,
    Pause,
    Resume,
    Stop,
    SetVolume,
    AnalyzeNewOrUnknownBpm,
    ReanalyzeAllBpm,
    CancelBpmAnalysis,
    PlaySong,
    SetPpgSource,
    SetImuSource,
    PowerOff,  // enter software standby
    PowerOn,   // leave standby; playback stays stopped
    RestartTrain  // restart the current Train plan from stage 1
};

struct UiCommandElement {
    UiCommandType command;
    uint8_t volumePercent;
    PlayMode mode;
    uint8_t trainPlan;  // SetMode Train: index into TRAIN_PLANS
    int16_t songIndex;  // PlaySong: musicLibrary index
    SensorSource source;  // SetPpgSource / SetImuSource
};

// Log 資料
enum class LogLevel : uint8_t {
    Debug,
    Info,
    Warning,
    Error
};

struct LogElement {
    uint32_t timestampMs;
    LogLevel level;
    char message[LOG_MESSAGE_MAX];
};

enum class ControlCommandType : uint8_t {
    Start,
    Stop,
    SetSampleRate,
    SetSensorMask,
    PlayAudio
};

struct ControlCmdElement {
    ControlCommandType command;
    uint32_t value;
    char text[64];
};

#endif // GOLOBAL_DATA_H