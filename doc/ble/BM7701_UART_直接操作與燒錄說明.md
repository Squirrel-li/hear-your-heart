# BM7701-00-1 UART 直接操作與燒錄說明

## 1. 先釐清「燒錄」的意思

BM7701-00-1 是以 Holtek BC7701 為核心的 Bluetooth 5.2 BLE UART 模組。

有兩種不同的操作：

1. **修改 BLE 參數**：例如藍牙名稱、廣播資料、發射功率、UART 波特率等。這可以直接使用 UART，不需要 BCT-7701-001、BCE-GenTrx32-002 或 e-Link。
2. **更新 BC7701 內部韌體**：例如將自製 `.bin` 寫入 `swBoot`、`swStack` 或 `swApp`。官方公開文件沒有提供完整的 UART 燒錄封包協定與 PC 燒錄工具，因此不能直接把 `.bin` 當成 UART 資料串流送入模組。

官方 BM7701 範例中的 e-Link32/e-Link，是用來燒錄開發板上的 HT32 或 HT8 MCU，並不是用來燒錄 BM7701 模組本身。

## 2. 直接操作所需硬體

- BM7701-00-1
- 3.3 V USB-TTL UART 轉接器，或一顆具有 3.3 V I/O 的 MCU
- 穩定的 3.3 V 電源
- 可控制 `RST_N` 的 GPIO，或一個 Reset 按鍵
- 建議在自製載板上預留測試點

注意：不可使用 RS-232 電平或 5 V UART。BM7701 的 VCC 工作範圍為 2.0～3.6 V，I/O 電壓也不可超過 VCC。

## 3. UART 直接接線

### 使用 UART1

| USB-TTL / MCU | BM7701-00-1 | 說明 |
|---|---|---|
| 3.3 V | VCC，Pin 9 | 電源 |
| GND | GND，Pin 1 或 Pin 24 | 建議兩個 GND 都接 |
| RXD | UART1_TX，Pin 23 | 交叉連接 |
| TXD | UART1_RX，Pin 22 | 交叉連接 |
| GPIO / Reset | RST_N，Pin 6 | 低有效 Reset |

### 使用 UART2

| USB-TTL / MCU | BM7701-00-1 | 說明 |
|---|---|---|
| RXD | UART2_TX，Pin 8 | 交叉連接 |
| TXD | UART2_RX，Pin 7 | 交叉連接 |

UART1 與 UART2 擇一使用即可，另一組可以不接。

## 4. 上電與 UART 啟動時序

初次測試建議使用預設的 115200 bps：

1. 設定 UART 為 `115200 baud, 8N1`。
2. 將 `RST_N` 拉低至少 10 ms。
3. 將 `RST_N` 拉高。
4. 等待至少 60 ms。
5. 傳送 UART 指令。

UART 工具必須傳送**原始二進位資料**，不是 ASCII 十六進位字串，也不要自動附加 CR/LF。

## 5. Reset 後的 UART 模式選擇

BC7701 會在 `RST_N` 從低電位變高電位後的前 60 ms 讀取 UART TX/RX 電位。

以下的 TX/RX 是指 BM7701 UART 腳位在啟動期間的電位：

| TX | RX | 波特率 | 啟動結果 |
|---:|---:|---:|---|
| 1 | 1 | 115200 | 進入應用程式，預設模式 |
| 0 | 1 | 9600 | 進入應用程式 |
| 1 | 0 | 9600 | 停留在 boot mode |
| 0 | 0 | 115200 | 停留在 boot mode |

一般參數設定使用 `TX=1、RX=1` 的預設模式即可。

若要使用 boot mode，模組 TX 腳位應使用暫時性的弱上拉、弱下拉或三態控制。不要將模組 TX 永久短接到 GND，因為啟動完成後該腳位會恢復為輸出，可能造成電流衝突。

## 6. UART 指令格式

BC7701 支援三種主要封包：

- `CmdType1`：HCI 指令，主要用於 BLE 測試。
- `CmdType2`：存取 BLE Service、Characteristic 等資料。
- `CmdType3`：設定 BLE 內部參數。

一般設定使用的 `CmdType3` 指令格式如下：

```text
77  LEN  FLAG  TYPE_L TYPE_H  VALUE...
```

模組回覆通常以 `78` 開頭：

```text
78  LEN  STATUS  TYPE_L TYPE_H  VALUE...
```

`STATUS` 的低 4 bit 為 `0` 時，通常表示成功。

## 7. 基本測試指令

### 7.1 讀取目前版本

```text
77 03 00 20 00
```

### 7.2 設定藍牙名稱為 BM7701

```text
77 09 00 05 00 42 4D 37 37 30 31
```

其中 `42 4D 37 37 30 31` 是 ASCII 字串 `BM7701`。

### 7.3 開啟 BLE 廣播

```text
77 04 00 07 00 01
```

第一次開啟廣播時可能需要等待約 600～900 ms 進行 RF 校準。

### 7.4 設定最大發射功率

```text
77 04 00 0B 00 0F
```

功率參數範圍為 `0x00`～`0x0F`。實際 dBm 數值應以模組文件與實測結果為準。

### 7.5 設定晶振 C-Load

BM7701-00-1 建議使用 C-Load 值 `4`：

```text
77 04 00 0E 00 04
```

## 8. 將 BLE 參數保存到 Flash

UART 修改的 BLE 參數預設只暫存在 RAM。設定完名稱、廣播資料等參數後，送出以下指令將 `swParaUpdate` 設為 1：

```text
77 07 10 10 00 10 00 00 00
```

成功後，重新 Reset 或重新上電，模組就會使用保存於 Flash 的參數。

建議測試流程如下：

```text
77 09 00 05 00 42 4D 37 37 30 31
77 07 10 10 00 10 00 00 00
77 04 00 07 00 01
```

每一筆指令都應等待上一筆的 `78 ...` 回覆，再傳送下一筆。

## 9. 真正的 BC7701 韌體更新

BC7701 內部 Flash 不是單一的使用者程式區，而是包含多個區域：

| 區域 | 位址範圍 |
|---|---|
| `swBoot` | `0x00000000`～`0x00001FFF` |
| `swStack` | `0x00002000`～`0x00019FFF` |
| `swApp1` | `0x0001A000`～`0x00020DFF` |
| `swApp2` | `0x00020E00`～`0x00027BFF` |
| `swPara` | `0x00027C00`～`0x00027DFF` |
| `swRes` | `0x00027E00`～`0x00027FF7` |

韌體映像需要包含區塊資訊、版本資料、有效標記與 32-bit checksum。因此不能把普通 `.bin` 檔直接透過 UART 傳送。

官方 API 文件確實提到 UART update、boot mode 及 `flashWriteEnable` 等欄位，但沒有公開完整的：

- UART 韌體更新封包格式
- Flash erase/write 流程
- 每個封包的 ACK/錯誤格式
- PC 端燒錄工具
- 可自行編譯的 BC7701 使用者韌體 SDK

所以，若目標是更新 BC7701 內部韌體，應向 Holtek 或 Best Modules 索取：

1. BC7701 UART firmware update protocol
2. 合法且符合版本格式的韌體映像檔
3. 對應的更新工具或 SDK

在沒有上述資料前，不建議嘗試寫入 `swBoot` 或 `swStack`，否則可能使模組無法正常啟動。

## 10. 常見故障排查

### 沒有任何回覆

- 確認 USB-TTL 是 3.3 V I/O。
- 確認 TX/RX 是交叉接線。
- 確認 USB-TTL 與 BM7701 共地。
- 確認 `RST_N` 拉高後已等待 60 ms。
- 確認 UART 工具使用原始二進位傳送，而非傳送字串 `77 03 ...`。
- 重新以 115200 bps 測試，不要先使用已保存但未知的波特率。

### 回覆資料看起來是亂碼

- 可能是模組已保存其他 UART 波特率。
- 重新依照 Reset 後前 60 ms 的 TX/RX 電位選擇 115200 或 9600。
- 確認沒有同時連接 UART1 與 UART2。

### 設定重開機後消失

- 只送了參數設定指令，沒有送 `swParaUpdate` 保存指令。
- 送出保存指令後，必須重新 Reset 或重新上電。

## 11. 官方文件

- [BM7701-00-1 官方產品頁](https://www.bestmodulescorp.com/tc/bm7701-00-1.html)
- [BM7701-00-1 Datasheet](https://www.bestmodulescorp.com.cn/zh/amfile/file/download/file/584/product/931/)
- [BC7701 BLE API User's Guide](https://www.bestmodulescorp.com.cn/zh/amfile/file/download/file/265/product/931/)
- [BM7701-00-1 應用範例 AN0614TC](https://www.holtek.com.tw/webapi/105580/an0614tc.pdf)
- [BC7701 Datasheet](https://www.holtek.com.tw/webapi/11842/BC7701v110.pdf)

## 12. 網路搜尋後的完整可行性整理

以下結論是依照目前可索引到的 Best Modules / Holtek 官方文件、官方 GitHub Arduino 函式庫，以及精確搜尋 `BC7701 UART update`、`BC7701_XY.bin`、`BC7701 firmware updater`、`BC7701 flash programming UART` 等關鍵字整理。這不是宣稱網路上不存在任何未公開或私下提供的工具；而是目前公開資料中沒有找到可直接使用的 BC7701 韌體燒錄器。

| 方法 | 能否不使用 e-Link32 | 實際用途 | 判定 |
|---|---|---|---|
| USB-TTL / 外部 MCU + UART BCI/HCI 指令 | 可以 | 設定藍牙名稱、MAC、廣播、功率、UART 波特率等 | **已確認可行** |
| UART 指令加 `swParaUpdate` | 可以 | 將 BLE 參數保存到 BC7701 的 `swPara` Flash | **已確認可行，但不是韌體燒錄** |
| BC7701 `urMode2/urMode3` UART boot mode | 理論上可以 | 讓 BC7701 停在 `swBoot`，準備 UART update | **官方有入口描述，但缺上傳協定** |
| UART 傳送 `.bin` | 不可直接這樣做 | 需要廠商定義的 erase/write、封包、checksum、ACK 流程 | **目前沒有公開證據可行** |
| BLE OTA / APP Air update | 可能可以 | API 文件描述 `para1` 與 `bleMode`，可用 BLE 更新 | **有架構線索，但缺完整 OTA 流程與映像檔** |
| 外部 HT32/其他 MCU 自己用 UART ISP/IAP | 可以 | 燒錄自製載板上的外部 MCU | **可行，但不會更新 BM7701 內部 BC7701** |
| ST-Link、J-Link、一般 SWD 直接接 BM7701 | 未證實 | 官方模組資料沒有公開 SWD/OCDS/ICP 接腳或燒錄工具 | **不建議假設可行** |
| e-Link32 直接接 BM7701 UART/GPIO | 不適用 | e-Link32 官方範例用途是燒 BCE 開發板上的 HT32/HT8 MCU | **不是 BM7701 內部韌體燒錄介面** |

### 12.1 為什麼可以確定 UART 參數寫入可行

BM7701-00-1 官方產品頁把模組描述為 BC7701 BLE 控制器的 UART 透明傳輸模組，並列出兩組 UART。官方 API 文件也明確定義 CmdType1、CmdType2、CmdType3；其中 CmdType3 用來設定 BLE 參數。設定值先在 RAM 中生效，再以 `swParaUpdate` 寫入 Flash。

官方 Arduino 函式庫也是「透過 UART 存取 BM7701-00-1/BMC77M001」的主機端函式庫，並不是 BC7701 韌體燒錄器。因此可以完全不使用 e-Link32 來操作模組功能，但不能因此推論可以用同一組 UART 函式庫更新 BC7701 程式碼。

### 12.2 為什麼又可以判斷「UART 韌體更新」可能存在

BC7701 BLE API User's Guide 公開了下列內容：

- BC7701 Flash 分成 `swBoot`、`swStack`、`swApp1`、`swApp2`、`swPara`、`swRes`。
- `para0` 的說明包含 `Use for UART update`。
- `updateSwEnterMode` 定義了 `urMode0`～`urMode3`、`mcuMode` 與 `bleMode`。
- `urMode2` 與 `urMode3` 會讓晶片停在 `swBoot`，而不是跳到 `swApp`。
- `flashWriteEnable` 說明為「UART update 時允許寫入所有區塊」，但同時註明 `for TEST use only`，且預設不能寫入自身區塊。
- 韌體映像需要 block information、版本、valid check 與 32-bit checksum；正式檔名格式為 `BC7701_XY.bin`。

這些資料足以證明 BC7701 的韌體架構為 UART/OTA 更新預留了機制，但官方公開文件沒有接著提供「如何傳送映像」的完整規格。因此目前不能安全地從這些欄位反推出燒錄封包。

### 12.3 目前找不到的關鍵資料

公開資料中仍缺少：

1. BC7701 UART update 的命令編號與封包格式。
2. Flash erase、page write、續傳、timeout、ACK、錯誤碼規則。
3. `.bin` 如何拆成 `swBoot`、`swStack`、`swApp1`/`swApp2`，以及如何處理 checksum。
4. 可下載的官方 PC UART updater。
5. 可合法使用於 BM7701-00-1 的完整 `BC7701_XY.bin` 韌體。
6. 公開的 BC7701 使用者韌體 SDK 或可自行編譯的 BLE stack/bootloader。

所以不能把一般 HT32 UART ISP/IAP 協定、STM32 ROM bootloader 協定或 Arduino serial upload 協定套用到 BC7701。

## 13. 若要自行驗證 UART boot mode

這一節只說明官方文件描述的「進入條件」，不代表已知完整燒錄方法。

### UART1 接腳

- BM7701 Pin 23：`UART1_TX`，對應 BC7701 `PA0`。
- BM7701 Pin 22：`UART1_RX`，對應 BC7701 `PA1`。
- BM7701 Pin 6：`RST_N`。

### UART2 接腳

- BM7701 Pin 8：`UART2_TX`，對應 BC7701 `PB6`。
- BM7701 Pin 7：`UART2_RX`，對應 BC7701 `PB7`。

在 `RST_N` 由低變高後的前 60 ms，依目前使用的 UART 讓模組 TX/RX 維持下列電位：

| 模式 | 模組 TX | 模組 RX | UART 速度 | 結果 |
|---|---:|---:|---:|---|
| `urMode0` | 1 | 1 | 115200 | 進入 `swApp`，正常應用模式 |
| `urMode1` | 0 | 1 | 9600 | 進入 `swApp`，忽略已保存的波特率 |
| `urMode2` | 1 | 0 | 9600 | 停在 `swBoot`，另一組 UART 停用 |
| `urMode3` | 0 | 0 | 115200 | 停在 `swBoot`，另一組 UART 停用 |

實作時，模組 TX 是啟動後會恢復成輸出的腳位，應使用弱上拉/弱下拉或三態控制，不要使用低阻抗推挽輸出永久硬推同一條線。進入 `urMode2/urMode3` 後先觀察是否有官方工具或協定可交換資料；沒有協定時不要盲目送 `.bin`。

最安全的第一步是先在正常模式以 115200、8N1 讀取版本：

```text
77 03 00 20 00
```

這只能讀取版本與區塊狀態，不能開始燒錄。

## 14. 最實際的無 e-Link32 方案

### 方案 A：產品只需要 BLE 功能與參數設定

自製一塊最小載板，連接 3.3 V、GND、UART1、RST_N，再由 USB-TTL 或主控 MCU 傳送 BCI 指令。這是目前最完整、風險最低、官方文件支持最充分的方案。

### 方案 B：需要更新外部主控 MCU

讓外部 MCU 使用自己的 UART bootloader、USB DFU 或其他原生燒錄方式；BM7701 只保留固定的 BC7701 韌體。這樣可以完全不使用 e-Link32，但更新的是外部 MCU，不是 BM7701。

### 方案 C：一定要更新 BC7701 內部韌體

先向 Holtek 或 Best Modules 索取以下三項，再考慮寫工具：

1. `BC7701 UART firmware update protocol`。
2. 與目前 `swBoot`/`swStack` 版本相容的正式映像檔。
3. 官方 PC updater、SDK，或可驗證的更新範例。

詢問時應特別確認：目前販售的 BM7701-00-1 是否啟用 `urMode2/urMode3`、是否允許客戶更新 `swApp`、是否必須整包更新 `swBoot + swStack + swApp`，以及更新失敗如何救援。

## 15. 搜尋結論

截至 2026-09-04，最合理的結論是：

- **不使用 e-Link32，直接用 UART 與接腳操作 BM7701：可以。**
- **不使用 e-Link32，直接用 UART 保存 BLE 參數到 Flash：可以。**
- **不使用 e-Link32，直接用 UART 更新 BC7701 內部韌體：架構上可能可以，但公開資料不足，尚不能提供可靠的實作步驟。**
- **將一般 `.bin` 直接從 USB-TTL 丟進 BM7701：沒有證據可行，且可能破壞啟動區。**
- **目前最可靠的無 e-Link32 產品做法，是固定 BC7701 韌體，透過 UART 設定參數，並把自製程式放在外部 MCU。**

### 新增參考資料

- [Best Modules BM7701-00-1 官方產品頁](https://www.bestmodulescorp.com/bm7701-00-1.html)
- [Best Modules BMC77M001 官方頁](https://www.bestmodulescorp.com/en/bmc77m001.html)
- [Holtek BC7701 官方 Datasheet](https://www.holtek.com/webapi/116711/BC7701v110.pdf)
- [BC7701 BLE API User's Guide](https://www.bestmodulescorp.com.cn/zh/amfile/file/download/file/265/product/931/)
- [Best Modules BM7701-00-1 官方 Arduino 函式庫](https://github.com/BestModules-Libraries/BM7701-00-1)
- [Holtek BC7701 Programming User's Guide（官方索引；目前直接檔案連結可能失效）](https://www.holtek.com.cn/webapi/187536/BC7701_Programming_User_Guide_v110.pdf)
