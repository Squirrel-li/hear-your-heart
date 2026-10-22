"""Run directly: python test_motion_hr.py (NumPy only)."""
import csv
from pathlib import Path
import tempfile

import numpy as np

from motion_hr import estimate, replay, sequence_breaks


def check():
    assert sequence_breaks([{'sequence': '4294967295'}, {'sequence': '0'}, {'sequence': '2'}]).tolist() == [False, False, True]
    fs = 1 / 0.028
    t = np.arange(round(10 * fs)) / fs
    rng = np.random.default_rng(8735)
    for bpm in (45, 72, 120, 180, 195):
        pulse = np.sin(2 * np.pi * bpm / 60 * t)
        ppg = np.column_stack((4000 + 100 * pulse, 5000 + 70 * pulse))
        ppg += rng.normal(0, 2, ppg.shape)
        result = estimate(ppg, fs)
        assert abs(result['bpm'] - bpm) < 3, (bpm, result)

    pulse = np.sin(2 * np.pi * 1.2 * t)
    motion = np.sin(2 * np.pi * 2.1 * t)
    acc = np.column_stack((motion, 0.6 * np.sin(2 * np.pi * 2.1 * t + .4), t * 0))
    ppg = np.column_stack((4000 + 100 * pulse + 200 * motion,
                           5000 + 85 * pulse + 160 * motion))
    raw, cleaned = estimate(ppg, fs), estimate(ppg, fs, acc)
    assert abs(raw['bpm'] - 126) < 3, raw  # Demonstrate the actual failure without ACC.
    assert abs(cleaned['bpm'] - 72) < 3, cleaned
    assert cleaned['removed_fraction'] > .5, cleaned
    print(f"Motion fixture: PPG-only {raw['bpm']:.1f} -> ACC-assisted {cleaned['bpm']:.1f} BPM (truth 72)")

    # If pulse and motion overlap, reject instead of reporting a fabricated HR.
    overlap = np.column_stack((pulse, pulse, pulse))
    clean = np.column_stack((4000 + 100 * pulse, 5000 + 85 * pulse))
    assert estimate(clean, fs, overlap)['bpm'] == 0
    assert estimate(np.ones_like(clean), fs)['bpm'] == 0
    disagree = clean.copy()
    disagree[:, 1] = 5000 + 100 * np.sin(2 * np.pi * 1.8 * t)
    assert estimate(disagree, fs)['bpm'] == 0
    jump = clean.copy()
    jump[len(t)//2:] += 4000
    assert estimate(jump, fs)['bpm'] == 0
    spike = clean.copy()
    spike[100] += 3000
    assert abs(estimate(spike, fs)['bpm'] - 72) < 3
    # Constant ACC must not change a clean estimate.
    assert abs(estimate(clean, fs, np.ones((len(t), 3)))['bpm'] - 72) < 3
    try:
        estimate(clean * np.nan, fs)
        assert False, 'NaN accepted'
    except ValueError:
        pass

    with tempfile.TemporaryDirectory() as directory:
        path, apath = Path(directory) / 'ppg.csv', Path(directory) / 'acc.csv'
        tt = np.arange(0, 24, .028)
        def write_ppg(d3=72, gap=False):
            with path.open('w', newline='') as f:
                w = csv.writer(f)
                w.writerow(('elapsed_s', 'ir_ppg', 'red_ppg', 'status', 'd1'))
                for s in tt:
                    if gap and 8 < s < 8.5:
                        continue
                    pulse = 100 * np.sin(2 * np.pi * 1.2 * s)
                    w.writerow((s, 4000 + pulse, 5000 + pulse, 1, d3))
        write_ppg()
        baseline = replay(path, sensor_time=True)
        assert baseline and all(abs(r['bpm'] - 72) < 3 for r in baseline)
        write_ppg(d3=150)
        assert [r['bpm'] for r in replay(path, sensor_time=True)] == [r['bpm'] for r in baseline]
        with path.open() as f:
            records = list(csv.DictReader(f))
        with path.open('w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=list(records[0]) + ['sequence'])
            w.writeheader()
            for i, row in enumerate(records):
                if i != 100:
                    w.writerow(dict(row, sequence=i))
        # Only one missing sample (56 ms gap), but sequence must still split it.
        assert all(r['end_s'] > 12.7 for r in replay(path, sensor_time=True))
        write_ppg(gap=True)
        assert all(r['end_s'] >= 18.4 for r in replay(path, sensor_time=True))
        with apath.open('w', newline='') as f:
            w = csv.writer(f)
            w.writerow(('elapsed_s', 'acc_x', 'acc_y', 'acc_z'))
            w.writerows((i, 1, 2, 3) for i in range(25))
        try:
            replay(path, apath)
            assert False, '1 Hz ACC accepted'
        except ValueError:
            pass
        # Full CSV synchronization path, plus missing ACC coverage rejection.
        write_ppg()
        with apath.open('w', newline='') as f:
            w = csv.writer(f)
            w.writerow(('elapsed_s', 'acc_x', 'acc_y', 'acc_z'))
            w.writerows((s, np.sin(2*np.pi*2.1*s), 0, 0) for s in np.arange(0, 25, .02))
        assert all(abs(r['bpm'] - 72) < 3 for r in replay(path, apath, sensor_time=True))
        with apath.open('w', newline='') as f:
            w = csv.writer(f)
            w.writerow(('elapsed_s', 'acc_x', 'acc_y', 'acc_z'))
            w.writerows((s, 1, 0, 0) for s in np.arange(15, 25, .02))
        assert replay(path, apath, sensor_time=True)[0]['reason'] == 'acc_gap'
        path.write_text('elapsed_s,ir_ppg,red_ppg\n0,nan,1\n')
        try:
            replay(path)
            assert False, 'malformed CSV accepted'
        except ValueError:
            pass
    print('All motion HR checks passed.')


if __name__ == '__main__':
    check()
