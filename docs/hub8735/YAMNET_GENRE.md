# HUB8735：YAMNet 曲風分析（規劃草案）

> 狀態（2026-10-18）：**本文的自訂分類頭與多片段策略仍是提案；主程式已實作 SDK YAMNet 曲風分析及儲存。** 現行入口是 `audioTask.cpp::advanceBpmScan` 與 `music_genre_classifier.cpp`，不代表本文方案已全部實作，也不代表分類品質已實機驗收。使用者目前可在停止播放時選新檔／未知 BPM 或全部重分析；自動發現新增／替換檔與播放中延後分析尚未完成。有有效 BPM 即可自動選曲，曲風失敗不阻擋。

## 可行性依據

- Ameba 官方 [AI 模型轉換頁](https://www.amebaiot.com/en/amebapro2-ai-convert-model/) 的支援模型表列有 **YAMNet**，類別為 Sound Classification，需提供 `.h5` 檔，是表中唯一的音訊模型。
- 轉換採線上服務：上傳模型，完成後以 email 寄送下載連結；安裝方式見官方「Customized AI model Installation Guide」（需登入）。量化用圖片為選填，最多 10 張（該頁只在影像模型脈絡說明，是否適用 YAMNet 未載明）。
- 該頁**未說明** YAMNet 的輸入形狀、前處理、範例程式與限制，下方「待驗證」逐項列出。

## YAMNet 名詞

| 名詞 | 說明 |
| --- | --- |
| YAMNet | Google 以 AudioSet 訓練的音訊事件分類模型，骨幹為 MobileNetV1（depthwise separable convolution），約 3.7M 參數 |
| AudioSet | 訓練資料與 ontology，共 521 類；其中含音樂曲風類（如 Pop、Rock、Hip hop、Electronic、Classical、Jazz 等，實際名稱以 `yamnet_class_map.csv` 為準） |
| 波形輸入 | 16 kHz、單聲道、float，範圍 [-1, 1] |
| Frame / STFT | 視窗 25 ms、步長 10 ms，FFT 長度 512 |
| Mel 頻譜 | 64 個 mel 頻帶，範圍 125–7500 Hz，取 `log(mel + 0.001)` |
| Patch | 連續 96 個 frame（約 0.96 s）× 64 mel，是模型一次推論的輸入單位，輸入張量為 96×64 |
| Patch hop | 相鄰 patch 的間隔，預設 0.48 s（50% 重疊） |
| Scores | 每個 patch 輸出 521 維類別分數（sigmoid，各類獨立，不互斥） |
| Embedding | 分類層前的 1024 維特徵向量（global average pooling 後） |
| 分類頭 | 接在 embedding 後、自行訓練的小分類器，輸出本專案定義的曲風類別 |
| `.h5` | Keras 模型檔，Ameba 轉換服務接受的格式 |
| NB / 轉換後模型 | 轉換服務產出、可在 NPU 上執行的模型檔 |

以上數值為 YAMNet 公開規格（來自 TensorFlow models 的 `research/audioset/yamnet`），非 Ameba 頁面內容。

## 處理流程

### A. 離線準備（PC）

1. 取 YAMNet 的 Keras `.h5`，確認輸入端是「96×64 的 log-mel patch」還是「波形」（影響下方 CPU/NPU 分工）。
2. 若未來新增「期望曲風」選項，再決定使用者選項與分類結果的對應方式。
3. 用標註音樂資料（如 FMA）過 YAMNet 取 embedding，訓練分類頭；先評估準確度與混淆矩陣。
4. 組合「YAMNet + 分類頭」或僅轉換 YAMNet 並將分類頭放在 MCU 端，匯出 `.h5`。
5. 送 Ameba 線上服務轉換並安裝到韌體。

### B. 裝置端：自動發現新檔／替換檔（未完成的提案）

1. 偵測到新檔或檔案被替換 → 排入分析佇列（播放中發現的新檔延後處理）。
2. 解碼 MP3 → 重取樣為 16 kHz 單聲道。
3. 取數段代表性片段（例如歌曲中段 3 段，每段約 10 s）。
4. CPU 計算 log-mel，切成 96×64 patch。
5. NPU 推論每個 patch，得到 embedding / 曲風分數。
6. 對所有 patch 取平均，取最高分類別；信心不足時標為「未知」。
7. 記錄該檔曲風；有有效 BPM 即可自動選歌，曲風失敗不阻擋該檔。

### C. 選曲使用

目前自動播放只依目標 BPM 選曲；分析的曲風存入 JSON。未來可新增使用者設定「期望曲風」的選項，再決定曲風如何參與選曲與沒有符合曲目時的行為。

## 待驗證

| 項目 | 為何需要 |
| --- | --- |
| `.h5` 的輸入形式（波形或 96×64 patch） | 決定 log-mel 由 CPU 還是模型內計算；NPU 不一定支援 STFT 類運算 |
| 線上轉換是否成功、輸出精度 | 運算子支援與量化後準確度需實測 |
| 量化是否需校準資料 | 該頁僅說明影像；音訊應使用 log-mel patch |
| 單首歌分析耗時與記憶體 | 解碼＋多 patch 推論，需實測是否可接受 |
| 前處理與訓練一致 | CPU 端 mel 參數須與訓練完全相同，否則準確度下降 |
| 模型與 SD 卡音樂版權 | FMA 部分資料為 CC BY-NC-SA，用途需留意 |

## 待決架構問題

1. 未來新增「期望曲風」時，使用者選項如何對應現有分類結果？
2. 如何自動發現檔案被替換？目前只在手動掃描時以檔案大小與 FAT 修改時間判斷。
3. NPU 若日後同時用於麥克風即時推論（如語音關鍵字），與離線曲風分析如何排程或互斥。
