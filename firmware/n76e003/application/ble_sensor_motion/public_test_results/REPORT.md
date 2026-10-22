# TROIKA 公開運動資料外部測試

測試日期：2026-10-18。**目前 N76E003 演算法尚不能視為運動下持續可用的心率方案。**
加速度輔助降低部分錯誤輸出，但大量視窗被拒絕，且仍有錯誤有效輸出。
此次沒有修改演算法或用參考心率調整門檻。

## 資料與來源

- TROIKA／2015 IEEE Signal Processing Cup 的12組跑步機資料，約60分鐘。
- 每組有兩路手腕PPG、三軸加速度、胸部ECG；原始取樣率均為125 Hz。
- 使用資料提供的 `BPM0` ECG衍生心率真值：8秒視窗、每2秒更新。
- 資料來自 [Georgia Tech OLIVES 研究團隊 HeartBEAT repository](https://github.com/olivesgatech/HeartBEAT)，固定commit：`5ce28d20cc59dcca696120cc0db4797ce5af526a`。
- 原始資料論文：Z. Zhang, Z. Pi, B. Liu, *TROIKA: A General Framework for Heart Rate Monitoring Using Wrist-Type Photoplethysmographic Signals During Intensive Physical Exercise*, IEEE TBME, DOI [10.1109/TBME.2014.2359372](https://doi.org/10.1109/TBME.2014.2359372)。
- 資料隨附Readme允許適當引用下的學術研究用途；不把repository程式的MIT授權自動套到資料。
  原始約8MB MAT資料下載在 `.tmp/troika-public`，不隨程式碼重新散布。

這批資料是不同光學硬體的雙路PPG，不是我們模組的IR＋red波形，不能當成手錶實機驗收。

## 方法與可重現性

直接用GCC編譯同一份 `ppg_motion_hr.h`，沒有改寫成另一套Python算法。
演算法SHA256：`5f22b03bfec52234999d690169d309370cdbf94ea738439bca3d6be25c40da29`。
全部12組都納入，不因結果好壞挑選受試者。

輸入適配：

1. SciPy `resample_poly` 抗混疊重取樣：PPG 125→35.714 Hz、ACC 125→26 Hz。
2. PPG固定映射 `round(16384 + 8*x)` 到uint16；不使用每段／每人標準化，也不使用ECG標籤決定增益。
3. ACC按g單位假設轉為本機±8g感測器counts：`round(x/0.000244)`。
   原資料Readme沒有明列ACC單位，因此這是適配假設；另以0.5倍／2倍增益作敏感度檢查。
4. 主測試沒有數值clipping。輸入狀態設為有接觸（1），來源沒有本模組接觸旗標。
5. 從頭連續回放，維持時間回繞與演算法狀態；沒有每視窗重置或偷用未來心率。

重取樣濾波是離線輸入適配，不代表真實硬體取得一模一樣的波形；它也有有限的非因果鄰近樣本依賴。
5秒估測視窗與8秒ECG真值不完全一致：以估測視窗中心 `end-2.5s` 對齊真值中心 `4+2*i` 並線性插值。
排除開始10秒之前與超出真值時間範圍的輸出，不以邊界外插補出真值。
所以這不是TROIKA論文原本8秒／2秒評估協定的直接排名比較。

比較模式：

- **ACC輔助**：目前的完整算法，要求加速度有效。
- **PPG-only**：同一份新算法，不餵ACC且不要求ACC；不是舊 `ppg_hr.h` 的結果。

錯誤／無效輸出不沿用前一筆，無效的0不拿去計算MAE。
MAE必須連同coverage看；`within5_of_all_pct`把所有評估輸出作分母，避免大量拒絕讓數字看起來過度樂觀。

## 主結果

| 範圍／模式 | 有效／評估輸出 | 有效比例 | 有效輸出的MAE | 有效且誤差≤5 BPM，占全部輸出 |
| --- | --- | --- | --- | --- |
| 全部，ACC輔助 | 101／697 | **14.49%** | **4.66 BPM** | **13.49%** |
| 全部，PPG-only | 105／697 | 15.06% | 21.68 BPM | 10.33% |
| 跑步區段，ACC輔助 | 51／576 | **8.85%** | **6.39 BPM** | **7.81%** |
| 跑步區段，PPG-only | 65／576 | 11.28% | 34.03 BPM | 5.56% |
| 靜止區段，ACC輔助 | 50／121 | 41.32% | 2.90 BPM | 40.50% |

靜止／跑步以資料Readme的流程推定：前30秒與270秒後為靜止，中間為跑步。
各檔長度不同，並沒有逐樣本活動標記，因此這是粗略分組，非精準活動分段。

完整模式的697個評估輸出中：

- 335次因動作頻率歧義拒絕。
- 257次因訊號／心搏間隔品質不足拒絕。
- 4次因雙通道不一致拒絕。
- 101次輸出有效心率；其中 **5次仍誤差>10 BPM**。

跑步的51次有效結果裡，4次誤差>10 BPM，95百分位誤差約60.93 BPM。
因此連「已通過品質檢查」也不是可靠性保證。
個別受試者差異很大，例如第2組ACC輔助只有6次有效，這6次的MAE為32.62 BPM。

## 縮放敏感度：未挑選最好設定來替換主結果

固定同一份算法，只改輸入適配增益：

| 適配設定 | 整體有效比例 | 有效MAE | 跑步有效比例 |
| --- | --- | --- | --- |
| PPG增益4 | 11.91% | 6.50 BPM | 6.94% |
| **主設定：PPG增益8** | **14.49%** | **4.66 BPM** | **8.85%** |
| PPG增益16 | 16.28% | 2.26 BPM | 9.08% |
| ACC增益為主設定的一半 | 14.78% | 4.54 BPM | 8.51% |
| ACC增益為主設定的兩倍 | 14.35% | 4.67 BPM | 8.85% |

PPG增益16會產生少量飽和值及重置，僅有694個可評估輸出，和主設定697個並非完全相同分母；
上表coverage定義為實際發出的評估輸出比例，重置後不發出的暖機時間不計入，不能當成整段時間可用率。
即使如此，這些縮放選項的跑步有效比例都不到10%，不能靠這項適配就解決可用率不足。
敏感度是開發診斷，不是獨立驗證集上的最佳參數選擇。

## 交付檔案與重跑

- `windows.csv`：每筆估測、參考值、誤差、有效性、拒絕原因。
- `summary.json`：全體／粗分活動／每位受試者的統計。
- `manifest.json`：固定來源、每個資料檔SHA256、算法hash、NumPy／SciPy版本與適配設定。
- `sensitivity.json`：另外四種縮放的完整模式統計。
- runner：`../../ble_sensor_hr/benchmark_troika.py`，包含對齊與統計的可執行assert檢查。

在專案根目錄，有NumPy＋SciPy及gcc的環境：

```powershell
python firmware/n76e003/application/ble_sensor_hr/benchmark_troika.py --data .tmp/troika-public --out NEW_RESULTS_DIRECTORY --download
```

本次只把SciPy 1.16.3安裝在 `.tmp/ppg-public-deps`，沒有改系統Python或韌體依賴。
本機可用：

```powershell
$env:PYTHONPATH=(Resolve-Path .tmp/ppg-public-deps).Path
& "$env:USERPROFILE/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe" firmware/n76e003/application/ble_sensor_hr/benchmark_troika.py --data .tmp/troika-public --out NEW_RESULTS_DIRECTORY
```

後續若修改算法，這12組已成為開發用資料，應另留未調參的資料作最終驗證。
目前可支持的結論是：**記憶體與算力放得下，但目前這版的公開跑步資料效果還不夠，不能只憑先前合成測試稱為抗運動成功。**
