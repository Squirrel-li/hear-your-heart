# HUB8735 資料流程圖（概念版）

依目前實作整理，非已核定的架構規則。細節見 `HRMAX_MUSIC_FLOW.md`、`globalData.h`、`firmware/n76e003/ble-notify-data-format.md`。

## 1. 播放音樂

```mermaid
flowchart LR
    S["感測器<br/>BLE 胸帶／本地 PPG+IMU"]
    U["使用者<br/>OLED／序列埠"]
    LIB[("音樂資料<br/>檔名／BPM／曲風")]
    SPK["喇叭"]

    A("心率分析<br/>心率 → HRmax → Zone → 目標 BPM")
    C("控制<br/>選曲、模式、手動優先")
    P("播放<br/>播放 MP3")

    S -- "PPG／ACC／GYRO" --> A
    A -- "目標 BPM" --> C
    U -- "操作指令" --> C
    LIB -- "BPM／曲風" --> C
    C -- "播放命令" --> P
    P -- "播畢／失敗" --> C
    P -- "音訊" --> SPK
```

感測器心率 → 目標 BPM → 控制選曲 → 播放；使用者指令可隨時插手（手動優先）。

## 2. BPM／曲風分析

```mermaid
flowchart LR
    U["使用者<br/>OLED"]
    LIB[("音樂資料<br/>檔名／BPM／曲風")]
    MP3[("MP3 檔案")]

    C("控制<br/>轉送請求")
    M("曲目分析<br/>BPM（MiniBPM）<br/>曲風（NPU）")

    U -- "分析請求<br/>(新檔／全部重分析)" --> C
    C -- "分析請求" --> M
    MP3 -- "音檔" --> M
    LIB -- "既有紀錄（判斷新檔）" --> M
    M -- "BPM、曲風" --> LIB
    M -- "完成／失敗" --> C
```

只在使用者啟動時執行，歌曲停止後才開始；單曲失敗不影響其他曲目。

## 3. 感測器端（手錶）

```mermaid
flowchart LR
    PPG["PPG 模組<br/>BMH08002-4"]
    IMU["IMU<br/>LSM6DS3"]
    HUB["HUB8735"]
    U["使用者<br/>按鍵"]
    OLED["OLED"]

    N("N76E003<br/>心率計算<br/>封包組成")
    B("BLE 模組<br/>BM7701")

    PPG -- "UART 血氧／PPG 幀" --> N
    IMU -- "I2C ACC／GYRO（1 Hz）" --> N
    N -- "UART 指令＋資料" --> B
    B -- "BLE notify<br/>PPG（每約 10 秒）／IMU" --> HUB
    U -- "切換頁面／關閉 OLED" --> N
    N -- "PPG／IMU 數值" --> OLED
```

連線流程：重置 BLE 模組 → 設名稱 → 廣播 → 等連線與訂閱 → 持續送資料；斷線後重新廣播。

## 4. 感測器端（綁帶）

```mermaid
flowchart LR
    PPG["PPG 感測器<br/>MAX30103"]
    HUB["HUB8735"]

    E("ePy Lite<br/>心率計算<br/>封包組成<br/>內建 BLE")

    PPG -- "I2C 原始 PPG" --> E
    E -- "BLE notify<br/>PPG／心率" --> HUB
```

單一板子取代 N76E003＋BM7701（見 `docs/ePy_Lite/problems.md`），無 IMU；MAX30103 輸出原始 PPG，心率在板上計算。
