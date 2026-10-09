#ifndef SONG_CATALOG_H
#define SONG_CATALOG_H

#include <stdint.h>

struct Song {
    const char* filename;
    uint16_t bpm;
};

// The files are in testdata/sd_music; copy them all to the SD card root. Each file is
// named bpmNNN.mp3 after its BPM (8.3-safe); keep the list sorted by BPM.
// Rows marked (local) come from testdata/audio with offline onset/autocorrelation
// estimates. Rows marked (FMA) are 30 s clips from the Free Music Archive dataset
// (fma_small, CC BY-NC-SA, non-commercial); their BPM is the rounded Echonest tempo
// from fma_metadata, not measured here, so it may be a half/double-tempo misreading.
// 60-180 BPM is within 5% of some song, except 70-71, 90 and 124-125, where the
// closest-BPM rule applies. Replace the values after measuring any different MP3 files
// copied to the SD card.
// ponytail: A 16-bit unavailable mask limits this list to 16 songs; widen it to add more.
static const Song songs[] = {
    {"bpm060.mp3", 60},   // (FMA) Ai Aso, Folk
    {"bpm066.mp3", 66},   // (FMA) Frau, Pop
    {"bpm075.mp3", 75},   // (local) "Embrace", Sappheiros
    {"bpm080.mp3", 80},   // (local) "Dragon Castle", Makai Symphony
    {"bpm085.mp3", 85},   // (local) "Way Home", Tokyo Music Walker
    {"bpm095.mp3", 95},   // (local) "Filaments", Scott Buckley
    {"bpm097.mp3", 97},   // (local) "The Epic Hero", Keys Of Moon
    {"bpm106.mp3", 106},  // (FMA) KenLo Craqnuques, Hip-Hop
    {"bpm117.mp3", 117},  // (FMA) Gosprom, Pop
    {"bpm132.mp3", 132},  // (local) "The Travelling Symphony", Savfk
    {"bpm135.mp3", 135},  // (local) "Dawn", Sappheiros
    {"bpm137.mp3", 137},  // (local) "Indian Fusion", Shahed
    {"bpm138.mp3", 138},  // (local) "Fire And Thunder", Cjbeards
    {"bpm147.mp3", 147},  // (local) "The Grid", Savfk
    {"bpm156.mp3", 156},  // (FMA) So Cow, Pop
    {"bpm171.mp3", 171},  // (FMA) The Sounds of Taraab, International
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
