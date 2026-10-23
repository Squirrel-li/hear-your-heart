# N76E003 手錶資料擷取版本

這是供心率演算法驗證的獨立韌體，沿用 `ble_sensor` 的 UART、BM7701 與 I²C 驅動，
不執行手錶端心率演算法。`source/main.c` 透過 `ITAP_CAPTURE` 啟用擷取設定，
一般 `ble_sensor` 與 `ble_sensor_hr` 的產品設定不變。

## 擷取設定

| 項目 | 設定 |
| --- | --- |
| 裝置名稱 | `BLE_capture_ITAP`（避免 HUB 自動連線） |
| BLE | FFF0 service / FFF1 notify，所有通知 <=20 B，不需要提高 MTU |
| PPG | BMH08002-4 連續模式，既有實機輸出約每28 ms一筆；實際頻率以記錄為準 |
| PPG 封包 | 0x11，20 B，保留模組 status、D3、IR、red 波形 |
| ACC | LSM6DS3，0x6B，26 Hz，±8 g，gyro 關閉，高通關閉，保留重力 |
| ACC 封包 | 0x13，15 B，三軸 signed int16，0.244 mg/LSB |
| 同步 | 共用 N76E003 毫秒時鐘；PPG 記 UART 封包收完時間，ACC 記 DRDY 輪詢後的讀取時間 |
| 介面 | OLED 更新停用，保留原有 LED 連線提示 |

PPG 與 ACC 都沿用既有 big-endian 協定：`timestamp:u32 | type:u8 | sequence:u32 | payload`。
兩種訊號各有自己的序號，不可拿 PPG 與 ACC 序號互相比較。

PPG 先經 ISR 單槽交接，再進四筆主迴圈緩衝；滿時丟掉新到的樣本，之後的序號會出現缺口。
ACC 每2 ms檢查 XLDA，只有新資料才讀取；待送 ACC 被新樣本取代時，序號也會反映缺口。
兩種待送資料交替服務，每個 BM7701 命令仍等 ACK；沒有保證無漏樣。
感測器內部在尚未讀取前已被覆蓋的 ACC 樣本沒有序號，須另看 dt_ms／實際取樣率。
UART checksum 不合或硬體漏字元也不一定反映在有效封包序號；原始間隔與實測速率仍是必要檢查。

這是讀取時間同步，不是精密硬體同步：PPG 15-byte UART 傳送約3.9 ms，模組內部處理延遲未知；
ACC 有輪詢、I²C 與主迴圈延遲。沒有 FIFO、硬體 DRDY 中斷或光學取樣時間戳。
不要用電腦收到通知的時間當成光學取樣時間，也不要把插值當成補回遺失的資料。

## 編譯與燒錄

在專案根目錄：

```powershell
powershell -ExecutionPolicy Bypass -File firmware/n76e003/application/ble_sensor_capture/build.ps1
```

或在 NuEclipse 匯入本目錄既有 `.project`，以 Release 建置。
提供的 PowerShell 腳本使用已安裝 Nuvoton SDCC，檢查 768 B XRAM／18 KB code 上限。
不同安裝位置可傳入 `-SdccHome`。

輸出：`Release/ble_sensor_capture.hex`（Intel HEX）。腳本不燒錄、不改 MCU Config。
2026-10-23 實際編譯連結（含 BMH08002-4 轉送）：**XRAM 680 B／768 B，Flash 10,860 B／18,432 B**。
這是靜態連結數值，不是最壞執行時堆疊驗證。

使用原本 Nu-Link／NuEclipse 燒錄流程。UART1 的 P1.6／P0.2 與 ICP 共用：
燒錄時拔除 BM7701 的 UART 線，完成後拔除 Nu-Link，再接回 BM7701 UART。
不要讓 Debug launch 的 Config「Write」意外重寫 Config0。
感測器／OLED 的接線沿用 `ble_sensor`；本版本不要求額外硬體。

第一次執行會讀取 BMH08002-4 EEPROM 0x01；若不是 0x04，寫成連續模式並啟動量測。
這個模式會保留在模組中。之後燒回一般 `ble_sensor` 會由其開機流程恢復 0x00；
`ble_sensor_hr` 本來就使用連續模式。

## 電腦以藍牙記錄

不需要 USB-UART、ADB 或 HUB；需要電腦的 BLE 與支援 Web Bluetooth 的 Chrome／Edge。

1. 燒錄並啟動手錶。
2. 在專案根目錄啟動記錄頁伺服器：

   ```powershell
   powershell -ExecutionPolicy Bypass -File apps/bmh08002_ppg_analyzer/start_capture.ps1
   ```

   預設使用這台電腦的 Codex 內附 Python；其他環境用 `-Python C:/path/to/python.exe`。
   也可自行執行：`python -m http.server 8772 --bind 127.0.0.1 --directory apps/bmh08002_ppg_analyzer`。

3. 在 Chrome／Edge 開啟 **http://localhost:8772/ble_capture.html**，選擇 `BLE_capture_ITAP`。
4. 連線後先顯示即時波形（IR、紅光、ACC 合力，最近10秒），**不寫入資料**。按「開始記錄」才建立新記錄，可多次開始／停止而不斷線。
   切換活動標記，先各錄靜止／走路／擺手至少30–60秒。
5. 按「停止記錄」，分別下載 PPG CSV、ACC CSV、完整 JSON。

### BMH08002-4 設定

記錄頁的「BMH08002-4 設定」區經 FFF2 cmd opcode 0x02 轉送模組指令（格式見 `ble-notify-data-format.md` 第6節）：
讀寫 mode、ir/red 光強上下限、interval，開始／停止量測、校正靈敏度，以及任意 EEPROM 位址讀寫。
設定寫在模組 EEPROM，斷電後仍保留；本韌體開機會把 mode 改回 4。每個指令讓 PPG 中斷最多約0.5秒（序號缺口），
記錄中下的指令與結果會寫進該筆記錄的 JSON。其他手錶韌體不支援此指令。

每次連線都是新的記錄。記錄寫入瀏覽器 IndexedDB，重新整理後可選舊記錄；
資料庫只屬於同一瀏覽器設定檔與 `http://localhost:8772` 網站來源，換成127.0.0.1會是另一份。
不是雲端備份，清除網站資料／儲存回收可能移除它，完成後請下載檔案。
中途斷線會停止並保留已寫入資料，不自動把重新連線混入同一個實驗。

每筆保留手錶時間、序號、序號缺口、前筆間隔、PC 接收時間與活動標記。
JSON 額外保存原始 notify hex、異常封包與擷取設定。
活動標記是 PC 操作時間的註記，受到 BLE 傳輸延遲影響，不是感測器精準事件標記。
模組 D3 是比較值，不是獨立真值；評估運動準確度仍須同步錄製可信的參考心率。

## 分析匯出的資料

```powershell
python apps/bmh08002_ppg_analyzer/motion_hr.py SESSION_ppg.csv --sensor-time --acc SESSION_acc.csv --out result.csv
```

`elapsed_s` 兩份檔都保留手錶 uptime（已展開32-bit時間回繞），所以可直接對齊。
PPG 的 `d1` 就是模組 D3，符合原型讀取格式。
分析器現在會檢查 `sequence`：PPG 缺包切段重累積10秒，ACC 缺包影響的視窗拒絕輸出。
若序號缺口很多，不應先調整心率演算法，應先處理 BLE 連線間隔／ACK 延遲／收資料負荷。
只看到約1 Hz ACC或10秒一筆PPG通常是燒到其他版本；本頁也不會接受 0x12 的舊 IMU 通知作 ACC。

## 可重跑檢查與未驗證事項

```powershell
python firmware/n76e003/application/ble_sensor_capture/test_capture.py
python firmware/n76e003/application/ble_sensor_capture/test_bmh_cmd.py
node apps/bmh08002_ppg_analyzer/test_ble_capture.js
python apps/bmh08002_ppg_analyzer/test_motion_hr.py
```

第一項使用主機 gcc 編譯實際 ISR／緩衝函式，檢查 checksum、順序、溢位、忙碌槽位與序號缺口。
第二項檢查瀏覽器實際解析／CSV 匯出函式：長度、signed ACC、獨立序號、回繞、重啟與 CSV quoting。
第三項檢查訊號分析與缺包切段。
以上及 SDCC 編譯已通過；尚未燒錄驗證、量測完整串流漏包率或驗證瀏覽器 IndexedDB／BLE 的實機整合。
本次可用的瀏覽器自動化無法啟動，因此沒有宣稱已完成頁面操作驗收。

參考：[ST LSM6DS3 register definitions](https://github.com/STMicroelectronics/lsm6ds3-pid/blob/master/lsm6ds3_reg.h)、
[Chrome Web Bluetooth](https://developer.chrome.com/docs/capabilities/bluetooth)。
