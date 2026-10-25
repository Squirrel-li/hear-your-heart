/* SH1106 waveform page, shared by ble_sensor (and its capture / motion builds) and ble_sensor_hr.   */
/* P1 (IR) in the top half, P2 (red) in the bottom half, one column per PPG sample swept left to     */
/* right (128 samples, ~3.6 s in continuous mode). Like a patient monitor the slow baseline is        */
/* removed first (moving average of 2^WAVE_BASE_SHIFT samples, ~0.45 s, high-pass ~0.35 Hz, a 40 bpm */
/* pulse still passes), so drift does not push the trace into the edge; the rest is scaled to the    */
/* min..max of the previous and the current sweep together. A sudden cut (a step over 8x the usual   */
/* sample-to-sample change, the capture page does the same) moves the baseline with it, so the trace */
/* stays joined; a build that defines WAVE_NO_JOIN leaves it out. ../test_oled_wave.c checks it.     */
/* Include after: UINT8/16/32, __xdata, __bit, PKT_ST, disp[], oled_ok, wave_new (set once per new   */
/* disp sample), oled_at(), i2c_tx(), i2c_stop(), i2c_bus_init().                                    */
#define WAVE_H              32              /* rows per trace */
#define WAVE_GAP            3               /* blank columns ahead of the newest one */
#define WAVE_MAX_ERR        8               /* consecutive I2C failures before the panel is re-initialised */
#define WAVE_BASE_SHIFT     4               /* baseline average over 16 samples */
#define WAVE_WARMUP         64              /* samples of normal steps before cuts are joined */
typedef struct
{
    UINT32 base;                            /* baseline << WAVE_BASE_SHIFT, 0 = no sample yet */
#ifndef WAVE_NO_JOIN
    UINT16 last;                            /* previous raw sample */
    UINT16 step32;                          /* usual |step| x 32 (average of the non-zero ones) */
    UINT8 n;                                /* normal steps seen, up to WAVE_WARMUP */
#endif
    UINT16 lo, hi, show_lo, show_hi;        /* in baseline-removed units, 0x8000 = on the baseline */
    UINT8 y;
} wave_trace;
typedef struct
{
    wave_trace tr[2];
    UINT8 x;                                /* next column */
    UINT8 clr;                              /* < 8: next page to clear before the sweep starts */
    UINT8 err;                              /* consecutive failed I2C writes */
} wave_state;

static void wave_reset(__xdata wave_state *ws)  /* clear the panel, then start at column 0 */
{
    ws->clr = 0;
    ws->x   = 0;
}

static UINT8 wave_y(__xdata wave_trace *w, UINT8 x, UINT16 v)   /* 1 (top) .. WAVE_H - 2 of its half */
{
    UINT16 lo, hi, b;
#ifndef WAVE_NO_JOIN
    UINT16 a;
    a = v >= w->last ? v - w->last : w->last - v;
    if (w->base && w->n >= WAVE_WARMUP && a > (w->step32 >> 2))     /* > 8x usual: move the baseline too */
        w->base += ((UINT32)v << WAVE_BASE_SHIFT) - ((UINT32)w->last << WAVE_BASE_SHIFT);
    else if (a)                             /* flat no-data stretches do not shrink the average */
    {
        if (a > 2047) a = 2047;             /* keeps step32 inside 16 bits */
        w->step32 += a - (w->step32 >> 5);
        if (w->n < WAVE_WARMUP) w->n++;
    }
    w->last = v;
#endif
#ifdef ITAP_DEMO
    if (!demo_on)                           /* DEMO recording: raw shape like the capture page, no baseline removal */
#endif
    {
    if (!w->base) w->base = (UINT32)v << WAVE_BASE_SHIFT;
    b = (UINT16)(w->base >> WAVE_BASE_SHIFT);
    w->base += v;
    w->base -= b;
    v = v >= b ? (v - b > 0x7FFF ? 0xFFFF : 0x8000 + (v - b))
               : (b - v > 0x8000 ? 0x0000 : 0x8000 - (b - v));
    }
    if (x == 0)                             /* new sweep: keep the last one's range for scaling */
    {
        w->show_lo = w->lo;
        w->show_hi = w->hi;
        w->lo = w->hi = v;
    }
    if (v < w->lo) w->lo = v;
    if (v > w->hi) w->hi = v;
    lo = w->lo; hi = w->hi;                 /* v is inside the current sweep's range */
    if (w->show_hi > w->show_lo)
    {
        if (w->show_lo < lo) lo = w->show_lo;
        if (w->show_hi > hi) hi = w->show_hi;
    }
    if (hi <= lo) return WAVE_H / 2;
    return 1 + (UINT8)((UINT32)(hi - v) * (WAVE_H - 3) / (hi - lo));
}

/* A bus shared with a fast-polled IMU sees occasional errors: reset the I2C block and carry on, */
/* re-initialise the panel only when it keeps failing.                                          */
static void wave_fail(__xdata wave_state *ws)
{
    if (++ws->err >= WAVE_MAX_ERR) oled_ok = 0;
    else i2c_bus_init();
}

static void wave_task(__xdata wave_state *ws)
{
    UINT8 i, p, y[2], a, b, r, bits;

    if (ws->clr < 8)                        /* blank one page per call before the first column */
    {
        i = 0;
        if (oled_at(ws->clr, 0))
        {
            for (; i < 132; i++)
                if (!i2c_tx(0x00, 0x28)) break;
            i2c_stop();
        }
        if (i < 132) { wave_fail(ws); return; }
        ws->err = 0;
        ws->clr++;
        wave_new = 0;
        return;
    }
    if (!wave_new) return;
    wave_new = 0;

    y[0] = wave_y(&ws->tr[0], ws->x, ((UINT16)disp[PKT_ST + 7] << 8) | disp[PKT_ST + 8]);   /* P1 IR  */
    y[1] = wave_y(&ws->tr[1], ws->x, ((UINT16)disp[PKT_ST + 9] << 8) | disp[PKT_ST + 10]);  /* P2 red */
    for (p = 0; p < 8; p++)
    {
        a = y[p >> 2];
        b = ws->x ? ws->tr[p >> 2].y : a;   /* join to the previous column, not across the wrap */
        if (a > b) { r = a; a = b; b = r; }
        bits = 0;
        for (i = 0; i < 8; i++)
        {
            r = (p & 3) * 8 + i;            /* row inside the half */
            if (r >= a && r <= b) bits |= 1 << i;
        }
        i = oled_at(p, ws->x + 2);
        if (i)
        {
            i = i2c_tx(bits, 0x28);
            for (r = 0; i && r < WAVE_GAP; r++) i = i2c_tx(0x00, 0x28);   /* past 129: off-screen RAM */
            i2c_stop();
        }
        if (!i) break;
    }
    if (p < 8) wave_fail(ws);               /* the column stays incomplete, the sweep goes on */
    else ws->err = 0;
    ws->tr[0].y = y[0];
    ws->tr[1].y = y[1];
    ws->x = (ws->x + 1) & 127;
}
