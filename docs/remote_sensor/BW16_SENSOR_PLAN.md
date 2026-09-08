# BW16 Remote Sensor 操作規劃

## 文件目的

本文件規劃使用 BW16（RTL8720DN）作為 remote sensor，讀取 PPG 與 G sensor，並透過 BLE 傳送給 HUB 8735 Ultra。

BW16 可以直接執行 Arduino BLE firmware，因此很適合先當作 remote sensor mock device，快速驗證 8735 的連線、Notification、封包解析與斷線重連流程。

BW16 使用 RTL8720DN，支援 BLE 5.0 與 Arduino 開發環境。

## 建議架構

### BW16 直接連接 sensor

    PPG sensor ─┐
                ├── BW16 ── BLE ── HUB 8735 Ultra
    G sensor  ──┘

### BW16 只做 BLE bridge

    PPG/G sensor MCU ── UART ── BW16 ── BLE ── HUB 8735 Ultra

BLE 角色：

    BW16       ：Peripheral / GATT Server
    8735 Ultra ：Central / GATT Client

## 第一階段：使用 BLE UART 範例建立連線

### BW16 端

在 BW16 執行 SDK 的 BLEUartService 範例：

    external/HUB-8735/amebapro2_arduino/Arduino_package/hardware/libraries/BLE/examples/BLEUartService/BLEUartService.ino

### 8735 端

在 8735 執行：

    firmware/hub8735-ultra/examples/ble/uart_client/uart_client.ino

兩端第一版先使用同一組 Nordic UART UUID：

    Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
    RX:      6E400002-B5A3-F393-E0A9-E50E24DCCA9E
    TX:      6E400003-B5A3-F393-E0A9-E50E24DCCA9E

資料方向：

    BW16 Tx characteristic ── Notification ──> 8735
    BW16 Rx characteristic <── Write ───────── 8735

BLEUartService 範例中的 Tx 具有 Notify property；8735 的 BLEUartClient 則對遠端 Tx 設定 notification callback。

## 第二階段：先送固定測試封包

不要一開始就接入真實 sensor。先讓 BW16 每 100 ms 送一個固定 binary frame：

    AA 55 01 01 04 00 00 00 00 CRC16

可以讓其中一個欄位遞增：

    AA 55 01 01 04 SEQ_H SEQ_L 00 00 CRC16

8735 端需要確認：

- 是否每次都收到 Notification
- sequence 是否連續
- 封包長度是否正確
- CRC 是否正確
- 斷線後是否可以重新連線

## 第三階段：加入 PPG 與 G sensor

BW16 端負責：

1. 使用 I2C、SPI 或 UART 讀取 PPG sensor。
2. 讀取 G sensor X/Y/Z 軸資料。
3. 依照固定採樣週期產生 sensor sample。
4. 將 sample 封裝成 binary frame。
5. 寫入 BLE Tx characteristic。
6. 呼叫 Tx.notify(connID)。

8735 端負責：

1. 建立 BLE connection。
2. Discover Service / Characteristic。
3. 開啟 Tx Notification。
4. 接收 bytes 並放入 parser。
5. 驗證 frame。
6. 將有效的 PPG 與 G sensor 資料交給分析流程。

## BW16 Server 端資料流程

概念上可以是：

    readSensor()
        ↓
    packSensorFrame()
        ↓
    Tx.setData(frame, frameLength)
        ↓
    Tx.notify(connID)

第一版可以直接使用 SDK 提供的 setData 與 notify，不需要另外建立資料傳輸 abstraction。

## 建議封包格式

    Header | Version | Type | Length | Sequence | Timestamp | Payload | CRC16

建議的 Type：

| Type | 意義 |
|---:|---|
| 0x01 | PPG sample |
| 0x02 | G sensor sample |
| 0x03 | PPG + G sensor combined frame |
| 0x10 | Battery / status |
| 0x11 | Error / event |

資料排列順序、數值寬度與 endian 必須在兩端固定，不要直接使用 C struct 跨裝置傳送，避免 padding 與 endian 差異。

第一版每個 BLE frame 建議先控制在 20 bytes 以內，確認穩定後再測試較大 MTU 與分包。

## 8735 BLEUartClient 需要調整的地方

以目前的 [BLEUartClient 範例](../../firmware/hub8735-ultra/examples/ble/uart_client/uart_client.ino) 為基礎：

### 修改裝置名稱

BW16 端可將名稱改成容易辨識的名稱，例如 REMOTE_BW16。8735 端不要繼續固定搜尋 AMEBA_BLE_DEV。

### Notification callback 改為 binary

目前範例使用 String(msg)，只適合文字 UART。PPG 與 G sensor 必須依照 data 與 len 解析 bytes。

初期可先印成 hex，確認格式後再交給 parser。

### 加入重連流程

BLEUartClient 的 setup() 只連線一次，適合 demo，不適合長時間 sensor。正式流程應在斷線後重新執行：

    Scan → Connect → Discover → Enable Notify

## 控制命令建議

8735 → BW16 可使用控制 characteristic 傳送命令：

    01             Start
    02             Stop
    03 rate       設定採樣率
    04 mask       設定感測資料種類

重要命令不要只依賴 Write Without Response。可以使用：

    8735 → Start command
    BW16  → Status / ACK notification

這樣可以確認 BW16 確實切換到指定狀態。

## 測試階段

### BLE 連線測試

- [ ] BW16 可以 Advertising
- [ ] 8735 BLEScan 可以看到 BW16
- [ ] 8735 可以建立 connection
- [ ] Service discovery 成功
- [ ] 找到 Nordic UART Service
- [ ] 找到 Tx characteristic
- [ ] Tx Notification 可以啟用

### Notification 測試

- [ ] 送出固定文字資料
- [ ] 送出固定 binary 資料
- [ ] 送出遞增 sequence
- [ ] 8735 可以偵測漏封包
- [ ] 長時間傳輸沒有 buffer overflow

### Sensor 測試

- [ ] PPG sensor I2C/SPI/UART 讀值正常
- [ ] G sensor X/Y/Z 讀值正常
- [ ] sample rate 符合設定
- [ ] timestamp 單調遞增
- [ ] PPG 與 G sensor frame 可正確解析

### 斷線測試

- [ ] 關閉 BW16 電源
- [ ] 8735 偵測 connection loss
- [ ] BW16 恢復後重新 Advertising
- [ ] 8735 自動重新連線
- [ ] 重新連線後 Notification 會再次啟用

## BW16 與 BM7701-00-1 的選擇

| 項目 | BW16 | BM7701-00-1 |
|---|---|---|
| 主要用途 | 可直接執行 sensor 與 BLE firmware | BLE UART bridge/module |
| 原型開發 | 容易 | 需要理解 UART command/API |
| 自訂 GATT | 容易 | 受模組 firmware/profile 限制 |
| 低功耗 | 通常較差 | 通常較適合低功耗 sensor |
| 感測器處理 | 可直接在 BW16 執行 | 由外部 MCU 執行 |
| 8735 接收端 | BLEUartClient | BLEUartClient，UUID 改為 FFF0/FFF1/FFF2 |
| Remote 端範例 | BLEUartService | BM7701 UART library/API |

建議先用 BW16 完成整條資料路徑，再用 BM7701 驗證低功耗與實際模組整合。

## 完成條件

1. 8735 能穩定建立並維持 connection。
2. BW16 能透過 Tx Notification 傳送 binary frame。
3. PPG 與 G sensor frame 可以通過長度、sequence 與 CRC 驗證。
4. 控制命令與 status response 可以運作。
5. 斷線後能自動重新連線並恢復 Notification。
6. BLE 接收流程不會阻塞 8735 的 AI、音訊與 UI 任務。

## 參考資料

- [Realtek BW16 Getting Start Guide](https://forum.amebaiot.com/t/resources-bw16-getting-start-guide/683/1)
- [HUB 8735 BLEUartService 範例](../../external/HUB-8735/amebapro2_arduino/Arduino_package/hardware/libraries/BLE/examples/BLEUartService/BLEUartService.ino)
- [HUB 8735 BLEUartClient 範例](../../firmware/hub8735-ultra/examples/ble/uart_client/uart_client.ino)
- [HUB 8735 BLEScan 範例](../../firmware/hub8735-ultra/examples/ble/scan/scan.ino)
