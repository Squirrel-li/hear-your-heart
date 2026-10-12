"""Offline heart rate from a bmh08002_control.py CSV (continuous upload, 50 Hz); pure stdlib.

  python analyze_csv.py log.csv [--channel ir|red] [--out hr.csv]
"""

import argparse
import csv
import statistics
import sys

FS = 50.0  # continuous upload: one packet every 20 ms
GAP_S = 0.15  # host-time gap treated as dropped packets (packets carry no sequence number)


def moving_average(x, n):
    out, acc = [], 0.0
    for i, v in enumerate(x):
        acc += v
        if i >= n:
            acc -= x[i - n]
        out.append(acc / min(i + 1, n))
    return out


def load_segments(path, channel):
    """Split rows into runs with finger+data (status 2) and no host-time gap."""
    segments, cur, last_t = [], [], None
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            t, ok = float(row["elapsed_s"]), row["status"] == "2"
            if not ok or (last_t is not None and t - last_t > GAP_S):
                if cur:
                    segments.append(cur)
                cur = []
            last_t = t
            if ok:
                cur.append((float(row[channel]), int(row["d1"])))  # d1 = D3 module heart rate
    if cur:
        segments.append(cur)
    return segments


def detect_peaks(x):
    # zero-phase-free filter: subtract ~1 s mean (high-pass), then ~0.1 s mean (low-pass, ~5 Hz)
    base = moving_average(x, int(FS))
    y = moving_average([a - b for a, b in zip(x, base)], 5)
    n = len(y)
    if n < int(3 * FS):
        return []
    s = sorted(y)
    low, mid, high = s[n // 10], s[n // 2], s[(9 * n) // 10]
    sign = 1 if high - mid >= mid - low else -1  # pulse peaks may be inverted
    y = [sign * v for v in y]
    s = sorted(y)
    thr = s[n // 10] + 0.5 * (s[(9 * n) // 10] - s[n // 10])
    min_gap = int(0.27 * FS)  # up to ~220 BPM
    peaks = []
    for i in range(1, n - 1):
        if y[i] >= thr and y[i] > y[i - 1] and y[i] >= y[i + 1]:
            if peaks and i - peaks[-1] < min_gap:
                if y[i] > y[peaks[-1]]:
                    peaks[-1] = i
            else:
                peaks.append(i)
    return peaks


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("csv")
    p.add_argument("--channel", choices=("ir", "red"), default="ir")
    p.add_argument("--out", help="輸出每個峰值間隔的 CSV")
    a = p.parse_args()

    rows = []  # (segment, time_s within segment, bpm, module_bpm)
    for seg_no, seg in enumerate(load_segments(a.csv, a.channel + "_ppg")):
        peaks = detect_peaks([v for v, _ in seg])
        for k in range(1, len(peaks)):
            interval = (peaks[k] - peaks[k - 1]) / FS
            if 60 / 220 <= interval <= 60 / 30:
                rows.append((seg_no, peaks[k] / FS, 60 / interval, seg[peaks[k]][1]))
    if not rows:
        print("沒有可用的峰值（資料太短、沒有手指，或訊號太差）", file=sys.stderr)
        return 1

    # per-beat intervals are noisy: smooth with the median of the last 5 beats per segment
    bpm_all, mod_all, last_seg, window = [], [], None, []
    smooth = []
    for seg_no, t, bpm, mod in rows:
        if seg_no != last_seg:
            window, last_seg = [], seg_no
        window.append(bpm)
        window = window[-5:]
        smooth.append(statistics.median(window))
        bpm_all.append(smooth[-1])
        if mod:
            mod_all.append((smooth[-1], mod))
    print(f"channel={a.channel}  beats={len(rows)}  BPM median={statistics.median(bpm_all):.1f}  "
          f"min={min(bpm_all):.1f}  max={max(bpm_all):.1f}")
    if mod_all:
        err = [abs(m - r) for m, r in mod_all]
        print(f"與模組 D3 比較：平均絕對誤差 {statistics.mean(err):.1f} BPM（{len(err)} 筆）")
    if a.out:
        with open(a.out, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["segment", "time_s", "bpm_beat", "bpm_smooth", "module_bpm"])
            for (seg_no, t, bpm, mod), sm in zip(rows, smooth):
                w.writerow([seg_no, f"{t:.2f}", f"{bpm:.1f}", f"{sm:.1f}", mod])
    return 0


if __name__ == "__main__":
    sys.exit(main())
