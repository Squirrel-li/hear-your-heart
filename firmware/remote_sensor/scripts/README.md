# BM7701 UART 控制與電腦 BLE 測試

Windows PowerShell 5.1 + .NET SerialPort／HttpListener；電腦使用 Chrome Web Bluetooth。無 npm 或 Python 第三方套件。腳本位於 `F:\Universitiy\ITAP\firmware\remote_sensor\scripts`。

## 接線與前提

- USB-TTL 必須是 3.3 V I/O，TX → BM7701 UART1_RX（Pin 22）、RX → UART1_TX（Pin 23），共地，供電 2.0–3.6 V。不可接 RS-232 或 5 V UART。
- 模組已正常啟動；預設 115200、8N1、無流控。Reset 後至少等 60 ms。腳本不切換波特率、不拉動 Reset，DTR／RTS 關閉；不要把控制線接成不明的 Reset 電路。
- 本次設備預設為 COM11；可用參數覆寫。一次只有一個程式能開啟 COM11。
- 電腦需有可用 Bluetooth，Chrome 需支援 Web Bluetooth；第一次連線須在裝置選擇器選 BM7701-ITAP。

## 快速開始

在 PowerShell 中：

```powershell
Set-Location 'F:\Universitiy\ITAP\firmware\remote_sensor\scripts'
powershell -NoProfile -File .\test-bm7701.ps1
powershell -NoProfile -File .\bm7701.ps1 -Command Ports
powershell -NoProfile -File .\bm7701.ps1 -Command Version -Port COM11
powershell -NoProfile -File .\bm7701.ps1 -Command Web -Port COM11 -HttpPort 8771
```

Web 模式由同一個 `bm7701.ps1` 開啟 UART、監看非同步事件、綁定 `127.0.0.1:8771`，並提供網頁與固定的 BM7701 API。網頁不直接開 COM，也不需要 ADB 或另一個 HTTP server。開啟 `http://127.0.0.1:8771/` 即可。

保持啟動視窗執行；Ctrl+C 可停止 UART 與本機服務。`-Seconds 60` 可限時執行；埠占用時改用 `-HttpPort 8772`。服務只監聽本機回環位址，不公開工作區檔案。


## UART 操作

另開 PowerShell，開啟互動模式，輸入下列指令；等待輸入時仍持續接收 BLE 事件。

```powershell
powershell -NoProfile -File .\bm7701.ps1 -Command Shell -Port COM11 -LogPath "$env:TEMP\bm7701-uart.log"
```

| 互動指令                               | 用途                                               |
| -------------------------------------- | -------------------------------------------------- |
| `version` / `status`               | 讀版本／連線狀態，保留原始回覆                     |
| `name BM7701-ITAP`                   | 設定名稱，1–31 UTF-8 bytes                        |
| `advertise on` / `advertise off`   | 開啟／停止廣播                                     |
| `power 0` 至 `power 15`            | 設定模組功率代碼；不將代碼宣稱為 dBm               |
| `send 00 01 02 03 04 05 06 07 08 09` | 以 FFF1 發送原始 bytes                             |
| `text Hello BM7701`                  | 發送 UTF-8                                         |
| `counter`                            | 發送 100 筆 10-byte 序號資料，預設間隔 100 ms      |
| `save`                               | **明確寫 Flash**；一般測試不需要，不自動重置 |
| `quit`                               | 關閉串口                                           |

Shell 需要互動式 Windows 主控台；不要把 stdin 重新導向它。Ctrl+C 可停止。命令錯誤或逾時會關閉串口並回傳非零 exit code；修正後重開。BCI 沒有 transaction ID，因此不在同一 session 自動重試逾時命令。

單次命令與限時接收：

```powershell
powershell -NoProfile -File .\bm7701.ps1 -Command Send -Value '00 01 02 03 04 05 06 07 08 09'
powershell -NoProfile -File .\bm7701.ps1 -Command Send -Encoding Text -Value 'Hello BM7701'
powershell -NoProfile -File .\bm7701.ps1 -Command Receive -Seconds 60 -LogPath "$env:TEMP\bm7701-rx.log"
powershell -NoProfile -File .\bm7701.ps1 -Command Counter -Count 100 -IntervalMs 100
```

`-Baud` 只設定電腦 UART，`-TimeoutMs` 預設 3000，涵蓋首次 RF 校準時間。`Receive -Seconds 0` 持續接收直到 Ctrl+C。HEX 只接受完整 byte pairs，可有空白，不接受 `0x` 前綴；傳送限制採官方函式庫保守範圍 1–127 bytes。第一輪用 ≤20 bytes；較大傳輸須實測 MTU／模組限制，不自動分包。Counter 格式為 `42 4D seq_lo seq_hi 00 01 02 03 04 05`，序號由 0 起。

回覆判斷包含 Type、旗標高位與狀態低四位。FFF2 收到資料及連線事件會另外列出，不因等待 ACK 而丟棄。接收支援拆包、黏包；逾時的不完整封包會記錄並清除。串流無 checksum，無法保證從任意損壞資料中無歧義復原。

## 電腦 Web Bluetooth 操作

1. 先執行 bm7701.ps1 -Command Web，然後用電腦 Chrome 開啟 http://127.0.0.1:8771/。
2. 點「選擇 BM7701 並連線」，選 BM7701-ITAP。
3. FFF0 中預期 FFF1 支援 Notify、FFF2 支援 Write Without Response；以實際探索屬性為準。
4. FFF1 點「開始通知／指示」；FFF2 可用「寫入（無回覆）」測試 8735 → BM7701 方向。
5. 上方 UART 控制區可呼叫同一個腳本設定名稱、廣播、功率、發送 FFF1 與 counter。
6. 其他服務可輸入 1800、1801、180A、180F 後重新連線探索；不支援的操作不顯示。

日誌最多 2,000 筆，超過丟棄最舊資料；長測試定期匯出。UTF-8 顯示只供觀察，binary 比對以 HEX 為準。頁面只針對開發測試，不包含感測器解析或背景自動重連。

## 可重現驗收

| 項目                | 操作與通過條件                                                                                        |
| ------------------- | ----------------------------------------------------------------------------------------------------- |
| 離線 UART           | 執行`test-bm7701.ps1`：編碼、非法輸入、各拆包邊界、交錯事件、真正命令等待迴圈、錯誤 ACK、逾時都通過 |
| 離線頁面            | 點「離線自我檢查」顯示 PASS；只驗證轉碼與比對邏輯，不算硬體測試                                       |
| 電腦 BLE 收資料    | FFF1 訂閱後點「比對下一筆」，上方 UART 控制區送固定 10 bytes；頁面顯示 PASS，匯出比對日誌             |
| 電腦 BLE 發資料    | FFF2 寫固定 10 bytes；腳本事件日誌必須顯示相同 BLE WRITE HEX                              |
| 取消／恢復訂閱      | 停止 FFF1 通知後再送固定資料，電腦筆數不增加；重新訂閱再傳必須收到                                    |
| 100 筆              | 頁面開始驗證後按 UART counter；完成後點結束，須 100/100、零缺包、零重複、零亂序、零錯誤             |
| 斷線／重連          | 電腦斷線，必要時由 UART 重新開廣播，再連線、重新訂閱，重做固定資料比對                            |
| 其他 Characteristic | 依实际可用屬性逐一操作，記錄結果；被限制、不存在、未操作都標示未測試                                  |

**UART ACK 不代表 BLE 已到達。** Write Without Response 在 GATT 層沒有對端回覆，必須以 UART 收到資料佐證。測試紀錄見 `TEST-RESULTS.md`。

## 來源

- [官方 BM7701 函式庫](https://github.com/BestModules-Libraries/BM7701-00-1)：依 V1.0.3 `sendData`，`77 LEN 20 F1 FF DATA`，不使用舊 `writeData` 的額外 B2 byte。
- [Chrome Web Bluetooth](https://developer.chrome.com/docs/capabilities/bluetooth)：安全來源、使用者選擇、optionalServices 與訂閱流程。
- 專案既有 `docs/ble/BM7701_UART_直接操作與燒錄說明.md`。

## 補充操作

- 在額外 Service UUID 輸入 `1800,1801,180A,180F`，連線後可按「讀取所有可讀值」。本次模組共探索到 16 個 Characteristic，其中 14 個可讀，均已讀取成功。
- Web 模式按 Ctrl+C 結束，會關閉 localhost listener 與 UART；重新載入網頁前先斷開舊的 Bluetooth 連線。
