# HUB8735 序列埠指令（移除前紀錄）

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

- 除 `auto`、`demo`、`train` 外，`play`/`pause`/`resume`/`stop` 都會讓自動選歌讓出控制權（手動優先）。
- 指令錯誤或無法辨識時只輸出 `LOGW`，不會有其他動作。

## 只有OLED按鈕有、序列埠沒有的控制

| UiCommand | 行為 |
| --------- | ---- |
| `SetMode` Enjoy / Manual | 自Train切到Enjoy時不換歌；選Manual則停止自動選歌 |
| `AnalyzeNewOrUnknownBpm` | 分析新檔或BPM未知的曲目 |
| `ReanalyzeAllBpm` | 重新分析全部曲目BPM |
| `CancelBpmAnalysis` | 取消正在進行的BPM分析 |

## BLE現況

`BLETask` 目前是central，只接收感測端資料；收到cmd frame（type `0x00`）時直接忽略，payload尚未定義，所以BLE目前**不能**做上述任何控制。
