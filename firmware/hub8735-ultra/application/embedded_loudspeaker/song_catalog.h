#ifndef SONG_CATALOG_H
#define SONG_CATALOG_H

#include <stdint.h>

#include "music_library.h"

// 曲目清單來自 SD 卡根目錄的 music_library.json（範例見 testdata/sd_music），
// 由 audioTask 開機載入到 musicLibrary；檔名與 BPM 以該檔為準。
// 60-180 BPM 以 5% 容差比對，沒有符合者改選最接近目標 BPM 的曲目。
// 無法播放的曲目以位元陣列記錄（每首 1 bit），nullptr 表示沒有排除任何曲目。
constexpr size_t SONG_MASK_WORDS = (MUSIC_LIBRARY_MAX + 31) / 32;

static uint32_t bpmDistance(uint16_t a, uint16_t b)
{
    return a > b ? a - b : b - a;
}

static bool songUnavailable(int index, const uint32_t* unavailableMask)
{
    return unavailableMask != nullptr && index >= 0 && index < static_cast<int>(MUSIC_LIBRARY_MAX) &&
           (unavailableMask[index / 32] & (1u << (index % 32))) != 0;
}

// genre: nullptr = any genre, otherwise only songs whose genre equals it.
static bool songSelectable(int index, const uint32_t* unavailableMask, const char* genre)
{
    return !songUnavailable(index, unavailableMask) && musicLibrary.entries[index].bpm != 0 &&
           (genre == nullptr || strcmp(musicLibrary.entries[index].genre, genre) == 0);
}

static int nextMatchingSong(uint16_t targetBpm, int currentSong,
                            const uint32_t* unavailableMask = nullptr, const char* genre = nullptr)
{
    if (targetBpm == 0 || musicLibrary.count == 0) return -1;
    for (int step = 1; step <= static_cast<int>(musicLibrary.count); ++step) {
        const int index = (currentSong + step) % static_cast<int>(musicLibrary.count);
        if (!songSelectable(index, unavailableMask, genre)) continue;
        if (bpmDistance(musicLibrary.entries[index].bpm, targetBpm) * 100u <=
            (uint32_t)targetBpm * 5u) return index;
    }
    return -1;
}

static int closestSong(uint16_t targetBpm, int currentSong,
                        const uint32_t* unavailableMask = nullptr, const char* genre = nullptr)
{
    if (targetBpm == 0 || musicLibrary.count == 0) return -1;
    // ponytail: Linear scan; fine for MUSIC_LIBRARY_MAX songs once per selection.
    int best = -1;
    uint32_t bestDistance = UINT32_MAX;
    for (int step = 1; step <= static_cast<int>(musicLibrary.count); ++step) {
        const int index = (currentSong + step) % static_cast<int>(musicLibrary.count);
        if (!songSelectable(index, unavailableMask, genre)) continue;
        const uint32_t distance = bpmDistance(musicLibrary.entries[index].bpm, targetBpm);
        if (distance < bestDistance) {
            best = index;
            bestDistance = distance;
        }
    }
    return best;
}

// A preferred genre picks the closest BPM within that genre; only when the genre has no selectable song
// does the whole library count (user decision 2026-10-27).
static int songForStart(uint16_t targetBpm, int currentSong,
                         const uint32_t* unavailableMask = nullptr, const char* genre = nullptr)
{
    const int matching = nextMatchingSong(targetBpm, currentSong, unavailableMask, genre);
    if (matching >= 0) return matching;
    const int closest = closestSong(targetBpm, currentSong, unavailableMask, genre);
    return closest >= 0 || genre == nullptr ? closest : songForStart(targetBpm, currentSong, unavailableMask);
}

static int songAfterFinish(uint16_t targetBpm, int currentSong,
                            const uint32_t* unavailableMask = nullptr, const char* genre = nullptr)
{
    const int matching = nextMatchingSong(targetBpm, currentSong, unavailableMask, genre);
    if (matching >= 0) return matching;
    if (currentSong >= 0 && currentSong < static_cast<int>(musicLibrary.count) &&
        songSelectable(currentSong, unavailableMask, genre)) return currentSong;
    const int closest = closestSong(targetBpm, currentSong, unavailableMask, genre);
    return closest >= 0 || genre == nullptr ? closest : songAfterFinish(targetBpm, currentSong, unavailableMask);
}
// No song has a BPM yet (first boot, results cleared): any playable song, starting at seed, preferring
// one other than currentSong. -1 when every song is unavailable.
static int randomSong(uint32_t seed, int currentSong, const uint32_t* unavailableMask = nullptr)
{
    const int count = static_cast<int>(musicLibrary.count);
    int fallback = -1;
    for (int step = 0; step < count; ++step) {
        const int index = static_cast<int>((seed + static_cast<uint32_t>(step)) % static_cast<uint32_t>(count));
        if (songUnavailable(index, unavailableMask)) continue;
        if (index != currentSong) return index;
        fallback = index;
    }
    return fallback;
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
    assert(randomSong(7, -1) == 0 && randomSong(0, 0) == 0);  // the only song, even if it is the current one
    static uint32_t none[SONG_MASK_WORDS];
    none[0] = 1u;
    assert(randomSong(3, -1, none) == -1);
    assert(music_json::parse(json, musicLibrary));
    assert(randomSong(17, -1) == 1 && randomSong(1, 1) == 2);  // seed wraps; skips the current song

    // Preferred genre: closest BPM inside the genre, whole library only when the genre has nothing.
    strcpy(musicLibrary.entries[2].genre, "Rock");   // 100 BPM
    strcpy(musicLibrary.entries[14].genre, "Rock");  // 160 BPM
    assert(songForStart(135, -1, nullptr, "Rock") == 14);
    assert(songForStart(103, -1, nullptr, "Rock") == 2);
    assert(songAfterFinish(135, 2, nullptr, "Rock") == 2);  // no match: keep the current genre song, as without a genre
    assert(songAfterFinish(160, 14, nullptr, "Rock") == 14);  // only one match: repeat it
    assert(songForStart(135, -1, nullptr, "Classical") == 0);  // no Classical song: any genre
    static uint32_t rockFailed[SONG_MASK_WORDS];
    rockFailed[0] = (1u << 2) | (1u << 14);
    assert(songForStart(135, -1, rockFailed, "Rock") == 0);  // every Rock song unplayable: any genre
    assert(songAfterFinish(135, 0, rockFailed, "Rock") == 10);

    // Library beyond the old 32-song limit; the failure mask must reach high indexes.
    static char big[MUSIC_LIBRARY_JSON_MAX];
    size_t n = (size_t)snprintf(big, sizeof(big), "{\"songs\":[");
    for (unsigned i = 0; i < MUSIC_LIBRARY_MAX; ++i)
        n += (size_t)snprintf(big + n, sizeof(big) - n, "%s{\"file\":\"s%04u.mp3\",\"bpm\":%u}",
                              i ? "," : "", i, i == 1000 || i == 1001 ? 200u : 60u);
    snprintf(big + n, sizeof(big) - n, "]}");
    assert(music_json::parse(big, musicLibrary) && musicLibrary.count == MUSIC_LIBRARY_MAX);
    assert(songForStart(200, -1) == 1000);
    static uint32_t failed[SONG_MASK_WORDS];
    failed[1000 / 32] |= 1u << (1000 % 32);
    assert(songForStart(200, -1, failed) == 1001);
    static MusicLibrary over;
    n = (size_t)snprintf(big, sizeof(big), "{\"songs\":[");
    for (unsigned i = 0; i <= MUSIC_LIBRARY_MAX; ++i)
        n += (size_t)snprintf(big + n, sizeof(big) - n, "%s{\"file\":\"s%04u.mp3\"}", i ? "," : "", i);
    snprintf(big + n, sizeof(big) - n, "]}");
    assert(!music_json::parse(big, over));  // one past the limit is rejected, not truncated
    for (unsigned i = 0; i < MUSIC_LIBRARY_MAX; ++i) {  // worst case: longest fields everywhere
        MusicEntry& e = musicLibrary.entries[i];
        snprintf(e.file, sizeof(e.file), "%027u.mp3", i);
        snprintf(e.genre, sizeof(e.genre), "%015u", i);
        e.bpm = 300;
        e.fileSize = e.fileModified = 0xFFFFFFFFu;
    }
    assert(music_json::format(musicLibrary, big, sizeof(big)) > 0);  // a full library fits the buffer
    return 0;
}
#endif

#endif
