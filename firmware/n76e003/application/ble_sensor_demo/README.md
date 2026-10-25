# ble_sensor_demo

`ble_sensor_motion`的DEMO版：可以把PPG輸入從BMH08002-4模組切換成燒在Flash裡的預錄資料。預錄資料走跟真實PPG一樣的路徑（寫進`tx_pkt`），所以心率還是由手錶本地的motion演算法算，OLED與BLE上報都跟真的輸入相同。

`source/main.c`以`#include`共用`../ble_sensor/source/main.c`（`ITAP_MOTION_HR`＋`ITAP_DEMO`）。DEMO專用的程式只放在本資料夾，其他版本的編譯結果逐位元組不變（2026-10-25以hex比對確認）。

## 開啟方式

- **開機時按住P3.0**（換頁鍵）→ DEMO：UART0中斷不開、不送PPG模組設定，改播預錄資料並循環。演算法不要求ACC（`mh_reset(&motion, 0)`），手怎麼動都不影響心率。
- 開機時沒按 → 跟motion版相同，讀真實PPG並參考ACC。
- 要離開DEMO請重開機。

## 畫面

P3.0在「合併文字頁」與「波形頁」之間切換。文字頁（第2列只有DEMO時才顯示`DEMO`，讀真實PPG時只有時間）：

```
BLE_watch_ITAP(S)      BLE：I(nit) A(dvertising) C(onnected) S(treaming) D(isconnected)
12M34S DEMO            開機時間
PPG(2) HR  67          PPG狀態（- 尚無、L 中斷、0 無手指、1 有手指、2 有資料）、本地算出的心率
 4321  4110            PPG1（IR）PPG2（紅光）
A  -123   456  4096    加速度 x y z
G     0     0     0    陀螺儀 x y z（motion系列停用gyro）
```

波形頁在DEMO時不扣基線，直接畫原始值（形狀跟capture網頁一樣），縮放仍是「上一輪＋這一輪」掃描的最小～最大值；讀真實PPG時維持原本扣0.45秒基線的畫法（`../ble_sensor/source/oled_wave.h`的`ITAP_DEMO`分支）。

字型只收錄這頁用到的35個字元（`source/oled_font_demo.h`），小寫只有藍牙名稱用到的字母。新增顯示文字時，若用到表外字元會畫成空白，要同時補進`font_chars`與`font5x7`。

## 預錄資料

`source/demo_ppg.h`由`make_demo_ppg.py`產生，不要手改：

```
python make_demo_ppg.py RECORDING_raw.json --start 89 --seconds 15
```

- 輸入：`ble_capture.html`的`*_raw.json`匯出（`--start`從第一筆PPG起算，不可有序號缺口），或含`elapsed_s, status, ir_ppg, red_ppg`欄位的CSV；區間內必須全部是status 2。
- 每兩筆平均成一點（35.7Hz → 17.9Hz），存成IR／紅光各一個int8步距，單位2^shift（依該通道最大步距自動選）。播放時每28ms送一筆，奇數筆取前後兩點的中點，資料最後會補步距回到第一點，讓循環接縫連續。
- 腳本會用gcc編譯真正的`ppg_motion_hr.h`，比較「原始錄音」與「解碼後循環播放3次」的心率輸出（需要gcc在PATH上）。

目前使用2026-10-25以`ble_sensor_capture`錄的靜止資料（session `d57085cb-3514-4ffb-b4c0-7e7c235415cf`，118秒、全部status 2、無缺口）的89～104秒：270點、540 B、循環15.1秒，最大量化誤差2 counts。原始錄音心率62；循環播放3次為62、62、61、62、62、61、62、62。這份錄音振幅較小，motion演算法對整段118秒只有約3成視窗有效，所以片段是逐秒掃描起點、挑循環後全部有效的一段；換錄音時建議同樣先掃描。

## 容量

N76E003只有18KB Flash（18432 B），預錄資料與程式共用。**NuEclipse建置預設帶`--debug`，同一份原始碼會比`build.ps1`大約114 B，而且NuEclipse的連結不檢查Flash上限，超過也照樣產生hex**；超出的部分是程式最後的XRAM初始值表（`demo_p`、`tx_pkt`等），燒進去後DEMO與真實PPG的波形都會亂。2026-10-25曾因此燒入18493 B的版本，所以資料縮到15秒。目前：`build.ps1` 18243 B、NuEclipse（debug）18357 B、XRAM 755／768 B。換錄音或改程式後，看NuEclipse的`Release/ble_sensor_demo.mem`裡ROM那一列不得超過18432；超過時把`--seconds`調短。

## 建置

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

輸出`Release/ble_sensor_demo.hex`，不會自動燒錄。

## 限制

- 只在電腦上驗證解碼與心率，尚未燒錄實機。
- 開機時按住P3.0才進DEMO，要離開必須重開機；`BLE_watch_ITAP`名稱不變，HUB看不出是預錄資料。
- `demo_task`以主迴圈輪詢28ms節拍；主迴圈被阻塞超過約110ms時會從當下重新起算，時間戳會跳一段，效果等同真實模組掉樣本。
