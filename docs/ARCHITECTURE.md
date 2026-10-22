# ITAP 系統架構與敘述核對

核對日期：2026-10-20（手錶變體與心率來源同步自 AGENTS.md）。需求依根目錄 [AGENTS.md](../AGENTS.md)，實作依目前工作區原始碼。本次為靜態核對，未重新編譯、燒錄或執行硬體測試；有程式路徑不等於已通過實機驗收。

## 三種裝置與責任邊界

| 裝置 | 工作 | 負責範圍 |
| --- | --- | --- |
| HUB8735 Ultra | 接收資料、心率／運動分析、曲庫分析、選曲、播放、操作介面 | 本專案 |
| 穿戴－手錶，N76E003 | BMH08002-4 PPG、LSM6DS3 IMU、心率、OLED／按鍵；經 BM7701 傳送 BLE | 本專案，`ble_sensor`、`ble_sensor_hr`、`ble_sensor_motion` 三個變體都是手錶 |
| 穿戴－綁帶 | 提供外部感測資料 | 其他人實作；本專案保留 HUB 接收與整合，不指定其內部硬體／演算法 |

使用者已確認手錶型號為 N76E003。BM7701 是 BLE 模組。程式中的 Chest／BLE_sensor_ITAP 表示外部綁帶來源，不表示此倉庫維護綁帶韌體。舊 ePy Lite／MAX30103 方案不是本專案交付承諾。

`apps/remote_sensor_simulator` 為可扮演 Watch／Chest 的測試工具，不是第四種產品裝置；`apps/phone` 不納入。TFT 觸控 UI 位於 examples，目前正式主程式仍是 SH1106 OLED。`ble_sensor_capture`（`BLE_capture_ITAP`，HUB 不自動連線）與 `apps/bmh08002_ppg_analyzer` 是心率演算法的擷取／回放驗證工具；`ble_sensor_keil` 是舊副本，不在範圍內。

**心率來源**：一般由手錶或綁帶直接在 PPG 封包提供心率；HUB 使用 0x05 原始 PPG 批次時，改由 HUB 計算心率。BLETask 驗證 0x05 長度後逐筆送 `ppgRawQueue`，analysisTask 以 `ppg_raw_hr.h`（手錶 `ppg_hr.h` 的副本）每 10 秒算一次；結果有效時優先於 0x01 心率，無效或逾時才用 0x01。

## 模組與資料流

| 模組 | 職責及主要資料流 |
| --- | --- |
| BLETask | 同時管理 Watch／Chest 連線；notificationCB → bleRxFrameQueue → 解析／來源篩選 → 感測 Queue；0x05 逐筆送 ppgRawQueue |
| analysisTask | 消費 PPG／ACC／GYRO；執行本地感測輪詢、心率 Zone／目標 BPM、Demo 強度；不控制播放 |
| controlTask | 消費目標 BPM、強度、UI 指令及播放事件；持有模式、手動優先、課表、待機與選曲狀態 |
| audioTask | 應用層 SD 與 MAX98357 的唯一操作端；載入曲庫、播放、分析並回報狀態 |
| UITask | OLED／按鍵、動畫與狀態顯示；播放等指令送 controlTask，BLE 操作送 BLETask |

| Queue | 生產者 → 消費者 |
| --- | --- |
| bleRxFrameQueue | BLE notificationCB → BLETask |
| ppgSensorQueue／accSensorQueue／gyroSensorQueue | BLETask 或 analysisTask 內的 localSensorPoll → analysisTask |
| ppgRawQueue | BLETask（0x05 逐筆）→ analysisTask 的 HUB 心率演算法 |
| targetBpmResultQueue／motionLevelQueue | analysisTask → controlTask |
| analysisCmdQueue | controlTask → analysisTask，切換 PPG／IMU 來源 |
| uiCmdQueue | UITask → controlTask |
| bleCmdQueue | UITask → BLETask |
| audioCmdQueue | controlTask → audioTask |
| audioPlaybackEventQueue | audioTask → controlTask，自動播放的完成／失敗 |
| uiUpdateQueue | BLETask／analysisTask（含本地感測）／controlTask／audioTask → UITask |
| bleTxQueue | 已建立但尚無應用生產者／消費者，不能當成已完成下傳功能 |

曲庫由 audioTask 載入共享 RAM，controlTask／UITask 讀取 metadata；分析進度與結果送 UI，不經播放事件 Queue。序列埠由 controlTask 直接讀取，不經 uiCmdQueue。

## 目前功能

- **Enjoy**：有效心率 → 穩定 Zone → 目標 BPM → 選曲。有效樣本為 status=2、HR 非零，最多保留 15 秒；換 Zone 需跨界 2% HRmax 並維持 10 秒。目標每秒更新，上升最多 5 BPM、下降立即。未取得有效心率時等待；失效後保留最後目標，手動控制優先。
- **Train**：內建 11 分與 5 分課表，順序 Z2 → Z4 → Z2 → Z5 → Z1。依段落 Zone 中點心率選曲，不依實際心率；換段立即選歌。手動暫停／停止凍結進度，同課表可續接，完成後停止。
- **Manual**：手動選歌、播放／暫停／恢復／停止；未分析 BPM 的歌也能手動播放。
- **Demo**：動態加速度分 0–4 級，新等級持續約 0.5 秒後切換 `/demo/DEMO_nn.mp3`。這些歌曲不加入一般曲庫與分析。本地 IMU 每 80 ms 讀取，手錶每秒上報；不推定外部綁帶頻率。
- **曲庫／分析**：從 SD 的 music_library.json 載入，最多 1024 首。使用者可選新檔／未知 BPM 掃描或全部重分析，後者可重跑未變更歌曲；依序跑 BPM（`bpm_tempo.h`，2026-10-22 取代 MiniBPM）及 SDK YAMNet 曲風分析，結果存入曲庫；使用者可在曲庫清單或播放中把單曲 BPM x2／/2 修正，「全部重分析」會覆蓋修正。播放或暫停中請求直接拒絕；掃描中播放會取消分析，已存結果保留。寫入採 tmp／bak 換檔與備份回復。
- **選曲**：有有效 BPM 即可參與自動選曲，曲風失敗不阻擋。目前只比較 BPM。首次／換 Zone 找目標 ±5% 內曲目，沒有則取最近；自然播畢無符合者可重播原曲。失敗曲目跳過，全部不可用則停止。未來可加入使用者的「期望曲風」選項。
- **來源與連線**：PPG／IMU 各自可選 Chest、Watch、Local。BLE 支援兩條連線及各自 Auto／Manual；Auto 尚未連上時重試，連上後失聯轉 Manual；使用者可重新 Connect 或啟用 Auto。
- **待機**：停止播放／分析、停用本地感測、斷開 BLE 並停掃描、關 OLED；長按 OK 喚醒，保留設定但不自行恢復播放。進度條目前使用動畫加曲庫就緒門檻，15 秒逾時可離開動畫。
- **手錶變體**：ble_sensor 使用模組心率；ble_sensor_hr 使用自身 PPG 心率演算法，10 秒非重疊週期、首週期暖機，另送 0x15 原始 PPG 批次；ble_sensor_motion 使用參考 ACC 的抗運動心率試驗演算法，約每 5 秒更新，ACC 讀 26 Hz、上報 1 Hz，OLED 停用，尚未燒錄驗證。三者皆為 BLE_watch_ITAP、source=1。ble_sensor_motion 與 ble_sensor_capture 以 `#include` 共用 ble_sensor 的 main.c，修改該檔會同時影響三者。心率計算週期與通知頻率應分別描述。

程式入口為 [embedded_loudspeaker](../firmware/hub8735-ultra/application/embedded_loudspeaker/) 的 BLETask、analysisTask、ControlTask、audioTask、UITask、localSensor、song_catalog、train_plan、motion_level、music_library、music_genre_classifier，以及 [手錶 application](../firmware/n76e003/application/)。

## BLE 整合現況

HUB 依名稱辨識 BLE_watch_ITAP／BLE_sensor_ITAP，先探索 FFF0／FFF1 notify／FFF2 write，失敗再試 524CACC0-3C17-D293-8E48-14FE2E4DA212／D002 notify／D001 write。兩組服務不是 UUID 別名；發現 write characteristic 不代表已完成控制命令下傳。

Frame 為 `sensor_time_ms 4B | msgType 1B | sequence 4B | fields`，多位元組 Big-endian。msgType 高 4 bit 是來源（0=Chest、1=Watch），低 4 bit 是類型：01 PPG 20B、02 ACC+GYRO 21B、03 ACC 15B、04 GYRO 15B、05 PPG 原始批次 10+4N B；00 保留且忽略。手錶實際送出的類型位元組為 0x11／0x12，`ble_sensor_hr` 另送 0x15（N=2，18B），`ble_sensor_motion` 送 0x11／0x13。

21B 通知要求 ATT MTU 至少 24；其他 central 曾收到 BM7701 的 25B 通知，不足以證明 HUB 與外部綁帶的端到端相容性。綁帶內部實作不在範圍內，其名稱、來源碼、資料格式與連線行為仍需整合驗證。

## 敘述衝突與需求落差

| 原敘述／位置 | 核對結果與處理 |
| --- | --- |
| AGENTS：BM7701 是感測端、N76E003 胸帶 | 已改為三裝置責任邊界；N76E003 是手錶 MCU，BM7701 是其 BLE 模組，綁帶外部實作 |
| AGENTS：BPM 只有獨立範例、尚未整合 | 已修正；audioTask::advanceBpmScan 已串接 BPM 與曲風階段 |
| HRMAX／YAMNET／記憶：BPM 選單不跑 NPU、曲風完全未實作 | 主程式已有分類與儲存；自訂分類頭仍屬提案，不宣稱分類品質已驗收 |
| HRMAX：播放中分析排隊、PPG 保留 6 秒 | 現況為拒絕／Cancelled、有效 PPG 保留 15 秒；已修正摘要 |
| 舊架構／DEMO：ble_sensor 是胸帶、本地 IMU 1Hz | 三個 N76E003 變體都是手錶；本地 IMU 12.5Hz，手錶上報 1Hz |
| 需求：使用 0x05 原始 PPG 時由 HUB 計算心率 | 已實作（2026-10-22），只在主機以模擬器錄音回放驗證；OLED 心率仍顯示 0x01 的值 |
| ble_sensor_motion 運動中心率 | 公開跑步資料有效輸出比例仍低，未達運動中可靠輸出，也未燒錄驗證 |
| BLE 快速參考與早期感測計畫 | 部分只列單一服務、無條件自動重連，或提議 Header／CRC；與目前雙服務探索、連線模式與實際封包不同，不能直接當成現行契約 |
| 架構圖 prompt／DFD／ePy Lite 資料 | 仍有歷史硬體假設與舊資料流；已標示參考地位，不據此把外部綁帶列入實作範圍 |
| 需求：新增／替換自動分析、播放中延後處理 | 未完成；目前只有手動掃描會比對檔案身分 |
| 未來功能：使用者期望曲風 | 曲風已有儲存；選項與曲風選曲尚未接入，目前以 BPM 選曲 |
| 自動選曲資格 | 使用者確認有效 BPM 即可；曲風失敗仍可選入，與目前程式一致 |
| 需求：替換不可沿用舊結果 | 掃描以大小／FAT 修改時間比對；同大小同時間可能漏檢，掃描前也尚未發現替換 |
| 手動重分析 | 使用者確認保留目前「全部重分析」，可重分析未變更歌曲 |
| 需求：開機進度反映實際進度 | 目前僅曲庫就緒門檻與動畫／逾時，不是完整進度量測 |

## 待決與驗證

已確認：保留手動全部重分析；自動發現新檔與播放中延後分析列為未完成；有效 BPM 即可自動選曲；曲風分析結果存入 JSON，未來可新增「期望曲風」選項。未來實作曲風選曲時，選項如何對應分析類別及無符合曲目時如何回退，屆時再決定。

硬體驗證需另有證據：雙 BLE 長時間連線與 MTU、外部綁帶相容性、三個手錶變體的心率／記憶體與重燒結果、NPU 模型載入／記憶體／分類品質、播放與待機喚醒。編譯或主機自測不能代替這些驗收。
