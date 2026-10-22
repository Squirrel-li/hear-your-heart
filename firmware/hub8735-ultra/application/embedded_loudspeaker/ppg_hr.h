/* HUB copy of firmware/n76e003/application/ble_sensor_hr/source/ppg_hr.h (Arduino builds only see the     */
/* sketch folder). Keep it byte-identical below this note; tests/ppg_raw_hr_check.cpp compares the two. */
/* ppg_hr.h : heart rate from the BMH08002-4 PPG1 / PPG2 samples (integer only, streaming).         */
/*                                                                                                  */
/* C port of firmware/hub8735-ultra/examples/uart/bmh08002_4_control/ppg_hr_int.py - keep both in   */
/* step (constants and rules). The Python file is the tested reference (hr1/hr2 recordings).        */
/*                                                                                                  */
/* Beat detection: Elgendi et al. 2013 (PLoS ONE 8(10):e76585), two moving averages, done with      */
/* recursive averages so no sample buffer is needed.                                                */
/*   per sample (each channel) : x = ppg >> 1;  b += (x - b) >> 5;  a = x - b;  s += (a - s) >> 1;   */
/*                               p = max(s, 0);  mp += (p - mp) >> 2;  mb += (p - mb) >> 3;          */
/*                               mean += (p - mean) >> 6;  thr = mb + (mean >> 3)                    */
/*                               a block is a run of samples with mp > thr, its maximum is the beat  */
/*   per 10 s cycle            : intervals 300-2000 ms -> median -> drop > 20 % from the median     */
/*                               -> >= 5 left and >= 60 % of them (else 0) -> mean -> 60000 / mean  */
/*   both channels             : PPG1 and PPG2 must agree within 10 %, otherwise 0                  */
/*   first cycle after start   : warm-up, result 0                                                  */
/*                                                                                                  */
/* Result 0 = no valid heart rate. Assumes the AC part of ppg >> 1 stays below +-16000 counts        */
/* (int16 arithmetic). RAM: sizeof(hr_t) bytes, see the map file.                                   */
#ifndef PPG_HR_H
#define PPG_HR_H

#include <stdint.h>

#ifdef __SDCC__
#define HR_XD __xdata
#else
#define HR_XD
#endif

#define HR_WINDOW_MS        10000U
#define HR_BASE_SHIFT       5
#define HR_SMOOTH_SHIFT     1
#define HR_PEAK_SHIFT       2
#define HR_BEAT_SHIFT       3
#define HR_MEAN_SHIFT       6
#define HR_OFFSET_SHIFT     3
#define HR_MIN_AMP          4
#define HR_MIN_BLOCK        3
#define HR_MAX_BLOCK_MS     600U
#define HR_REFRACT_MS       300U
#define HR_MIN_BEAT_MS      300U
#define HR_MAX_BEAT_MS      2000U
#define HR_MAX_IV           16
#define HR_MIN_IV           3
#define HR_MIN_KEPT         5
#define HR_KEEP_PCT         60
#define HR_TRIM_DIV         5
#define HR_CROSS_DIV        10
#define HR_WARMUP_CYCLES    1

typedef struct {
    int16_t  b, s, mp, mb, mean, best;
    uint16_t best_t, blk_t, last_t;
    uint8_t  first, in_blk, skip, have_last, w, n;
    uint16_t iv[HR_MAX_IV];
} hr_ch_t;

typedef struct {
    hr_ch_t  ir, red;
    uint16_t start;
    uint8_t  started, cycles;
    uint8_t  last_b1, last_b2;                      /* per-channel BPM of the last finished cycle (display) */
} hr_t;

static void hr_ch_reset(hr_ch_t HR_XD *c)
{
    c->b = c->s = c->mp = c->mb = c->mean = c->best = 0;
    c->best_t = c->blk_t = c->last_t = 0;
    c->first = 1;
    c->in_blk = c->skip = c->have_last = 0;
    c->w = 0;
    c->n = 0;
}

static void hr_reset(hr_t HR_XD *h)
{
    hr_ch_reset(&h->ir);
    hr_ch_reset(&h->red);
    h->start = 0;
    h->started = 0;
    h->cycles = 0;
    h->last_b1 = h->last_b2 = 0;
}

static void hr_ch_beat(hr_ch_t HR_XD *c, uint16_t t)
{
    if (c->have_last) {
        uint16_t iv = (uint16_t)(t - c->last_t);
        if (iv < HR_REFRACT_MS) return;             /* too close to the previous beat: ignore this one */
        if (iv >= HR_MIN_BEAT_MS && iv <= HR_MAX_BEAT_MS && c->n < HR_MAX_IV)
            c->iv[c->n++] = iv;
    }
    c->last_t = t;
    c->have_last = 1;
}

/* raw: PPG sample, t: millisecond time stamp of the packet (uint16, wraps; only differences are used) */
static void hr_ch_feed(hr_ch_t HR_XD *c, uint16_t raw, uint16_t t)
{
    int16_t x = (int16_t)(raw >> 1);
    int16_t a, p, thr;

    if (c->first) {
        c->b = x;
        c->first = 0;
    }
    c->b += (int16_t)(x - c->b) >> HR_BASE_SHIFT;
    a = x - c->b;
    c->s += (int16_t)(a - c->s) >> HR_SMOOTH_SHIFT;
    p = (c->s > 0) ? c->s : 0;
    c->mp += (int16_t)(p - c->mp) >> HR_PEAK_SHIFT;
    c->mb += (int16_t)(p - c->mb) >> HR_BEAT_SHIFT;
    c->mean += (int16_t)(p - c->mean) >> HR_MEAN_SHIFT;
    thr = c->mb + (c->mean >> HR_OFFSET_SHIFT);

    if (c->mp > thr && c->mp > HR_MIN_AMP) {
        if (c->skip) return;                        /* drift block already dropped, wait for it to end */
        if (!c->in_blk) {
            c->in_blk = 1;
            c->w = 0;
            c->best = p;
            c->best_t = c->blk_t = t;
        }
        if (c->w < 255) c->w++;
        if (p > c->best) {
            c->best = p;
            c->best_t = t;
        }
        if ((uint16_t)(t - c->blk_t) > HR_MAX_BLOCK_MS) {
            c->in_blk = 0;                          /* drift / motion, not a pulse: restart the chain */
            c->skip = 1;
            c->have_last = 0;
        }
    } else {
        c->skip = 0;
        if (c->in_blk) {
            c->in_blk = 0;
            if (c->w >= HR_MIN_BLOCK) hr_ch_beat(c, c->best_t);
        }
    }
}

/* End of a cycle: BPM of this channel (0 = invalid); clears the interval table. */
static uint8_t hr_ch_finish(hr_ch_t HR_XD *c)
{
    uint8_t n = c->n, i, j, cnt = 0;
    uint16_t tmp, med, mean, total = 0, d;

    c->n = 0;
    if (n < HR_MIN_IV) return 0;

    for (i = 1; i < n; i++) {                       /* insertion sort, n <= 16 */
        tmp = c->iv[i];
        j = i;
        while (j > 0 && c->iv[j - 1] > tmp) {
            c->iv[j] = c->iv[j - 1];
            j--;
        }
        c->iv[j] = tmp;
    }
    med = (n & 1) ? c->iv[n >> 1] : (uint16_t)((c->iv[(n >> 1) - 1] + c->iv[n >> 1]) >> 1);

    for (i = 0; i < n; i++) {
        d = (c->iv[i] > med) ? (uint16_t)(c->iv[i] - med) : (uint16_t)(med - c->iv[i]);
        if ((uint16_t)(d * HR_TRIM_DIV) <= med) {
            total += c->iv[i];
            cnt++;
        }
    }
    if (cnt < HR_MIN_KEPT || (uint16_t)(cnt * 100U) < (uint16_t)(n * HR_KEEP_PCT)) return 0;

    mean = (uint16_t)((total + (cnt >> 1)) / cnt);
    return (uint8_t)((60000U + (mean >> 1)) / mean);
}

static uint8_t hr_cross_check(uint8_t b1, uint8_t b2)
{
    uint8_t d;
    if (b1 == 0 || b2 == 0) return 0;
    d = (b1 > b2) ? (uint8_t)(b1 - b2) : (uint8_t)(b2 - b1);
    if ((uint16_t)(d * HR_CROSS_DIV) > (uint16_t)((b1 + b2) >> 1)) return 0;
    return (uint8_t)((b1 + b2 + 1) >> 1);
}

/* Feed one packet. Returns 1 when a 10 s cycle just ended; *bpm then holds the result (0 = invalid). */
static uint8_t hr_feed(hr_t HR_XD *h, uint16_t ppg1, uint16_t ppg2, uint16_t t, uint8_t HR_XD *bpm)
{
    uint8_t b1, b2;

    if (!h->started) {
        h->start = t;
        h->started = 1;
    }
    hr_ch_feed(&h->ir, ppg1, t);
    hr_ch_feed(&h->red, ppg2, t);
    if ((uint16_t)(t - h->start) < HR_WINDOW_MS) return 0;

    h->start = t;
    b1 = hr_ch_finish(&h->ir);
    b2 = hr_ch_finish(&h->red);
    h->last_b1 = b1;
    h->last_b2 = b2;
    if (h->cycles < 255) h->cycles++;
    *bpm = (h->cycles <= HR_WARMUP_CYCLES) ? 0 : hr_cross_check(b1, b2);
    return 1;
}

#endif
