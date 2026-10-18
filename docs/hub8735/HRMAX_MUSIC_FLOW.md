# HUB8735：HRmax 到選歌、切歌

## Task 分工與資料契約

| 執行位置 | 輸入 | 輸出 |
| --- | --- | --- |
| `setup()` | `userInfo.age` | 建立各 Queue 與 Task |
| `BLETask` | 穿戴端 BLE PPG 封包 | 解出 `PpgSensorElement`（含狀態、心率、接收時間），送入 `ppgSensorQueue` |
| `analysisTask` | `userInfo.age`、最新有效 PPG（`status == 2`、心率非 0） | 算出 HRmax、穩定 Zone 與目標歌曲 BPM；變化時覆寫 `targetBpmResultQueue` 的最新結果 |
| `controlTask` | 目標 BPM 結果、播放事件、曲目 BPM 表、序列埠控制 | 依目標 BPM 選曲並把 `Play(filename)` 命令送到 `audioCmdQueue`；手動控制優先 |
| UITask | OLED 按鍵、uiUpdateQueue | 顯示 BPM 分析選單與排隊、分析中、完成摘要；透過 uiCmdQueue 傳送操作 |
| audioTask | audioCmdQueue、SD 卡 MP3 | 唯一呼叫 MAX98357 的 task，也是 SD 唯一擁有者；歌曲停止時執行 BPM 掃描與分析，回報播放與分析狀態 |

`analysisTask` 只送目標 BPM 結果，不操作播放裝置。`controlTask` 預設自動選曲；序列埠 `play/stop/pause/resume` 會切到手動控制，只有輸入 `auto` 才恢復感測器自動選曲。`volume` 不改變播放模式。`audioTask` 是唯一呼叫 MP3 播放 API 的 task，並用 `audioPlaybackEventQueue` 回報自動歌曲播畢或播放失敗。

## 心率到目標 BPM

首次有效心率立即設定 Zone。之後候選 Zone 必須越過原 Zone 邊界 **2% HRmax**，連續保持 **10 秒**才生效；失去有效 PPG 會清除候選倒數。現有模擬資料每 5 秒一筆，最新樣本最多保留 6 秒，目標 BPM 以 1 Hz 更新。

| 穩定 Zone | 心率區間 | 穩態目標歌曲 BPM |
| --- | --- | --- |
| 1 | 低於 60% HRmax | 心率線性映射到 60–80 |
| 2–3 | 60% 至低於 80% HRmax | 即時心率 |
| 4–5 | 達到 80% HRmax | 即時心率 × 0.75，四捨五入 |

跨區後仍使用**穩定 Zone**的公式；Zone 1 在遲滯期間不超過 80 BPM。每次 1 Hz 更新最多上升 5 BPM，下降立即生效。

## 選歌與切歌

曲目 metadata 從 SD 卡根目錄的 music_library.json 載入至 musicLibrary；song_catalog.h 負責 BPM 選曲。SD 範例清單有 16 首，容量上限 1024 首；符合條件為歌曲 BPM 與目標 BPM 相差不超過目標值的 5%。BPM 為 0 的未分析曲目不參加自動選曲。

- 首次選歌、穩定 Zone 切換：`controlTask` 從目前歌曲的下一首起依序找符合曲目；Zone 切換確認後立即送播放命令。完全沒有符合曲目時播放 BPM 最近的一首。若結果仍是目前歌曲，維持播放。
- 同一 Zone 內目標 BPM 改變：目前歌曲繼續播完，播畢後用最新目標選下一首。
- 歌曲自然播畢：`audioTask` 回報 `Finished`，`controlTask` 從下一首起依序找符合曲目；完全沒有符合曲目時重播剛播完的歌曲。
- 播放失敗：`audioTask` 回報失敗曲目索引，`controlTask` 跳過該曲並嘗試下一首；所有曲目都失敗時停止自動播放。
- PPG 無效或暫時失聯：不更新目標 BPM，也不確認新的 Zone；目前歌曲繼續播放。

## Train 模式（訓練課表）

課表定義於 `train_plan.h`，由 `controlTask` 依時間推進；實際心率只顯示、不影響選曲，不需有效心率即可開始。

| 課表（OLED Mode／序列埠） | 段落（Zone／時長） |
| --- | --- |
| `Train 11m`／`train 1` | Z2 3分 → Z4 2分 → Z2 2分 → Z5 1分 → Z1 3分 |
| `Train 5m`／`train 2` | 同上順序，每段1分（展示用） |

- 段落歌曲 BPM = 該 Zone 中點心率：55／65／75／85／95% HRmax（HRmax = 206.9 − 0.67 × 年齡，四捨五入）。年齡25（HRmax 190）時為105／124／143／162／181 BPM；曲庫沒有夠快的曲目時選最接近者。
- 換段時立即用新 BPM 選歌（與 Enjoy 換 Zone 相同的選曲規則）；段內歌曲播畢則依同段 BPM 選下一首。
- OLED 在 Mode 清單選 Train 後彈出課表清單，選定才開始。全部段落結束後停止播放並轉為 Manual，Mode 列顯示 `Mode: Train done`。
- 暫停／停止／手動選歌（Manual）或待機時進度凍結；再選同一課表從中斷處接續，選其他課表或已完成則從頭開始。自 Train 切到 Enjoy 不換歌，進度同樣保留。
- 主機檢查：`g++ -std=c++11 -Ifirmware/hub8735-ultra/application/embedded_loudspeaker firmware/hub8735-ultra/tests/train_plan_check.cpp`。

## OLED 主選單依模式變化

| 模式 | Mode 列下方多出的列 | Songs |
| --- | --- | --- |
| Enjoy | ` Z3 Target 142 BPM`（唯讀；尚無有效心率顯示 `Target: wait HR`） | 隱藏 |
| Train | ` Stage 2/5 1:23`（段/總段、本段剩餘時間；按 OK 從第一段重新開始） | 隱藏 |
| Demo | ` Level: 2`（運動強度 0~4，唯讀） | 隱藏 |
| Manual | 無 | 顯示 |

其餘項目（Play/Pause、Stop、Volume、BPM Analysis、Source、BLE、Power off）各模式都有。模式切換時游標停在原本的項目上。

## OLED 手動選歌

- 只有 Manual 模式的主選單有 Songs，列出 musicLibrary（檔名與 BPM，未分析顯示 `--`）；按 OK 送出 `PlaySong`，controlTask 轉成 `Play` 並進入手動模式，之後須在 Mode 選 Enjoy／Train 或序列埠 `auto` 才恢復自動選曲。
- 曲庫未載入、BPM 分析進行中或沒有曲目時不開啟清單。未分析 BPM 的曲目可手動播放，但不參加自動選曲。

## OLED 手動 BPM 分析

- 主選單的 BPM Analysis 子選單有 New / unknown BPM 與 Reanalyze all BPM。
- 掃描只由使用者啟動。controlTask 轉送請求；audioTask 在歌曲停止後掃描 SD 並呼叫 music_bpm_analyzer，不新增 FreeRTOS task，也不在 analysisTask 執行。
- 若請求時正在播放或暫停，工作排隊；播放命令優先，分析於目前曲目停止後開始。分析以 Helix 解碼 MP3、混成單聲道 PCM，分段送入 MiniBPM；最多分析 60 秒，少於 8 秒或沒有有效估計就算失敗。
- 新檔／未知 BPM 包含未列入清單、BPM 為 0、缺少舊檔案資訊，或檔案大小／FAT 修改時間有變更的曲目。BPM 為 0 的曲目不參加自動選曲。分析成功後以整數 BPM 更新清單；失敗曲目不阻擋其他曲目。
- 取代檔案依大小與 FAT 修改時間判斷。相同大小和時間戳記的內容替換可能漏檢；若需嚴格保證，應升級為內容指紋比對。
- BPM 分析選單只分析 BPM，不執行 NPU 曲風分類。
## 離線 BPM 估計

原有 10 首（`testdata/audio` 的 `music1`～`music10`）用 FFmpeg 將 MP3 解碼，對各曲目前約 180 秒的音訊做頻譜起音強度與自相關分析，再檢查三段各 60 秒的候選節拍。另 6 首是 FMA 資料集（fma_small，CC BY-NC-SA）的 30 秒片段，BPM 取自 Echonest 的 `tempo`（四捨五入，未在此量測）。這些數值都是**估計值**，節奏可能有半拍／雙拍解讀；換 SD 卡曲目或進行正式展示前，應人工核對。

SD 卡內容放在 `testdata/sd_music`，檔名與原檔對照如下（完整曲名與作者見 `song_catalog.h` 註解）：

| 檔名 | 原檔／來源 | 檔名 | 原檔／來源 |
| --- | --- | --- | --- |
| 001 | FMA 3264 | 010 | music10 |
| 002 | FMA 27552 | 011 | music7 |
| 003 | music1 | 012 | music5 |
| 004 | music3 | 013 | music9 |
| 005 | music4 | 014 | music8 |
| 006 | music2 | 015 | FMA 1661 |
| 007 | music6 | 016 | FMA 1688 |
| 008 | FMA 29602 | 009 | FMA 61670 |

檔名為流水號（`001.mp3`～`016.mp3`），不再以 BPM 命名；BPM 以 `music_library.json` 為準，值取自 MiniBPM 的分析結果（板上 60 秒分析，002 取自電腦端 MiniBPM，因板上解碼曾失敗），與上方離線估計不同。OLED 播放標頭顯示 `001.mp3(136)`。

## 本機檢查

```powershell
gcc -std=c99 -DTARGET_BPM_SELF_TEST firmware/hub8735-ultra/target_bpm.c -o target_bpm_test.exe
.\target_bpm_test.exe
g++ -std=c++11 -x c++ -DSONG_CATALOG_SELF_TEST firmware/hub8735-ultra/application/embedded_loudspeaker/song_catalog.h -o song_catalog_test.exe
.\song_catalog_test.exe
```

尚未在實際 HUB8735、SD 卡與喇叭上驗證；音檔必須以表中的檔名放在播放器讀取的 SD 卡根目錄。
