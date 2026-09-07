# BM7701 UART 控制與平板 BLE 測試

Windows PowerShell 5.1 + .NET SerialPort；平板使用 Chrome Web Bluetooth。無 npm 或 Python 第三方套件。腳本位於 `F:\Universitiy\ITAP\firmware\remote_sensor\scripts`。

## 接線與前提

- USB-TTL 必須是 3.3 V I/O，TX → BM7701 UART1_RX（Pin 22）、RX → UART1_TX（Pin 23），共地，供電 2.0–3.6 V。不可接 RS-232 或 5 V UART。
- 模組已正常啟動；預設 115200、8N1、無流控。Reset 後至少等 60 ms。腳本不切換波特率、不拉動 Reset，DTR／RTS 關閉；不要把控制線接成不明的 Reset 電路。
- 本次設備：COM11、Android 平板 ADB `95595018`。可用參數覆寫；一次只有一個程式能開啟 COM11。
- 平板 USB 偵錯已授權，藍牙開啟，Chrome 需「附近裝置」權限。第一次連線須在平板裝置選擇器選 BM7701-ITAP 並配對。

## 快速開始

在 PowerShell 中：

```powershell
Set-Location 'F:\Universitiy\ITAP\firmware\remote_sensor\scripts'
powershell -NoProfile -File .\test-bm7701.ps1
powershell -NoProfile -File .\bm7701.ps1 -Command Ports
powershell -NoProfile -File .\bm7701.ps1 -Command Version -Port COM11
powershell -NoProfile -File .\bm7701.ps1 -Command Name -Value BM7701-ITAP
powershell -NoProfile -File .\bm7701.ps1 -Command Advertise -Value on
powershell -NoProfile -File .\start-tablet-test.ps1
```

啟動腳本使用 Python 的 `http.server`，只將 HTML 複製到暫存目錄供應，綁定 `127.0.0.1:8771`，透過 `adb reverse` 在平板開啟 `http://localhost:8771/ble-test.html`。HTTP 不提供 UART API，也不公開專案與日誌。

保持啟動視窗執行；Ctrl+C 清除本次的 HTTP 子程序、USB reverse 與暫存頁面。`-Seconds 60` 可限時執行。既有 reverse 或 TCP 埠占用時拒絕覆寫，可用 `-HttpPort 8772`。修改 HTML 後須重啟啟動腳本，再重新整理平板。

```powershell
powershell -NoProfile -File .\start-tablet-test.ps1 -Device 95595018 -HttpPort 8771 -AdbPath 'C:\itap-tools\android-sdk\platform-tools\adb.exe' -PythonPath python
```

## UART 操作

另開 PowerShell，開啟互動模式，輸入下列指令；等待輸入時仍持續接收 BLE 事件。

```powershell
powershell -NoProfile -File .\bm7701.ps1 -Command Shell -Port COM11 -LogPath "$env:TEMP\bm7701-uart.log"
```

| 互動指令 | 用途 |
|---|---|
| `version` / `status` | 讀版本／連線狀態，保留原始回覆 |
| `name BM7701-ITAP` | 設定名稱，1–31 UTF-8 bytes |
| `advertise on` / `advertise off` | 開啟／停止廣播 |
| `power 0` 至 `power 15` | 設定模組功率代碼；不將代碼宣稱為 dBm |
| `send 00 01 02 03 04 05 06 07 08 09` | 以 FFF1 發送原始 bytes |
| `text Hello BM7701` | 發送 UTF-8 |
| `counter` | 發送 100 筆 10-byte 序號資料，預設間隔 100 ms |
| `save` | **明確寫 Flash**；一般測試不需要，不自動重置 |
| `quit` | 關閉串口 |

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

## 平板 Characteristic 操作

1. 點「選擇 BM7701 並連線」，選 `BM7701-ITAP`。
2. FFF0 中預期 FFF1 支援 Notify、FFF2 支援 Write Without Response。以實際探索屬性為準，不硬套操作。
3. FFF1 點「開始通知／指示」；通知會顯示 HEX、UTF-8、時間戳與筆數。通知與指示由瀏覽器／Characteristic 決定，並非可任選協定模式。
4. FFF2 點「寫入（無回覆）」，寫入共用資料欄。先在電腦開啟 Receive 或 Shell，才不會漏掉測試資料。
5. 其他服務可在連線前輸入 UUID，例如 `180A, 180F`。不存在或被 Chrome 阻擋會留在日誌；不能以此頁證明模組沒有其他 GATT。Chrome 的 blocklist 與服務授權不繞過。
6. 依屬性提供 Read、Write、Write Without Response、訂閱／取消；不支援的操作不顯示。重新連線會清除舊參照，必須重新訂閱。

日誌最多 2,000 筆，超過丟棄最舊資料；長測試定期匯出。UTF-8 顯示只供觀察，binary 比對以 HEX 為準。頁面只針對開發測試，不包含感測器解析或背景自動重連。

## 可重現驗收

| 項目 | 操作與通過條件 |
|---|---|
| 離線 UART | 執行 `test-bm7701.ps1`：編碼、非法輸入、各拆包邊界、交錯事件、真正命令等待迴圈、錯誤 ACK、逾時都通過 |
| 離線頁面 | 點「離線自我檢查」顯示 PASS；只驗證轉碼與比對邏輯，不算硬體測試 |
| 電腦 → 平板 | FFF1 訂閱後點「比對下一筆」，電腦送固定 10 bytes；頁面顯示 PASS，匯出比對日誌 |
| 平板 → 電腦 | 電腦 Receive／Shell 中，FFF2 寫固定 10 bytes；UART `BLE WRITE` 必須逐 byte 相同 |
| 取消／恢復訂閱 | 停止 FFF1 通知後再送固定資料，平板筆數不增加；重新訂閱再傳必須收到 |
| 100 筆 | 頁面開始驗證後電腦 Counter；完成後點結束，須 100/100、零缺包、零重複、零亂序、零錯誤 |
| 斷線／重連 | 平板斷線，必要時電腦重新開廣播，再連線、重新訂閱，重做固定資料比對 |
| 其他 Characteristic | 依实际可用屬性逐一操作，記錄結果；被限制、不存在、未操作都標示未測試 |

**UART ACK 不代表 BLE 已到達。** Write Without Response 在 GATT 層沒有對端回覆，必須以 UART 收到資料佐證。測試紀錄見 `TEST-RESULTS.md`。

## 來源

- [官方 BM7701 函式庫](https://github.com/BestModules-Libraries/BM7701-00-1)：依 V1.0.3 `sendData`，`77 LEN 20 F1 FF DATA`，不使用舊 `writeData` 的額外 B2 byte。
- [Chrome Web Bluetooth](https://developer.chrome.com/docs/capabilities/bluetooth)：安全來源、使用者選擇、optionalServices 與訂閱流程。
- 專案既有 `docs/ble/BM7701_UART_直接操作與燒錄說明.md`。

## 補充操作

- 在額外 Service UUID 輸入 `1800,1801,180A,180F`，連線後可按「讀取所有可讀值」。本次模組共探索到 16 個 Characteristic，其中 14 個可讀，均已讀取成功。
- 啟動脚本可按 **Q** 或 Ctrl+C 正常退出。外部強制終止／關閉程序可能來不及清理；確認已無本次服務後，可執行 `adb -s 95595018 reverse --remove tcp:8771` 移除其殘留轉送。不要移除其他工作的轉送。
- `-NoOpen` 只啟動 HTTP／USB reverse，不另開 Chrome 分頁；可手動重新整理原頁面。重啟前先在舊頁斷線，避免多個分頁同時連接模組。
