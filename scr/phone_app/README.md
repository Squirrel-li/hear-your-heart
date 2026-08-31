# 聽見你的心

依即時心率與呼吸狀態動態選歌的 Flutter 展示 APP。第一版以軟體模擬穿戴式感測器，並保留未來 BLE 資料源的接入邊界。

## 執行與建置

```powershell
flutter pub get
flutter analyze
flutter test
flutter run
flutter build apk --release
```

Android 最低版本為 API 24。Release APK 使用 Flutter 專案預設的開發簽章，只適合展示安裝，不可直接上架商店。iOS 專案已保留，需在 macOS 與 Xcode 環境建置。

## 操作

- 儀表板：查看 HR、RR、HRmax%、心率 Zone、活動狀態、選歌模式與訓練建議。
- 感測器模擬：選擇暖身、有氧、耐力或過載，或切換手動模式直接調整 HR、RR、活動強度。
- 播放：播放六段程式生成的示範節奏，並查看 BPM、energy 與 style。
- 設定：保存年齡、健身目標，或暫停模擬器驗證 3 秒失聯行為。

## BLE 接入介面

`SensorDataSource` 只要求 `Stream<SensorSnapshot>`、`start`、`stop` 與 `dispose`。未來 BLE adapter 應完成封包 header、版本、長度、CRC16、sequence 與 timestamp 驗證後，再輸出：

```text
SensorSnapshot(timestamp, hr, respiratoryRate, rmssd, sdnn, activity, valid)
```

第一版不猜測尚未定義的 payload 位元寬度、endianness 或 CRC polynomial，也不加入 BLE plugin。真實規格確認後，只需以 BLE adapter 取代 `SimulatedSensorDataSource`，UI 與決策引擎不需修改。

## 音樂 catalog

`lib/domain.dart` 的 `demoTracks` 保存 `title / bpm / energy / style / assetPath`。音訊位於 `assets/audio/`，可用下列命令確定性重建：

```powershell
dart run tool/generate_demo_audio.dart
```

目前音軌為數學波形生成的展示節奏，不含第三方音樂素材。

> 本 APP 為專題展示用途，Zone 4/5 提示不是醫療診斷。
