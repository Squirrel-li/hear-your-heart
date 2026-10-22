# PPG 運動偽影處理原型

`motion_hr.py` 是電腦端回放／驗證版本，沒有接入 HUB 或改動手錶韌體。
使用現有 NumPy，無需 SciPy；目前執行環境 NumPy 2.3.5。

可用獨立的 [ble_sensor_capture 擷取韌體與 BLE 記錄頁](../../firmware/n76e003/application/ble_sensor_capture/README.md)
收集兩路 PPG＋26 Hz ACC。匯出檔以 `--sensor-time` 分析；若含 `sequence`，PPG 序號缺口會切段，ACC 缺口會拒絕受影響視窗。

## 執行

在專案根目錄執行（`python` 須為有 NumPy 的 Python）：

```powershell
python apps/bmh08002_ppg_analyzer/test_motion_hr.py
python apps/bmh08002_ppg_analyzer/motion_hr.py firmware/hub8735-ultra/examples/uart/bmh08002_4_control/hr2.csv --out hr2_result.csv
python apps/bmh08002_ppg_analyzer/motion_hr.py ppg.csv --sensor-time --acc acc.csv --out motion_result.csv
```

這台 Codex 環境沒有 PATH 上的 python，已驗證可用的執行檔為：
`C:/Users/Song-Hao Lee/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe`。
PowerShell 用 `& '完整執行檔路徑'` 取代上述 `python`。
輸出使用 exclusive create，既有檔案不覆寫。

## 輸入與時間

- PPG CSV：`elapsed_s,ir_ppg,red_ppg`；兩路為 0–65535 的模組波形數值。
- 可選 `status`：0 無接觸，1 有接觸但模組尚無有效結果，2 模組已有有效結果。
  自行分析允許 status=1，不依賴模組先算出心率。缺省視為未知接觸、由訊號檢查處理。
- 可選 `d1`：這個舊記錄器的模組 D3 心率欄位，只輸出中位數作比較，絕不送入演算法。
- ACC 獨立 CSV：`elapsed_s,acc_x,acc_y,acc_z`，必須是同一手錶、同一時間原點的實際新樣本。
  可用 g 或 raw counts；至少 20 Hz，建議手錶硬體的 26／52 Hz。程式不能辨別偽造時間戳的重複舊讀值。
- 指定 ACC 但缺少視窗覆蓋、出現 >100 ms 缺口或局部取樣不足時拒絕該視窗，不偷偷退回 PPG-only。
- 預設支援舊 PC CSV：以連續片段的筆數／時間跨度推算取樣率、均勻配置樣本，消除主機時間量化抖動。
  可用 `--sample-ms 28` 明確指定；這是假設片段內沒有無法從時間看出的漏樣，不能用來證明無漏包。
- `--sensor-time` 表示 `elapsed_s` 已換成感測器秒時間戳；依時間插值到均勻網格，視窗內 >2.5 倍週期缺口拒絕。
  原始 BLE 擷取仍須利用 sequence 檢查漏包。感測器時間戳需先展開計數器回繞。
- >150 ms PPG 缺口或錄製時間倒退會切段，不跨缺口分析；有 ACC 時不接受附加多次錄製的檔案。

## 處理流程

每 2 秒分析一個過去 10 秒視窗（首筆需等待約 10 秒）。這是原型參數，不變更正式韌體已確認的 10 秒不重疊週期。

1. 五點中位數偵測並替換孤立尖峰；大量尖峰、持續階躍／接觸突變、平線拒絕輸出。
2. 線性去趨勢、Hann 加窗，頻域保留 0.6–3.5 Hz。心率搜尋範圍 40–200 BPM。
3. 有 ACC：找三軸主要運動頻率，排除其附近 ±0.15 Hz 的 PPG 頻譜，抑制運動候選。
   沒有 ACC：只做 PPG 的預處理與品質檢查，不能宣稱消除共同的周期性運動。
4. 檢查剩餘能量、峰附近能量集中程度及其他競爭頻率；太靠近被移除的運動頻段也拒絕。
5. 兩路估計須在 max(6 BPM, 8%) 內一致，才取平均。品質不足輸出 0 與原因，不沿用舊心率冒充新量測。

`quality` 是 0–1 的頻譜集中度指標，不是準確率／機率。
`removed_fraction` 是被排除的頻帶能量比例，不能解讀為「已消除多少真實運動偽影」。
FFT 零填充讓候選搜尋較細，不提升 10 秒觀測窗本身的頻率解析度。
此版抑制的是心率估測所用的頻譜，沒有輸出可用於 HRV／血氧的乾淨波形。

## 已執行驗證（2026-10-18）

| 資料 | 結果 | 可支持的結論 |
| --- | --- | --- |
| 合成乾淨訊號＋雜訊，45/72/120/180/195 BPM | 每項誤差 <3 BPM | 基本頻率估計可運作 |
| 真值 72 BPM＋較強 126 BPM 動作訊號 | PPG-only 125.6 → ACC-assisted 72.2 BPM | 此合成條件下成功排除運動峰 |
| 心率與運動同頻 | 拒絕輸出 | 不將不可分辨訊號強行解讀成心率 |
| 平線、兩路不一致、持續階躍、NaN、1 Hz ACC | 拒絕 | 基本輸入與品質防線 |
| 孤立尖峰、固定 ACC、缺口切段、ACC 缺少覆蓋 | 檢查通過 | 回放與異常處理可運作 |
| 修改模組 D3 為不同數值 | 自算結果完全相同 | 未洩漏比較值到估測流程 |
| 原始 hr1.csv，約30秒 | 5/10 視窗接受，接受值中位數 59.8 BPM | 真實資料能回放，部分品質不足 |
| 原始 hr2.csv，約70秒 | 20/25 視窗接受，接受值中位數 69.2 BPM | 真實資料能回放，缺口後重新累積視窗 |

逐窗結果在 `results/hr1_motion.csv`、`results/hr2_motion.csv`。
這兩份原始 CSV 沒有同步加速度、獨立 ECG 心率真值或活動標註；模組 D3 也可能受運動影響。
因此未驗證真實運動準確度、未證明優於既有 `ppg_hr_int.py`，合成結果不能外推成產品準確度。
尚未以公開運動資料集作統計驗證，本次使用公開論文作方法參考。

## 方法依據與限制

參考 [SPECMAR 原論文](https://arxiv.org/html/1810.06196v1) 的加速度輔助頻譜抑制方向，
這裡採更保守的頻段排除，**不是 SPECMAR 重現**，不引用其準確率作本實作結果。
論文測試的光學硬體也不同於本專案 IR／紅光模組。

目前沒有時間追蹤器，避免將歷史值當成當下量測；頻譜閾值尚未用多人運動資料調校。
同頻心跳與運動、ACC 沒量到的皮膚形變、動作諧波、低灌流、光學飽和、強非平穩動作仍可能失敗。
特別是沒有 ACC 時，兩路共同的假心跳可能通過全部檢查。

BMH08002-4 的兩路是經滿刻度參數縮放的波形，不是未處理 ADC：
[模組資料表](https://www.bestmodulescorp.com/en/amfile/file/download/file/884/product/773/)。
自動增益／縮放帶來的非線性變化不能保證由這個原型恢復。

移植 HUB 前，需要同步記錄手錶 PPG＋26/52 Hz ACC＋獨立參考心率，涵蓋靜止、走路、跑步、擺手與鬆動佩戴。
驗收同時看接受比例、接受視窗的誤差和錯誤有效輸出率，不能只看平均值。
NumPy 用於電腦端研究；尚未測量 HUB 的 C/C++ 實作記憶體、執行時間或與播放並行的表現。
