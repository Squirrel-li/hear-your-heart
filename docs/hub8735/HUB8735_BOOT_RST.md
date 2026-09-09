# HUB8735 Ultra 外部開發板 BOOT/RESET 控制

## 目的

使用另一塊開發板自動控制 HUB8735 Ultra 進入燒錄模式與 Reset。建議第一版只讓外部開發板負責 BOOT/RESET 時序，實際韌體仍由 PC 經 HUB8735 Ultra 的 DEBUG USB 上傳。

目前設計定案：外部控制板使用 3.3 V GPIO；BOOT 使用 `JA1/JA2`，RESET 另飛一條訊號線至 HUB8735 Ultra 的 `CHIP_EN`／RESET 節點。

## 已確認的硬體條件

- HUB8735 Ultra 的 `JA2-2`／Arduino 腳位 `14` 是 `BOOT_MODE`。
- `JA2-1` 是 `3.3V`，專門作為 BOOT pin pull-up 電源。
- 官方 Arduino 使用說明要求將 `BOOT_MODE (14)` 短接到 `3.3V`，再按下 RESET 進入燒錄模式。
- 原理圖中的 RESET 按鍵為 `SWA1`，連接至 `CHIP_EN` 網路。
- DEBUG USB 內建 CH340N，可由 PC 提供下載與除錯介面。
- `GPIOE_5` 在原理圖上有 `TPA6` 測試點；標註說明 `GPIOA_5` 與 `GPIOE_5` 都拉高可進入 load-code mode，但官方開發板操作只要求 `BOOT_MODE (14)` 接至 `3.3V`。

## 建議接線

| HUB8735 Ultra | 外部開發板／控制電路 | 用途 |
|---|---|---|
| `JA2-2 / 14 / BOOT_MODE` | `BOOT_CTL`，只能輸出 3.3 V 或高阻態 | 控制燒錄模式 |
| `JA2-1 / 3.3V` | 受控開關的 3.3 V 來源 | 拉高 `BOOT_MODE` |
| `JA1-2 / GND` | 外部開發板 `GND` | 共地 |
| `CHIP_EN`／RESET 節點 | 另飛一條 RESET 訊號線；經 PhotoMOS、開漏 NMOS 或 NPN 控制 | 模擬 RESET 按鍵 |

HUB8735 Ultra 建議由自己的 USB-C 供電，外部開發板只共地並控制訊號，不要任意用外部開發板的電源替 HUB8735 Ultra 供電。

## 電氣注意事項

1. 目前控制板固定使用 3.3 V GPIO，因此不需要 level shifter。若日後改用 5 V 開發板，BOOT 控制必須加入 3.3 V level shifter 或 3.3 V 三態緩衝器，不可直接接到 `BOOT_MODE`。
2. `BOOT_CTL` 在外部開發板上電、重置及非燒錄狀態都應為高阻態，避免意外讓 HUB8735 Ultra 一直進入燒錄模式。
3. `BOOT_MODE` 只在進入燒錄時暫時拉高，完成取樣後應解除連接。
4. RESET 飛線不建議由 push-pull GPIO 直接硬推。優先使用 PhotoMOS 或開漏 NMOS/NPN；接線前應先用萬用表確認 `CHIP_EN`／RESET 的低有效節點。
5. 控制板與 HUB8735 Ultra 的電源可能不同，避免經 GPIO 保護二極體回灌電源；必要時在控制線加入隔離或三態緩衝。

## 建議控制時序

```text
1. HUB8735 Ultra 已上電，DEBUG USB 已連接 PC。
2. BOOT_CTL 接通，使 BOOT_MODE = 3.3V。
3. 模擬按下 RESET，先以約 50～100 ms 作為測試脈衝。
4. 放開 RESET。
5. 短時間保持 BOOT_MODE 有效，等待 USB/COM 下載介面出現。
6. BOOT_CTL 回到高阻態，解除 BOOT_MODE 短接。
7. 由 Arduino IDE 或其他已確認的下載工具上傳韌體。
```

`50～100 ms` 是控制器的實作起始值，不是目前文件明列的 RTL8735B 最小 Reset 時間；應依 USB/COM 枚舉結果與實機波形調整。若控制器無法偵測 COM，可先在 RESET 放開後固定保持 BOOT 約 100～500 ms，再解除。

## 控制器韌體的安全初始狀態

```text
BOOT_CTL  = high-impedance / released
RESET_CTL = released
```

只有收到燒錄命令後才執行上述時序。若控制器重置或斷電，兩個控制輸出都必須回到 released，避免 HUB8735 Ultra 被卡在 RESET 或燒錄模式。

## 測試順序

1. 先不接外部控制板，手動短接 `14` 與 `3.3V`，按 RESET，確認 HUB8735 Ultra 能進入燒錄模式。
2. 使用 3.3 V 邏輯分析儀或萬用表確認 `BOOT_MODE` 在 RESET 釋放時為高電位。
3. 外部控制板先只接 `GND` 與 `BOOT_MODE`，手動按 RESET，確認外部拉高有效。
4. 再接入 `CHIP_EN` RESET 飛線，確認控制板上電後 HUB8735 Ultra 不會被持續重置。
5. 最後才接上 PC 下載流程，確認每次都能穩定枚舉 COM 並完成上傳。
6. 若只控制 `14` 仍無法進入燒錄模式，再檢查原理圖中的 `GPIOE_5/TPA6` 是否需要額外控制；不要直接把 `TPA6` 接到未確認電壓的 GPIO。

## PC 輔助與獨立燒錄的界線

本方案中的另一塊開發板只負責按鍵與時序控制。它不會因為控制了 `BOOT_MODE` 和 RESET 就自動具備燒錄 `.bin` 的能力。

若要完全獨立燒錄，還需要另外實作或移植 HUB8735/RTL8735B 的下載協定，並處理 USB host 或 LOG UART 連線；這是另一個工作項目，不應與 BOOT/RESET 控制混在第一版硬體中。

## 製作前需要確認

1. 外部開發板的確切型號，供後續撰寫控制器韌體時指定 GPIO 編號。
2. RESET 飛線的實際焊接位置，以及量測確認的 `CHIP_EN` 有效電位。
3. HUB8735 Ultra 是否由 USB-C 獨立供電。



## 參考文件

- `external/HUB-8735/HUB 8735 ultra toolkit/Hardware/HUB 8735 ultra Module Specification_V01_20240508.pdf`
- `external/HUB-8735/HUB 8735 ultra toolkit/Hardware/HUB 8735 ultra Schematic.pdf`
- `external/HUB-8735/HUB 8735 ultra toolkit/Software/HUB 8735 ultra 開發板【Arduino 使用說明】_20240508.pdf`
- `external/HUB-8735/amebapro2_arduino/Arduino_package/hardware/variants/ameba_hub8735_ultra/variant.h`