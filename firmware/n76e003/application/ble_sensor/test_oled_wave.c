/* PC check of source/oled_wave.h: fake SH1106 RAM, sine input, one solid trace per half, gap at the cursor,
 * no clipping on a drifting baseline (baseline removed), sudden cuts joined, I2C errors tolerated until WAVE_MAX_ERR in a row.
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
static int oled_ok = 1, wave_new, fails, bus_resets;
static int oled_at(UINT8 p, UINT8 c) { if (fails) { fails--; return 0; } cur_p = p; cur_c = c; return 1; }
static void i2c_bus_init(void) { bus_resets++; }
static int i2c_tx(UINT8 b, UINT8 s) { (void)s; if (cur_c < 140) ram[cur_p][cur_c] = b; cur_c++; return 1; }
static void i2c_stop(void) {}
#include "source/oled_wave.h"

static int px(int x, int y) { return (ram[y >> 3][x + 2] >> (y & 7)) & 1; }

static void feed(wave_state *ws, UINT16 ir, UINT16 red)
{
    disp[16] = ir >> 8; disp[17] = ir; disp[18] = red >> 8; disp[19] = red;
    wave_new = 1;
    wave_task(ws);
}

int main(void)
{
    wave_state ws = { 0 };
    int n, x, y, k, best;
    for (n = 0; n < 8; n++) for (x = 0; x < 140; x++) ram[n][x] = 0xFF;   /* random power-up RAM */
    wave_reset(&ws);
    for (n = 0; n < 8; n++) wave_task(&ws);                               /* clear pass */
    for (n = 0; n < 128 * 2 + 60; n++)                                    /* two sweeps + 60 columns */
        feed(&ws, 20000 + 800 * sin(n * 0.2), 15000 + 500 * cos(n * 0.2));
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

    for (n = 0; n < 68; n++) feed(&ws, 20000 + 800 * sin(n * 0.2), 15000);   /* finish the sweep */
    for (n = 0; n < 128; n++)                                             /* baseline climbs 6000 in one sweep */
        feed(&ws, 20000 + 800 * sin(n * 0.2) + 47 * n, 15000);
    for (k = best = x = 0; x < 128; x++)                                  /* flat line on the top row = clipped */
        if (px(x, 1) && !px(x, 2)) { if (++k > best) best = k; } else k = 0;
    assert(best < 10);                                                    /* 99 columns before the baseline fix */

    for (n = 0; n < 128; n++)                                             /* cuts of -3000 and +2000 mid-sweep */
        feed(&ws, 20000 + 800 * sin(n * 0.2) - (n >= 40 ? 3000 : 0) + (n >= 90 ? 2000 : 0), 15000);
    for (x = 0; x < 128; x++)                                             /* joined: every column a short step, */
    {                                                                     /* a raw cut would span the whole half */
        for (k = y = 0; y < WAVE_H; y++) k += px(x, y);
        assert(k <= 8);
    }

    fails = 1;
    feed(&ws, 20000, 15000);
    assert(oled_ok && bus_resets == 1 && ws.x == 1);                     /* one error: reset the bus, go on */
    feed(&ws, 20000, 15000);
    assert(ws.err == 0);
    fails = WAVE_MAX_ERR;
    for (n = 0; n < WAVE_MAX_ERR; n++) feed(&ws, 20000, 15000);
    assert(!oled_ok);                                                     /* keeps failing -> re-init */
    puts("oled_wave checks passed");
    return 0;
}
