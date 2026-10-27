# 聽見你的心（Hear Your Heart）

依穿戴裝置量到的心率與動作，即時挑選節奏相符的音樂播放。

- **HUB8735 Ultra**：接收感測資料、分析曲庫BPM與曲風、依心率選歌播放，以OLED與按鍵操作。
- **手錶（N76E003＋BM7701）**：量測PPG心率與IMU動作，經BLE傳給HUB。
- **綁帶**：-----。

## 專案結構

```text
hear-your-heart/
├── apps/                    # 電腦與手機端測試工具（BLE感測模擬器、PPG分析）
├── assets/                  # 硬體架構圖、系統與資料流程圖
├── docs/                    # 系統架構、HUB操作與BLE文件、原廠參考資料
├── external/                # AmebaPro2 SDK與Arduino函式庫
└── firmware/
    ├── hub8735-ultra/       # HUB主程式、範例、主機檢查、SD卡內容
    └── n76e003/             # 手錶韌體、BSP（子模組）、BM7701測試腳本
```

## 開發工具

| 用途 | 工具與版本 |
| --- | --- |
| HUB韌體 | Arduino IDE 2.3.10、AmebaPro2板子套件4.1.1、MAX98357函式庫1.1.0（`external/libraries`） |
| 手錶韌體 | NuEclipse 8051 V1.02.028r、SDCC 4.2.6、Nu-Link燒錄；BSP為子模組[N76E003_BSP](https://github.com/OpenNuvoton/N76E003_BSP)（`firmware/n76e003/N76E003_BSP`），clone後執行`git submodule update --init` |
| BLE感測模擬器 | Flutter 3.44.7（Dart 3.12.2） |
| 分析與測試腳本 | Python 3.12、g++（MinGW GCC 6.3.0） |

## 重要文件

- [系統架構](docs/ARCHITECTURE.md)：模組分工、資料流與已知落差
- [HUB操作說明](docs/hub8735/HUB_UI_operation.md)：OLED選單與按鍵操作
- [心率選曲流程](docs/hub8735/HRMAX_MUSIC_FLOW.md)：心率Zone、目標BPM與曲庫分析
- [序列埠指令](docs/hub8735/SERIAL_COMMANDS.md)：HUB除錯與控制指令
- [BLE封包格式](firmware/n76e003/ble-notify-data-format.md)：手錶／綁帶傳給HUB的資料格式
- [HUB接腳](firmware/hub8735-ultra/pinset.md)：HUB8735 Ultra的接線
- SD卡內容：repo只附曲風模型`firmware/hub8735-ultra/sd_card/NN_MDL/yamnet.nb`；歌曲與DEMO音樂因授權不隨repo散布，請自行放入SD卡（DEMO曲目路徑見[DEMO模式](docs/hub8735/DEMO_MODE.md)）

## 授權

本專案以[Apache License 2.0](LICENSE)授權，散布時須保留[NOTICE](NOTICE)。子模組與第三方元件依各自授權。

> 僅供專題研究與展示，不提供醫療診斷。
