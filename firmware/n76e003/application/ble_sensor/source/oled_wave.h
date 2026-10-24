/* SH1106 waveform page, shared by ble_sensor (and its capture / motion builds) and ble_sensor_hr.   */
/* P1 (IR) in the top half, P2 (red) in the bottom half, one column per PPG sample swept left to     */
/* right (128 samples, ~3.6 s in continuous mode). Each half is scaled to the min..max of the         */
/* previous sweep (the running one during the first sweep). ../test_oled_wave.c checks it on a PC.  */
/* Include after: UINT8/16/32, __xdata, __bit, PKT_ST, disp[], oled_ok, wave_new (set once per new   */
/* disp sample), oled_at(), i2c_tx(), i2c_stop(). The state is passed in so that a build short of    */
/* XRAM can keep it in a buffer the waveform page does not use.                                      */
#define WAVE_H              32              /* rows per trace */
#define WAVE_GAP            3               /* blank columns ahead of the newest one */
typedef struct { UINT16 lo, hi, show_lo, show_hi; UINT8 y; } wave_trace;
typedef struct
{
    wave_trace tr[2];
    UINT8 x;                                /* next column */
    UINT8 clr;                              /* < 8: next page to clear before the sweep starts */
} wave_state;

static void wave_reset(__xdata wave_state *ws)  /* clear the panel, then start at column 0 */
{
    ws->clr = 0;
    ws->x   = 0;
}

static UINT8 wave_y(__xdata wave_trace *w, UINT8 x, UINT16 v)   /* 1 (top) .. WAVE_H - 2 of its half */
{
    UINT16 lo, hi;
    if (x == 0)                             /* new sweep: keep the last one's range for scaling */
    {
        w->show_lo = w->lo;
        w->show_hi = w->hi;
        w->lo = w->hi = v;
    }
    if (v < w->lo) w->lo = v;
    if (v > w->hi) w->hi = v;
    lo = w->show_lo; hi = w->show_hi;
    if (hi <= lo) { lo = w->lo; hi = w->hi; }
    if (hi <= lo) return WAVE_H / 2;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return 1 + (UINT8)((UINT32)(hi - v) * (WAVE_H - 3) / (hi - lo));
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
        if (i < 132) { oled_ok = 0; return; }
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
        if (!oled_at(p, ws->x + 2)) { oled_ok = 0; return; }
        i = i2c_tx(bits, 0x28);
        for (r = 0; i && r < WAVE_GAP; r++) i = i2c_tx(0x00, 0x28);   /* past 129: off-screen RAM */
        i2c_stop();
        if (!i) { oled_ok = 0; return; }
    }
    ws->tr[0].y = y[0];
    ws->tr[1].y = y[1];
    ws->x = (ws->x + 1) & 127;
}
