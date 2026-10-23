# BLE 通訊協定（sensor ↔ hub8735）

> 2026-10-18 核對：以下保留協定細節，部分連線描述未同步：ble_sensor 與 ble_sensor_hr 都是手錶，廣播 BLE_watch_ITAP、source=1；BLE_sensor_ITAP 是外部綁帶。HUB 有雙服務探索及各連線 Auto／Manual，不應依舊描述假定只有單一裝置或永遠自動重連。 現況見[架構核對](../../docs/ARCHITECTURE.md)。

對照：`hub8735-ultra/application/embedded_loudspeaker/BLETask.{h,cpp}`、`globalData.h`、`n76e003/application/ble_sensor/source/main.c`
更新：2026-10-11

## 1. 角色與 GATT

- sensor = Peripheral（GATT Server）；hub8735 = Central（GATT Client）。
- 廣播名稱：`BLE_sensor_ITAP`（hub 只以名稱完全相等比對）。
- 不配對、不加密；所有屬性都不得要求安全性。

| UUID     | 類型           | 屬性                           | 方向          | 內容                                        |
| -------- | -------------- | ------------------------------ | ------------- | ------------------------------------------- |
| `FFF0` | Service        | —                             | —            | 本協定唯一服務                              |
| `FFF1` | Characteristic | Notify（含 CCCD`0x2902`）    | sensor → hub | Frame（第 3 節）：PPG / Acc / Gyro / cmd 回應 |
| `FFF2` | Characteristic | Write / Write Without Response | hub → sensor | cmd 請求（第 6 節，不套用 Frame）           |

## 2. 連線流程

UUID 雖已事先約定，hub 仍須做服務探索來取得 handle，並寫入 CCCD 開啟 notify；任何一步失敗，hub 會斷線並每 3 s 重試。

1. sensor 廣播 → hub 掃描（2 s）找到名稱。
2. hub 連線（逾時 3 s）。
3. hub 探索服務（逾時 10 s），依序找 `FFF0` → `FFF1`（必要），以及 `FFF2`（選用）。
4. hub 將 `FFF1` 的 CCCD 寫成 `0x0001`，notify 才算開啟。
5. sensor **確認 CCCD 已開啟後**才送 notify；之後每筆紀錄送一個 notify。
6. 斷線後 sensor 清除 notify 狀態並重新廣播。

## 3. Frame

- 只用於 FFF1：一個 notify = 一個 frame = 一筆紀錄。
- 不加 length 欄位，長度由 BLE 封包本身決定；上限 `BLE_FRAME_MAX = 100`。
- 多位元組欄位一律 **Big-endian**。

```
| sensor_time_ms | msgType | payload |
| 4B               | 1B       | nB      |
```

| 欄位                 | 型別   | 說明                                                         |
| -------------------- | ------ | ------------------------------------------------------------ |
| `sensor_time_ms` | uint32 | sensor 時鐘，資料產生（取樣）時刻，不是送出時刻；1 ms，約 49.7 天歸零，長時間會漂移 |
| `msgType`         | uint8  | 高 4 bit = source，低 4 bit = 類型（`0x00` cmd、`0x01` PPG、`0x02` Acc+Gyro、`0x03` Acc、`0x04` Gyro、`0x05` PPG 原始批次，與hub `BLETask.cpp` 一致）。source：`0` 胸帶（現有 sensor，不另外處理）、`1` 手錶端（保留）；`2` 為 Hub 本地直接接的 BMH08002-4 + LSM6DS3，不經 BLE，不會出現在 frame 裡 |
| `payload`          | —     | 依 msgType 決定（第 4–6、4.1 節）                             |

hub 收到時另記 `hub_rx_time_ms`（hub 時鐘，不在空中傳送）。兩者來自不同時鐘，不能直接相減；對時以 `hub_rx_time_ms` 校正。

| msgType | 類型                | payload | frame 總長 | 方向 / 特徵                              |
| -------- | ------------------- | ------- | ---------- | ---------------------------------------- |
| `0x00` | cmd 回應            | 3–15B   | 8–20B      | sensor → hub：FFF1（第 6 節）           |
| `0x01` | PPG                 | 15B     | 20B        | sensor → hub：FFF1                      |
| `0x02` | Acc+Gyro（LSM6DS3） | 16B     | 21B        | sensor → hub：FFF1（`ble_sensor`、`ble_sensor_hr` 使用，需 MTU ≥ 24） |
| `0x03` | Acc（LSM6DS3）      | 10B     | 15B        | sensor → hub：FFF1（協定保留，現有韌體不送） |
| `0x04` | Gyro（LSM6DS3）     | 10B     | 15B        | sensor → hub：FFF1（協定保留，現有韌體不送） |
| `0x05` | PPG 原始批次        | 5+4N B  | 10+4N B    | sensor → hub：FFF1（`ble_sensor_hr` 每 N=2 筆送一次，18B；第 4.1 節） |

## 4. Payload：PPG（0x01，15B）→ `PpgSensorElement`

| 偏移 | 欄位                | 型別   | 說明                                      |
| ---- | ------------------- | ------ | ----------------------------------------- |
| 0    | `sequence`        | uint32 | PPG 計數，從 0 起；跳號 = sensor 端丟樣本 |
| 4    | `status`          | uint8  | 0 finger off、1 finger on、2 get data     |
| 5    | `SpO2`            | uint8  | 血氧 %                                    |
| 6    | `heartRateBpm`    | uint8  | 心率 BPM                                  |
| 7    | `bpi`             | uint8  | 血流灌注指數                              |
| 8    | `hrv`             | uint8  | 心率變異                                  |
| 9    | `pulseAmplitude`  | uint8  | 脈搏振幅                                  |
| 10   | `systoleDuration` | uint8  | 收縮期時間                                |
| 11   | `ppg1`            | uint16 |                                           |
| 13   | `ppg2`            | uint16 |                                           |

偏移 4–14 原樣對應 PPG 模組 frame 的 `ST, D0..D9`。

## 4.1 Payload：PPG 原始批次（0x05，5+4N B）

一個 frame 帶 N 筆連續的 `ppg1`／`ppg2` 原始值，供 hub 自行分析心率。N 不另設欄位，由長度決定：`N = (frame 長度 − 10) / 4`，N ≥ 1。
長度不符（`(len − 10) % 4 ≠ 0` 或 N = 0）時 hub 丟棄整個 frame。

| 偏移 | 欄位                | 型別        | 說明                                                       |
| ---- | ------------------- | ----------- | ---------------------------------------------------------- |
| 0    | `sequence`        | uint32      | **第一筆**樣本的 PPG 樣本計數（以樣本計，不是以 frame 計） |
| 4    | `status`          | uint8       | 本批**最後一筆**的模組 `ST`（0 finger off、1 finger on、2 get data） |
| 5    | `ppg1` / `ppg2` ×N | uint16 ×2N | 依取樣先後排列，每筆 4B：ppg1 後接 ppg2                    |

- `sensor_time_ms` 是**最後一筆**樣本收完的時刻。模組約每 28 ms 輸出一筆，第 i 筆（0 起）約為 `sensor_time_ms − (N−1−i) × 28`；實際間隔以錄製資料為準。
- 漏樣判斷：下一個 0x05 frame 的 `sequence` 應等於本 frame 的 `sequence + N`；不相等表示中間有樣本沒送出（sensor 忙碌或未訂閱時丟棄）。
- 與 0x01 共用同一個 PPG 樣本計數，因此兩種 frame 的 `sequence` 可以互相對照。
- 預設 MTU（20B）時 N = 2；調大 MTU 可直接增加 N，hub 依長度解析，不需改格式。

## 5. Payload：Acc（0x03，10B）→ `AccElement`、Gyro（0x04，10B）→ `GyroElement`、Acc+Gyro（0x02，16B）

Acc 與 Gyro 是兩種獨立的 frame，同一次取樣會各送一筆，兩筆的 `sensor_time_ms` 與 `sequence` 相同；Acc 先送、Gyro 後送。0x02 則把同一次取樣合併成一筆：`sequence` 後接 acc xyz（偏移 4）再接 gyro xyz（偏移 10）。

| 偏移 | 欄位                                       | 型別      | 說明                    |
| ---- | ------------------------------------------ | --------- | ----------------------- |
| 0    | `sequence`                               | uint32    | IMU 取樣計數            |
| 4    | `acc_x` / `acc_y` / `acc_z`（0x03）      | int16 ×3 | LSM6DS3 原始 counts     |
| 4    | `gyro_x` / `gyro_y` / `gyro_z`（0x04）   | int16 ×3 | LSM6DS3 原始 counts     |

由 hub 換算：acc(mg) = raw × 靈敏度；gyro(mdps) = raw × 靈敏度。預設量程為 **±8 g / ±1000 dps**。

| acc 量程        | mg/LSB          | gyro 量程            | mdps/LSB     |
| --------------- | --------------- | -------------------- | ------------ |
| ±2 g           | 0.061           | ±245 dps            | 8.75         |
| ±4 g           | 0.122           | ±500 dps            | 17.5         |
| **±8 g** | **0.244** | **±1000 dps** | **35** |
| ±16 g          | 0.488           | ±2000 dps           | 70           |

量程可以由 cmd 動態調整（指令待定）；hub 必須知道目前的量程才能正確換算。

## 6. cmd

hub → sensor 寫入 FFF2，**不套用 Frame**（不帶 `sensor_time_ms` 與 `msgType`）；sensor 的回應走 FFF1，套用 Frame，`msgType = 0x00`。

請求（FFF2）：

```
| opcode | seq | args   |
| 1B     | 1B  | 0..18B |
```

回應（FFF1，Frame 的 payload）：

```
| opcode|0x80 | seq | status | data   |
| 1B          | 1B  | 1B     | 0..12B |
```

| 欄位       | 說明                                                                    |
| ---------- | ----------------------------------------------------------------------- |
| `opcode` | bit7 = 0 為請求；回應時把 bit7 設為 1。`0x00` 保留為無效                |
| `seq`    | hub 每送出一個請求就加 1（uint8，會歸零）；sensor 回應時原樣帶回         |
| `args`   | 個數與寬度由各 opcode 定義，用不到就不送；長度由寫入長度決定            |
| `status` | `0x00` OK、`0x01` 未知 opcode、`0x02` 參數長度或值錯誤、`0x03` 下游裝置未回覆 |
| `data`   | 回傳資料，由各 opcode 定義                                              |

- 每個請求只會有一個回應；少於 2B 的寫入直接丟棄，不回應。
- 長度上限依預設 MTU 計算：寫入最多 20B，回應 frame 最多 20B。

| opcode            | 名稱 | args | 回應 data | 說明                                                                                                                   |
| ----------------- | ---- | ---- | --------- | ---------------------------------------------------------------------------------------------------------------------- |
| `0x01`          | 握手 | 無   | 無        | 選用。hub 寫入 CCCD 後送出，sensor 收到即可開始送資料；hub 不送時，sensor 仍用既有的試探方式偵測 notify 是否開啟 |
| `0x02`          | BMH08002-4 轉送 | `op addr hi lo`（4B） | 模組回覆的 `op addr hi lo`（4B） | 只有 `ble_sensor_capture` 實作。手錶把 args 包成模組指令 `55 B1 op addr hi lo CS AA` 送出，等待同 op 的 `55 B1` 回覆最多 500 ms。允許 op：`01` 工作狀態（lo 0 開始／1 停止／2 休眠）、`02` 更新波形、`04` 寫 EEPROM、`05` 讀 EEPROM、`06` 校正靈敏度。期間 PPG 中斷，會出現序號缺口。status `0x03` = 模組未回覆（此時無 data） |
| `0x03`–`0x7F` | 待定 |      |           |                                                                                                                        |

## 7. MTU

- 預設 ATT MTU 為 23，一個 notify 最多帶 20B：PPG（20B）、Acc／Gyro（各 15B）、N=2 的 PPG 原始批次（18B）都放得下，不需要調大 MTU。
- Acc 與 Gyro 拆成兩個 frame，就是為了不超過預設 MTU。

## 8. 待定

1. cmd 指令列表（opcode `0x02` 起），例如 Start、Stop、取樣率、sensor mask、IMU 量程。
2. 量程回報：用 cmd 回應，或在 Acc／Gyro payload 加上 range byte。
3. IMU 已在 sensor 端實作（1 Hz、±8 g / ±1000 dps、I2C 位址 0x6A），Acc／Gyro 各送一筆 15B frame，尚未實測。
4. N76E003 韌體仍以「FFF2 首位元組為 `0x01`」判斷握手，需改成依第 6 節解析（opcode `0x01`、長度 ≥ 2）並送回應；hub 端也尚未實作 cmd 的收送。
