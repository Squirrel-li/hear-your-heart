// Host check for heart_zone.h:
//   g++ -std=c++11 -I../application/embedded_loudspeaker heart_zone_check.cpp -o heart_zone_check && ./heart_zone_check
#include <assert.h>
#include <stdio.h>

#include "heart_zone.h"

int main()
{
    // HRmax 190 (age 25): zone edges 114 / 133 / 152 / 171.
    assert(heart_zone_observed(190, 113) == 1 && heart_zone_observed(190, 114) == 2);
    assert(heart_zone_observed(190, 152) == 4 && heart_zone_observed(190, 171) == 5);

    // Per-zone music (planning spec): relax, sync, sync +5%, -20%, -30%.
    assert(target_bpm_for_zone(190, 57, 1) == 70 && target_bpm_for_zone(190, 113, 1) == 80);
    assert(target_bpm_for_zone(190, 120, 2) == 120);
    assert(target_bpm_for_zone(190, 140, 3) == 147);
    assert(target_bpm_for_zone(190, 160, 4) == 128);
    assert(target_bpm_for_zone(190, 180, 5) == 126);

    // Rising targets are limited to +5 per update, falls are immediate.
    TargetBpmState t{190, 0};
    assert(target_bpm_update_for_zone(&t, 120, 2) == 120);
    assert(target_bpm_update_for_zone(&t, 140, 3) == 125);
    assert(target_bpm_update_for_zone(&t, 160, 4) == 128);
    assert(target_bpm_update_for_zone(&t, 100, 2) == 100);
    assert(target_bpm_update_for_zone(&t, 0, 2) == 0 && target_bpm_update_for_zone(&t, 100, 6) == 0);

    // Z3 -> Z4 needs 2% HRmax past the edge (156) held for the dwell time.
    HeartZoneState z{};
    assert(heart_zone_update(&z, 190, 145, 0, 10000, 2) == 3);
    assert(heart_zone_update(&z, 190, 154, 1000, 10000, 2) == 3 && z.pending == 0);  // inside hysteresis
    assert(heart_zone_update(&z, 190, 160, 2000, 10000, 2) == 3 && z.pending == 4);
    assert(heart_zone_update(&z, 190, 160, 11999, 10000, 2) == 3);
    assert(heart_zone_update(&z, 190, 160, 12000, 10000, 2) == 4);
    // Into Z5 at once (>= 92% = 175 bpm), no dwell.
    assert(heart_zone_update(&z, 190, 176, 12500, 10000, 2) == 5);
    // Down from Z5 still waits for the dwell.
    assert(heart_zone_update(&z, 190, 150, 13000, 10000, 2) == 5);
    assert(heart_zone_update(&z, 190, 150, 23000, 10000, 2) == 3);

    puts("heart_zone_check: OK");
    return 0;
}
