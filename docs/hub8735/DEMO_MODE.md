# HUB8735 DEMO模式

目的：讓小朋友直觀感受「運動強度 → 音樂節奏」的變化。

## 流程

1. `analysisTask` 收到加速度樣本時計算三軸合力。LSM6DS3設定為±8g（0.244mg/LSB），並開啟高通濾波去掉重力，所以合力約等於動態加速度。
2. 每0.5秒算出該段的平均合力，依下表分級。新等級需連續維持0.5秒（連續兩段）才確認，單段突波不切換，確認後寫入 `motionLevelQueue`。
3. `controlTask` 在Demo模式下，等級改變時立即播放 `demo/DEMO_nn.mp3`（nn = 等級）。播完重播同一首；檔案無法播放時保持靜音，直到等級改變。IMU超過5秒沒有資料時強度視為未知（OLED顯示`Level: --`），目前曲目播完不再重播，資料恢復後第一筆樣本即決定新等級。
4. 來源：本地LSM6DS3每80ms讀一次（12.5Hz）；手錶（N76E003 `ble_sensor`／`ble_sensor_hr`）每秒1筆。兩者都適用同一套分級；外部綁帶的實作與上報頻率不由本專案指定。

程式：`firmware/hub8735-ultra/application/embedded_loudspeaker/motion_level.h`，主機端檢查：`firmware/hub8735-ultra/tests/motion_level_check.cpp`。

## 分級（估計值，需實測校正）

| 等級 | 平均動態合力 | 情境 | 檔案 | 建議曲目（BPM） |
| ---- | ------------ | ---- | ---- | --------------- |
| 0 | < 50 mg | 靜止 | `DEMO_00.mp3` | Teddy Bear Waltz（75） |
| 1 | 50~250 mg | 走路 | `DEMO_01.mp3` | Carefree（96） |
| 2 | 250~600 mg | 快走 | `DEMO_02.mp3` | Voxel Revolution（122） |
| 3 | 600~1200 mg | 跑步 | `DEMO_03.mp3` | Monkeys Spinning Monkeys（144） |
| 4 | ≥ 1200 mg | 跳躍 | `DEMO_04.mp3` | Fiddles McGinty（174） |

校正：開debug log看 `ANALYSIS motion mean=... mg`，依實際配戴位置調整 `MOTION_LEVEL_MG`。

## 進入／離開

- OLED：Mode → `Demo mode`；序列埠：`demo`。
- pause暫停後仍在Demo，暫停中不切換曲目，Resume後才依當時強度換歌；stop／手動選歌會進入Manual（手動優先）。選Enjoy或輸入 `auto` 會回到心率選歌，選Train則依課表選歌，沒有目標BPM時會先停止DEMO曲目。

## 音樂授權

曲目來自Kevin MacLeod（incompetech.com），採CC BY 4.0授權，使用時需要標示出處，例如：

> "Carefree" Kevin MacLeod (incompetech.com), Licensed under Creative Commons: By Attribution 4.0 License, http://creativecommons.org/licenses/by/4.0/

其餘四首依相同格式替換曲名。
