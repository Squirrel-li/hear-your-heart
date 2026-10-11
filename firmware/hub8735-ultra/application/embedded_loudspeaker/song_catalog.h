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
        if (songUnavailable(index, unavailableMask)) continue;
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
        if (songUnavailable(index, unavailableMask)) continue;
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
        : currentSong >= 0 && !songUnavailable(currentSong, unavailableMask)
            ? currentSong
            : closestSong(targetBpm, currentSong, unavailableMask);
}
#ifdef SONG_CATALOG_SELF_TEST
#include <assert.h>
MusicLibrary musicLibrary{};
int main(void)
{
    const char* json = "{\"version\":1,\"songs\":["
        "{\"file\":\"bpm060.mp3\",\"bpm\":60},{\"file\":\"bpm066.mp3\",\"bpm\":66},"
        "{\"file\":\"bpm075.mp3\",\"bpm\":75},{\"file\":\"bpm080.mp3\",\"bpm\":80},"
        "{\"file\":\"bpm085.mp3\",\"bpm\":85},{\"file\":\"bpm095.mp3\",\"bpm\":95},"
        "{\"file\":\"bpm097.mp3\",\"bpm\":97},{\"file\":\"bpm106.mp3\",\"bpm\":106},"
        "{\"file\":\"bpm117.mp3\",\"bpm\":117},{\"file\":\"bpm132.mp3\",\"bpm\":132},"
        "{\"file\":\"bpm135.mp3\",\"bpm\":135},{\"file\":\"bpm137.mp3\",\"bpm\":137},"
        "{\"file\":\"bpm138.mp3\",\"bpm\":138},{\"file\":\"bpm147.mp3\",\"bpm\":147},"
        "{\"file\":\"bpm156.mp3\",\"bpm\":156},{\"file\":\"bpm171.mp3\",\"bpm\":171}]}";
    assert(music_json::parse(json, musicLibrary) && musicLibrary.count == 16);
    musicLibrary.count = 0;
    assert(songForStart(100, -1) == -1);  // 空表
    assert(music_json::parse(json, musicLibrary));
    assert(nextMatchingSong(135, -1) == 9);
    assert(nextMatchingSong(135, 9) == 10);
    assert(nextMatchingSong(135, 10) == 11);
    assert(nextMatchingSong(135, 12) == 9);
    assert(nextMatchingSong(70, -1) == -1);
    assert(closestSong(70, -1) == 1);
    assert(closestSong(0, -1) == -1);
    assert(songForStart(70, -1) == 1);
    assert(nextMatchingSong(60, -1) == 0);
    assert(nextMatchingSong(117, -1) == 8);
    assert(nextMatchingSong(171, -1) == 15);
    assert(songAfterFinish(70, 4) == 4);
    assert(songAfterFinish(135, 4) == 9);
    return 0;
}
#endif

#endif
