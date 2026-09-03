# BLE 名詞與通訊方式

## 文件目的

本文件整理本專案會用到的 Bluetooth Low Energy（BLE）名詞，並對應 HUB 8735 Ultra 與 remote sensor 的通訊架構。

本專案建議架構：

    remote sensor                         HUB 8735 Ultra
    Peripheral / GATT Server              Central / GATT Client
             │                                  │
             └──────── BLE connection ──────────┘
                      sensor data: Notification
                      control: Write

## BLE、GAP、GATT、ATT

### BLE

Bluetooth Low Energy，針對低功耗、短封包與週期性資料交換設計的 Bluetooth 通訊技術。

### GAP

Generic Access Profile，負責裝置如何被發現、如何建立連線，以及裝置在無線通訊中的角色。

### GATT

Generic Attribute Profile，負責定義連線後的資料結構與操作，例如 Service、Characteristic、Read、Write、Notification。

### ATT

Attribute Protocol，是 GATT 底層使用的資料存取協定。一般應用程式主要透過 GATT API 使用，不需要直接操作 ATT 封包。

## BLE 角色

### Central

主動搜尋並發起連線的裝置。

本專案中：

    HUB 8735 Ultra = Central

Arduino SDK 中通常會看到 beginCentral。

### Peripheral

等待 Central 連線的裝置，通常會先送出 Advertising。

本專案中：

    remote sensor = Peripheral

### Broadcaster

只發送 Advertising 廣播，不接受連線。適合 Beacon 或低頻率狀態廣播。

### Observer

只接收 Advertising，不建立連線。8735 執行 BLEScan 搜尋時，可以視為 Observer。

## Advertising

Advertising 是尚未建立 connection 前的廣播封包。Remote sensor 會週期性廣播自己的資訊，附近的 BLE Scanner 都可能收到。

常見內容：

- 裝置名稱
- Service UUID
- Manufacturer Data
- Sensor ID
- 電池狀態
- 裝置狀態
- 最新心率摘要

Legacy Advertising 的 Advertising Data 最多 31 bytes，Scan Response 另外最多 31 bytes。Advertising 適合裝置發現與少量狀態，不適合連續傳送原始 PPG waveform。

### Passive Scan

只接收裝置主動發送的 Advertising Data。

### Active Scan

收到 Advertising 後，Scanner 會送出 Scan Request，要求裝置回傳 Scan Response，因此可以取得較完整的名稱或資料。

## Scan

Scan 是搜尋附近 BLE 裝置的動作。

BLEScan 可以取得：

- 裝置名稱
- MAC address
- RSSI
- Advertising Data
- Scan Response
- 廣播中的 Service UUID

Scan 只代表「看見」裝置，不代表已經建立 connection，也不代表已經可以收到 Notification。

## Connection

Connection 是兩個 BLE 裝置建立固定的無線資料通道。建立後，Central 與 Peripheral 會按照協議好的時間交換資料，而不是一直重新搜尋。

典型流程：

    Advertising
        ↓
    Scanning
        ↓
    Connect request
        ↓
    BLE connection
        ↓
    Service / Characteristic discovery
        ↓
    啟用 Notification
        ↓
    持續交換資料

Connection 會涉及：

- Connection interval：兩次連線事件的時間間隔
- Connection event：實際交換資料的時間點
- Supervision timeout：多久沒有收到對方資料後判定斷線
- MTU：一次 ATT 資料交換可使用的最大封包大小
- PHY：實際使用的 BLE 無線速率

Connection 中斷後，Notification 不會繼續傳送。程式需要重新 Scan、Connect、Discovery，並重新啟用 Notification。

## Pairing、Bonding、Encryption

這三個名詞不是 Connection 的同義詞。

| 名詞 | 意義 |
|---|---|
| Connection | 建立資料通道 |
| Pairing | 協議安全金鑰 |
| Bonding | 保存金鑰，方便下次使用 |
| Encryption | 使用金鑰加密連線資料 |

兩個裝置可以在沒有 Pairing 的情況下建立 Connection，此時資料可能沒有加密。

Advertising 通常是公開廣播，附近裝置可能看到其中的資料；不要在 Advertising 中放入敏感的個人健康資料。

## GATT Server、GATT Client

### GATT Server

提供資料結構與資料的裝置。

Remote sensor 會提供：

    Sensor Service
        ├── PPG characteristic
        ├── G sensor characteristic
        └── Control characteristic

### GATT Client

搜尋並存取 Server 資料的裝置。

8735 會：

1. Discover Services
2. 找到 Characteristics
3. Read 或 Write 資料
4. 啟用 Notification

## Service、Characteristic、Descriptor

### Service

將相關資料群組在一起。例如 Sensor Service。

### Characteristic

真正存放資料或提供操作功能的項目，例如 PPG Data、G Sensor Data、Control。

### Descriptor

Characteristic 的附加設定。Notification 常用的 descriptor 是 CCCD，UUID 通常為 0x2902。

8735 必須寫入 CCCD，remote sensor 才會開始推送 Notification。

## GATT 資料操作

### Read

由 Client 主動讀取 Server 的目前值。

適合電池容量、Firmware version、裝置名稱與設定值。

### Write

由 Client 寫資料給 Server，通常會等待 GATT 層級的寫入結果。

適合開始感測、停止感測、修改採樣率與設定資料種類。

### Write Without Response

由 Client 寫資料給 Server，但不等待 GATT 層級回覆，速度較快；重要命令建議另外設計 application-level ACK。

### Notification

Server 主動把 Characteristic 的新資料推送給已連線的 Client。

    remote sensor ── Notification ──> 8735

Notification 沒有 GATT 層級的確認回覆，適合高頻率、連續性的 PPG 與 G sensor 資料。封包建議加入 sequence number、timestamp 與 CRC16，讓 8735 能檢查漏封包與資料錯誤。

### Indication

Server 主動送資料給 Client，Client 需要回覆確認。可靠性較高，但速度與頻率通常低於 Notification。

適合重要警報、低頻率結果與必須確認收到的事件，不建議用於高頻率原始 PPG 串流。

## Notification 與 Advertising 的差別

| 項目 | Notification | Advertising |
|---|---|---|
| 是否需要 Connection | 需要 | 不需要 |
| 資料方向 | Server → Client | Broadcaster → Observer |
| 資料對象 | 已連線的特定裝置 | 附近所有掃描裝置 |
| 資料量 | 較大，受 MTU 限制 | Legacy 單一資料最多 31 bytes |
| 是否適合 raw PPG | 適合 | 不適合 |
| 是否適合裝置發現 | 不適合 | 適合 |
| 是否可傳控制命令 | 搭配 Write 可以 | 傳統方式不行 |
| 是否有 GATT ACK | 沒有 | 沒有 |

## 本專案建議

    Advertising：裝置名稱、Sensor ID、電池、狀態
    Connection：建立穩定的資料通道
    Notification：PPG 與 G sensor binary data
    Write：Start / Stop / 設定採樣率
    Read：電池、版本、設定
    Indication：重要警報或需要確認的事件

## 參考資料

- [Bluetooth LE Primer](https://www.bluetooth.com/bluetooth-le-primer/)
- [Bluetooth Core Specification - GATT](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-61/out/en/host/generic-attribute-profile--gatt-.html)
- [HUB 8735 BLEUartClient 範例](../../test/BLEUartClient/BLEUartClient.ino)
- [HUB 8735 BLEScan 範例](../../test/BLEScan/BLEScan.ino)
