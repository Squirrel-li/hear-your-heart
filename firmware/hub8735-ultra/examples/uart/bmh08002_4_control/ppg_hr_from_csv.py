"""Heart rate from the PPG1 (ir) / PPG2 (red) columns of a bmh08002_control.py CSV (stdlib only).

Continuous-upload mode on this module sends one packet every ~28 ms (~35.8 Hz), not the
datasheet's 20 ms. The sample interval is therefore measured from the CSV (elapsed_s span / packet
count); --sample-ms overrides it.

  python ppg_hr_from_csv.py ppg_run1.csv                   # both channels
  python ppg_hr_from_csv.py ppg_run1.csv --channel ir --out beats.csv
  python ppg_hr_from_csv.py ppg_run1.csv --d3              # also compare with the module's D3
"""

import argparse
import csv
import statistics
import sys

DEFAULT_SAMPLE_S = 0.028   # measured on the module; used only when the CSV is too short to measure
MAX_GAP_S = 0.15           # host-time gap that is treated as dropped packets (nominal gap is 28 ms)
MIN_BEAT_S = 0.3           # refractory period (<= 200 BPM)
MAX_BEAT_S = 2.0           # intervals longer than this are not used (>= 30 BPM)
WINDOW_S = 10.0            # same length as `bmh08002_control.py measure`
TRIM_TOL = 0.2             # drop beat intervals more than 20% away from the median


def moving_average(values, n):
    out, acc = [], 0.0
    for i, v in enumerate(values):
        acc += v
        if i >= n:
            acc -= values[i - n]
        out.append(acc / min(i + 1, n))
    return out


def load_segments(rows, column):
    """Split into runs of consecutive packets; a host-time gap marks dropped packets.

    Returns [(values, d3_values, t_first, t_last)].
    """
    segments, cur, last_t = [], None, None
    for r in rows:
        t = float(r["elapsed_s"])
        # time going backwards = a new recording appended to the same CSV (--csv appends)
        if cur is None or (last_t is not None and (t < last_t or t - last_t > MAX_GAP_S)):
            if cur:
                segments.append(cur)
            cur = ([], [], t, t)
        cur[0].append(int(r[column]))
        cur[1].append(int(float(r["d1"])) if r.get("d1") else 0)
        cur = (cur[0], cur[1], cur[2], t)
        last_t = t
    if cur:
        segments.append(cur)
    return segments


def measure_sample_s(segments):
    """Mean packet interval over all gap-free segments (host jitter averages out)."""
    span = sum(s[3] - s[2] for s in segments)
    count = sum(len(s[0]) - 1 for s in segments)
    if count < 50 or span <= 0:
        return DEFAULT_SAMPLE_S, False
    return span / count, True


def find_beats(values, invert, sample_s=DEFAULT_SAMPLE_S):
    """Return sample indices of pulse peaks."""
    if invert:
        values = [-v for v in values]
    base = moving_average(values, max(int(1.0 / sample_s), 2))     # remove drift (~1 s)
    ac = [v - b for v, b in zip(values, base)]
    sm = moving_average(ac, max(int(0.1 / sample_s), 1))           # ~100 ms low-pass
    sd = statistics.pstdev(sm) if len(sm) > 1 else 0.0
    if sd == 0:
        return []
    half = max(int(MIN_BEAT_S / sample_s), 1)
    peaks = []
    for i in range(half, len(sm) - half):
        if sm[i] == max(sm[i - half:i + half + 1]) and sm[i] > 0.3 * sd:
            if peaks and i - peaks[-1] < half:
                if sm[i] > sm[peaks[-1]]:
                    peaks[-1] = i
            else:
                peaks.append(i)
    return peaks


def robust_bpm(ivs, tol=TRIM_TOL):
    """Mean BPM after dropping beat intervals more than tol away from the median.

    Returns (bpm, kept, valid) or None when fewer than 3 plausible intervals exist.
    """
    valid = [iv for iv in ivs if iv and MIN_BEAT_S <= iv <= MAX_BEAT_S]
    if len(valid) < 3:
        return None
    med = statistics.median(valid)
    kept = [iv for iv in valid if abs(iv - med) <= tol * med]
    return 60.0 / statistics.mean(kept), len(kept), len(valid)


def analyze(rows, column, label, invert, sample_override, out_rows, d3):
    segments = load_segments(rows, column)
    if sample_override:
        sample_s, measured = sample_override, False
    else:
        sample_s, measured = measure_sample_s(segments)
    beats = []  # (time_s, interval_s or None, module D3 bpm)
    t_off = 0.0
    for vals, d3s, _, _ in segments:
        peaks = find_beats(vals, invert, sample_s)
        for k, i in enumerate(peaks):
            iv = (peaks[k] - peaks[k - 1]) * sample_s if k else None
            beats.append((t_off + i * sample_s, iv, d3s[i]))
        t_off += len(vals) * sample_s + MAX_GAP_S  # keep segments apart on the time axis
    total = sum(len(s[0]) for s in segments)
    note = "實測" if measured else ("指定" if sample_override else "預設，資料太短無法實測")
    print("[%s] 資料 %.1f s（%d 段，%d 筆），取樣間隔 %.1f ms（%s），偵測到 %d 個脈搏" % (
        label, total * sample_s, len(segments), total, sample_s * 1000, note, len(beats)))
    ivs = [iv for _, iv, _ in beats]
    res = robust_bpm(ivs)
    if res is None:
        print("  可用心跳間隔不足，無法計算（檢查手指、通道、--invert）\n")
        return
    bpm, kept, valid = res
    print("  平均心率 %.1f BPM（%d 個間隔，剔除 %d 個極端值）" % (bpm, kept, valid - kept))
    if d3:
        mod = [m for _, _, m in beats if 30 <= m <= 250]  # D3 valid range per datasheet
        if mod:
            print("  模組 D3 心率：平均 %.1f、中位數 %.0f BPM，本次減 D3 = %+.1f" % (
                statistics.mean(mod), statistics.median(mod), bpm - statistics.mean(mod)))
        else:
            print("  模組 D3 沒有有效值（30–250 BPM）")
    good = [iv for iv in ivs if iv and MIN_BEAT_S <= iv <= MAX_BEAT_S]
    if len(good) >= 3:
        diffs = [(good[i + 1] - good[i]) * 1000 for i in range(len(good) - 1)]
        print("  SDNN %.1f ms  RMSSD %.1f ms（%d 個間隔，未剔除；解析度約 %.0f ms）" % (
            statistics.pstdev(good) * 1000, (sum(x * x for x in diffs) / len(diffs)) ** 0.5,
            len(good), sample_s * 1000))
    print("  每 %.0f s 視窗（同 measure：剔除偏離中位數 %d%% 以上的間隔）：" % (WINDOW_S, TRIM_TOL * 100))
    start = beats[0][0]
    while start < beats[-1][0]:
        w = robust_bpm([iv for t, iv, _ in beats if start < t <= start + WINDOW_S])
        if w:
            print("    %6.1f–%6.1f s  %5.1f BPM（%d 個間隔，剔除 %d）" % (
                start, start + WINDOW_S, w[0], w[1], w[2] - w[1]))
        start += WINDOW_S
    print()
    for t, iv, m in beats:
        row = [label, "%.2f" % t, "" if iv is None else "%.3f" % iv, "" if not iv else "%.1f" % (60.0 / iv)]
        out_rows.append(row + [m] if d3 else row)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("csv")
    p.add_argument("--channel", choices=("ir", "red", "both"), default="both", help="PPG1=ir、PPG2=red")
    p.add_argument("--invert", action="store_true", help="波形上下顛倒時使用（以谷值當脈搏）")
    p.add_argument("--sample-ms", type=float, help="手動指定取樣間隔（毫秒）；預設由 CSV 實測")
    p.add_argument("--d3", action="store_true", help="對照模組 D3 心率（CSV 的 d1 欄），輸出差值")
    p.add_argument("--out", help="輸出每個脈搏的時間、間隔與瞬時 BPM")
    a = p.parse_args()

    with open(a.csv, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    channels = {"ir": ("ir_ppg", "PPG1 紅外"), "red": ("red_ppg", "PPG2 紅光")}
    use = ("ir", "red") if a.channel == "both" else (a.channel,)
    if a.d3 and rows and "d1" not in rows[0]:
        raise SystemExit("CSV 缺少 d1 欄（模組 D3 心率）")
    if not rows or any(channels[c][0] not in rows[0] for c in use):
        raise SystemExit("CSV 缺少 ir_ppg / red_ppg 欄位，請用 bmh08002_control.py 記錄的檔案")

    override = a.sample_ms / 1000.0 if a.sample_ms else None
    out_rows = []
    for c in use:
        analyze(rows, channels[c][0], channels[c][1], a.invert, override, out_rows, a.d3)
    if a.out:
        with open(a.out, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["channel", "time_s", "interval_s", "inst_bpm"] + (["module_d3_bpm"] if a.d3 else []))
            w.writerows(out_rows)
        print("已輸出 %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
