# HUB8735：HRmax 到選歌、切歌

## Task 分工與資料契約

| 執行位置 | 輸入 | 輸出 |
| --- | --- | --- |
| `setup()` | `userInfo.age` | 建立各 Queue 與 Task |
| `BLETask` | 穿戴端 BLE PPG 封包 | 解出 `PpgSensorElement`（含狀態、心率、接收時間），送入 `ppgSensorQueue` |
| `analysisTask` | `userInfo.age`、最新有效 PPG（`status == 2`、心率非 0） | 算出 HRmax、穩定 Zone 與目標歌曲 BPM；變化時覆寫 `targetBpmResultQueue` 的最新結果 |
| `controlTask` | 目標 BPM 結果、播放事件、曲目 BPM 表、序列埠控制 | 依目標 BPM 選曲並把 `Play(filename)` 命令送到 `audioCmdQueue`；手動控制優先 |
| `audioTask` | `audioCmdQueue`、SD 卡 MP3 | 唯一呼叫 MAX98357 播放 API 的 task；回報自動歌曲播畢或播放失敗 |

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

曲目表在 `song_catalog.h`，共 16 首，檔名為 `bpmNNN.mp3`（NNN 為 BPM），依 BPM 由低到高的順序循環。符合條件為歌曲 BPM 與目標 BPM 相差不超過目標值的 5%。

- 首次選歌、穩定 Zone 切換：`controlTask` 從目前歌曲的下一首起依序找符合曲目；Zone 切換確認後立即送播放命令。完全沒有符合曲目時播放 BPM 最近的一首。若結果仍是目前歌曲，維持播放。
- 同一 Zone 內目標 BPM 改變：目前歌曲繼續播完，播畢後用最新目標選下一首。
- 歌曲自然播畢：`audioTask` 回報 `Finished`，`controlTask` 從下一首起依序找符合曲目；完全沒有符合曲目時重播剛播完的歌曲。
- 播放失敗：`audioTask` 回報失敗曲目索引，`controlTask` 跳過該曲並嘗試下一首；所有曲目都失敗時停止自動播放。
- PPG 無效或暫時失聯：不更新目標 BPM，也不確認新的 Zone；目前歌曲繼續播放。

## 離線 BPM 估計

原有 10 首（`testdata/audio` 的 `music1`～`music10`）用 FFmpeg 將 MP3 解碼，對各曲目前約 180 秒的音訊做頻譜起音強度與自相關分析，再檢查三段各 60 秒的候選節拍。另 6 首是 FMA 資料集（fma_small，CC BY-NC-SA）的 30 秒片段，BPM 取自 Echonest 的 `tempo`（四捨五入，未在此量測）。這些數值都是**估計值**，節奏可能有半拍／雙拍解讀；換 SD 卡曲目或進行正式展示前，應人工核對。

SD 卡內容放在 `testdata/sd_music`，檔名與原檔對照如下（完整曲名與作者見 `song_catalog.h` 註解）：

| 檔名 | 原檔／來源 | 檔名 | 原檔／來源 |
| --- | --- | --- | --- |
| bpm060 | FMA 3264 | bpm132 | music10 |
| bpm066 | FMA 27552 | bpm135 | music7 |
| bpm075 | music1 | bpm137 | music5 |
| bpm080 | music3 | bpm138 | music9 |
| bpm085 | music4 | bpm147 | music8 |
| bpm095 | music2 | bpm156 | FMA 1661 |
| bpm097 | music6 | bpm171 | FMA 1688 |
| bpm106 | FMA 29602 | bpm117 | FMA 61670 |

## 本機檢查

```powershell
gcc -std=c99 -DTARGET_BPM_SELF_TEST firmware/hub8735-ultra/target_bpm.c -o target_bpm_test.exe
.\target_bpm_test.exe
g++ -std=c++11 -x c++ -DSONG_CATALOG_SELF_TEST firmware/hub8735-ultra/application/embedded_loudspeaker/song_catalog.h -o song_catalog_test.exe
.\song_catalog_test.exe
```

尚未在實際 HUB8735、SD 卡與喇叭上驗證；音檔必須以表中的檔名放在播放器讀取的 SD 卡根目錄。
