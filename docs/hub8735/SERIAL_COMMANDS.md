# HUB8735 序列埠指令

來源：`firmware/hub8735-ultra/application/embedded_loudspeaker/ControlTask.cpp` 的 `handleSerialCommand()`。
`controlTask` 每100 ms檢查一次 `Serial`（LOG UART，RX = D16，2000000 baud），讀到 `\n` 為一行指令，前後空白會被去掉。

## 指令

| 指令 | 參數 | 類型 | 行為 | 對應的UiCommand（OLED按鈕） |
| ---- | ---- | ---- | ---- | --------------------------- |
| `auto` | 無（不分大小寫） | 控制 | 啟用自動模式，固定進入Enjoy；清除失敗曲目清單，有目標BPM時立即選歌，否則等待有效心率 | `SetMode`（可選Enjoy/Train/Manual） |
| `demo` | 無（不分大小寫） | 控制 | 進入DEMO模式：依Gsensor運動強度（0~4）播放SD卡 `/demo/DEMO_nn.mp3`，見 `DEMO_MODE.md` | `SetMode` Demo |
| `train [1\|2]` | 內建課表編號，預設1（1＝11分鐘完整版，2＝5分鐘短版） | 控制 | 進入Train模式依課表播放；同一課表未完成時接續，否則從頭開始，見 `HRMAX_MUSIC_FLOW.md` | `SetMode` Train（選課表） |
| `play <filename>` | SD卡上的檔名，長度需小於 `AudioCmdElement::filename` | 控制 | 切到手動模式並播放指定檔案（直接用檔名，不檢查是否在曲庫） | `PlaySong`（用曲庫索引） |
| `pause` | 無（不分大小寫） | 控制 | 切到手動模式並暫停 | `Pause` |
| `resume` | 無（不分大小寫） | 控制 | 切到手動模式並繼續播放 | `Resume` |
| `stop` | 無（不分大小寫） | 控制 | 切到手動模式並停止 | `Stop` |
| `volume <0-100>` | 整數，超出範圍夾到0或100 | 設定 | 設定音量，不改變播放模式 | `SetVolume` |
| `src ppg <chest\|watch\|local>` | 感測來源 | 設定 | 切換PPG（心率）資料來源 | `SetPpgSource` |
| `src motion <chest\|watch\|local>` | 感測來源 | 設定 | 切換IMU（動作）資料來源 | `SetImuSource` |
| `bpm list` | 無 | 查詢 | 逐行印出曲庫：`#索引 檔名 BPM 曲風`（BPM 0＝未分析） | Songs清單 |
| `bpm new` | 無 | 分析 | 分析新檔或BPM未知的曲目（播放或暫停中會被拒絕），每首完成印出 `timing:` log | `AnalyzeNewOrUnknownBpm` |
| `bpm all` | 無 | 分析 | 重新分析全部曲目，會覆蓋使用者的x2／/2修正 | `ReanalyzeAllBpm` |
| `bpm cancel` | 無 | 分析 | 取消進行中的BPM分析，已完成的結果保留 | `CancelBpmAnalysis` |
| `bpm x2 <曲目>` | `bpm list` 的索引或檔名 | 設定 | 該曲BPM乘2並存入 `music_library.json`（結果須在30～300，未分析的曲目不可用） | 單曲選單 BPM x2 |
| `bpm /2 <曲目>` | 同上 | 設定 | 該曲BPM除以2（四捨五入） | 單曲選單 BPM /2 |
| `bpm set <曲目> <bpm>` | 曲目＋30～300的整數 | 設定 | 直接指定該曲BPM | 無 |
| `status` | 無（不分大小寫） | 查詢 | 印出模式、自動與否、目標BPM／Zone、DEMO強度、PPG／IMU來源、曲目數、分析中與否、剩餘heap；Train時另印課表進度 | 無 |
| `ble connect\|disconnect\|auto\|manual <chest\|watch>` | 動作＋裝置 | 設定 | 與OLED BLE選單相同：手動連線（最多3次）、斷線（並轉Manual）、切Auto／Manual | BLE選單 |
| `help` | 無（不分大小寫） | 查詢 | 印出指令總表 | 無 |

電腦端可用 `apps/hub_serial_console/index.html`（Chrome／Edge開啟，Web Serial）連線、送指令，並依等級／TAG／關鍵字篩選log。

- 除 `auto`、`demo`、`train` 外，`play`/`pause`/`resume`/`stop` 都會讓自動選歌讓出控制權（手動優先）。
- 指令錯誤或無法辨識時只輸出 `LOGW`，不會有其他動作。
- `bpm` 指令不改變播放模式；BPM掃描進行中，x2／/2／set會被拒絕。

## 只有OLED按鈕有、序列埠沒有的控制

| UiCommand | 行為 |
| --------- | ---- |
| `SetMode` Enjoy / Manual | 自Train切到Enjoy時不換歌；選Manual則停止自動選歌 |

## BLE現況

`BLETask` 目前是central，只接收感測端資料；收到cmd frame（type `0x00`）時直接忽略，payload尚未定義，所以BLE目前**不能**做上述任何控制。
