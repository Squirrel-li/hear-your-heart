# ITAP 系統架構與敘述核對

核對日期：2026-10-27。需求依根目錄 [AGENTS.md](../AGENTS.md)，實作依目前工作區原始碼。本次為靜態核對加 arduino-cli 編譯與主機檢查，未燒錄或執行硬體測試；有程式路徑不等於已通過實機驗收。

## 三種裝置與責任邊界

| 裝置 | 工作 | 負責範圍 |
| --- | --- | --- |
| HUB8735 Ultra | 接收資料、心率／運動分析、曲庫分析、選曲、播放、操作介面 | 本專案 |
| 穿戴－手錶，N76E003 | BMH08002-4 PPG、LSM6DS3 IMU、心率、OLED／按鍵；經 BM7701 傳送 BLE | 本專案，`ble_sensor`、`ble_sensor_hr`、`ble_sensor_motion`、`ble_sensor_demo` 四個變體都是手錶 |
| 穿戴－綁帶 | 提供外部感測資料 | 其他人實作；本專案保留 HUB 接收與整合，不指定其內部硬體／演算法 |

N76E003 是手錶 MCU，BM7701 是其 BLE 模組。程式中的 Chest／BLE_sensor_ITAP 表示外部綁帶來源，不表示此倉庫維護綁帶韌體。舊 ePy Lite／MAX30103 方案不是本專案交付承諾。

`apps/remote_sensor_simulator` 為可扮演 Watch／Chest 的測試工具，不是第四種產品裝置；`apps/phone` 不納入。TFT 觸控 UI 位於 examples，正式主程式是 SH1106 OLED。`ble_sensor_capture`（`BLE_capture_ITAP`，HUB 不自動連線）與 `apps/bmh08002_ppg_analyzer` 是心率演算法的擷取／回放驗證工具；`ble_sensor_keil` 是舊副本，不在範圍內。

**心率來源**：一般由手錶或綁帶直接在 PPG 封包提供心率；HUB 收到 0x05 原始 PPG 批次時改由 HUB 計算。BLETask 驗證 0x05 長度後逐筆送 `ppgRawQueue`，analysisTask 以 `ppg_raw_hr.h`（手錶 `ppg_hr.h` 的同一演算法）每 10 秒算一次；結果有效時優先於 0x01 心率，無效或逾時（15 秒）才用 0x01。

## 模組與資料流

| 模組 | 職責及主要資料流 |
| --- | --- |
| BLETask | 同時管理 Watch／Chest 連線；notificationCB → bleRxFrameQueue → 解析／來源篩選 → 感測 Queue；0x05 逐筆送 ppgRawQueue |
| analysisTask | 消費 PPG／ACC／GYRO；本地感測輪詢、心率 Zone／目標 BPM（`heart_zone.h`）、DEMO 強度（`motion_level.h`）；不控制播放 |
| controlTask | 消費目標 BPM、強度、UI 指令及播放事件；持有模式、暫停、手動優先、課表、待機、選曲與年齡設定（Flash） |
| audioTask | 應用層 SD 與 MAX98357 的唯一操作端；載入曲庫、播放、在播放空檔分析並回報狀態 |
| UITask | OLED／按鍵、動畫與狀態顯示；播放等指令送 controlTask，BLE 操作送 BLETask |

| Queue／共享變數 | 生產者 → 消費者 |
| --- | --- |
| bleRxFrameQueue | BLE notificationCB → BLETask |
| ppgSensorQueue／accSensorQueue／gyroSensorQueue | BLETask 或 analysisTask 內的 localSensorPoll → analysisTask |
| ppgRawQueue | BLETask（0x05 逐筆）→ analysisTask 的 HUB 心率演算法 |
| targetBpmResultQueue／motionLevelQueue | analysisTask → controlTask（強度未知時送 `MOTION_LEVEL_NONE`） |
| analysisCmdQueue | controlTask → analysisTask，切換 PPG／IMU 來源 |
| uiCmdQueue | UITask → controlTask |
| bleCmdQueue | UITask → BLETask |
| audioCmdQueue | controlTask → audioTask |
| audioPlaybackEventQueue | audioTask → controlTask，自動播放的完成／失敗 |
| uiUpdateQueue | BLETask／analysisTask／controlTask／audioTask → UITask |
| `audioPlaybackState` | audioTask 寫 → controlTask 讀（暫停判斷） |
| `musicLibraryRevision` | audioTask 在歌曲取得 BPM 時遞增 → controlTask 讀（替換隨機歌） |
| `userInfo.age` | controlTask 寫（OLED 設定、Flash 載入）→ analysisTask／controlTask 讀 HRmax |
| `systemStandby` | controlTask 寫 → BLETask／analysisTask 讀 |
| bleTxQueue | 已建立但尚無應用生產者／消費者，不能當成已完成下傳功能 |

曲庫由 audioTask 載入共享 RAM，controlTask／UITask 讀取 metadata；分析進度與結果送 UI，不經播放事件 Queue。序列埠由 controlTask 直接讀取，不經 uiCmdQueue。

## 目前功能

- **Enjoy**（開機與喚醒的預設）：有效心率 → 穩定 Zone → 目標 BPM → 選曲。Zone 對應依專題規劃書：Z1 放鬆 60–80、Z2 心率、Z3 心率＋5%、Z4 心率－20%、Z5 心率－30%（進入 Z5 不等駐留）。有效樣本為 status=2、HR 非零，最多保留 15 秒；換 Zone 需跨界 2% HRmax 並維持 10 秒。目標每秒更新，上升最多 5 BPM、下降立即。未取得有效心率時等待；失效後保留最後目標；待機清除目標。
- **Train**：內建 11 分與 5 分課表，順序 Z2 → Z4 → Z2 → Z5 → Z1。依段落 Zone 中點心率選曲，不依實際心率，也不套用 Enjoy 的降速；換段立即選歌。暫停凍結進度，Resume 接續；停止／手動選歌後再選同課表可續接，完成後停止。
- **Manual**：手動選歌、播放／暫停／恢復／停止；未分析 BPM 的歌也能手動播放。
- **Demo**：動態加速度分 0–4 級，新等級持續約 0.5 秒後切換 `/demo/DEMO_nn.mp3`；IMU 5 秒無資料時強度未知，播完不重播。這些歌曲不加入一般曲庫與分析。本地 IMU 每 80 ms 讀取，手錶每秒上報；不推定外部綁帶頻率。
- **暫停**：自動模式按 Pause 仍維持原模式；暫停中不換歌、DEMO 不切換、課表凍結，Resume 後執行累積的換歌。Stop 或手動選歌才轉 Manual。
- **曲庫／分析**：從 SD 的 music_library.json 載入，最多 1024 首。開機自動跑「新檔／未知」掃描（需要曲風時於開機畫面載入 NPU 模型）；使用者可另選新檔／未知、全部重分析或清除全部結果。分析與播放同時進行、播放優先。依序跑 BPM（`bpm_tempo.h`）及 SDK YAMNet 曲風分析，結果存入曲庫；使用者可在曲庫清單或播放中把單曲 BPM x2／/2 修正（掃描中也可），「全部重分析」會覆蓋修正。寫入採 tmp／bak 換檔與備份回復。
- **選曲**：有有效 BPM 即可參與自動選曲，曲風失敗不阻擋，目前只比較 BPM。首次／換 Zone 找目標 ±5% 內曲目，沒有則取最近；自然播畢無符合者可重播原曲。沒有任何歌有 BPM 時隨機播一首，有歌取得 BPM 後替換。失敗曲目跳過，全部不可用則停止。
- **年齡**：OLED `Age` 設定 6～99（預設 25），存於 HUB 內部 Flash 0xFD0000（分割表 mp 區之後的使用者區），開機載入。
- **來源與連線**：PPG／IMU 各自可選 Chest、Watch、Local（預設 PPG=Watch、IMU=Local）。BLE 支援兩條連線及各自 Auto／Manual；Auto 未連線時持續重試，失聯後也自動重連；使用者 Disconnect 或切 Manual 後才停止重試。依名稱連第一個找到的裝置，同一空間只使用一套（使用者接受的限制）。
- **待機**：停止播放／分析、停用本地感測、斷開 BLE 並停掃描、關 OLED、清除目標 BPM；長按 OK 喚醒並回到 Enjoy。開機進度條依初始化階段跳段（20／40／70／90／100%），最短 3 秒、30 秒逾時。
- **手錶變體**：ble_sensor 使用模組心率；ble_sensor_hr 使用自身 PPG 心率演算法（10 秒非重疊週期、首週期暖機），另送 0x15 原始 PPG 批次；ble_sensor_motion 使用參考 ACC 的抗運動心率試驗演算法，約每 5 秒更新，ACC 讀 26 Hz、上報 1 Hz，尚未燒錄驗證；ble_sensor_demo 同 motion，開機按住 P3.0 改播 Flash 內預錄 PPG。OLED 以 P3.0 切換頁面（demo 為文字頁＋波形頁），capture 只有波形頁。四者皆為 BLE_watch_ITAP、source=1。motion、demo 與 capture 以 `#include` 共用 ble_sensor 的 main.c，修改該檔會同時影響四者。

程式入口為 [embedded_loudspeaker](../firmware/hub8735-ultra/application/embedded_loudspeaker/) 與 [手錶 application](../firmware/n76e003/application/)。主機檢查在 `firmware/hub8735-ultra/tests/`（heart_zone、motion_level、train_plan、ppg_raw_hr、bpm_tempo）與 `song_catalog.h` 的 `SONG_CATALOG_SELF_TEST`。

## BLE 整合現況

HUB 依名稱辨識 BLE_watch_ITAP／BLE_sensor_ITAP，先探索 FFF0／FFF1 notify／FFF2 write，失敗再試 524CACC0-3C17-D293-8E48-14FE2E4DA212／D002 notify／D001 write。兩組服務不是 UUID 別名；發現 write characteristic 不代表已完成控制命令下傳。

Frame 為 `sensor_time_ms 4B | msgType 1B | sequence 4B | fields`，多位元組 Big-endian。msgType 高 4 bit 是來源（0=Chest、1=Watch），低 4 bit 是類型：01 PPG 20B、02 ACC+GYRO 21B、03 ACC 15B、04 GYRO 15B、05 PPG 原始批次 10+4N B；00 保留且忽略。手錶實際送出的類型位元組為 0x11／0x12，`ble_sensor_hr` 另送 0x15（N=2，18B），`ble_sensor_motion` 送 0x11／0x13。

21B 通知要求 ATT MTU 至少 24；其他 central 曾收到 BM7701 的 25B 通知，不足以證明 HUB 與外部綁帶的端到端相容性。綁帶內部實作不在範圍內，其名稱、來源碼、資料格式與連線行為仍需整合驗證。

## 已知落差

| 項目 | 現況 |
| --- | --- |
| 2026-10-27 變更（Zone 對應、暫停、隨機選歌、喚醒 Enjoy、年齡 Flash、分析進度頁 Back、IMU 失聯） | 只通過 arduino-cli 編譯與主機檢查，未實機驗證；Flash 扇區抹除可能短暫影響音訊 |
| 規劃書 Z5「語音安全警示」 | 未實作 |
| 播放與分析同時進行 | 依賴 `libhmp3.a` 內部配置（`static_assert` 核對），SDK 更新後要重新確認；單一步驟超過約 1.5 秒仍可能斷音 |
| HUB 以 0x05 自算心率 | 只在主機以模擬器錄音回放驗證；OLED 心率仍顯示 0x01 封包內的值 |
| ble_sensor_motion 運動中心率 | 公開跑步資料有效輸出比例仍低，未燒錄驗證 |
| 替換偵測 | 以大小／FAT 修改時間比對；同大小同時間可能漏檢 |
| 自動發現 | 只在開機時；開機後放入的檔案要手動掃描或重開機 |
| 期望曲風 | 曲風已儲存；選項與曲風選曲尚未接入 |
| 歷史文件 | BLE 快速參考、早期感測計畫、架構圖 prompt、DFD 舊版與 ePy Lite 資料含舊假設，不能直接當成現行契約 |

硬體驗證需另有證據：雙 BLE 長時間連線與 MTU、外部綁帶相容性、四個手錶變體的心率／記憶體與重燒結果、NPU 模型載入／記憶體／分類品質、播放與待機喚醒。編譯或主機自測不能代替這些驗收。
