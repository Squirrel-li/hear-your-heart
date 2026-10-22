"""Frozen-algorithm external benchmark on the 12 public TROIKA treadmill records.

Requires NumPy, SciPy and gcc. Downloaded data stay in --data, not in source control.
python benchmark_troika.py --data DATA_DIRECTORY --out NEW_RESULTS_DIRECTORY [--download]
No threshold tuning, label-based normalization or carrying forward rejected HR.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import urllib.request

import numpy as np
import scipy
from scipy.io import loadmat
from scipy.signal import resample_poly

from test_motion_hr import HERE, WRAPPER, run

COMMIT = '5ce28d20cc59dcca696120cc0db4797ce5af526a'
SOURCE = f'https://raw.githubusercontent.com/olivesgatech/HeartBEAT/{COMMIT}/Code/data/'
NAMES = [f'DATA_{i:02}_TYPE{1 if i == 1 else 2:02}' for i in range(1, 13)]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(sig, ppg_gain, acc_gain):
    if sig.ndim != 2 or sig.shape[0] != 6 or not np.isfinite(sig).all():
        raise ValueError('expected finite 6 x N signal matrix')
    # 125 Hz -> 35.714 Hz and 26 Hz with FIR anti-aliasing. This offline
    # resampling is an input adapter, not extra motion removal in the MCU.
    ppg = resample_poly(sig[1:3], 2, 7, axis=1)
    acc = resample_poly(sig[3:6], 26, 125, axis=1)
    ppg = np.rint(16384 + ppg_gain * ppg)
    acc = np.rint(acc_gain * acc)
    clipping = dict(ppg=int(np.sum((ppg < 0) | (ppg > 65535))),
                    acc=int(np.sum((acc < -32768) | (acc > 32767))))
    ppg = np.clip(ppg, 0, 65535).astype(int)
    acc = np.clip(acc, -32768, 32767).astype(int)
    events = [(0, i * 28, int(a), int(b), 0, 1) for i, (a, b) in enumerate(ppg.T)]
    events += [(1, round(i * 1000 / 26), int(a) & 65535, int(b) & 65535, int(c) & 65535, 0)
               for i, (a, b, c) in enumerate(acc.T)]
    return sorted(events, key=lambda e: (e[1], -e[0])), clipping


def score(rows, labels, name, mode):
    labels = np.asarray(labels).reshape(-1)
    if not len(labels) or not np.isfinite(labels).all() or np.any(labels <= 0):
        raise ValueError('invalid ECG reference')
    # Reference BPM0: [0,8], [2,10], ... seconds; centers 4,6,... .
    # Compare the center of our trailing 5-second window to those centers.
    reference_times = 4 + 2 * np.arange(len(labels))
    scored = []
    for row in rows:
        end = int(row['time_ms']) / 1000
        center = end - 2.5
        if end < 10 or not reference_times[0] <= center <= reference_times[-1]:
            continue
        target = float(np.interp(center, reference_times, labels))
        bpm = int(row['bpm'])
        # Protocol phases are approximate, not sample-accurate activity labels.
        phase = 'rest_protocol' if center < 30 or center >= 270 else 'run_protocol'
        scored.append(dict(record=name, mode=mode, end_s=end, reference_center_s=center,
                           phase=phase, reference_bpm=target, bpm=bpm, valid=int(bpm > 0),
                           absolute_error=abs(bpm-target) if bpm else '', reason=int(row['reason']),
                           quality=int(row['quality']), ir_bpm=int(row['ir_bpm']), red_bpm=int(row['red_bpm'])))
    return scored


def metrics(rows):
    errors = np.array([r['absolute_error'] for r in rows if r['valid']], dtype=float)
    n = len(rows)
    return dict(windows=n, accepted=len(errors), coverage_pct=round(100*len(errors)/n, 2) if n else None,
                mae_accepted=round(float(errors.mean()), 2) if len(errors) else None,
                median_ae_accepted=round(float(np.median(errors)), 2) if len(errors) else None,
                p95_ae_accepted=round(float(np.percentile(errors, 95)), 2) if len(errors) else None,
                accepted_error_gt10=int(np.sum(errors > 10)),
                within5_of_all_pct=round(100*int(np.sum(errors <= 5))/n, 2) if n else None)


def checks():
    # References are aligned by observation-window center, not array position.
    row=dict(time_ms='10000',bpm='71',reason='0',quality='100',ir_bpm='71',red_bpm='71')
    r=score([row], np.array([50,62,74,86]), 'test','test')[0]
    assert r['reference_bpm'] == 71 and r['absolute_error'] == 0
    invalid=dict(r,valid=0,bpm=0,absolute_error='')
    m=metrics([r,invalid]);assert m['coverage_pct']==50 and m['mae_accepted']==0 and m['within5_of_all_pct']==50
    assert metrics([invalid])['mae_accepted'] is None
    signal=np.zeros((6,1250));signal[1]=3;signal[2]=-3;signal[3]=1
    events,clip=prepare(signal,8,1/.000244)
    assert not any(clip.values()) and len([e for e in events if e[0]])==260
    assert all(0<=e[2]<=65535 for e in events)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--data',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--download',action='store_true')
    p.add_argument('--ppg-gain',type=float,default=8)
    p.add_argument('--acc-gain',type=float,default=1/.000244)
    args=p.parse_args()
    if any(not np.isfinite(g) or g<=0 for g in (args.ppg_gain,args.acc_gain)):
        p.error('gains must be finite and positive')
    checks()
    args.out.mkdir(parents=True,exist_ok=False)
    if args.download:
        args.data.mkdir(parents=True,exist_ok=True)
        for name in NAMES:
            for suffix in ('.mat','_BPMtrace.mat'):
                path=args.data/(name+suffix)
                if not path.exists():
                    with urllib.request.urlopen(SOURCE+path.name,timeout=60) as response:
                        content=response.read(5_000_001)
                    if len(content)>5_000_000: raise ValueError('unexpected download size')
                    with path.open('xb') as f: f.write(content)
    manifest=dict(source=SOURCE,commit=COMMIT,algorithm_sha256=digest(HERE/'source/ppg_motion_hr.h'),
                  harness_sha256=hashlib.sha256(WRAPPER.encode()).hexdigest(),numpy=np.__version__,scipy=scipy.__version__,
                  ppg_gain=args.ppg_gain,ppg_offset=16384,acc_gain=args.acc_gain,
                  note='ACC assumed g; conversion not confirmed by dataset readme. No algorithm changes.',files={})
    all_rows=[]
    with tempfile.TemporaryDirectory() as tmp:
        c,exe=Path(tmp)/'replay.c',Path(tmp)/'replay.exe';c.write_text(WRAPPER)
        subprocess.run(['gcc','-std=c99','-O2','-I'+str(HERE/'source'),str(c),'-o',str(exe)],check=True)
        for name in NAMES:
            data=args.data/(name+'.mat');truth=args.data/(name+'_BPMtrace.mat')
            manifest['files'][name]={'signal_sha256':digest(data),'reference_sha256':digest(truth)}
            events,clipping=prepare(loadmat(data)['sig'],args.ppg_gain,args.acc_gain)
            manifest['files'][name]['clipping']=clipping
            labels=loadmat(truth)['BPM0']
            for mode in ('acc_assisted','ppg_only'):
                supplied=events if mode=='acc_assisted' else [e for e in events if e[0]==0]
                _,rows=run(exe,supplied,acc=mode=='acc_assisted')
                scored=score(rows,labels,name,mode)
                all_rows.extend(scored)
                print(name,mode,json.dumps(metrics(scored)),flush=True)
    assert digest(HERE/'source/ppg_motion_hr.h')==manifest['algorithm_sha256'],'algorithm changed during benchmark'
    with (args.out/'windows.csv').open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(all_rows[0]));w.writeheader();w.writerows(all_rows)
    summary={mode:{phase:metrics([r for r in all_rows if r['mode']==mode and (phase=='all' or r['phase']==phase)])
                   for phase in ('all','rest_protocol','run_protocol')} for mode in ('acc_assisted','ppg_only')}
    summary['subjects']={name:{mode:metrics([r for r in all_rows if r['record']==name and r['mode']==mode])
                              for mode in ('acc_assisted','ppg_only')} for name in NAMES}
    (args.out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    (args.out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('TOTAL',json.dumps({k:v for k,v in summary.items() if k!='subjects'},indent=2))


if __name__=='__main__': main()
