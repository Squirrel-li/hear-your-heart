# 聽見你的心（Hear Your Heart）

系統分為三種裝置：**HUB8735 Ultra 運算播放端、N76E003 穿戴手錶、外部穿戴綁帶**。本專案實作 HUB 與手錶；綁帶由其他人實作，本專案保留 HUB 接收與整合。BM7701 是手錶的 BLE 傳輸模組。

目前 HUB 提供 Enjoy 心率選曲、Train 課表、Manual 手動播放、Demo 運動強度播放，以及 OLED 操作、雙 BLE 連線、曲庫分析與軟體待機。主程式已接入 BPM／NPU 曲風分析，曲風存入 JSON；有有效 BPM 即可自動選曲。未來可新增使用者的「期望曲風」選項。自動偵測新檔與播放中延後分析尚未完成。手錶的 `ble_sensor`／`ble_sensor_hr` 分別使用模組心率／自行計算心率，兩者都不是綁帶韌體。

`apps/remote_sensor_simulator` 是可模擬手錶／綁帶的測試工具；產品手機 App 不在範圍內。需求與責任邊界見 [AGENTS.md](AGENTS.md)，目前功能、資料流及文件衝突見 [架構核對](docs/ARCHITECTURE.md)。程式存在不代表已通過實機驗收。

## 專案檔案結構

```text
hear-your-heart/
├── .agents/                     # 專案記憶
├── apps/
│   ├── phone/                   # Flutter App，目前不在產品範圍
│   └── remote_sensor_simulator/ # 感測器測試工具
├── assets/
│   ├── hardware/                # 硬體架構圖與相關文件
│   └── software/                # 系統、資料流程圖與預覽圖
├── docs/
│   ├── ble/                     # 通用 BLE 文件
│   ├── hub8735/                 # HUB8735 韌體與音樂流程
│   ├── remote_sensor/           # BM7701 感測端文件
│   └── references/              # 原廠文件與參考資料
├── external/                    # 第三方 SDK 與函式庫
├── firmware/
│   ├── hub8735-ultra/
│   │   ├── application/
│   │   ├── examples/
│   │   └── testdata/
│   └── n76e003/                 # N76E003 手錶韌體與 BM7701 測試工具
│       ├── application/
│       ├── bm7701/              # 模組參考文件
│       └── scripts/             # UART 與 BLE 測試工具
├── project/                     # 本機專題文件
├── tools/                       # 本機開發工具
├── AGENTS.md                    # 專案工作與架構規則
└── README.md
```

> 本專案僅供專題研究與展示使用，不提供醫療診斷。
