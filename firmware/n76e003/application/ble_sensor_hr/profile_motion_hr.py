"""SDCC + s51 instruction profile and host/8051 parity, no hardware claims.
Run: python profile_motion_hr.py --out NEW_DIRECTORY
Requires the installed Nuvoton SDCC toolchain, gcc and Python stdlib.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

from test_motion_hr import HERE, WRAPPER, run, synthetic


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--sdcc',type=Path,default=Path('C:/Program Files (x86)/Nuvoton Tools/Toolchain/SDCC'))
    args=p.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    env=dict(os.environ,SDCC_HOME=str(args.sdcc));env['PATH']=str(args.sdcc/'bin')+os.pathsep+env['PATH']
    host=args.out/'host.c';host.write_text(WRAPPER)
    exe=args.out/'host.exe'
    subprocess.run(['gcc','-std=c99','-O2','-I'+str(HERE/'source'),str(host),'-o',str(exe)],check=True)
    reports={}
    for scenario in ('clean','one_channel','both_channels','same_frequency','spikes','flat'):
        bpm=120 if scenario=='same_frequency' else 72
        events=synthetic(bpm,120 if scenario=='same_frequency' else 126,scenario,seconds=12)
        _,reference=run(exe,events)
        c=args.out/f'{scenario}.c'
        c.write_text('''#include "ppg_motion_hr.h"
typedef struct { uint16_t t,a,b,c; uint8_t kind; } event;
__xdata motion_hr h;
__xdata volatile uint8_t results[4];
__xdata volatile uint8_t state_size;
__xdata uint16_t i;
__xdata uint8_t n;
__code const event events[]={
'''+',\n'.join('{%d,%d,%d,%d,%d}'%(e[1],e[2],e[3],e[4],e[0]) for e in events)+'''};
void bench_start(void) { __asm nop __endasm; }
void before_feed(void) { __asm nop __endasm; }
void after_feed(void) { __asm nop __endasm; }
void bench_done(void) { for(;;); }
void main(void) {
    mh_reset(&h,1); state_size=sizeof(h); n=0;
    bench_start();
    for(i=0;i<sizeof(events)/sizeof(events[0]);i++) {
        before_feed();
        if(events[i].kind) mh_acc(&h,(int16_t)events[i].a,(int16_t)events[i].b,(int16_t)events[i].c,events[i].t);
        else if(mh_ppg(&h,events[i].a,events[i].b,events[i].t,1)) results[n++]=h.bpm;
        after_feed();
    }
    bench_done();
}
''')
        firmware=args.out/f'{scenario}.ihx'
        subprocess.run([str(args.sdcc/'bin/sdcc.exe'),'--model-large','-D__SDCC__',
                        '-I'+str(args.sdcc/'include'),'-I'+str(HERE/'source'),'-L'+str(args.sdcc/'lib/large'),
                        '--xram-size','768','--code-size','18432',str(c),'-o',str(firmware)],check=True,env=env)
        mapping=firmware.with_suffix('.map').read_text()
        def address(symbol):
            return int(re.search(r'\b([0-9A-Fa-f]{8})\s+_'+symbol+r'\s',mapping).group(1),16)
        # Exact dynamic instruction counts; simulator clocks are classic 8051,
        # NOT N76E003's 1T timing. Include event dispatch/array load overhead.
        commands=[f'break 0x{address("bench_start"):x}','run','state',
                  f'clear 0x{address("bench_start"):x}',f'break 0x{address("bench_done"):x}','run','state',
                  f'dx 0x{address("results"):x} 0x{address("results")+3:x}',
                  f'dx 0x{address("state_size"):x} 0x{address("state_size"):x}','quit']
        sim=subprocess.run([str(args.sdcc/'bin/s51.exe'),'-b','-c','-',str(firmware)],input='\n'.join(commands)+'\n',text=True,capture_output=True,check=True,timeout=60)
        (args.out/f'{scenario}_sim.txt').write_text(sim.stdout)
        counts=[int(s) for s in re.findall(r'Inst= (\d+)',sim.stdout)]
        assert len(counts)==2,sim.stdout[-2000:]
        dump=re.findall(r'^0x%04x\s+((?:[0-9a-fA-F]{2} ?)+)'%address('results'),sim.stdout,re.M)
        assert dump,sim.stdout[-2000:]
        actual=[int(v,16) for v in dump[-1].split()][:len(reference)]
        expected=[int(r['bpm']) for r in reference]
        assert actual==expected,(scenario,actual,expected)
        size_dump=re.findall(r'^0x%04x\s+([0-9a-fA-F]{2})'%address('state_size'),sim.stdout,re.M)
        assert size_dump,sim.stdout[-2000:]
        report={'instructions_12s':counts[1]-counts[0],'equivalent_MIPS':round((counts[1]-counts[0])/12e6,4),
                '8051_results':actual,'host_results':expected,'events':len(events),
                'state_bytes_sdcc':int(size_dump[-1],16),
                'sim_max_stack_address':max(int(v,16) for v in re.findall(r'Max value of stack pointer= 0x([0-9a-f]+)',sim.stdout))}
        if scenario=='clean':
            commands=[f'break 0x{address("before_feed"):x}',f'break 0x{address("after_feed"):x}']
            for _ in events: commands.extend(['run','state','run','state'])
            commands.append('quit')
            detail=subprocess.run([str(args.sdcc/'bin/s51.exe'),'-b','-c','-',str(firmware)],input='\n'.join(commands)+'\n',text=True,capture_output=True,check=True,timeout=60)
            inst=[int(v) for v in re.findall(r'Inst= (\d+)',detail.stdout)]
            assert len(inst)==2*len(events),len(inst)
            costs=[inst[2*i+1]-inst[2*i] for i in range(len(events))]
            report['max_ppg_call_instructions']=max(c for c,e in zip(costs,events) if e[0]==0)
            report['max_acc_call_instructions']=max(c for c,e in zip(costs,events) if e[0]==1)
        reports[scenario]=report
    (args.out/'profile.json').write_text(json.dumps(reports,indent=2)+'\n')
    print(json.dumps(reports,indent=2))


if __name__=='__main__':main()
