#include "localSensor.h"

// Arduino.h 要在 Wire.h 之前，否則 Wire.h 帶入的 rt_printf.h 巨集會與 Arduino.h 的 dbg_printf 宣告衝突。
#include <Arduino.h>
#include <Wire.h>

#include "globalData.h"
#include "debug.h"

namespace {

constexpr const char* TAG = "LOCAL";

// ---- BMH08002-4 ----
constexpr uint8_t PPG_EN_PIN = 7;  // D7
constexpr uint32_t PPG_BAUD = 38400;
constexpr uint32_t PPG_BOOT_MS = 200;  // ponytail: EN -> module ready time is not documented; 200 ms is a guess, check on the bench.
constexpr uint32_t PPG_REPLY_MS = 100;

// 定時傳送：模組自己算心率，每個間隔送一個 frame（EEPROM 0x0A，單位 4 ms）。
constexpr uint8_t EE_MODE = 0x01;
constexpr uint16_t MODE_TIMED = 0x0000;
constexpr uint8_t EE_INTERVAL = 0x0A;
constexpr uint16_t INTERVAL_UNITS = 125;  // 125 x 4 ms = 500 ms

constexpr uint8_t OP_START_STOP = 0x01;  // data 0 = start, 1 = stop
constexpr uint8_t OP_EE_WRITE = 0x04;    // 寫入需 50 ms
constexpr uint8_t OP_EE_READ = 0x05;

bool ppgOn = false;

// 指令 frame：55 B1 op addr hi lo CS AA，CS = sum(B1..lo) & 0xFF。
void ppgSend(uint8_t op, uint8_t addr, uint16_t value)
{
    uint8_t f[8] = {0x55, 0xB1, op, addr, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value), 0, 0xAA};
    uint8_t sum = 0;
    for (uint8_t i = 1; i <= 5; i++) sum += f[i];
    f[6] = sum;
    Serial3.write(f, 8);
}

// 讀 EEPROM，回覆為 55 B1 05 addr hi lo CS AA；串流中會夾雜 55 B0 frame，逐 byte 找 header。
bool ppgEeRead(uint8_t addr, uint16_t* out)
{
    while (Serial3.available()) Serial3.read();
    ppgSend(OP_EE_READ, addr, 0);
    const uint8_t hdr[4] = {0x55, 0xB1, OP_EE_READ, addr};
    uint8_t st = 0, hi = 0, lo = 0;
    const uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < PPG_REPLY_MS) {
        if (!Serial3.available()) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        const uint8_t c = Serial3.read();
        if (st < 4)       st = (c == hdr[st]) ? st + 1 : (c == 0x55 ? 1 : 0);
        else if (st == 4) { hi = c; st = 5; }
        else if (st == 5) { lo = c; st = 6; }
        else if (st == 6) st = (c == static_cast<uint8_t>(0xB1 + OP_EE_READ + addr + hi + lo)) ? 7 : 0;
        else if (c == 0xAA) {
            *out = static_cast<uint16_t>((hi << 8) | lo);
            return true;
        } else st = 0;
    }
    return false;
}

// 值不同才寫入（EEPROM 有寫入壽命），寫完讀回確認。回傳 true 表示 EEPROM 已是目標值。
bool ppgEeEnsure(uint8_t addr, uint16_t value, bool* wrote)
{
    uint16_t cur = 0;
    if (ppgEeRead(addr, &cur) && cur == value) return true;
    ppgSend(OP_EE_WRITE, addr, value);
    vTaskDelay(pdMS_TO_TICKS(60));
    *wrote = true;
    return ppgEeRead(addr, &cur) && cur == value;
}

// 模組 frame（15 B）：55 B0 ST D0..D9 CS AA，CS = sum(B0, ST, D0..D9) & 0xFF。
// 欄位對應與 BLE PPG frame 相同（皆 Big-endian）：[2]=status, [3]=SpO2, [4]=模組心率, [5]=bpi, [6]=hrv,
// [7]=pulseAmplitude, [8]=systoleDuration, [9..10]=ppg1, [11..12]=ppg2。
constexpr uint8_t PPG_FRAME_LEN = 15;

uint8_t ppgFrame[PPG_FRAME_LEN];
uint8_t ppgIdx = 0;
uint32_t ppgSeq = 0;

void postUi(const UiUpdateElement& update)
{
    if (uiUpdateQueue != nullptr) xQueueSend(uiUpdateQueue, &update, 0);
}

// 每個有效 frame 送出一筆 PPG 樣本；心率 0 或 status != 2 由 analysisTask 判為無效，與 BLE 胸帶相同。
void ppgOnFrame()
{
    const uint32_t now = millis();
    PpgSensorElement e{};
    e.hub_rx_time_ms = now;
    e.sensor_time_ms = now;
    e.sequence = ppgSeq++;
    e.status = ppgFrame[2];
    e.SpO2 = ppgFrame[3];
    e.heartRateBpm = ppgFrame[4];
    e.bpi = ppgFrame[5];
    e.hrv = ppgFrame[6];
    e.pulseAmplitude = ppgFrame[7];
    e.systoleDuration = ppgFrame[8];
    e.ppg1 = static_cast<uint16_t>((ppgFrame[9] << 8) | ppgFrame[10]);
    e.ppg2 = static_cast<uint16_t>((ppgFrame[11] << 8) | ppgFrame[12]);
    LOGD(TAG, "PPG HR=%u SpO2=%u status=%u", static_cast<unsigned>(e.heartRateBpm), static_cast<unsigned>(e.SpO2),
         static_cast<unsigned>(e.status));
    if (xQueueSend(ppgSensorQueue, &e, 0) != pdPASS) LOGW(TAG, "PPG queue full, sample dropped");
    UiUpdateElement ui{};
    ui.type = UiUpdateType::Sensor;
    ui.timestampMs = now;
    ui.heartRateBpm = e.heartRateBpm;
    ui.spo2 = e.SpO2;
    ui.sensorStatus = e.status;
    postUi(ui);
}

// 逐 byte 同步並驗證 frame。
void ppgPoll()
{
    while (Serial3.available()) {
        const uint8_t c = Serial3.read();
        if (ppgIdx == 0) {
            if (c == 0x55) ppgFrame[ppgIdx++] = c;
            continue;
        }
        if (ppgIdx == 1) {
            if (c == 0xB0) ppgFrame[ppgIdx++] = c;
            else if (c != 0x55) ppgIdx = 0;  // 55 55 B0：停在第二個 55
            continue;
        }
        ppgFrame[ppgIdx++] = c;
        if (ppgIdx < PPG_FRAME_LEN) continue;
        ppgIdx = 0;
        uint8_t sum = 0;
        for (uint8_t i = 1; i <= 12; i++) sum += ppgFrame[i];
        if (sum == ppgFrame[13] && ppgFrame[14] == 0xAA) ppgOnFrame();
    }
}

// ---- LSM6DS3 ----
constexpr uint8_t IMU_ADDR = 0x6B;  // SA0 = VCC
constexpr uint8_t LSM_WHO_AM_I = 0x0F;
constexpr uint8_t LSM_CTRL1_XL = 0x10;
constexpr uint8_t LSM_CTRL2_G = 0x11;
constexpr uint8_t LSM_CTRL3_C = 0x12;
constexpr uint8_t LSM_CTRL4_C = 0x13;
constexpr uint8_t LSM_CTRL7_G = 0x16;
constexpr uint8_t LSM_CTRL8_XL = 0x17;
constexpr uint8_t LSM_TAP_CFG = 0x58;

bool imuOn = false;

// 與 SDK 的 I2Cdev 相同：先送暫存器位址並 STOP，再另開一次讀取（不用 repeated start，
// 這個 Wire 實作的 endTransmission(false) 在沒有 TX 完成回呼時會回傳錯誤）。
bool imuReadBlock(uint8_t reg, uint8_t* buf, uint8_t n)
{
    Wire1.beginTransmission(IMU_ADDR);
    Wire1.write(reg);
    if (Wire1.endTransmission() != 0) return false;  // 位址沒有 ACK
    Wire1.requestFrom(IMU_ADDR, n);
    vTaskDelay(pdMS_TO_TICKS(2));
    for (uint8_t i = 0; i < n; i++) {
        if (!Wire1.available()) return false;
        buf[i] = Wire1.read();
    }
    return true;
}

constexpr uint8_t LSM_OUTX_L_G = 0x22;  // 陀螺儀 XYZ 接著加速度計 XYZ，little-endian
// 跟著 ODR 12.5 Hz 連續讀，供 DEMO 運動強度使用（N76E003 仍為 1 Hz）；UI 與 log 維持 1 Hz。
constexpr uint32_t IMU_PERIOD_MS = 80;
constexpr uint32_t IMU_REPORT_MS = 1000;
uint32_t imuLastMs = 0;
uint32_t imuReportMs = 0;
uint32_t imuSeq = 0;

int16_t le16(const uint8_t* p)
{
    return static_cast<int16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

void imuPoll()
{
    const uint32_t now = millis();
    if ((uint32_t)(now - imuLastMs) < IMU_PERIOD_MS) return;
    imuLastMs = now;

    uint8_t raw[12];
    if (!imuReadBlock(LSM_OUTX_L_G, raw, 12)) {
        LOGW(TAG, "LSM6DS3 read failed");
        return;
    }
    const bool report = (uint32_t)(now - imuReportMs) >= IMU_REPORT_MS;
    if (report) {
        imuReportMs = now;
        LOGD(TAG, "IMU acc=%d,%d,%d gyro=%d,%d,%d", le16(&raw[6]), le16(&raw[8]), le16(&raw[10]), le16(&raw[0]),
             le16(&raw[2]), le16(&raw[4]));
    }

    AccElement acc{};
    acc.hub_rx_time_ms = now;
    acc.sensor_time_ms = now;
    acc.sequence = imuSeq;
    acc.acc_x = le16(&raw[6]);
    acc.acc_y = le16(&raw[8]);
    acc.acc_z = le16(&raw[10]);
    GyroElement gyro{};
    gyro.hub_rx_time_ms = now;
    gyro.sensor_time_ms = now;
    gyro.sequence = imuSeq++;
    gyro.gyro_x = le16(&raw[0]);
    gyro.gyro_y = le16(&raw[2]);
    gyro.gyro_z = le16(&raw[4]);
    if (xQueueSend(accSensorQueue, &acc, 0) != pdPASS) LOGW(TAG, "Acc queue full, sample dropped");
    if (xQueueSend(gyroSensorQueue, &gyro, 0) != pdPASS) LOGW(TAG, "Gyro queue full, sample dropped");
    if (!report) return;

    UiUpdateElement ui{};
    ui.type = UiUpdateType::Acc;
    ui.timestampMs = now;
    ui.acc[0] = acc.acc_x;
    ui.acc[1] = acc.acc_y;
    ui.acc[2] = acc.acc_z;
    postUi(ui);
    ui = UiUpdateElement{};
    ui.type = UiUpdateType::Gyro;
    ui.timestampMs = now;
    ui.gyro[0] = gyro.gyro_x;
    ui.gyro[1] = gyro.gyro_y;
    ui.gyro[2] = gyro.gyro_z;
    postUi(ui);
}

bool imuWrite(uint8_t reg, uint8_t val)
{
    Wire1.beginTransmission(IMU_ADDR);
    Wire1.write(reg);
    Wire1.write(val);
    return Wire1.endTransmission() == 0;
}

bool imuReadReg(uint8_t reg, uint8_t* out)
{
    return imuReadBlock(reg, out, 1);
}

} // namespace

void localSensorInit()
{
    pinMode(PPG_EN_PIN, OUTPUT);
    digitalWrite(PPG_EN_PIN, LOW);
    ppgOn = false;
    // 匯流排只開一次；停用只靠 EN 腳（PPG）與輸出頻率歸零（IMU）。
    Serial3.begin(PPG_BAUD);
    Wire1.begin();
    imuWrite(LSM_CTRL1_XL, 0x00);  // 開機後的 LSM6DS3 可能還在上次的設定，先 power-down
    imuWrite(LSM_CTRL2_G, 0x00);
    imuOn = false;
}

bool localPpgSetEnabled(bool enabled)
{
    if (enabled == ppgOn) return true;
    if (!enabled) {
        digitalWrite(PPG_EN_PIN, LOW);
        ppgOn = false;
        LOGI(TAG, "BMH08002-4 disabled (EN low)");
        return true;
    }

    digitalWrite(PPG_EN_PIN, HIGH);
    vTaskDelay(pdMS_TO_TICKS(PPG_BOOT_MS));
    // 模式與傳送間隔不對才寫入；有寫入時先 stop，再 start 讓新設定生效。
    bool wrote = false;
    const bool ok = ppgEeEnsure(EE_MODE, MODE_TIMED, &wrote) && ppgEeEnsure(EE_INTERVAL, INTERVAL_UNITS, &wrote);
    if (!ok) {
        LOGE(TAG, "BMH08002-4 config not confirmed (mode 0x%02X, interval %u x 4 ms); check EN (D7) and UART3 wiring",
             MODE_TIMED, static_cast<unsigned>(INTERVAL_UNITS));
        digitalWrite(PPG_EN_PIN, LOW);
        return false;
    }
    if (wrote) {
        ppgSend(OP_START_STOP, 0, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ppgSend(OP_START_STOP, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    while (Serial3.available()) Serial3.read();  // 丟掉指令回覆，之後從乾淨狀態解析
    ppgIdx = 0;
    ppgOn = true;
    LOGI(TAG, "BMH08002-4 enabled (EN high, timed upload every 500 ms)");
    return true;
}

bool localImuSetEnabled(bool enabled)
{
    if (enabled == imuOn) return true;
    if (!enabled) {
        // 加速度計與陀螺儀輸出頻率歸零 = power-down。
        imuWrite(LSM_CTRL1_XL, 0x00);
        imuWrite(LSM_CTRL2_G, 0x00);
        imuOn = false;
        LOGI(TAG, "LSM6DS3 powered down");
        return true;
    }

    uint8_t id = 0;
    const bool ok = imuReadReg(LSM_WHO_AM_I, &id) && (id == 0x69 || id == 0x6A) &&
        imuWrite(LSM_CTRL3_C, 0x44) &&    // BDU, auto-increment
        imuWrite(LSM_CTRL4_C, 0x80) &&    // XL_BW_SCAL_ODR
        imuWrite(LSM_TAP_CFG, 0x10) &&    // SLOPE_FDS: 啟用加速度計高通／低通濾波
        imuWrite(LSM_CTRL8_XL, 0x24) &&   // HP_SLOPE_XL_EN, 高通 ODR/100（去除重力）
        imuWrite(LSM_CTRL7_G, 0x50) &&    // HP_G_EN, 陀螺儀高通（去除零點偏移）
        imuWrite(LSM_CTRL1_XL, 0x1F) &&   // 12.5 Hz, ±8 g, anti-alias 50 Hz
        imuWrite(LSM_CTRL2_G, 0x18);      // 12.5 Hz, ±1000 dps
    if (!ok) {
        LOGE(TAG, "LSM6DS3 init failed (WHO_AM_I=0x%02X); check Wire1 D0/D1 wiring", id);
        imuWrite(LSM_CTRL1_XL, 0x00);
        imuWrite(LSM_CTRL2_G, 0x00);
        return false;
    }
    imuLastMs = millis();
    imuOn = true;
    LOGI(TAG, "LSM6DS3 enabled (WHO_AM_I=0x%02X, 12.5 Hz)", id);
    return true;
}

void localSensorPoll()
{
    if (ppgOn) ppgPoll();
    if (imuOn) imuPoll();
}
