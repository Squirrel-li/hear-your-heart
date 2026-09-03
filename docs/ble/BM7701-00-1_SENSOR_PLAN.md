# BM7701-00-1 Remote Sensor 操作規劃

## 文件目的

本文件規劃使用 BM7701-00-1 作為 remote sensor 的 BLE 無線傳輸模組，將 PPG 與 G sensor 資料傳送給 HUB 8735 Ultra。

BM7701-00-1 是由外部 MCU 透過 UART 控制的 BLE transparent-transmission module。它不是像 BW16 一樣直接在 Arduino 上執行完整 BLE application；remote MCU 需要透過 UART 將感測資料交給 BM7701。

## 建議架構

    PPG sensor ─┐
                ├── remote MCU ── UART ── BM7701-00-1
    G sensor  ──┘                         │
                                          │ BLE
                                          ▼
                                  HUB 8735 Ultra

BLE 角色：

    BM7701-00-1：Peripheral / GATT Server
    8735 Ultra ：Central / GATT Client

BM7701 官方資料說明它是透過 UART 控制的 BLE transparent-transmission module；BC7701 GATT 使用指南定義了資料服務與 characteristic。

## BM7701 GATT 資料表

| 項目 | UUID | 方向 | 用途 |
|---|---|---|---|
| Manufacture Define Service | FFF0 | - | BM7701 資料服務 |
| Read Data | FFF1 | BM7701 → 8735 | Notification 感測資料 |
| Write Data | FFF2 | 8735 → BM7701 | Write Without Response 控制資料 |

BC7701 文件列出 FFF1 為 Notify、FFF2 為 Write Without Response，資料長度上限為 128 bytes；實際每次可傳輸的資料量仍需配合 MTU、韌體與模組設定。

8735 接收端的 UUID 應修改為：

    SENSOR_SERVICE_UUID = FFF0
    SENSOR_NOTIFY_UUID  = FFF1
    SENSOR_CONTROL_UUID = FFF2

## 第一階段：先確認 BLE 連線

### 目標

先不接入真實 PPG 與 G sensor，確認 BM7701 能被 8735 找到、建立連線並送出 Notification。

### 操作

1. 確認 BM7701 的供電、UART baud rate、TX/RX 交叉連接與共地。
2. 使用手機 BLE 工具或 BM7701 官方工具確認裝置可以被看到。
3. 在 8735 執行 BLEScan，確認名稱、MAC address 與 RSSI。
4. 將 8735 的 BLEUartClient 改為 FFF0、FFF1、FFF2。
5. 連線後執行 Service / Characteristic discovery。
6. 對 FFF1 啟用 Notification。
7. 從 remote MCU 傳送固定測試資料，例如遞增 counter。

### 第一筆測試資料

初期先使用容易辨識的 binary payload：

    00 01 02 03 04 05 06 07 08 09

8735 callback 應直接以 hex 印出，不要先轉成 String。

## 第二階段：加入 PPG 與 G sensor

remote MCU 負責：

1. 讀取 PPG sensor。
2. 讀取 G sensor。
3. 以固定時間間隔取樣。
4. 將資料封裝成 binary frame。
5. 透過 BM7701 UART 傳送。

8735 負責：

1. 維持 BLE connection。
2. 接收 FFF1 Notification。
3. 將 Notification bytes 放入 parser 或 ring buffer。
4. 驗證長度、sequence、timestamp 與 CRC。
5. 將有效資料交給分析與音樂決策流程。

## 建議封包格式

    Header | Version | Type | Length | Sequence | Timestamp | Payload | CRC16

| 欄位 | 用途 |
|---|---|
| Header | 找到封包開頭，例如 AA 55 |
| Version | 區分未來封包版本 |
| Type | PPG、G sensor 或 status |
| Length | 確認 payload 長度 |
| Sequence | 偵測漏封包、重複封包 |
| Timestamp | 保留感測資料時間 |
| Payload | 實際 sensor data |
| CRC16 | 檢查資料完整性 |

第一版每個 BLE frame 建議先控制在 20 bytes 以內，確認穩定後再測試較大 MTU 與分包。

## 8735 BLEUartClient 需要調整的地方

以目前的 [BLEUartClient 範例](../../firmware/hub8735-ultra/examples/ble/uart_client/uart_client.ino) 為基礎：

### 修改 UUID

使用 FFF0、FFF1、FFF2，不要繼續使用 Nordic UART 的 6E4000xx UUID。

### 修改裝置搜尋條件

BM7701 預設名稱通常是 BM7701，也可以改用 MAC address 或 Service discovery 結果識別。

### Notification 使用 binary parser

不要使用 String(msg)。應使用 callback 提供的 data 與 len，並將 bytes 交給 parser。

### 只接收資料時不必使用 RX

如果第一版只要求 BM7701 → 8735，8735 可以先只取得 FFF1 並啟用 Notification，不必使用 FFF2。

### 增加錯誤檢查

需要檢查：

- Scan 是否找到目標裝置
- Connection 是否成功
- Service 是否存在
- FFF1 是否存在且支援 Notify
- CCCD 是否存在
- Notification callback 是否真的收到資料
- 斷線後是否能重新連線

## BM7701 UART 注意事項

BM7701 的 UART 不是單純把任意 bytes 丟進去就一定能送出 BLE 資料。BC7701 使用 Command / Event 格式與 Host MCU 溝通；remote MCU 應優先使用 BM7701 官方 Arduino library 或官方 UART API，避免自行猜測 Command format。

應分成兩層除錯：

    UART 層：remote MCU 是否正確把資料交給 BM7701
    BLE 層 ：8735 是否在 FFF1 收到 Notification

若 UART 層正確但 8735 沒收到資料，優先檢查 GATT UUID、CCCD Notification 與 BM7701 BLE firmware 設定。

## 測試項目

### 連線測試

- [ ] 8735 可以找到 BM7701
- [ ] 可以建立 connection
- [ ] 可以完成 service discovery
- [ ] 找到 FFF0
- [ ] 找到 FFF1
- [ ] FFF1 可以啟用 Notification

### 資料測試

- [ ] 固定 counter 可以正確遞增
- [ ] PPG 資料數值沒有 endian 錯誤
- [ ] G sensor X/Y/Z 軸順序正確
- [ ] sequence 可以偵測漏封包
- [ ] CRC 錯誤封包會被丟棄
- [ ] 長時間傳輸沒有 buffer overflow

### 斷線測試

- [ ] 拔掉 BM7701 電源
- [ ] 8735 顯示斷線
- [ ] BM7701 恢復後重新 Advertising
- [ ] 8735 可以自動重新連線
- [ ] 重新連線後會再次啟用 Notification

## 完成條件

1. 連線可以穩定建立。
2. FFF1 Notification 能持續收到 binary sensor frame。
3. PPG 與 G sensor 資料能通過長度、sequence 與 CRC 驗證。
4. 可以偵測斷線並重新連線。
5. 不會因高頻率 Notification 阻塞音訊、AI 或 UI 流程。

## 參考資料

- [BM7701 官方產品資料](https://www.bestmodulescorp.com/bm7701-00-1.html)
- [BC7701 Programming User Guide](https://www.holtek.com.cn/webapi/187536/BC7701_Programming_User_Guide_v110.pdf)
- [HUB 8735 BLEUartClient 範例](../../firmware/hub8735-ultra/examples/ble/uart_client/uart_client.ino)
- [HUB 8735 BLEScan 範例](../../firmware/hub8735-ultra/examples/ble/scan/scan.ino)
