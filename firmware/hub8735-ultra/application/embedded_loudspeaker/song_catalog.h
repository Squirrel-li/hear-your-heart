#ifndef SONG_CATALOG_H
#define SONG_CATALOG_H

#include <stdint.h>

struct Song {
    const char* filename;
    uint16_t bpm;
};

// Offline onset/autocorrelation estimates from testdata/audio. Replace these
// values after measuring any different MP3 files copied to the SD card.
static const Song songs[] = {
    {"music1.mp3", 75}, {"music2.mp3", 95}, {"music3.mp3", 80},
    {"music4.mp3", 85}, {"music5.mp3", 137}, {"music6.mp3", 97},
    {"music7.mp3", 135}, {"music8.mp3", 147}, {"music9.mp3", 138},
    {"music10.mp3", 132},
};

static const int songCount = sizeof(songs) / sizeof(songs[0]);

static uint32_t bpmDistance(uint16_t a, uint16_t b)
{
    return a > b ? a - b : b - a;
}

// ponytail: A 16-bit mask covers the current 10-song catalog; widen it above 16 tracks.
static bool songUnavailable(int index, uint16_t unavailableMask)
{
    return index >= 0 && index < 16 &&
           (unavailableMask & static_cast<uint16_t>(1u << index)) != 0;
}

static int nextMatchingSong(uint16_t targetBpm, int currentSong,
                            uint16_t unavailableMask = 0)
{
    if (targetBpm == 0) return -1;
    for (int step = 1; step <= songCount; ++step) {
        const int index = (currentSong + step) % songCount;
        if (songUnavailable(index, unavailableMask)) continue;
        if (bpmDistance(songs[index].bpm, targetBpm) * 100u <=
            (uint32_t)targetBpm * 5u) return index;
    }
    return -1;
}

static int closestSong(uint16_t targetBpm, int currentSong,
                        uint16_t unavailableMask = 0)
{
    if (targetBpm == 0) return -1;
    // ponytail: Linear scan is sufficient for this fixed ten-song catalog.
    int best = -1;
    uint32_t bestDistance = UINT32_MAX;
    for (int step = 1; step <= songCount; ++step) {
        const int index = (currentSong + step) % songCount;
        if (songUnavailable(index, unavailableMask)) continue;
        const uint32_t distance = bpmDistance(songs[index].bpm, targetBpm);
        if (distance < bestDistance) {
            best = index;
            bestDistance = distance;
        }
    }
    return best;
}

static int songForStart(uint16_t targetBpm, int currentSong,
                         uint16_t unavailableMask = 0)
{
    const int matching = nextMatchingSong(targetBpm, currentSong, unavailableMask);
    return matching >= 0 ? matching : closestSong(targetBpm, currentSong, unavailableMask);
}

static int songAfterFinish(uint16_t targetBpm, int currentSong,
                            uint16_t unavailableMask = 0)
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
int main(void)
{
    assert(nextMatchingSong(135, -1) == 4);
    assert(nextMatchingSong(135, 4) == 6);
    assert(nextMatchingSong(135, 8) == 9);
    assert(nextMatchingSong(70, -1) == -1);
    assert(closestSong(70, -1) == 0);
    assert(closestSong(0, -1) == -1);
    assert(songForStart(70, -1) == 0);
    assert(songAfterFinish(70, 4) == 4);
    assert(songAfterFinish(135, 4) == 6);
    return 0;
}
#endif

#endif
