# 系統架構圖產生 Prompt

> 2026-10-18 核對：本文是歷史繪圖輸入，含已過時的綁帶硬體、單一 BLE 連線及「不跑 NPU」敘述。再次使用時，必須依 AGENTS.md 與 ARCHITECTURE.md 重建現況，不能直接把下列細節當作最新要求。 現況見[架構核對](ARCHITECTURE.md)。

> 給任何 agent 使用。工作目錄：`F:\Universitiy\ITAP`。所有路徑皆相對於此目錄，agent 可直接讀取。

---

## 任務

為 ITAP 專案產出**一張**系統架構圖，同時呈現三個面向：

1. **硬體模組**：實體板子、晶片、感測器、外設，與它們之間的實體連線與介面。
2. **軟體流程**：HUB8735 韌體內的 FreeRTOS task 與其職責、控制優先順序、狀態切換。
3. **資料路徑**：資料從何處產生、經過什麼介面／Queue、轉成什麼、最後到哪裡。

輸出：Mermaid（`flowchart`，可用 subgraph 分層）寫入 `docs/ARCHITECTURE.md`，並附一段文字說明圖例。若 Mermaid 排版過於擁擠，可改輸出 SVG，但需另存 `.mmd` 原始碼。**不要**修改任何韌體或應用程式碼。

## 專案一句話

穿戴式心率感測 → HUB8735 計算目標 BPM → 依目標 BPM 自動選 MP3 播放（跑步／運動配樂）；未來可新增「期望曲風」選項。

## 先讀這些檔案（依序，事實以它們為準）

| 檔案 | 取得什麼 |
| --- | --- |
| `AGENTS.md` | 「已確認的架構規則」（需求，優先於現有程式）與範圍界定 |
| `.agents/MEMORY.md` | 已驗證的硬體事實、坑、決策 |
| `docs/hub8735/DFD.md` | 既有概念版 DFD（4 張 Mermaid），可沿用節點命名 |
| `docs/hub8735/HRMAX_MUSIC_FLOW.md` | task 分工與資料契約、Zone→目標 BPM、選歌規則 |
| `docs/remote_sensor/BLE_PROTOCOL_QUICKREF.md`、`firmware/n76e003/ble-notify-data-format.md` | BLE GATT 與 frame 格式 |
| `docs/remote_sensor/BM7701-00-1_SENSOR_PLAN.md` | 感測端規劃 |
| `docs/hub8735/YAMNET_GENRE.md` | NPU 曲風分析（提案） |
| `docs/ePy_Lite/problems.md` | 綁帶端 ePy Lite 方案 |
| `firmware/hub8735-ultra/application/embedded_loudspeaker/`（`embedded_loudspeaker.ino`、`globalData.h`、`BLETask.cpp`、`analysisTask.cpp`、`ControlTask.cpp`、`audioTask.cpp`、`UITask.cpp`、`localSensor.h`、`music_library.h`、`music_bpm_analyzer.h`、`music_genre_classifier.h`、`song_catalog.h`） | 實際 task／Queue／模組 |
| `firmware/hub8735-ultra/target_bpm.c/.h` | 心率→目標 BPM 純 C 模組 |

## 圖中必須包含的元素

### 硬體模組（以區塊／subgraph 分組，連線標明介面）

**A. 感測端（兩種形態，並列）**
- 手錶：BMH08002-4（PPG 模組）──UART──> N76E003（MCU，算心率、組封包）；LSM6DS3（IMU）──I2C──> N76E003；N76E003 ──UART──> BM7701（BLE 模組）；SSD1306/OLED 與按鍵接 N76E003。韌體在 `firmware/n76e003/application/ble_sensor_hr`（含板上心率）。
- 綁帶：MAX30103 ──I2C──> ePy Lite（內建 BLE，板上算心率，無 IMU）。
- 測試工具：`apps/remote_sensor_simulator`（Flutter Android，Kotlin 端作 GATT server 回放資料）以虛線標示為「測試用」，**不是**產品手機 App；`apps/phone` 暫不納入，不要畫。

**B. HUB8735 Ultra（主機板）**
- 本地感測（可選來源，與 BLE 二選一）：BMH08002-4 ──UART3 38400（D6/D5，EN=D7）；LSM6DS3 ──Wire1 I2C（D0/D1，0x6B）。
- BLE Central（連 `BM7701-ITAP`，自動重連每 3 s）。
- SD 卡（根目錄：`NNN.mp3` + `music_library.json`）。
- MAX98357 I2S 功放 ──> 喇叭（左右兩顆共用 EN=D15；D22 為 MCLK 不可佔用）。
- SH1106 128×64 OLED（Wire，D3/D4，0x3C）＋ 3 顆按鍵（pin 8/9/10，上／下／OK）。
- 序列埠（除錯與手動指令：play/stop/pause/resume/volume/auto）。
- NPU（曲風推論，見下方「狀態」）。

### 軟體流程（HUB8735 FreeRTOS，5 個 task + Queue）

task：`BLETask`、`analysisTask`、`controlTask`、`audioTask`、`UITask`。
Queue（名稱照 `globalData.h`）：`bleRxFrameQueue`、`ppgSensorQueue`、`accSensorQueue`、`gyroSensorQueue`、`targetBpmResultQueue`、`audioCmdQueue`、`audioPlaybackEventQueue`、`uiUpdateQueue`、`uiCmdQueue`、`analysisCmdQueue`、`bleTxQueue`。
**每條 Queue 都要標出「生產者 → 消費者」，方向不可畫錯**（以程式碼為準，不要憑名稱猜）。

需在圖或圖例中表達的行為：
- `analysisTask`：有效 PPG（`status==2` 且 HR≠0）→ HRmax → 穩定 Zone（越界 2% HRmax 並維持 10 s 才換區）→ 目標 BPM（1 Hz，上升每次最多 +5、下降立即）。只輸出目標 BPM，不碰播放。
- `controlTask`：自動／手動模式；手動（play/pause/stop）優先，只有明確 `auto`／選 Enjoy／Train 才恢復；目前依目標 BPM ±5% 內符合者選曲，否則最近 BPM；播放失敗跳過、全失敗則停止。
- `audioTask`：**唯一**呼叫 MAX98357 與**唯一**擁有 SD；回報 `Finished`／失敗；歌曲停止後才執行 BPM 掃描（Helix 解 MP3 → 單聲道 PCM → `bpm_tempo.h`，最多 60 s）。
- `UITask`：OLED 選單（Mode、Play/Pause、Stop、Volume、BPM Analysis、Songs）→ `uiCmdQueue` → `controlTask`。
- 感測斷線：已有有效心率後失聯，沿用最後目標 BPM 繼續播放。

### 資料路徑（每條邊標「資料內容＋介面／頻率」）

1. 感測 → 心率：PPG／ACC／GYRO →（BLE notify 或本地 UART/I2C）→ `ppgSensorQueue`／`acc`／`gyro`。
2. 心率 → 目標 BPM → `targetBpmResultQueue` → `controlTask` → `Play(filename)` → `audioCmdQueue` → `audioTask` → I2S → 喇叭。
3. 回饋：`audioPlaybackEventQueue`（播畢／失敗）→ `controlTask`。
4. 使用者：按鍵／序列埠 → `uiCmdQueue` → `controlTask`；`uiUpdateQueue` → OLED。
5. 曲庫：SD 的 `music_library.json`（檔名、BPM、大小、FAT 日期時間；上限 1024 首；BPM=0 不參加自動選曲；寫入走 `.tmp`→`.bak` 備份）→ `controlTask` 選曲。
6. 分析：使用者在 OLED 觸發 → `controlTask` 轉送 → `audioTask`（停止後）→ `bpm_tempo.h`（BPM）／NPU（曲風）→ 更新 `music_library.json` → 回報完成／失敗。

BLE frame（供邊上標註）：GATT service `524CACC0-3C17-D293-8E48-14FE2E4DA212`，notify `D002`（sensor→hub）、write `D001`（hub→sensor）；frame = `sensor_time_ms 4B | msgType 1B | payload`，Big-endian；`01` PPG 20B、`02` acc+gyro 21B、`03` Acc、`04` Gyro。

## 狀態標註（必做，避免把提案畫成已完成）

用統一圖例區分三種線型／顏色：**已實作**（實線）、**已確認需求但尚未實作**（虛線）、**提案／待驗證**（點線＋灰）。至少標註：

- 未完成：播放中發現新檔的延後分析、檔案被替換自動偵測。現況：NPU 曲風已記錄，歌曲有有效 BPM 即可參與自動選曲；「期望曲風」選項為未來功能（`AGENTS.md`）。
- 提案：YAMNet 曲風（`docs/hub8735/YAMNET_GENRE.md` 寫「目前程式未實作」，但 `music_genre_classifier.cpp/.h` 已存在——**先讀程式確認實際到哪一步，再決定線型**）。
- 現況：BPM 分析目前只由使用者在 OLED 觸發，BPM 選單不跑 NPU 曲風。
- 未於硬體驗證：UI 韌體編譯／燒錄、`ble_sensor_hr` 板上 XRAM 與 `.XINIT` 修正、HUB `BLETask` 解析 msgType 01–04。

## 已知文件矛盾（請自行查證後在圖旁註明採用哪個）

- BLE characteristic 名稱：`BLE_PROTOCOL_QUICKREF.md` 用 `D002`／`D001`，`.agents/MEMORY.md` 部分段落寫 `FFF1`／`FFF0`（BM7701 側與 128-bit service 的對應）。以 `BLETask.cpp` 與 `firmware/n76e003/ble-notify-data-format.md` 為準。
- 廣播名稱：`BLE_sensor_ITAP` 與 `BM7701-ITAP` 並見；以 `BLETask.cpp` 實際連線名稱為準。
- 資料來源：目前有「BLE 遠端感測」與「本地 BMH08002-4＋LSM6DS3」兩條，需確認選擇機制（編譯期或執行期，見 `localSensor.*`、`analysisTask.cpp`），圖中標為二選一。
- `DFD.md` 是依現況整理的概念版，非核定設計；與 `AGENTS.md` 衝突時以 `AGENTS.md` 為準。

## 繪圖規格

- 由左至右：感測端 → 傳輸（BLE／UART）→ HUB8735（內分「介面層／task 層／儲存與輸出層」）→ 輸出（喇叭、OLED）。
- 一張圖不超過約 40 個節點；細節（Zone 公式、選曲規則）放圖例文字，不畫進節點。
- 節點命名沿用程式與文件的英文識別字（task、Queue、檔名），說明用繁體中文。
- 中文與英文／數字之間不加空格。
- 不確定的地方不要自行假設：列在圖後「待確認事項」，每項附你的建議。

## 驗收

- [ ] 三面向（硬體、軟體、資料）皆在同一張圖中可辨識
- [ ] 每條 Queue 方向與 `globalData.h`／各 task 原始碼一致（逐條對過）
- [ ] 手錶與綁帶兩種感測端、本地感測來源皆有，且測試模擬器與產品邊界清楚
- [ ] 已實作／已確認未實作／提案三種狀態標示無誤
- [ ] 腳位、UART 速率、I2C 位址與文件一致
- [ ] 圖後列出「待確認事項」與「文件矛盾的採用結論」
