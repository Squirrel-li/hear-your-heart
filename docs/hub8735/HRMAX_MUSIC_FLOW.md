# HUB8735：HRmax 到選歌、切歌

## Task 分工與資料契約

| 執行位置 | 輸入 | 輸出 |
| --- | --- | --- |
| `setup()` | Flash 內的年齡設定（`settingsLoadAge`，未設定時 25） | 設定 `userInfo.age`，建立各 Queue 與 Task |
| `BLETask` | 穿戴端 BLE PPG 封包 | 解出 `PpgSensorElement`（含狀態、心率、接收時間），送入 `ppgSensorQueue` |
| `analysisTask` | `userInfo.age`（可隨 OLED 設定改變）、最新有效 PPG（`status == 2`、心率非 0） | 以 `heart_zone.h` 算出 HRmax、穩定 Zone 與目標歌曲 BPM；變化時覆寫 `targetBpmResultQueue` 的最新結果 |
| `controlTask` | 目標 BPM 結果、播放事件、曲目 BPM 表、序列埠控制 | 依目標 BPM 選曲並把 `Play(filename)` 命令送到 `audioCmdQueue`；手動控制優先 |
| UITask | OLED 按鍵、uiUpdateQueue | 顯示 BPM／曲風分析選單、分析中、完成或取消狀態；透過 uiCmdQueue 傳送操作 |
| audioTask | audioCmdQueue、SD 卡 MP3 | 唯一呼叫 MAX98357 的 task，也是 SD 唯一擁有者；在播放空檔執行 BPM／曲風分析（播放優先），回報播放與分析狀態，歌曲取得 BPM 時遞增 `musicLibraryRevision` |

`analysisTask` 只送目標 BPM 結果，不操作播放裝置。`controlTask` 開機與喚醒都進 Enjoy 自動選曲；`play`／`stop` 與 OLED 手動選歌、Stop 會切到手動控制，輸入 `auto` 或在 OLED Mode 選 Enjoy 才恢復感測器自動選曲（Train／Demo 則恢復各自的自動模式）。`pause`／`resume` 不離開自動模式：暫停期間不換歌、Train 課表凍結、DEMO 不切換，Resume 後才執行期間累積的換歌。`volume` 不改變播放模式。`audioTask` 是唯一呼叫 MP3 播放 API 的 task，並用 `audioPlaybackEventQueue` 回報自動歌曲播畢或播放失敗。

## 心率到目標 BPM

首次有效心率立即設定 Zone。之後候選 Zone 必須越過原 Zone 邊界 **2% HRmax**，連續保持 **10 秒**才生效；失去有效 PPG 會清除候選倒數。現有模擬資料每 5 秒一筆，最新有效樣本最多保留 15 秒，目標 BPM 以 1 Hz 更新。

| 穩定 Zone | 心率區間 | 穩態目標歌曲 BPM |
| --- | --- | --- |
| 1 暖身／休息（沉浸與放鬆） | 低於 60% HRmax | 心率線性映射到 60–80 |
| 2 有氧燃脂（穩定節奏維持） | 60% 至低於 70% | 即時心率 |
| 3 耐力心肺（節奏同步、適度激勵） | 70% 至低於 80% | 即時心率 × 1.05 |
| 4 無氧極限（引導降速） | 80% 至低於 90% | 即時心率 × 0.80 |
| 5 疲勞危險（立即降速） | 90% 以上 | 即時心率 × 0.70；往上進入 Z5 不等 10 秒駐留 |

對應依專題規劃書「目標規格說明」的 ACSM 五區（2026-10-27 使用者確認）；實作與主機檢查在 `heart_zone.h`、`firmware/hub8735-ultra/tests/heart_zone_check.cpp`。跨區後仍使用**穩定 Zone**的公式；Zone 1 在遲滯期間不超過 80 BPM。每次 1 Hz 更新最多上升 5 BPM，下降立即生效。規劃書 Z5 的「語音安全警示」尚未實作。

- 待機會清除 Zone 與目標 BPM（可能換人配戴），喚醒後等新的有效心率。改年齡會立即換算 HRmax，Zone 由下一筆心率重新決定。

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
- 暫停時進度凍結，Resume 後接續（仍在 Train）。停止／手動選歌（Manual）或待機時進度也凍結；再選同一課表從中斷處接續，選其他課表或已完成則從頭開始。自 Train 切到 Enjoy 不換歌，進度同樣保留。Train 依自己的課表設計，不套用 Enjoy 的 Z4–Z5 降速。
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

- 只有 Manual 模式的主選單有 Songs，列出 musicLibrary（檔名與 BPM，未分析顯示 `--`）；按 OK 開啟單曲選單 Play／BPM x2／BPM /2／Back。Play 送出 `PlaySong`，controlTask 轉成 `Play` 並進入手動模式，之後須在 Mode 選 Enjoy／Train 或序列埠 `auto` 才恢復自動選曲。
- BPM 倍數修正：演算法偶爾把節拍判成兩倍或一半，由使用者修正。播放或暫停中，主選單多一列 `Song BPM: 120`，按 OK 開啟同一個單曲選單（無 Play）。x2／/2 送出 `SetSongBpm`（結果須在 30～300），controlTask 轉給 audioTask 寫入 `music_library.json`；BPM 掃描進行中也可修正。之後全部重分析（OLED `Debug` → `Analysis` → `Reanalyze all` 或序列埠 `bpm all`）會覆蓋使用者修正（2026-10-22 使用者決定）。修正不改變手動／自動模式。
- 曲庫未載入、BPM 分析進行中或沒有曲目時不開啟清單。未分析 BPM 的曲目可手動播放，但不參加自動選曲。

## OLED 手動 BPM 分析

- 主選單的 `Rescan BPM` 是單一按鍵：按 OK 重新掃描 SD 並分析新檔／未知 BPM 或曲風的歌曲，接著顯示進度頁；分析中標籤顯示 `BPM: n / total`，再按 OK 開進度頁：`Back` 回主選單、分析在背景繼續；OLED 不提供取消，取消用序列埠 `bpm cancel` 或關機待機。全部重分析與清除全部結果放在 `Debug` → `Analysis`（先確認，預設 Cancel），序列埠 `bpm all`／`bpm clear` 也可。
- 開機時自動跑一次「新檔／未知」掃描；之後由使用者啟動。controlTask 轉送請求；audioTask 在播放空檔掃描 SD 並呼叫 music_bpm_analyzer（播放優先，見 AGENTS.md 4.2），不新增 FreeRTOS task，也不在 analysisTask 執行。
- 自動模式下若沒有任何歌有 BPM，controlTask 先隨機播一首；每有一首歌分析出 BPM 就重試，有可選的歌即替換掉隨機歌。分析以 Helix 解碼 MP3、混成單聲道 PCM，分段送入 `bpm_tempo.h`；最多分析 60 秒，少於 8 秒或沒有有效估計就算失敗。每首完成時印出 `timing:` log（解碼、特徵、估計各自耗時）。
- `bpm_tempo.h`（2026-10-22 取代 MiniBPM）：log-mel 頻譜差 onset 曲線（全頻＋4 頻帶）→ 自相關與傅立葉週期分布 → 約 25 個候選 → 300 棵梯度提升樹（`bpm_tempo_model.h`）打分取最高。模型以 GTZAN、Ballroom、GiantSteps Tempo、ISMIR04 Songs 共 2824 首訓練；重新訓練用 `firmware/hub8735-ultra/tools/bpm_tempo/`（`dump.cpp` 用同一份 `bpm_tempo.h` 在電腦上算特徵，`train.py` 匯出模型）。2026-10-22 起特徵改用與 HUB 相同的 Helix 解碼計算（先前用 minimp3）。保留測試集 1404 首（Helix 特徵）：直接正確 75.4%（MiniBPM 56.8%），含 x2／/2 可修正 90.5%（MiniBPM 76.1%）；<70 BPM 的可修正率最低（68%）。板上實測（CMSIS FFT）：60 秒音訊約 12 秒、20～34 秒音訊 3～6 秒；SD 卡 42 首測試曲取自保留測試集，且不參與最終模型訓練（模型以其餘 2782 首訓練），只作驗證：直接正確 26、含 x2／/2 可修正 34（MiniBPM 19／31）；每首開頭丟 3 個 frame 後，42 首 PCM 雜湊與電腦版 Helix 完全相同。每首的 timing log 附 `pcm <雜湊>/<樣本數>`，可與 `dump.cpp -DDUMP_ESTIMATE`（電腦端改用同一個 Helix 解碼）比對解碼是否一致。
- 新檔／未知 BPM 包含未列入清單、BPM 為 0、缺少舊檔案資訊，或檔案大小／FAT 修改時間有變更的曲目。BPM 為 0 的曲目不參加自動選曲。分析成功後以整數 BPM 更新清單；失敗曲目不阻擋其他曲目。
- 取代檔案依大小與 FAT 修改時間判斷。相同大小和時間戳記的內容替換可能漏檢；若需嚴格保證，應升級為內容指紋比對。
- BPM 分析選單已接入 NPU 曲風分類：BPM 階段後由 MusicGenreClassifier 以 NPU 執行自訓五類 YAMNet（`tools/yamnet_genre/models/yamnet_c5u11_fp16.nb`，複製到 SD 卡 `NN_MDL/yamnet.nb`，Arduino 選單 LoadNNModelSource 須選 SD Card），取約 15 秒音訊、各視窗 softmax 平均後取最高者（Classical／Electronic／Hip-Hop／Rock／Other），存入 `music_library.json`；每首前 2 個視窗含上一首殘留樣本而不計分。尚未實機驗證。有有效 BPM 即可參加自動選曲，曲風分析失敗不阻擋。未來可新增使用者的「期望曲風」選項；目前選曲只依 BPM。
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

檔名為流水號（`001.mp3`～`016.mp3`），不再以 BPM 命名；BPM 以 `music_library.json` 為準，值取自 MiniBPM 的分析結果（板上 60 秒分析，002 取自電腦端 MiniBPM，因板上解碼曾失敗），與上方離線估計不同；改用 `bpm_tempo.h` 後須重新分析才會更新。OLED 播放標頭顯示 `001.mp3(136)`。

## 本機檢查

```powershell
gcc -std=c99 -DTARGET_BPM_SELF_TEST firmware/hub8735-ultra/target_bpm.c -o target_bpm_test.exe
.\target_bpm_test.exe
g++ -std=c++11 -x c++ -DSONG_CATALOG_SELF_TEST firmware/hub8735-ultra/application/embedded_loudspeaker/song_catalog.h -o song_catalog_test.exe
.\song_catalog_test.exe
```

尚未在實際 HUB8735、SD 卡與喇叭上驗證；音檔必須以表中的檔名放在播放器讀取的 SD 卡根目錄。
