/* PC check of source/oled_wave.h: fake SH1106 RAM, sine input, one solid trace per half, gap at the cursor.
 *   gcc -Wall -o test_oled_wave test_oled_wave.c -lm && ./test_oled_wave */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <assert.h>
typedef uint8_t UINT8; typedef uint16_t UINT16; typedef uint32_t UINT32;
#define __xdata
#define __bit int
#define PKT_ST 9
static UINT8 ram[8][140], cur_p, cur_c, disp[20];
static int oled_ok = 1, wave_new, fail_at = -1, txn;
static int oled_at(UINT8 p, UINT8 c) { if (txn++ == fail_at) return 0; cur_p = p; cur_c = c; return 1; }
static int i2c_tx(UINT8 b, UINT8 s) { (void)s; if (cur_c < 140) ram[cur_p][cur_c] = b; cur_c++; return 1; }
static void i2c_stop(void) {}
#include "source/oled_wave.h"

static int px(int x, int y) { return (ram[y >> 3][x + 2] >> (y & 7)) & 1; }

int main(void)
{
    wave_state ws = { 0 };
    int n, x, y, k;
    for (n = 0; n < 8; n++) for (x = 0; x < 140; x++) ram[n][x] = 0xFF;   /* random power-up RAM */
    wave_reset(&ws);
    for (n = 0; n < 8; n++) wave_task(&ws);                               /* clear pass */
    for (n = 0; n < 128 * 2 + 60; n++)                                    /* two sweeps + 60 columns */
    {
        UINT16 ir = 20000 + 800 * sin(n * 0.2), red = 15000 + 500 * cos(n * 0.2);
        disp[16] = ir >> 8; disp[17] = ir; disp[18] = red >> 8; disp[19] = red;
        wave_new = 1;
        wave_task(&ws);
    }
    assert(ws.x == 60);
    for (x = 0; x < 128; x++)
        for (k = 0; k < 2; k++)
        {
            int on = 0, first = -1, last = -1;
            for (y = 0; y < WAVE_H; y++) if (px(x, k * WAVE_H + y)) { on++; if (first < 0) first = y; last = y; }
            if (x >= 60 && x < 60 + WAVE_GAP) { assert(on == 0); continue; }
            assert(on >= 1 && last - first + 1 == on);                    /* one solid vertical segment */
            assert(first >= 1 && last <= WAVE_H - 2);                     /* the halves never touch */
        }
    wave_new = 0;
    wave_task(&ws);
    assert(ws.x == 60);                                                   /* no new sample, no column */
    fail_at = txn;
    wave_new = 1;
    wave_task(&ws);
    assert(!oled_ok);                                                     /* I2C failure -> re-init */
    puts("oled_wave checks passed");
    return 0;
}
