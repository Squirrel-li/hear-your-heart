# HUB8735 MP3 BPM 分析

把 `external/libraries/MiniBPM` 加到 Arduino IDE 的程式庫搜尋路徑，開啟 `analyze_mp3_bpm.ino`。將要分析的 MP3 放在 SD 卡根目錄，修改草稿開頭的 `song` 檔名，燒錄後以 115200 baud 看輸出。草稿使用 HUB8735 套件內建的 Helix `mp3dec.h` 解碼，將聲道混成 mono PCM，分析前 60 秒；不需要 MAX98357 或喇叭。這是獨立範例，尚未連到現有選歌任務。

電腦端可用同一份 MiniBPM 程式庫快速檢查：

```powershell
g++ -std=c++11 -O2 -Iexternal/libraries/MiniBPM/src external/libraries/MiniBPM/src/MiniBpm.cpp tools/analyze_pcm_bpm.cpp -o analyze_pcm_bpm.exe
.\analyze_pcm_bpm.exe --self-test
ffmpeg -i firmware/hub8735-ultra/testdata/audio/music2.mp3 -t 60 -ac 1 -ar 22050 -f s16le music2.pcm
.\analyze_pcm_bpm.exe music2.pcm
```

`music2.mp3` 的本機結果為 95.0 BPM。MiniBPM 假設固定速度；半拍、雙拍或複合拍的解讀可能不同於 `song_catalog.h`，所以不自動覆寫目前曲目表。尚未在實際 HUB8735 上量測記憶體、耗時或核對輸出。
