#ifndef SONG_CATALOG_H
#define SONG_CATALOG_H

#include <stdint.h>

#include "music_library.h"

// 曲目清單來自 SD 卡根目錄的 music_library.json（範例見 testdata/sd_music），
// 由 audioTask 開機載入到 musicLibrary；檔名與 BPM 以該檔為準。
// 60-180 BPM 以 5% 容差比對，沒有符合者改選最接近目標 BPM 的曲目。
// ponytail: 失敗遮罩為 32 位元，與 MUSIC_LIBRARY_MAX 相同；曲目上限提高時需一併加寬。

static uint32_t bpmDistance(uint16_t a, uint16_t b)
{
    return a > b ? a - b : b - a;
}

static bool songUnavailable(int index, uint32_t unavailableMask)
{
    return index >= 0 && index < 32 && (unavailableMask & (1u << index)) != 0;
}

static int nextMatchingSong(uint16_t targetBpm, int currentSong,
                            uint32_t unavailableMask = 0)
{
    if (targetBpm == 0 || musicLibrary.count == 0) return -1;
    for (int step = 1; step <= static_cast<int>(musicLibrary.count); ++step) {
        const int index = (currentSong + step) % static_cast<int>(musicLibrary.count);
        if (songUnavailable(index, unavailableMask) || musicLibrary.entries[index].bpm == 0) continue;
        if (bpmDistance(musicLibrary.entries[index].bpm, targetBpm) * 100u <=
            (uint32_t)targetBpm * 5u) return index;
    }
    return -1;
}

static int closestSong(uint16_t targetBpm, int currentSong,
                        uint32_t unavailableMask = 0)
{
    if (targetBpm == 0 || musicLibrary.count == 0) return -1;
    // ponytail: Linear scan is sufficient for at most 32 songs.
    int best = -1;
    uint32_t bestDistance = UINT32_MAX;
    for (int step = 1; step <= static_cast<int>(musicLibrary.count); ++step) {
        const int index = (currentSong + step) % static_cast<int>(musicLibrary.count);
        if (songUnavailable(index, unavailableMask) || musicLibrary.entries[index].bpm == 0) continue;
        const uint32_t distance = bpmDistance(musicLibrary.entries[index].bpm, targetBpm);
        if (distance < bestDistance) {
            best = index;
            bestDistance = distance;
        }
    }
    return best;
}

static int songForStart(uint16_t targetBpm, int currentSong,
                         uint32_t unavailableMask = 0)
{
    const int matching = nextMatchingSong(targetBpm, currentSong, unavailableMask);
    return matching >= 0 ? matching : closestSong(targetBpm, currentSong, unavailableMask);
}

static int songAfterFinish(uint16_t targetBpm, int currentSong,
                            uint32_t unavailableMask = 0)
{
    const int matching = nextMatchingSong(targetBpm, currentSong, unavailableMask);
    return matching >= 0
        ? matching
        : currentSong >= 0 &&
          currentSong < static_cast<int>(musicLibrary.count) &&
          musicLibrary.entries[currentSong].bpm != 0 &&
          !songUnavailable(currentSong, unavailableMask)
            ? currentSong
            : closestSong(targetBpm, currentSong, unavailableMask);
}
#ifdef SONG_CATALOG_SELF_TEST
#include <assert.h>
MusicLibrary musicLibrary{};
int main(void)
{
    const char* json = "{\"version\":1,\"songs\":["
        "{\"file\":\"001.mp3\",\"bpm\":136},{\"file\":\"002.mp3\",\"bpm\":143},"
        "{\"file\":\"003.mp3\",\"bpm\":100},{\"file\":\"004.mp3\",\"bpm\":120},"
        "{\"file\":\"005.mp3\",\"bpm\":85},{\"file\":\"006.mp3\",\"bpm\":95},"
        "{\"file\":\"007.mp3\",\"bpm\":97},{\"file\":\"008.mp3\",\"bpm\":89},"
        "{\"file\":\"009.mp3\",\"bpm\":117},{\"file\":\"010.mp3\",\"bpm\":111},"
        "{\"file\":\"011.mp3\",\"bpm\":135},{\"file\":\"012.mp3\",\"bpm\":103},"
        "{\"file\":\"013.mp3\",\"bpm\":138},{\"file\":\"014.mp3\",\"bpm\":111},"
        "{\"file\":\"015.mp3\",\"bpm\":160},{\"file\":\"016.mp3\",\"bpm\":113}]}";
    assert(music_json::parse(json, musicLibrary) && musicLibrary.count == 16);
    musicLibrary.count = 0;
    assert(songForStart(100, -1) == -1);  // 空表
    assert(music_json::parse(json, musicLibrary));
    assert(nextMatchingSong(135, -1) == 0);
    assert(nextMatchingSong(135, 0) == 10);
    assert(nextMatchingSong(135, 10) == 12);
    assert(nextMatchingSong(135, 12) == 0);
    assert(nextMatchingSong(70, -1) == -1);
    assert(closestSong(70, -1) == 4);
    assert(closestSong(0, -1) == -1);
    assert(songForStart(70, -1) == 4);
    assert(nextMatchingSong(85, -1) == 4);
    assert(nextMatchingSong(117, -1) == 3);
    assert(nextMatchingSong(160, -1) == 14);
    assert(closestSong(171, -1) == 14);
    assert(songAfterFinish(70, 4) == 4);
    assert(songAfterFinish(135, 4) == 10);
    const char* unknown = "{\"songs\":[{\"file\":\"unknown.mp3\"}]}";
    assert(music_json::parse(unknown, musicLibrary));
    assert(songForStart(100, -1) == -1);  // Unknown BPM is not selectable.
    return 0;
}
#endif

#endif
