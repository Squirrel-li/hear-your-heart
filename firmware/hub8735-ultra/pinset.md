| PIN NUMBER | TYPE                         | 用途                                    | 換成ZJY180-TV2（去掉OLED、保留按鈕）          |
| ---------- | ---------------------------- | --------------------------------------- | --------------------------------------------- |
| D0 (PF2)   | I2C1 SDA (Wire1)             | LSM6DS3 IMU，位址0x6B                   | 不變                                          |
| D1 (PF1)   | I2C1 SCL (Wire1)             | LSM6DS3 IMU                             | 不變                                          |
| D2 (PF0)   | ADC0                         | 電池電壓（經1/2分壓）                   | 不變                                          |
| D3 (PE4)   | I2C0 SDA (Wire)              | SH1106 OLED                             | GPIO OUTPUT：螢幕CS2（觸控片選）              |
| D4 (PE3)   | I2C0 SCL (Wire)              | SH1106 OLED                             | 未使用                                        |
| D5 (PE2)   | UART3 RX (Serial3)           | BMH08002-4 PPG TX，38400 baud           | 不變                                          |
| D6 (PE1)   | UART3 TX (Serial3)           | BMH08002-4 PPG RX                       | 不變                                          |
| D7 (PD17)  | GPIO OUTPUT                  | BMH08002-4 EN                           | 不變                                          |
| D8 (PD15)  | GPIO INPUT_PULLUP            | UP按鈕                                  | 不變                                          |
| D9 (PA2)   | GPIO INPUT_PULLUP            | DOWN按鈕                                | 不變                                          |
| D10 (PA3)  | GPIO INPUT_PULLUP            | OK按鈕                                  | 不變                                          |
| D11 (PF14) | I2S0 DIN                     | MAX98357 DIN（左右兩顆共用）            | 不變                                          |
| D12 (PF15) | I2S0 LRC                     | MAX98357 LRC（與板上Func button同腳）   | 不變                                          |
| D13 (PF10) | 未使用                       | 板上Camera LED同腳                      | GPIO OUTPUT：螢幕DC（LED會跟著亮滅）          |
| D14 (PA5)  | 未使用                       | 燒錄腳 FUNCTION                         | 不變（不可用）                                |
| D15 (PD16) | GPIO OUTPUT                  | MAX98357 EN（左右兩顆SD_MODE共用）      | 不變                                          |
| D16 (PF3)  | LOG UART RX (Serial)         | DEBUG_IN                                | 不變                                          |
| D17 (PF4)  | LOG UART TX (Serial)         | DEBUG_OUT                               | 不變                                          |
| D18 (PF5)  | 未使用                       |                                         | SPI1 MISO：螢幕MIS（觸控讀值）                |
| D19 (PF6)  | 未使用                       |                                         | SPI1 SCLK：螢幕CLK                            |
| D20 (PF7)  | 未使用                       |                                         | SPI1 MOSI：螢幕MOS                            |
| D21 (PF8)  | 未使用                       |                                         | SPI1 CS：螢幕CS1（LCD片選）                   |
| D22 (PF11) | I2S0 MCLK                    | 未接線，但I2S初始化會佔用，不可他用     | 不變                                          |
| D23 (PF12) | I2S0 SD_RX                   | 未接線，但I2S初始化會佔用，不可他用     | 不變                                          |
| D24 (PF13) | I2S0 BCLK                    | MAX98357 BCLK（左右兩顆共用）           | 不變                                          |

MAX98357 EN（D15）接法：

- 兩片模組都拆掉SD_MODE上的1M上拉（以及先前並聯的104／334），只靠晶片內部約100k下拉。
- 左聲道那片的SD_MODE直接接D15；右聲道那片經約200k（150k~220k）串聯電阻接D15。
- D15拉LOW時兩片都是0V，硬體關斷；開機GPIO尚未設定時，內部下拉也讓兩片維持關斷。
- D15推HIGH（3.3V）時，左片為3.3V（>1.4V，左聲道），右片約1.1V（0.77~1.4V，右聲道）。不可改成放開高阻抗，否則兩片都會被內部下拉關斷。
- 接好後實測：HIGH時左片>1.4V、右片0.9~1.3V；LOW時兩片接近0V。

換成ZJY180-TV2時：

- 螢幕RES接板子RESET（CHIP_EN），跟主板一起重置；接線前先量RESET節點確認是低有效。
- 螢幕PEN不接，觸控改由程式透過SPI1輪詢XPT2046。
- 螢幕BLK接3.3V（背光常亮）、VCC接3.3V、GND接GND、NC不接。
- MAX98357 EN維持D15，不受換螢幕影響。
- I2S0的 `i2s_init()` 寫死佔用D11/D12/D22/D23/D24五腳（見 `.agents/MEMORY.md`），D22/D23不可他用。
