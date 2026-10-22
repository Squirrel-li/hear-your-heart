"""Offline, trailing-window dual PPG HR prototype. Requires NumPy; not a clinical estimator.

python motion_hr.py recording.csv --out results.csv [--acc synchronized_acc.csv]
PPG: elapsed_s,ir_ppg,red_ppg[,status,d1]. Acc: elapsed_s,acc_x,acc_y,acc_z.
See MOTION_HR.md for timing assumptions, limitations and verification.
"""
import argparse
import csv
import math
from pathlib import Path

import numpy as np

WINDOW = 10.0
STEP = 2.0
MAX_GAP = 0.15


def load_csv(path, columns):
    """Reject malformed inputs rather than quietly inventing samples."""
    with open(path, newline="", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        if not set(columns).issubset(reader.fieldnames or []):
            raise ValueError(f"{path}: required columns: {', '.join(columns)}")
        rows = []
        for line, row in enumerate(reader, 2):
            try:
                values = [float(row[c]) for c in columns]
                if not all(math.isfinite(v) for v in values):
                    raise ValueError("non-finite number")
                if values[0] < 0:
                    raise ValueError("negative time")
                rows.append((values, row))
            except (ValueError, TypeError) as exc:
                raise ValueError(f"{path}:{line}: {exc}") from exc
    if not rows:
        raise ValueError(f"{path}: empty recording")
    return np.array([r[0] for r in rows]), [r[1] for r in rows]


def spectrum(values, fs, nfft):
    """Remove isolated impulses and linear drift, then Hann-window the spectrum."""
    x = np.array(values, dtype=float, copy=True)
    # ponytail: five-point median fixes isolated spikes only; sustained contact
    # changes are rejected below, not 'recovered' by an unvalidated model.
    median = np.median(np.lib.stride_tricks.sliding_window_view(
        np.pad(x, 2, mode="edge"), 5), axis=1)
    residual = np.abs(x - median)
    scale = np.percentile(x, 95) - np.percentile(x, 5)
    spikes = residual > max(0.35 * scale, 1e-12)
    x[spikes] = median[spikes]
    u = np.linspace(-1, 1, len(x))
    x -= np.mean(x)
    x -= u * np.dot(u, x) / np.dot(u, u)
    power = abs(np.fft.rfft(x * np.hanning(len(x)), n=nfft)) ** 2
    freq = np.fft.rfftfreq(nfft, 1 / fs)
    power[(freq < 0.6) | (freq > 3.5)] = 0
    return power, float(np.mean(spikes)), scale, float(np.max(np.abs(np.diff(median))))


def sequence_breaks(rows):
    """Optional BLE sequence: do not interpolate silently over a known lost frame."""
    breaks = np.zeros(len(rows), dtype=bool)
    if not rows or 'sequence' not in rows[0]:
        return breaks
    previous = None
    for i, row in enumerate(rows):
        try:
            sequence = int(row['sequence'])
            if not 0 <= sequence <= 0xffffffff:
                raise ValueError('out of range')
        except (ValueError, TypeError) as exc:
            raise ValueError('sequence must be an unsigned 32-bit integer') from exc
        if previous is not None:
            breaks[i] = ((sequence - previous) & 0xffffffff) != 1
        previous = sequence
    return breaks


def estimate(ppg, fs, acc=None):
    """One already uniformly sampled window; acc must be aligned to PPG.

    Returns HR=0 on rejection. Quality is a spectral heuristic, not probability.
    No previous HR is used to manufacture a value in a rejected window.
    """
    ppg = np.asarray(ppg, dtype=float)
    if not math.isfinite(fs) or fs < 12 or ppg.ndim != 2 or ppg.shape[1] != 2:
        raise ValueError("need fs >= 12 Hz and an N x 2 PPG array")
    if len(ppg) < 8 * fs or not np.isfinite(ppg).all():
        raise ValueError("need >= 8 seconds of finite PPG samples")
    if acc is not None:
        acc = np.asarray(acc, dtype=float)
        if acc.shape != (len(ppg), 3) or not np.isfinite(acc).all():
            raise ValueError("acc must be finite, synchronized N x 3 samples")
    result = dict(bpm=0.0, quality=0.0, reason="", mode="acc" if acc is not None else "ppg_only",
                  ir_bpm=0.0, red_bpm=0.0, removed_fraction=0.0)
    nfft = 1 << (4 * len(ppg) - 1).bit_length()
    freq = np.fft.rfftfreq(nfft, 1 / fs)
    powers = []
    for channel in ppg.T:
        p, spikes, scale, jump = spectrum(channel, fs, nfft)
        if scale < 1e-8 or np.mean(np.diff(channel) == 0) > 0.9:
            result["reason"] = "flat_signal"
            return result
        # A discontinuity can look periodic after band limiting; reject first.
        if spikes > 0.05 or jump > 0.8 * scale:
            result["reason"] = "contact_or_gain_jump"
            return result
        if p.sum() <= 1e-20:
            result["reason"] = "no_band_energy"
            return result
        powers.append(p / p.sum())
    powers = np.array(powers)
    motion_mask = np.zeros(len(freq), dtype=bool)
    if acc is not None:
        for axis in acc.T:
            # Units may be g or raw counts. A constant axis contributes nothing.
            if np.ptp(axis) < 1e-10:
                continue
            ap, _, _, _ = spectrum(axis, fs, nfft)
            if ap.max() <= 1e-20:
                continue
            peaks = np.where((ap[1:-1] > ap[:-2]) & (ap[1:-1] >= ap[2:]) &
                             (ap[1:-1] > 0.15 * ap.max()))[0] + 1
            for k in peaks:
                motion_mask |= abs(freq - freq[k]) <= 0.15
        # Conservative spectral exclusion, inspired by spectral MA methods.
        # This is NOT a reproduction of SPECMAR nor a reconstructed clean PPG.
        result["removed_fraction"] = float(powers[:, motion_mask].sum(axis=1).mean())
        powers[:, motion_mask] = 0
        if np.min(powers.sum(axis=1)) < 0.08:
            result["reason"] = "motion_overlap_or_no_pulse"
            return result
    allowed = (freq >= 40 / 60) & (freq <= 200 / 60)
    powers[:, ~allowed] = 0
    qualities, peaks = [], []
    for p in powers:
        k = int(np.argmax(p))
        near = abs(freq - freq[k]) <= 0.12
        concentration = p[near].sum() / max(p.sum(), 1e-20)
        other = p.copy()
        other[abs(freq - freq[k]) <= 0.25] = 0
        if concentration < 0.40 or other.max() > 0.65 * p[k]:
            result["reason"] = "ambiguous_spectrum"
            return result
        if motion_mask.any() and np.min(abs(freq[motion_mask] - freq[k])) < 0.10:
            result["reason"] = "motion_overlap_or_no_pulse"
            return result
        peaks.append(float(freq[k] * 60))
        qualities.append(float(concentration))
    result["ir_bpm"], result["red_bpm"] = peaks
    if abs(peaks[0] - peaks[1]) > max(6, 0.08 * np.mean(peaks)):
        result["reason"] = "channel_disagreement"
        return result
    result.update(bpm=float(np.mean(peaks)), quality=min(qualities), reason="ok")
    return result


def replay(path, acc_path=None, sample_ms=None, sensor_time=False):
    data, rows = load_csv(path, ("elapsed_s", "ir_ppg", "red_ppg"))
    if np.any((data[:, 1:] < 0) | (data[:, 1:] > 65535)):
        raise ValueError("PPG values must be unsigned 16-bit counts")
    if sample_ms is not None and (not math.isfinite(sample_ms) or not 0 < sample_ms <= 1000 / 12):
        raise ValueError("sample-ms must be positive and imply >= 12 Hz")
    if sensor_time and sample_ms is not None:
        raise ValueError("sample-ms and sensor-time are mutually exclusive")
    acc = None
    if acc_path:
        acc, acc_rows = load_csv(acc_path, ("elapsed_s", "acc_x", "acc_y", "acc_z"))
        acc_breaks = sequence_breaks(acc_rows)
        if len(acc) < 2 or np.any(np.diff(acc[:, 0]) <= 0):
            raise ValueError("ACC timestamps must strictly increase")
        if np.median(np.diff(acc[:, 0])) > 0.050001:
            raise ValueError("ACC must contain actual new samples at >= 20 Hz; 1 Hz is insufficient")
    dt = np.diff(data[:, 0])
    boundaries = np.flatnonzero((dt < 0) | (dt > MAX_GAP) | sequence_breaks(rows)[1:]) + 1
    if acc_path and np.any(dt < 0):
        raise ValueError("split appended recordings before aligning an ACC recording")
    results = []
    for segment, indices in enumerate(np.split(np.arange(len(data)), boundaries), 1):
        run = data[indices]
        if len(run) < 2 or run[-1, 0] - run[0, 0] < WINDOW:
            continue
        if sensor_time and np.any(np.diff(run[:, 0]) <= 0):
            raise ValueError("sensor timestamps must strictly increase")
        period = sample_ms / 1000 if sample_ms else (run[-1, 0] - run[0, 0]) / (len(run) - 1)
        fs = 1 / period
        if fs < 12 or fs > 1000:
            raise ValueError(f"implausible sample rate: {fs:.2f} Hz")
        # ponytail: legacy PC logs lack sequence numbers and have quantized times.
        # Count/span assumes no hidden losses within a segment. For future BLE
        # captures use sensor-time plus sequence validation at acquisition.
        times = run[:, 0] if sensor_time else run[0, 0] + np.arange(len(run)) * period
        grid = np.arange(times[0], times[-1] + period * 0.01, period)
        values = np.column_stack([np.interp(grid, times, run[:, c]) for c in (1, 2)])
        status = []
        for i in indices:
            try:
                status.append(float(rows[i].get("status", "1")))
            except (ValueError, TypeError) as exc:
                raise ValueError("invalid status") from exc
        if not all(s in (0, 1, 2) for s in status):
            raise ValueError("status must be 0, 1 or 2")
        n, hop = round(WINDOW * fs), max(1, round(STEP * fs))
        for end in range(n, len(grid) + 1, hop):
            ts = grid[end - n:end]
            result = dict(bpm=0.0, quality=0.0, reason="no_contact", mode="acc" if acc_path else "ppg_only",
                          ir_bpm=0.0, red_bpm=0.0, removed_fraction=0.0)
            selected = (times >= ts[0]) & (times <= ts[-1])
            aligned = None
            reason = ""
            if acc is not None:
                lo = max(0, np.searchsorted(acc[:, 0], ts[0], side="right") - 1)
                hi = min(len(acc), np.searchsorted(acc[:, 0], ts[-1]) + 1)
                a = acc[lo:hi]
                if (len(a) < 2 or a[0, 0] > ts[0] or a[-1, 0] < ts[-1]
                        or np.max(np.diff(a[:, 0])) > 0.10
                        or np.median(np.diff(a[:, 0])) > 0.050001
                        or np.any(acc_breaks[lo + 1:hi])):
                    reason = "acc_gap"
                else:
                    aligned = np.column_stack([np.interp(ts, a[:, 0], a[:, c]) for c in (1, 2, 3)])
            if reason:
                result["reason"] = reason
            elif np.any(np.array(status)[selected] == 0):
                pass
            elif sensor_time and np.any(np.diff(times[selected]) > 2.5 * period):
                result["reason"] = "ppg_gap"
            else:
                result = estimate(values[end - n:end], fs, aligned)
            # d1 is module D3, only a diagnostic comparator, never an input to HR.
            d3 = []
            for i in indices[selected]:
                try:
                    v = float(rows[i].get("d1", "0"))
                    if 30 <= v <= 250:
                        d3.append(v)
                except (ValueError, TypeError):
                    pass
            results.append(dict(segment=segment, end_s=round(float(ts[-1]), 3), fs_hz=round(fs, 4),
                                **result, module_d3_bpm=float(np.median(d3)) if d3 else ""))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--acc", type=Path, help="same clock origin as PPG; real >=20 Hz samples")
    parser.add_argument("--sample-ms", type=float)
    parser.add_argument("--sensor-time", action="store_true", help="elapsed_s contains sensor timestamps")
    parser.add_argument("--out", type=Path, required=True, help="new file; refuses overwrite")
    args = parser.parse_args()
    try:
        results = replay(args.csv, args.acc, args.sample_ms, args.sensor_time)
        if not results:
            raise ValueError("no continuous segment long enough for a 10-second window")
        with args.out.open("x", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=list(results[0]))
            writer.writeheader()
            writer.writerows(results)
        valid = [r for r in results if r["bpm"] > 0]
        print(f"{len(valid)}/{len(results)} windows accepted; output: {args.out}")
        if valid:
            print(f"Accepted median: {np.median([r['bpm'] for r in valid]):.1f} BPM")
        if not args.acc:
            print("PPG only: common motion cannot be distinguished reliably from pulse.")
    except (ValueError, OSError) as exc:
        parser.exit(2, f"error: {exc}\n")


if __name__ == "__main__":
    main()
