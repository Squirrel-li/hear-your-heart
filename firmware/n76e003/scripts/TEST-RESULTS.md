# BM7701 實測結果

測試日期：2026-09-06（Asia/Taipei）。電腦 Windows PowerShell 5.1、USB-TTL COM11、115200 8N1；Android 15 平板 Chrome 152，BM7701 名稱以 RAM 設為 `BM7701-ITAP`。本次未執行 Flash save、韌體更新或硬體 Reset。

## 已通過

| 項目 | 證據 |
|---|---|
| UART 版本 | TX `77 03 00 20 00`；RX `78 13 00 20 00 03 07 01 00 00 A0 01 00 00 6E 00 00 E0 00 01 00` |
| 名稱設定／廣播 | Type 0005、0007 均回 status 00；平板看到並連上 BM7701-ITAP |
| FFF1 Notification | 固定資料 `00 01 02 03 04 05 06 07 08 09` 逐 byte 比對 PASS |
| FFF2 Write Without Response | UART RX `78 0D 00 F2 FF 00 01 02 03 04 05 06 07 08 09`，資料完全一致 |
| 取消訂閱 | 平板筆數未增加；發送指令回覆 `78 03 26 F1 FF`，腳本回傳 exit 1，未誤判成功 |
| 恢復訂閱 | 再送固定資料，平板逐 byte 比對 PASS |
| 100 筆序號 | 100/100，錯誤 0、重複 0、亂序 0、缺少 0；平板已匯出結果 |
| 斷線／重新連線 | 舊項目清除，新連線通知筆數為 0；重新訂閱後收到固定資料 |
| 通用 Read | 14 個可讀 Characteristic 依序讀取成功；Read 事件未增加 Notification 計數 |
| Indication 訂閱介面 | 2A05 可開始訂閱；本次沒有刻意觸發 Service Changed，不能宣稱已驗證 Indication 到達 |
| 日誌匯出 | 平板成功下載 `.log.txt`；經 ADB 讀回核對 |
| 互動 Shell | `version` 收到實機回覆，`quit` 正常結束 |
| 離線檢查 | UART 封包、拆包／黏包、交錯事件、錯誤 ACK、逾時及输入驗證；頁面 HEX／UTF-8／UUID／counter 自我檢查通過 |
| 啟動與清理 | 限時退出與互動 Q 退出成功；HTTP 埠、reverse 及本次暫存目錄清除 |
| 衝突保護 | 第二個啟動程序拒絕覆寫既有 reverse，原服務仍回 HTTP 200 |

100 筆記錄於平板 UTC 2026-09-05T20:09:20.530Z 顯示：

```text
PASS | 100/100 | 錯誤 0 | 重複 0 | 亂序 0 | 缺少 0
```

## 探索與讀取結果

指定 FFF0、1800、1801、180A、180F，共找到 16 個 Characteristic。以下為本次模組回傳值，不推論其他韌體版本相同。

| Service | Characteristic | Read 回傳 HEX／文字 |
|---|---|---|
| FFF0 | FFF1 | Notify；無 Read |
| FFF0 | FFF2 | Write Without Response；無 Read |
| 1800 | 2A00 | `42 4D 37 37 30 31 2D 49 54 41 50`，BM7701-ITAP |
| 1800 | 2A01 | `00 00` |
| 1800 | 2A02 | `00` |
| 1800 | 2A04 | `06 00 06 00 00 00 F4 01` |
| 1801 | 2A05 | `01 00 FF FF` |
| 180A | 2A23 | `00 00 00 00` |
| 180A | 2A24 | `BC7701` 後接 10 個空白 |
| 180A | 2A26 | `0.01` |
| 180A | 2A27 | `0.01` |
| 180A | 2A28 | `0.01` |
| 180A | 2A29 | `Holtek` 後接 10 個空白 |
| 180A | 2A2A | `00 00 00 00` |
| 180A | 2A50 | `00 00 00 00 00 00 00` |
| 180F | 2A19 | `64`（模組提供的原始值，未接入真實電池量測） |

讀取所有可讀值日誌：UTC 2026-09-05T20:22:23.350Z 至 20:22:24.570Z，共 14 次 Read 成功。

## 限制與未測試

- Read 與 FFF2 Write Without Response 已實機驗證；其他 Characteristic 的 Write With Response 尚未實機寫入驗證。
- 未測試 Flash 保存、功率設定的 RF 效果、超過 20 bytes 的資料、長時間壓力測試、實際感測器資料或 Service Changed 的 Indication 到達。
- 此清單只涵蓋指定服務與瀏覽器允許的項目，不是對所有可能 GATT UUID 的全面盤點。
- 外部工具強制中止程序可略過 finally；早期強制中止留下的 reverse／暫存目錄已另外清除。互動模式改以 Ctrl+C 當按鍵，另提供 Q，避免正常退出時取消清理命令。
- 100 筆測試是短時間 bench test，不代表任意 MTU、資料率或環境下都不丟包。
