#ifndef HELIX_STATE_H
#define HELIX_STATE_H

#include <stdint.h>

// libhmp3.a（SDK 的 Helix MP3 解碼器）以靜態緩衝區編譯：MP3InitDecoder() 每次都清空並回傳
// 同一份狀態，所以 MAX98357 播放、MusicBpmAnalyzer、MusicGenreClassifier 其實共用一個解碼器。
// 播放與分析交錯解碼時，由 audioTask 在切換前呼叫 helixUse()，備份目前使用者的狀態（約 23 KB）
// 並載入下一個使用者的狀態，兩邊的解碼結果就與各自單獨解碼完全相同。
enum class HelixUser : uint8_t { Playback, Analysis };

// 檢查狀態配置並建立兩份初始狀態。回傳 false 時不可交錯解碼（helixUse 不做任何事）。
bool helixStateInit();
// 之後的 MP3InitDecoder / MP3Decode 都作用在 user 的狀態上。
void helixUse(HelixUser user);

#endif
