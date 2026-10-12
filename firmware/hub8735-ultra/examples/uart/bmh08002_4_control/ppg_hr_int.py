"""Integer-only, streaming heart-rate algorithm for BMH08002-4 PPG1/PPG2 (prototype for the N76E003).

Everything here is written so it translates 1:1 to 8051 C (SDCC): no floats, no stored waveform,
only shifts / add / compare, one small interval table per channel, and a 1 ms time stamp per sample.
The constants at the top are the future `#define`s.

Beat detection follows Elgendi et al. 2013 (PLoS ONE 8(10):e76585, "two event-related moving
averages"), implemented with recursive (exponential) averages so that no window buffer is needed:

  per sample  : x = ppg >> 1;  b += (x - b) >> 5;  a = x - b           (remove drift, ~1 s)
                s += (a - s) >> 1;  p = max(s, 0)                       (light smoothing, rectify)
                mp += (p - mp) >> 2        short average ("peak", ~110 ms)
                mb += (p - mb) >> 3        long average ("beat", ~220 ms)
                mean += (p - mean) >> 6;  thr = mb + (mean >> 3)        (long average + offset)
                a "block" is the run of samples with mp > thr; its maximum is the beat
                (block >= 3 samples, beats >= 300 ms apart, a block > 600 ms is drift, not a pulse)
  per 10 s    : intervals 300-2000 ms -> median -> drop > 20 % from the median
                -> need >= 5 left and >= 60 % of the valid ones (else 0) -> mean -> BPM = 60000 / mean
  two channels: PPG1 (IR) and PPG2 (red) must agree within 10 %, otherwise the result is 0
  first cycle : warm-up, result 0

Replay a recorded CSV (sample times come from the measured packet period):

  python ppg_hr_int.py ppg_run1.csv
"""

import csv
import sys

# ---- constants (C: #define) -------------------------------------------------------------------
WINDOW_MS = 10000        # one measurement cycle
BASE_SHIFT = 5           # b += (x - b) >> 5        ~ 1 s baseline
SMOOTH_SHIFT = 1         # s += (a - s) >> 1
PEAK_SHIFT = 2           # short average, ~4 samples (111 ms in the paper)
BEAT_SHIFT = 3           # long average, ~8 samples (667 ms in the paper, shorter here: 36 Hz, and it must follow fast)
MEAN_SHIFT = 6           # slow mean of the rectified signal
OFFSET_SHIFT = 3         # offset = mean >> 3 (the paper's beta * mean)
MIN_AMP = 4              # a flat signal never triggers, in (ppg >> 1) counts
MIN_BLOCK = 3            # samples a block must last (about 3 x 28 ms)
MAX_BLOCK_MS = 600       # a longer block is baseline drift / motion, not a pulse: dropped
REFRACT_MS = 300         # <= 200 BPM
MIN_BEAT_MS = 300
MAX_BEAT_MS = 2000       # >= 30 BPM
MAX_IV = 16              # intervals kept per cycle (uint16 each)
MIN_IV = 3               # fewer valid intervals than this -> 0
MIN_KEPT = 5             # after trimming, at least this many intervals must remain ...
KEEP_PCT = 60            # ... and they must be at least this % of the valid ones (regularity gate)
TRIM_DIV = 5             # |iv - median| * 5 <= median  (20 %)
CROSS_DIV = 10           # |bpm1 - bpm2| * 10 <= (bpm1 + bpm2) / 2  (10 %)
WARMUP_CYCLES = 1        # first cycle after start: the filters/baseline are still settling (hr2.csv: 90 vs ~73) -> 0


class Channel:
    """State of one PPG channel: ~25 bytes of scalars plus MAX_IV x uint16 intervals."""

    def __init__(self):
        self.b = self.s = self.mp = self.mb = self.mean = 0
        self.first = True
        self.in_blk = False
        self.skip = False
        self.w = 0
        self.best = 0
        self.best_t = 0
        self.blk_t = 0
        self.last_t = 0
        self.have_last = False
        self.iv = []

    def feed(self, raw, t_ms):
        """raw: uint16 PPG sample, t_ms: uint16 millisecond time stamp (wraps, use differences)."""
        x = raw >> 1
        if self.first:
            self.b = x
            self.first = False
        self.b += (x - self.b) >> BASE_SHIFT
        a = x - self.b
        self.s += (a - self.s) >> SMOOTH_SHIFT
        p = self.s if self.s > 0 else 0
        self.mp += (p - self.mp) >> PEAK_SHIFT
        self.mb += (p - self.mb) >> BEAT_SHIFT
        self.mean += (p - self.mean) >> MEAN_SHIFT
        thr = self.mb + (self.mean >> OFFSET_SHIFT)

        if self.mp > thr and self.mp > MIN_AMP:
            if self.skip:
                return                                # drift block already dropped, wait for it to end
            if not self.in_blk:
                self.in_blk = True
                self.w = 0
                self.best, self.best_t, self.blk_t = p, t_ms, t_ms
            if self.w < 255:
                self.w += 1
            if p > self.best:
                self.best, self.best_t = p, t_ms
            if ((t_ms - self.blk_t) & 0xFFFF) > MAX_BLOCK_MS:
                self.in_blk = False
                self.skip = True
                self.have_last = False                # not a pulse: the chain restarts
        else:
            self.skip = False
            if self.in_blk:
                self.in_blk = False
                if self.w >= MIN_BLOCK:
                    self._beat(self.best_t)

    def _beat(self, t):
        if self.have_last:
            iv = (t - self.last_t) & 0xFFFF
            if iv < REFRACT_MS:
                return                                # too close to the previous beat: ignore this one
            if MIN_BEAT_MS <= iv <= MAX_BEAT_MS and len(self.iv) < MAX_IV:
                self.iv.append(iv)
        self.last_t, self.have_last = t, True

    def finish(self):
        """End of a cycle: returns BPM (0 = invalid) and clears the interval table."""
        v = sorted(self.iv)                       # insertion sort on <= 16 values in C
        self.iv = []
        n = len(v)
        if n < MIN_IV:
            return 0
        med = v[n // 2] if n & 1 else (v[n // 2 - 1] + v[n // 2]) >> 1
        total = cnt = 0
        for iv in v:
            if abs(iv - med) * TRIM_DIV <= med:
                total += iv
                cnt += 1
        if cnt < MIN_KEPT or cnt * 100 < n * KEEP_PCT:
            return 0                                  # too irregular (motion / loose finger)
        mean = (total + (cnt >> 1)) // cnt
        return (60000 + (mean >> 1)) // mean       # 60000 fits uint16


def cross_check(bpm1, bpm2):
    """Both channels must be valid and agree within 10 %; otherwise 0."""
    if bpm1 == 0 or bpm2 == 0:
        return 0
    if abs(bpm1 - bpm2) * CROSS_DIV > (bpm1 + bpm2) >> 1:
        return 0
    return (bpm1 + bpm2 + 1) >> 1


class HeartRate:
    """Feed (ppg1, ppg2, t_ms) per packet; returns (bpm, bpm1, bpm2) at the end of each cycle."""

    def __init__(self):
        self.ir, self.red = Channel(), Channel()
        self.start = None
        self.cycles = 0

    def feed(self, ppg1, ppg2, t_ms):
        if self.start is None:
            self.start = t_ms
        self.ir.feed(ppg1, t_ms)
        self.red.feed(ppg2, t_ms)
        if ((t_ms - self.start) & 0xFFFF) >= WINDOW_MS:
            self.start = t_ms
            b1, b2 = self.ir.finish(), self.red.finish()
            self.cycles += 1
            if self.cycles <= WARMUP_CYCLES:
                return 0, b1, b2
            return cross_check(b1, b2), b1, b2
        return None


def split_runs(rows):
    """--csv appends, so one file can hold several recordings: elapsed_s restarts at 0 for each."""
    runs, cur, last = [], [], None
    for r in rows:
        t = float(r["elapsed_s"])
        if last is not None and t < last:
            runs.append(cur)
            cur = []
        cur.append(r)
        last = t
    runs.append(cur)
    return runs


def replay(path):
    with open(path, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    runs = split_runs(rows)
    if len(runs) > 1:
        print("檔案內有 %d 次錄製（--csv 會附加寫入），分別處理" % len(runs))
    for n, run in enumerate(runs, 1):
        if len(run) < 50:
            print("[錄製 %d] 只有 %d 筆，略過" % (n, len(run)))
            continue
        span = float(run[-1]["elapsed_s"]) - float(run[0]["elapsed_s"])
        period_ms = span * 1000.0 / (len(run) - 1)
        print("[錄製 %d] %d 筆，%.1f s，取樣間隔 %.1f ms（實測）" % (n, len(run), span, period_ms))
        hr = HeartRate()
        for i, r in enumerate(run):
            res = hr.feed(int(r["ir_ppg"]), int(r["red_ppg"]), int(round(i * period_ms)) & 0xFFFF)
            if res:
                bpm, b1, b2 = res
                print("  %6.1f s  PPG1 %3d  PPG2 %3d  => %s" % (i * period_ms / 1000, b1, b2, bpm if bpm else "0（無效）"))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    replay(sys.argv[1])
