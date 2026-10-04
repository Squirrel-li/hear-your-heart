# 聽見你的心（Hear Your Heart）

專案以 BM7701-00-1 感測端與 HUB 8735 Ultra 嵌入式揚聲器為核心，播放時依心率更新目標 BPM，供自動選曲使用。產品手機 App 暫不納入目前範圍；apps/remote_sensor_simulator 是感測器測試工具。

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
│   └── n76e003/                 # 感測端 MCU 韌體與 BM7701 測試工具
│       ├── application/
│       ├── bm7701/              # 模組參考文件
│       └── scripts/             # UART 與 BLE 測試工具
├── project/                     # 本機專題文件
├── tools/                       # 本機開發工具
├── AGENTS.md                    # 專案工作與架構規則
└── README.md
```

> 本專案僅供專題研究與展示使用，不提供醫療診斷。
