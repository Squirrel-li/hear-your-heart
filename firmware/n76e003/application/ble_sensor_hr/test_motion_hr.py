"""Compile the real C header on host, replay recordings/synthetic motion.
Run with Python (stdlib only), gcc on PATH. --out writes a new results directory.
"""
import argparse
import csv
import json
import math
import random
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
WRAPPER = r'''
#include <stdio.h>
#include "ppg_motion_hr.h"
#include "ppg_hr.h"
int main(int argc,char **argv) {
    motion_hr h; hr_t old; unsigned k,t,a,b,c,st; uint8_t old_bpm=0,old_fresh;
    (void)argv; mh_reset(&h, argc>1); hr_reset(&old);
    printf("#state_bytes,%u\n",(unsigned)sizeof(h));
    puts("time_ms,bpm,raw_bpm,quality,reason,ir_bpm,red_bpm,old_bpm,old_fresh");
    while(scanf("%u %u %u %u %u %u",&k,&t,&a,&b,&c,&st)==6) {
        if(k) mh_acc(&h,(int16_t)a,(int16_t)b,(int16_t)c,(uint16_t)t);
        else {
            old_fresh=hr_feed(&old,(uint16_t)a,(uint16_t)b,(uint16_t)t,&old_bpm);
            if(mh_ppg(&h,(uint16_t)a,(uint16_t)b,(uint16_t)t,(uint8_t)st))
                printf("%u,%u,%u,%u,%u,%u,%u,%u,%u\n",t,h.bpm,h.raw_bpm,h.quality,h.reason,h.channel_bpm[0],h.channel_bpm[1],old_bpm,old_fresh);
        }
    }
    return 0;
}
'''


def synthetic(bpm=72, motion=126, scenario='clean', seconds=60, seed=8735):
    rng = random.Random(seed)
    events = []
    for i in range(round(seconds / .028)):
        t = round(i * 28)
        s = t / 1000
        pulse = 800 * math.sin(2 * math.pi * bpm / 60 * s)
        movement = 1800 * math.sin(2 * math.pi * motion / 60 * s)
        ir, red = pulse, .8 * pulse
        if scenario == 'one_channel': ir += movement
        if scenario == 'both_channels': ir += movement; red += movement
        if scenario == 'same_frequency': ir += movement; red += movement
        if scenario == 'flat': ir = red = 0
        if scenario == 'spikes' and i % 71 == 0: ir += 6000
        if scenario == 'drift':
            ir += 1500 * math.sin(2 * math.pi * .08 * s)
            red += 1000 * math.sin(2 * math.pi * .08 * s)
        if scenario == 'gain_step' and s >= 25: ir *= 2; red *= 2
        noise = 0 if scenario == 'flat' else 15
        a = max(0,min(65535,round(12000 + ir + rng.gauss(0,noise))))
        b = max(0,min(65535,round(15000 + red + rng.gauss(0,noise))))
        if scenario == 'gap' and 25 <= s < 26: continue
        events.append((0,t,a,b,0,1))
    for i in range(round(seconds * 26)):
        t = round(i * 1000 / 26)
        s = t / 1000
        moving = scenario in ('one_channel','both_channels','same_frequency')
        a = round(2500 * math.sin(2 * math.pi * motion / 60 * s)) if moving else 0
        events.append((1,t,a & 65535,0,4098,0))
    return sorted(events,key=lambda r:(r[1],-r[0]))


def run(exe, events, acc=True):
    result = subprocess.run([str(exe)]+(['acc'] if acc else []),input=''.join(' '.join(map(str,e))+'\n' for e in events),text=True,capture_output=True,check=True)
    lines = result.stdout.splitlines()
    return int(lines[0].split(',')[1]), list(csv.DictReader(lines[1:]))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path)
    args=parser.parse_args()
    summary={}
    with tempfile.TemporaryDirectory() as tmp:
        c, exe=Path(tmp)/'replay.c',Path(tmp)/'replay.exe'
        c.write_text(WRAPPER)
        subprocess.run(['gcc','-std=c99','-O2','-Wall','-Wextra','-Werror','-I'+str(HERE/'source'),str(c),'-o',str(exe)],check=True)
        # Exercise the actual firmware handoff, including immediate invalidation.
        source=(HERE/'../ble_sensor/source/main.c').read_text(encoding='utf-8')
        start=source.index('static void motion_task(void)')
        function=source[start:source.index('\n#endif',start)]
        integration=Path(tmp)/'handoff.c'
        integration.write_text('''#include <assert.h>
#include "ppg_motion_hr.h"
typedef uint8_t UINT8; typedef uint16_t UINT16;
#define PKT_LEN 20
motion_hr motion;
UINT8 tx_pkt[20],motion_pkt[20],ppg_ready,motion_ready,motion_has,motion_seen,motion_sequence;
'''+function+'''
int main(void) {
    mh_reset(&motion,1);motion.bpm=72;
    tx_pkt[9]=0;tx_pkt[16]=0x10;tx_pkt[18]=0x10;tx_pkt[11]=150;
    ppg_ready=1;motion_task();
    assert(!ppg_ready && motion_ready && motion_has && motion_pkt[11]==0 && motion_pkt[9]==0);
    tx_pkt[9]=1;tx_pkt[3]=28;tx_pkt[8]=1;motion_ready=0;ppg_ready=1;motion_task();
    assert(motion_pkt[11]==0 && motion_pkt[9]==1 && !motion_ready);
    motion.warmed=1;motion.bpm=72;tx_pkt[3]=56;tx_pkt[8]=3;ppg_ready=1;motion_task();
    assert(!motion.warmed && motion_ready && motion_pkt[11]==0);
    return 0;
}
''')
        check_exe=Path(tmp)/'handoff.exe'
        subprocess.run(['gcc','-std=c99','-O2','-I'+str(HERE/'source'),str(integration),'-o',str(check_exe)],check=True)
        subprocess.run([str(check_exe)],check=True)
        outputs={}
        for scenario,bpm,motion in [('clean',45,126),('clean',72,126),('clean',120,126),('clean',180,126),
                                    ('one_channel',72,126),('both_channels',72,126),('same_frequency',120,120),
                                    ('flat',72,126),('spikes',72,126),('drift',72,126),('gain_step',72,126),('gap',72,126)]:
            name=f'{scenario}_{bpm}'
            size, rows=run(exe,synthetic(bpm,motion,scenario))
            valid=[int(r['bpm']) for r in rows if int(r['time_ms'])>=10000 and int(r['bpm'])]
            summary[name]={'state_bytes_host':size,'accepted':len(valid),'windows':len(rows),'mae_accepted':round(sum(abs(b-bpm) for b in valid)/len(valid),2) if valid else None,'bpm':valid}
            baseline=[int(r['old_bpm']) for r in rows if int(r['old_fresh']) and int(r['old_bpm'])]
            summary[name]['old_fresh_accepted']=len(baseline)
            summary[name]['old_mae_accepted']=round(sum(abs(b-bpm) for b in baseline)/len(baseline),2) if baseline else None
            if scenario=='clean': assert len(valid)>=9 and max(abs(b-bpm) for b in valid)<=3,(name,valid)
            if scenario=='one_channel': assert len(valid)>=8 and max(abs(b-bpm) for b in valid)<=5,(name,valid)
            if scenario in ('both_channels','same_frequency','flat'): assert not valid,(name,valid)
            if scenario in ('spikes','drift','gain_step','gap'): assert len(valid)>=7 and max(abs(b-bpm) for b in valid)<=5,(name,valid)
            outputs[name]=rows
        # More than one hand-picked phase/noise realization, including clock wrap.
        for seed in (7,41,119):
            for bpm,motion in ((60,108),(90,138),(150,108)):
                events=synthetic(bpm,motion,'one_channel',seed=seed)
                _,rows=run(exe,events)
                accepted=[int(r['bpm']) for r in rows if int(r['bpm'])]
                assert all(abs(b-bpm)<=8 for b in accepted),(seed,bpm,motion,accepted)
        events=synthetic(seconds=20)
        _,normal=run(exe,events)
        _,wrapped=run(exe,[(k,t+60000,a,b,c,st) for k,t,a,b,c,st in events])
        assert [r['bpm'] for r in normal]==[r['bpm'] for r in wrapped]
        _,no_acc=run(exe,[e for e in events if e[0]==0])
        assert all(int(r['bpm'])==0 for r in no_acc)
        _,no_contact=run(exe,[(k,t,a,b,c,0) for k,t,a,b,c,st in events])
        assert not no_contact
        _,extremes=run(exe,[(k,t,65535 if i%2 else 0,0 if i%2 else 65535,c,st) if k==0 else (k,t,32768,32767,32768,st) for i,(k,t,a,b,c,st) in enumerate(events)])
        assert all(int(r['bpm'])==0 for r in extremes)
        fixture=ROOT/'firmware/hub8735-ultra/examples/uart/bmh08002_4_control'
        for name in ('hr1','hr2'):
            with (fixture/f'{name}.csv').open() as f: raw=list(csv.DictReader(f))
            # Legacy timestamps are host-quantized. Uniformize within gap-free runs.
            segments=[[]]
            for row in raw:
                if segments[-1] and (float(row['elapsed_s'])-float(segments[-1][-1]['elapsed_s'])>.15 or float(row['elapsed_s'])<float(segments[-1][-1]['elapsed_s'])): segments.append([])
                segments[-1].append(row)
            events=[]
            for seg in segments:
                if len(seg)<2: continue
                start=float(seg[0]['elapsed_s']);dt=(float(seg[-1]['elapsed_s'])-start)/(len(seg)-1)
                events.extend((0,round((start+i*dt)*1000),int(r['ir_ppg']),int(r['red_ppg']),0,int(r['status'])) for i,r in enumerate(seg))
            _,rows=run(exe,events,False)
            outputs[name]=rows
            summary[name]={'accepted':sum(int(r['bpm'])>0 for r in rows),'windows':len(rows),'bpm':[int(r['bpm']) for r in rows],'ground_truth':False}
        print(json.dumps(summary,indent=2))
        if args.out:
            args.out.mkdir(parents=True,exist_ok=False)
            (args.out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
            for name,rows in outputs.items():
                if rows:
                    with (args.out/f'{name}.csv').open('w',newline='') as f:
                        writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
    return summary


if __name__=='__main__': main()
