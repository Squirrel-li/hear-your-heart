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
extern QueueHandle_t gSensorQueue;		// G-sensor data queue
extern QueueHandle_t audioCmdQueue;		// Audio command queue
extern QueueHandle_t uiUpdateQueue;		// UI update queue
extern QueueHandle_t bleTxQueue;		// BLE transmission queue

bool appQueuesInit();

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

extern UserInfo userInfo;		// BLE transmission queue

// Big-endian format

// BLE frame data
// | length | data |
// | 2B     | nB   |

// data
// | device_timestamp | sensorId | data |
// | device_timestamp | sensorId | data |
// | 4B               | 1B       | nB   |
struct BleFrameElement {
    uint32_t receive_timestamp;
    uint16_t length;
    uint8_t data[BLE_FRAME_MAX];
};

// PPG 感測資料
// | sequence | status | SpO2 | heartRateBpm | bpi | hrv |
// | 4B       | 1B     | 1B   | 1B           | 1B  | 1B  |
// | pulseAmplitude | systoleDuration | ppg1High | ppg1Low | ppg2High | ppg2Low |
// | 1B             | 1B              | 1B       | 1B      | 1B       | 1B      |
struct PpgSensorElement {
    uint32_t receive_timestamp;
    uint32_t device_timestamp;
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

// G sensor 三軸資料
// | sequence | acc_x | acc_y | acc_z | gyro_x | gyro_y | gyro_z |
// | 4B       | 2B    | 2B    | 2B    | 2B     | 2B     | 2B     |
struct GSensorElement {
    uint32_t receive_timestamp;
    uint32_t device_timestamp;
    uint32_t sequence;
    int16_t acc_x;
    int16_t acc_y;
    int16_t acc_z;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
};

enum class AudioCommandType : uint8_t {
    Play,
    Stop,
    Pause,
    Resume,
    SetVolume
};

struct AudioCmdElement {
    AudioCommandType command;
    uint8_t volumePercent;
    char filename[AUDIO_FILENAME_MAX];
};

// UI 更新資料
enum class UiUpdateType : uint8_t {
    Sensor,
    Analysis,
    Audio,
    Ble,
    Error
};

struct UiUpdateElement {
    UiUpdateType type;
    uint32_t timestampMs;
    uint16_t heartRateBpm;
    uint8_t activityLevel;
    uint8_t volumePercent;
    uint8_t isPlaying;
    char filename[AUDIO_FILENAME_MAX];
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