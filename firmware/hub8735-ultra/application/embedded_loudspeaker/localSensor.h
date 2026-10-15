#ifndef LOCAL_SENSOR_H
#define LOCAL_SENSOR_H

#include <stdint.h>

// 直接接在 Hub 上的感測器電源／初始化控制（只在 analysisTask 內呼叫，會阻塞約 0.3 秒）。
//   BMH08002-4：UART3 (D6 TX / D5 RX) 38400，EN 腳接 D7
//   LSM6DS3   ：Wire1 (D0 SDA / D1 SCL)，7-bit 位址 0x6B

// 開機時呼叫一次：開 UART3 與 Wire1（之後不再關閉），EN 拉低、LSM6DS3 power-down。
void localSensorInit();

// 啟用：回傳 true 表示已初始化成功；失敗時已恢復為關閉狀態。
// PPG 的啟停只動 EN 腳；IMU 的啟停只改輸出頻率。
// 停用：回傳 true。重複呼叫同一狀態直接回傳 true。
bool localPpgSetEnabled(bool enabled);
bool localImuSetEnabled(bool enabled);

// 每圈由 analysisTask 呼叫：只處理已啟用的那一路，把樣本送進 ppg/acc/gyroSensorQueue 並更新 UI。
void localSensorPoll();

#endif
