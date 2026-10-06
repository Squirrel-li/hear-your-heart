# 感測端 BLE 介面

更新：2026-10-07

## GATT

- sensor = Peripheral，廣播名稱 `BLE_sensor_ITAP`；不配對、不加密。
- 多位元組欄位一律 Big-endian。

| UUID | 屬性 | 方向 | 用途 |
|---|---|---|---|
| `FFF0` | Service | — | — |
| `FFF1` | Notify（含 CCCD） | sensor → hub | 資料 frame |
| `FFF2` | Write／Write Without Response | hub → sensor | hub 下傳 |

## 流程

1. sensor 廣播 → hub 依名稱連線。
2. hub 探索 `FFF0`／`FFF1`／`FFF2`，並將 `FFF1` 的 CCCD 寫成 `0x0001`。
3. sensor 確認 notify 已開啟後才送資料；一個 notify 放一個 frame，不加長度欄位。
4. 斷線後 sensor 重新廣播，hub 會自動重連。

## Frame（FFF1）

```
| sensor_time_ms 4B | msgType 1B | payload |
```

- `sensor_time_ms`：uint32，sensor 時鐘，取樣時刻（ms）。
- `msgType`：`0x01` PPG、`0x02` G-sensor。

PPG（20B）：

```
| time 4B | 01 | sequence 4B | status | SpO2 | heartRateBpm | bpi | hrv | pulseAmplitude | systoleDuration | ppg1 2B | ppg2 2B |
```

- `status`：0 finger off、1 finger on、2 有效資料。

G-sensor（21B）：

```
| time 4B | 02 | sequence 4B | acc_x | acc_y | acc_z | gyro_x | gyro_y | gyro_z |   （各軸 int16 原始 counts）
```

- 預設量程 ±8 g、±1000 dps。21B 超過預設 MTU 的上限 20B，需要把 MTU 調大到至少 24，不分包。
