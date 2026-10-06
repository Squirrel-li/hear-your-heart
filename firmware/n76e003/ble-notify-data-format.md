# BLE 通訊協定（sensor ↔ hub8735）

對照：`hub8735-ultra/application/embedded_loudspeaker/BLETask.{h,cpp}`、`globalData.h`、`n76e003/application/ble_sensor/source/main.c`
更新：2026-10-07

## 1. 角色與 GATT

- sensor = Peripheral（GATT Server）；hub8735 = Central（GATT Client）。
- 廣播名稱：`BLE_sensor_ITAP`（hub 只以名稱完全相等比對）。
- 不配對、不加密；所有屬性都不得要求安全性。

| UUID     | 類型           | 屬性                           | 方向          | 內容                                        |
| -------- | -------------- | ------------------------------ | ------------- | ------------------------------------------- |
| `FFF0` | Service        | —                             | —            | 本協定唯一服務                              |
| `FFF1` | Characteristic | Notify（含 CCCD`0x2902`）    | sensor → hub | Frame（第 3 節）：PPG / G-sensor / cmd 回應 |
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
| `msgType`         | uint8  | `0x00` cmd、`0x01` PPG、`0x02` G-sensor                |
| `payload`          | —     | 依 msgType 決定（第 4–6 節）                               |

hub 收到時另記 `hub_rx_time_ms`（hub 時鐘，不在空中傳送）。兩者來自不同時鐘，不能直接相減；對時以 `hub_rx_time_ms` 校正。

| msgType | 類型                | payload | frame 總長 | 方向 / 特徵                              |
| -------- | ------------------- | ------- | ---------- | ---------------------------------------- |
| `0x00` | cmd 回應            | 3–15B   | 8–20B      | sensor → hub：FFF1（第 6 節）           |
| `0x01` | PPG                 | 15B     | 20B        | sensor → hub：FFF1                      |
| `0x02` | G-sensor（LSM6DS3） | 16B     | 21B        | sensor → hub：FFF1                      |

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

## 5. Payload：G-sensor（0x02，16B）→ `GSensorElement`

| 偏移 | 欄位                                 | 型別      | 說明                |
| ---- | ------------------------------------ | --------- | ------------------- |
| 0    | `sequence`                         | uint32    | G-sensor 計數       |
| 4    | `acc_x` / `acc_y` / `acc_z`    | int16 ×3 | LSM6DS3 原始 counts |
| 10   | `gyro_x` / `gyro_y` / `gyro_z` | int16 ×3 | LSM6DS3 原始 counts |

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
| `status` | `0x00` OK、`0x01` 未知 opcode、`0x02` 參數長度或值錯誤                 |
| `data`   | 回傳資料，由各 opcode 定義                                              |

- 每個請求只會有一個回應；少於 2B 的寫入直接丟棄，不回應。
- 長度上限依預設 MTU 計算：寫入最多 20B，回應 frame 最多 20B。

| opcode            | 名稱 | args | 回應 data | 說明                                                                                                                   |
| ----------------- | ---- | ---- | --------- | ---------------------------------------------------------------------------------------------------------------------- |
| `0x01`          | 握手 | 無   | 無        | 選用。hub 寫入 CCCD 後送出，sensor 收到即可開始送資料；hub 不送時，sensor 仍用既有的試探方式偵測 notify 是否開啟 |
| `0x02`–`0x7F` | 待定 |      |           |                                                                                                                        |

## 7. MTU

- 預設 ATT MTU 為 23，一個 notify 最多帶 20B：PPG（20B）放得下，G-sensor（21B）放不下。
- 決議：G-sensor **調大 MTU，不分包**。
- 目前 hub 不會主動交換 MTU，Ameba library 的 Peripheral 預設也不會主動請求，實際值需要實測。

## 8. 待定

1. cmd 指令列表（opcode `0x02` 起），例如 Start、Stop、取樣率、sensor mask、G-sensor 量程。
2. 量程回報：用 cmd 回應，或在 G-sensor payload 加上 range byte。
3. G-sensor 尚未在 sensor 端實作。
4. N76E003 韌體仍以「FFF2 首位元組為 `0x01`」判斷握手，需改成依第 6 節解析（opcode `0x01`、長度 ≥ 2）並送回應；hub 端也尚未實作 cmd 的收送。
