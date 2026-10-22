// Host check for ppg_raw_hr.h (run from the repo root):
//   g++ -std=c++11 -Ifirmware/hub8735-ultra/application/embedded_loudspeaker firmware/hub8735-ultra/tests/ppg_raw_hr_check.cpp -o ppg_raw_hr_check && ./ppg_raw_hr_check
// Replays the simulator's 0x05 recording (hr2) the way BLETask + analysisTask do and checks that the HUB
// copy of ppg_hr.h still matches the watch's file.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ppg_raw_hr.h"

static const char* HUB_COPY = "firmware/hub8735-ultra/application/embedded_loudspeaker/ppg_hr.h";
static const char* WATCH_FILE = "firmware/n76e003/application/ble_sensor_hr/source/ppg_hr.h";
static const char* RECORDING = "apps/remote_sensor_simulator/assets/testdata/ppg_raw_hr2.txt";

static bool sameAfterNote(const char* copyPath, const char* originalPath)
{
    FILE* a = fopen(copyPath, "rb");
    FILE* b = fopen(originalPath, "rb");
    assert(a != NULL && b != NULL);
    for (int lines = 0, c; lines < 2 && (c = fgetc(a)) != EOF;) lines += c == '\n';  // skip the copy note
    int ca, cb;
    do {
        ca = fgetc(a);
        cb = fgetc(b);
    } while (ca == cb && ca != EOF);
    fclose(a);
    fclose(b);
    return ca == cb;
}

int main()
{
    assert(sameAfterNote(HUB_COPY, WATCH_FILE));
    assert(ppg_raw_sample_time(1000, 2, 0) == 972 && ppg_raw_sample_time(1000, 2, 1) == 1000);

    FILE* f = fopen(RECORDING, "r");
    assert(f != NULL);
    static PpgRawHrState s;
    ppg_raw_hr_reset(s);
    int cycles = 0, valid = 0, first = -1;
    uint32_t lastTime = 0;
    char line[256];
    while (fgets(line, sizeof line, f) != NULL) {
        if (line[0] == '#') continue;
        unsigned b[64];
        int len = 0, used = 0;
        const char* p = line;
        while (len < 64 && sscanf(p, "%2x%n", &b[len], &used) == 1) {
            ++len;
            p += used;
        }
        if (len < 14 || (b[4] & 0x0F) != 0x05) continue;
        const uint32_t frameTime = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
        const uint32_t n = (len - 10) / 4;
        for (uint32_t i = 0; i < n; ++i) {
            const unsigned* q = &b[10 + 4 * i];
            lastTime = ppg_raw_sample_time(frameTime, n, i);
            const int bpm = ppg_raw_hr_feed(s, lastTime, (uint16_t)((q[0] << 8) | q[1]), (uint16_t)((q[2] << 8) | q[3]));
            if (bpm < 0) continue;
            if (first < 0) first = bpm;
            ++cycles;
            if (bpm > 0) {
                ++valid;
                assert(bpm >= 50 && bpm <= 130);
            }
            printf("cycle %d: %d BPM\n", cycles, bpm);
        }
    }
    fclose(f);
    assert(first == 0);         // warm-up cycle
    assert(cycles >= 5 && valid >= cycles / 2);

    // A dropout longer than PPG_RAW_LOST_MS starts over: the next finished cycle is a warm-up again.
    int afterGap = -1;
    for (uint32_t t = lastTime + PPG_RAW_LOST_MS + 1; afterGap < 0; t += PPG_RAW_SAMPLE_MS) {
        afterGap = ppg_raw_hr_feed(s, t, 1000, 1000);
    }
    assert(afterGap == 0);
    printf("ppg_raw_hr_check: OK (%d cycles, %d valid)\n", cycles, valid);
    return 0;
}
