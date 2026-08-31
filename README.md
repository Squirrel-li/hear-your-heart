# 聽見你的心（Hear Your Heart）

一套結合穿戴式感測器、嵌入式揚聲器與 Flutter APP 的智慧音樂系統。專案根據使用者的即時心率、呼吸與活動狀態分析身體狀況，動態選擇合適的音樂與節奏。

## 專案檔案結構

```text
hear-your-heart/
├── assets/                    # 硬體架構圖與專案素材
├── doc/                       # 參考文件
├── external/                  # 外部工具包與第三方函式庫
├── project/                   # 專題規劃、簡報與行政文件
├── scr/
│   ├── embbed_loudspeaker/    # 嵌入式揚聲器程式
│   ├── phone_app/             # Flutter 行動應用程式
│   │   ├── assets/audio/      # 展示用音訊
│   │   ├── lib/               # APP 主要程式碼
│   │   ├── test/              # Flutter 測試
│   │   └── tool/              # 輔助工具
│   └── remote_sensor/         # 穿戴式感測與資料傳輸程式
├── test/                      # 開發板與周邊元件測試
├── tools/                     # 開發工具
└── README.md
```

目前 Flutter APP 使用模擬感測資料展示完整操作流程，並保留 BLE 實體感測器的接入介面。

> 本專案僅供專題研究與展示使用，不提供醫療診斷。
