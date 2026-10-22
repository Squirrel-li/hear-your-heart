// Host check for motion_level.h:
//   g++ -std=c++11 -I../application/embedded_loudspeaker motion_level_check.cpp -o motion_level_check && ./motion_level_check
#include <assert.h>
#include <stdio.h>

#include "motion_level.h"

// One sample per bucket of the given dynamic magnitude (mg, on x only); returns the last update result.
static int feed(MotionLevelState& s, uint32_t& t, float mg, int buckets)
{
    int last = -1;
    for (int i = 0; i < buckets; ++i) {
        motion_level_add(s, (int16_t)(mg / ACC_MG_PER_LSB), 0, 0);
        t += MOTION_BUCKET_MS;
        const int r = motion_level_update(s, t);
        if (r >= 0) last = r;
    }
    return last;
}

int main()
{
    assert(motion_level_for(0) == 0);
    assert(motion_level_for(49) == 0);
    assert(motion_level_for(50) == 1);
    assert(motion_level_for(600) == 3);
    assert(motion_level_for(5000) == 4);

    MotionLevelState s;
    uint32_t t = 0;
    assert(feed(s, t, 10, 1) == 0);  // first level is accepted at once
    assert(s.active == 0);

    // A single-bucket (0.5 s) spike needs the dwell: no change.
    assert(feed(s, t, 2000, 1) == -1);
    assert(feed(s, t, 10, 3) == -1);
    assert(s.active == 0);

    // Sustained running (~800 mg) is confirmed after the 0.5 s dwell.
    assert(feed(s, t, 800, 1) == -1);
    assert(feed(s, t, 800, 1) == 3);
    assert(s.active == 3);

    // No samples: level kept.
    t += 10000;
    assert(motion_level_update(s, t) == -1);
    assert(s.active == 3);

    // Source switch keeps the confirmed level.
    motion_level_reset_samples(s);
    assert(s.active == 3 && s.windowCount == 0);

    // 12.5 Hz samples in one 0.5 s bucket average together.
    for (int i = 0; i < 6; ++i) motion_level_add(s, (int16_t)(1500 / ACC_MG_PER_LSB), 0, 0);
    t += MOTION_BUCKET_MS;
    motion_level_update(s, t);
    assert(s.lastMeanMg > 1490 && s.lastMeanMg < 1510);

    puts("motion_level_check: OK");
    return 0;
}
