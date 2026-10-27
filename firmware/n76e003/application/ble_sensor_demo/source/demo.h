/* DEMO build: one merged text page and the recorded-PPG player. Included by ../../ble_sensor/source/main.c */
/* in place of its PPG / IMU pages, so everything above (ln_*, disp, imu_pkt, motion, tx_pkt) is in scope.  */
/* The page uses only the glyphs in oled_font_demo.h:                                                     */
/*   BLE_watch_ITAP(S)      BLE: I(nit) A(dvertising) C(onnected) S(treaming) D(isconnected)              */
/*   12M34S DEMO            uptime; DEMO only while the recording plays                                   */
/*   PPG(2) HR  72          module status (- none yet, L lost, 0 no finger, 1 finger, 2 data), local HR  */
/*   12345 23456            PPG1 (IR) PPG2 (red)                                                          */
/*   A  -123   456  4096    acc x y z                                                                     */
static void ln_i(int v)                     /* signed, width 6, dashes when there is no sample */
{
    UINT8  i = 0, w = 6;
    UINT16 u = (UINT16)v;
    if (!imu_has) { while (w--) ln_c('-'); return; }
    if (v < 0) u = 0 - u;
    do { ln_dig[i++] = '0' + (UINT8)(u % 10); u /= 10; } while (u);
    w -= i + (v < 0);
    while (w--) ln_c(' ');
    if (v < 0) ln_c('-');
    while (i) ln_c(ln_dig[--i]);
}

__code const char ble_ch[] = "IIIIAAAACSD"; /* indexed by ST_RESET .. ST_DISC_WAIT */
UINT16 bat_avg;                             /* ADCRH x 256, seeded at boot; the high byte is the average */

static void oled_build(UINT8 row, UINT8 state)
{
    UINT8 k;
    __xdata UINT8 *p;
    ln_n = 0;
    switch (row)
    {
    case 0:
        ln_s((__code const char *)bm_name);
        ln_c('('); ln_c(ble_ch[state]); ln_c(')');
        break;
    case 1:
        ln_time(uptime_ms());
        if (demo_on) ln_s(" DEMO");
        /* battery: P1.1 = AIN7 behind a 1/2 divider, VDD 3.3 V reference; ADCRH 128..155 = 3.3..4.0 V */
        /* shortcut: linear 0..100 from the last conversion, no filter (cap on the pin); calibrate if it matters */
        /* exponential average of 256 conversions, one per redraw (8 x OLED_ROW_MS = 160 ms): ~40 s */
        bat_avg += ADCRH - (UINT8)(bat_avg >> 8);
        k = (UINT8)(bat_avg >> 8) - 128;
        if (k & 0x80) k = 0;                /* below 3.3 V wraps to >= 0x80 */
        else if (k > 26) k = 100;
        else k = (k << 2) - (k >> 2);       /* x3.75 ~ 100 / 27, 26 -> 98 */
        ADCS = 1;                           /* next result is read on a later redraw of this row */
        ln_s(" B"); ln_u(k, 3);
        break;
    case 2:
        ln_s("PPG(");
        if      (!disp_ok) ln_c('-');
        else if (!ppg_ok)  ln_c('L');
        else               ln_u(disp[PKT_ST], 1);
        ln_s(") HR "); ln_u(motion.bpm, 3);
        break;
    case 3:
        ln_u(((UINT16)disp[PKT_ST + 7] << 8) | disp[PKT_ST + 8], 5); ln_c(' ');
        ln_u(((UINT16)disp[PKT_ST + 9] << 8) | disp[PKT_ST + 10], 5);
        break;
    case 4:                                 /* no gyro row: off in the motion builds, and Flash is full */
        ln_c('A');
        p = imu_pkt + IMU_PKT_ACC;
        for (k = 0; k < 3; k++, p += 2) ln_i(((int)p[0] << 8) | p[1]);
        break;
    default: break;
    }
    while (ln_n < OLED_COLS) ln[ln_n++] = ' ';
}

/* Recorded PPG in place of the module (UART0 interrupt off): one sample per DEMO_TICK_MS into tx_pkt,  */
/* the same layout the UART0 ISR writes, so motion HR, OLED and BLE cannot tell the difference. Stored  */
/* points are every second sample; the one between is the midpoint (../make_demo_ppg.py replays the     */
/* same arithmetic to check the HR).                                                                    */
#define DEMO_TICK_MS        28              /* 35.7 Hz, the module's continuous rate */
UINT16 demo_t;
__code const signed char *demo_p = demo_ppg; /* next IR, red step */
UINT16 demo_v[2] = { DEMO_IR0, DEMO_RED0 };
__bit  demo_half;

static void demo_task(UINT16 t)
{
    UINT32 ts;
    UINT8 k;
    UINT16 v;
    int d;

    if (ppg_ready || (UINT16)(t - demo_t) < DEMO_TICK_MS) return;
    /* keep the average rate; after a long stall (or the first call) restart from now */
    demo_t = (UINT16)(t - demo_t) < 4 * DEMO_TICK_MS ? demo_t + DEMO_TICK_MS : t;
    for (k = 0; k < 2; k++)
    {
        v = demo_v[k];
        if (demo_half)                      /* midpoint, then step to the next stored point */
        {
            d = (int)demo_p[k] << (k ? DEMO_RED_SHIFT : DEMO_IR_SHIFT);
            v += d >> 1;
            demo_v[k] += d;
        }
        tx_pkt[PKT_ST + 7 + 2 * k] = (UINT8)(v >> 8);   /* ppg1 = IR, ppg2 = red, Big-endian */
        tx_pkt[PKT_ST + 8 + 2 * k] = (UINT8)v;
    }
    /* the steps return exactly to DEMO_IR0 / DEMO_RED0 */
    if (demo_half && (demo_p += 2) == demo_ppg + 2 * DEMO_N) demo_p = demo_ppg;
    demo_half = !demo_half;

    ts = uptime_ms();
    for (k = 0; k < 4; k++)
    {
        tx_pkt[PKT_TS + k] = ((UINT8 *)&ts)[3 - k];     /* SDCC is little-endian, the packet Big-endian */
        tx_pkt[PKT_SEQ + k] = ppg_seq[k];
    }
    if (++ppg_seq[3] == 0)
        if (++ppg_seq[2] == 0)
            if (++ppg_seq[1] == 0)
                ++ppg_seq[0];
    tx_pkt[PKT_ST] = 2;                     /* module status: data; SpO2..systole stay 0 (UART0 is off) */
    ppg_ready = 1;
}
