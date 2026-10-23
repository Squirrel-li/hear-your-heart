# 感測端 BLE 介面

> 2026-10-18 核對：以下速查尚有舊版敘述：目前 HUB 先探索 FFF0／FFF1／FFF2，失敗再試下列 128-bit 服務；Auto 失聯後維持 Auto 自動重連（2026-10-23 起），使用者 Disconnect 或切 Manual 才停止。msgType 高 4 bit 是來源（Chest=0、Watch=1），手錶實際為 0x11／0x12。 現況見[架構核對](../ARCHITECTURE.md)。

更新：2026-10-11

## GATT

- sensor = Peripheral，廣播名稱胸帶 `BLE_sensor_ITAP`、手錶 `BLE_watch_ITAP`（HUB 可同時連兩台）；不配對、不加密。
- 多位元組欄位一律 Big-endian。

| UUID                                     | 屬性                          | 方向          | 用途       |
| ---------------------------------------- | ----------------------------- | ------------- | ---------- |
| `524CACC0-3C17-D293-8E48-14FE2E4DA212` | Service                       | —            | —         |
| `D002`                                 | Notify（含 CCCD）             | sensor → hub | 資料 frame |
| `D001`                                 | Write／Write Without Response | hub → sensor | hub 下傳   |

## 流程

1. sensor 廣播 → hub 依名稱連線。
2. hub 探索 `524CACC0-3C17-D293-8E48-14FE2E4DA212`／`D002`／`D001`，並將 `D002` 的 CCCD 寫成 `0x0001`。
3. sensor 確認 notify 已開啟後才送資料；一個 notify 放一個 frame，不加長度欄位。
4. 斷線後 sensor 重新廣播，hub 會自動重連。

## Frame（D002）

```
| sensor_time_ms 4B | msgType 1B | payload |
```

- `sensor_time_ms`：uint32，sensor 時鐘，取樣時刻（ms）。
- `msgType`：

| 值 | 名稱 | 長度 | 說明 |
| --- | --- | --- | --- |
| `0x00` | cmd | — | 預留編號，內容、方向（sensor ↔ hub）尚未定義；收到時忽略。 |
| `0x01` | PPG | 20B | 見下。 |
| `0x02` | acc + gyro | 21B | 同一次取樣的 Acc 與 Gyro 合併成一個 frame，sensor 目前送這個。 |
| `0x03` | Acc | 15B | 只含 Acc，hub 可解析，sensor 目前不送。 |
| `0x04` | Gyro | 15B | 只含 Gyro，hub 可解析，sensor 目前不送。 |
| `0x05` | PPG 原始批次 | 10+4N B | `sequence`（第一筆）＋`status`＋N 筆 `ppg1`/`ppg2`；`ble_sensor_hr` 以 N=2（18B）送出，hub 驗證長度後以同一套演算法自算心率。格式見 `firmware/n76e003/ble-notify-data-format.md` 4.1 節。 |

PPG（20B）：

```
| time 4B | 01 | sequence 4B | status | SpO2 | heartRateBpm | bpi | hrv | pulseAmplitude | systoleDuration | ppg1 2B | ppg2 2B |
```

- `status`：0 finger off、1 finger on、2 有效資料。

acc + gyro（21B）：

```
| time 4B | 02 | sequence 4B | acc_x | acc_y | acc_z | gyro_x | gyro_y | gyro_z |      （各軸 int16 原始 counts）
```

單獨的 Acc（`03`）與 Gyro（`04`）各 15B，格式為 `| time 4B | 03/04 | sequence 4B | x | y | z |`。

- 預設量程 ±8 g、±1000 dps。
- 21B 的 `02` frame 超過預設 MTU 的 20B 有效載荷，central 需先協商 MTU ≥ 24（BM7701 以 25B notify 實測可行，見 `.agents/MEMORY.md`）；未協商時 sensor 端會被截成 20B。
