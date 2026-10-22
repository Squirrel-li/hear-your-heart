/* Experimental streaming integer HR. 35.7 Hz PPG, 26 Hz +/-8g raw ACC.
 * Motion-aware candidate rejection/selection, NOT clean-waveform reconstruction.
 * Call from main only, never ISR. t is uint16 milliseconds; call/reset on gaps.
 * A zero bpm is invalid; reason explains why. No held value masquerades as fresh.
 */
#ifndef PPG_MOTION_HR_H
#define PPG_MOTION_HR_H
#include <stdint.h>
#include <string.h>
#ifdef __SDCC__
#define MH_XD __xdata
#else
#define MH_XD
#endif
#define MH_WINDOW 5000u
#define MH_IV 16
enum { MH_OK, MH_WARMUP, MH_SIGNAL, MH_MOTION, MH_DISAGREE, MH_ACC_MISSING, MH_GAP };
typedef struct {
    int16_t base, smooth, short_mean, long_mean, mean, peak;
    uint16_t peak_t, block_t, last_t, iv[MH_IV];
    uint8_t initialized, block, skip, have_last, width, n, bad;
} mh_channel;
typedef struct {
    int16_t base;
    uint16_t amplitude, crossing, period;
    uint8_t initialized, positive, have_crossing, stable;
} mh_axis;
typedef struct {
    mh_channel ppg[2];
    mh_axis acc[3];
    uint16_t start, last_ppg, last_acc;
    uint8_t started, warmed, require_acc, have_acc, acc_count, acc_bad;
    uint8_t bpm, raw_bpm, quality, reason, previous, channel_bpm[2], channel_quality[2];
} motion_hr;

static void mh_reset(motion_hr MH_XD *h, uint8_t require_acc)
{
    memset(h, 0, sizeof(*h));
    h->require_acc = require_acc;
    h->reason = MH_WARMUP;
}
static uint16_t mh_distance(uint16_t a, uint16_t b)
{
    return a > b ? a - b : b - a;
}
static void mh_acc_axis(mh_axis MH_XD *a, int16_t raw, uint16_t t)
{
    int16_t x = raw / 8, ac;
    uint16_t magnitude, interval;
    if (!a->initialized) { a->base = x; a->initialized = 1; }
    a->base += (x - a->base) / 16;
    ac = x - a->base;
    magnitude = ac < 0 ? (uint16_t)-ac : (uint16_t)ac;
    a->amplitude = (uint16_t)((int16_t)a->amplitude + ((int16_t)magnitude - (int16_t)a->amplitude) / 8);
    if (!a->positive && ac > (int16_t)(a->amplitude / 4 + 4)) {
        if (a->have_crossing) {
            interval = (uint16_t)(t - a->crossing);
            if (interval >= 250 && interval <= 2000) {
                if (a->period && mh_distance(interval, a->period) <= a->period / 5) {
                    if (a->stable < 255) a->stable++;
                    a->period = (a->period + interval) / 2;
                } else { a->period = interval; a->stable = 0; }
            } else { a->period = 0; a->stable = 0; }
        }
        a->crossing = t; a->have_crossing = a->positive = 1;
    } else if (ac < -(int16_t)(a->amplitude / 4 + 4)) a->positive = 0;
}
static void mh_acc(motion_hr MH_XD *h, int16_t x, int16_t y, int16_t z, uint16_t t)
{
    if (h->have_acc && (uint16_t)(t - h->last_acc) > 100) {
        memset(h->acc, 0, sizeof(h->acc)); h->acc_bad = 1;
    }
    h->last_acc = t; h->have_acc = 1;
    if (h->acc_count < 255) h->acc_count++;
    mh_acc_axis(&h->acc[0], x, t);
    mh_acc_axis(&h->acc[1], y, t);
    mh_acc_axis(&h->acc[2], z, t);
}
static void mh_channel_feed(mh_channel MH_XD *c, uint16_t raw, uint16_t t)
{
    int16_t x = (int16_t)(raw >> 3), ac, positive, threshold;
    uint16_t interval;
    if (!c->initialized) { c->base = x; c->initialized = 1; }
    /* Divisions explicitly truncate toward zero on both host and 8051.
     * Input scaled to 13 bits keeps all signed intermediates within int16. */
    c->base += (x - c->base) / 32;
    ac = x - c->base;
    c->smooth += (ac - c->smooth) / 2;
    positive = c->smooth > 0 ? c->smooth : 0;
    c->short_mean += (positive - c->short_mean) / 4;
    c->long_mean += (positive - c->long_mean) / 8;
    c->mean += (positive - c->mean) / 64;
    threshold = c->long_mean + c->mean / 8;
    if (c->short_mean > threshold && c->short_mean > 2) {
        if (c->skip) return;
        if (!c->block) {
            c->block = 1; c->width = 0; c->peak = positive;
            c->peak_t = c->block_t = t;
        }
        if (c->width < 255) c->width++;
        if (positive > c->peak) { c->peak = positive; c->peak_t = t; }
        if ((uint16_t)(t - c->block_t) > 700) {
            c->block = 0; c->skip = 1; c->have_last = 0;
            if (c->bad < 255) c->bad++;
        }
    } else {
        c->skip = 0;
        if (c->block) {
            c->block = 0;
            if (c->width < 2) return;
            if (c->have_last) {
                interval = (uint16_t)(c->peak_t - c->last_t);
                if (interval < 300) return;
                if (interval <= 1500 && c->n < MH_IV) c->iv[c->n++] = interval;
                else if (c->bad < 255) c->bad++;
            }
            c->last_t = c->peak_t; c->have_last = 1;
        }
    }
}
static uint8_t mh_finish(mh_channel MH_XD *c, uint8_t MH_XD *quality)
{
    uint8_t i, j, n = c->n, kept = 0, bad = c->bad;
    uint16_t value, median, total = 0;
    c->n = c->bad = 0; *quality = 0;
    if (n < 2 || bad > 1) return 0;
    /* ponytail: bounded insertion sort, <=16 intervals, no waveform buffer. */
    for (i = 1; i < n; i++) {
        value = c->iv[i]; j = i;
        while (j && c->iv[j - 1] > value) { c->iv[j] = c->iv[j - 1]; j--; }
        c->iv[j] = value;
    }
    median = c->iv[n / 2];
    for (i = 0; i < n; i++) {
        if (mh_distance(c->iv[i], median) <= median / 6) { total += c->iv[i]; kept++; }
    }
    if (kept < 2 || (uint16_t)kept * 100 < (uint16_t)n * 70) return 0;
    *quality = (uint8_t)((uint16_t)kept * 100 / n);
    value = (total + kept / 2) / kept;
    return (uint8_t)((60000u + value / 2) / value);
}
static uint8_t mh_motion_match(motion_hr MH_XD *h, uint8_t bpm, uint16_t t)
{
    uint8_t i;
    uint16_t period;
    if (!bpm || !h->have_acc) return 0;
    period = 60000u / bpm;
    for (i = 0; i < 3; i++) {
        mh_axis MH_XD *a = &h->acc[i];
        /* ~0.08g mean absolute dynamic acceleration; raw +/-8g sensor units.
         * Cadence overlap is ambiguous, never proof that this is NOT a pulse. */
        if (a->amplitude < 40 || a->stable < 2 || (uint16_t)(t - a->crossing) > 2000) continue;
        /* Harmonic veto uses a narrower band (8.3%) than direct cadence
         * (12.5%): broad half/double bands otherwise reject nearby real HR. */
        if (mh_distance(period, a->period) <= a->period / 8 ||
            mh_distance(period, a->period / 2) <= a->period / 24 ||
            mh_distance(period, a->period * 2) <= a->period / 6) return 1;
    }
    return 0;
}
static uint8_t mh_ppg(motion_hr MH_XD *h, uint16_t ir, uint16_t red, uint16_t t, uint8_t status)
{
    uint8_t b0, b1, m0, m1;
    if (ir <= 1 || red <= 1 || ir >= 65534u || red >= 65534u) {
        mh_reset(h, h->require_acc); h->reason = MH_SIGNAL; return 0;
    }
    if (status == 0 || status > 2 ||
        (h->started && ((uint16_t)(t - h->last_ppg) > 100 || t == h->last_ppg))) {
        mh_reset(h, h->require_acc); h->reason = MH_GAP; return 0;
    }
    if (!h->started) { h->start = t; h->started = 1; }
    h->last_ppg = t;
    mh_channel_feed(&h->ppg[0], ir, t);
    mh_channel_feed(&h->ppg[1], red, t);
    if ((uint16_t)(t - h->start) < MH_WINDOW) return 0;
    h->start = t; h->bpm = h->raw_bpm = h->quality = 0;
    b0 = h->channel_bpm[0] = mh_finish(&h->ppg[0], &h->channel_quality[0]);
    b1 = h->channel_bpm[1] = mh_finish(&h->ppg[1], &h->channel_quality[1]);
    m0 = mh_motion_match(h, b0, t); m1 = mh_motion_match(h, b1, t);
    if (!h->warmed) { h->warmed = 1; h->reason = MH_WARMUP; }
    else if (h->require_acc && (!h->have_acc || h->acc_bad || h->acc_count < 80 ||
             (uint16_t)(t - h->last_acc) > 100)) h->reason = MH_ACC_MISSING;
    else if (b0 && b1 && !m0 && !m1 && mh_distance(b0, b1) <= (uint16_t)(b0 + b1) / 20) {
        h->raw_bpm = (b0 + b1 + 1u) / 2;
        h->quality = h->channel_quality[0] < h->channel_quality[1] ? h->channel_quality[0] : h->channel_quality[1];
    } else if (h->require_acc && b0 && !m0 && (!b1 || m1) && h->channel_quality[0] >= 85) {
        h->raw_bpm = b0; h->quality = 60; /* one trustworthy candidate; lower confidence */
    } else if (h->require_acc && b1 && !m1 && (!b0 || m0) && h->channel_quality[1] >= 85) {
        h->raw_bpm = b1; h->quality = 60;
    } else h->reason = (m0 || m1) ? MH_MOTION : (b0 && b1) ? MH_DISAGREE : MH_SIGNAL;
    if (h->raw_bpm) {
        h->bpm = h->raw_bpm;
        if (h->previous && mh_distance(h->previous, h->raw_bpm) <= 12)
            h->bpm = (2u * h->raw_bpm + h->previous + 1) / 3;
        h->reason = MH_OK;
    }
    h->previous = h->bpm; /* invalid windows clear smoothing; no stale hold */
    h->acc_count = h->acc_bad = 0;
    return 1;
}
#endif
