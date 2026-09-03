# 聽見你的心（Hear Your Heart）

一套結合穿戴式感測器、嵌入式揚聲器與 Flutter APP 的智慧音樂系統。專案根據使用者的即時心率、呼吸與活動狀態分析身體狀況，動態選擇合適的音樂與節奏。

## 專案檔案結構

```text
hear-your-heart/
├── apps/                      # 可獨立建置的應用程式
│   └── phone/                 # Flutter 行動應用程式
├── assets/
│   └── hardware/              # 硬體架構圖與相關文件
├── docs/                      # 專案文件與參考資料
│   ├── ble/
│   ├── hub8735/
│   └── references/
├── external/                  # 外部 SDK 與第三方函式庫子模組
├── firmware/
│   ├── hub8735-ultra/
│   │   ├── application/       # HUB8735 Ultra 主程式
│   │   ├── examples/          # BLE、音訊、顯示、檔案系統與影像範例
│   │   └── testdata/          # 韌體測試資料
│   └── remote_sensor/
│       ├── scripts/           # UART、燒錄與測試腳本
│       └── device/            # remote sensor MCU / BLE 韌體
├── project/                   # 本機專題文件（不納入 Git）
├── tools/                     # 本機開發工具（不納入 Git）
└── README.md
```

目前 Flutter APP 使用模擬感測資料展示完整操作流程，並保留 BLE 實體感測器的接入介面。

> 本專案僅供專題研究與展示使用，不提供醫療診斷。
