"""Host check of the ACTUAL ISR / raw_send code for the 0x15 raw PPG batch. Needs gcc on PATH.
python firmware/n76e003/application/ble_sensor_hr/test_raw_batch.py
"""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).parent / 'source/main.c').read_text(encoding='utf-8')
raw = source[source.index('#define SENSOR_PPG_RAW'):source.index('/* The UART0 ISR writes tx_pkt')]
isr = source[source.index('void SerialPort0_ISR'):source.index('\nstatic void UART0_Init')]
isr = re.sub(r'__interrupt\s*\(4\)', '', isr)
send = source[source.index('static void raw_send'):source.index('static void ppg_send')]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t UINT8;
typedef uint16_t UINT16;
#define __xdata
#define __bit uint8_t
#define SENSOR_SRC 0x10
#define PKT_ST 9
#define PKT_TS 0
#define PKT_SEQ 5
UINT8 tx_pkt[20], ppg_ready, ppg_cap, hr_raw[4], hr_new, hr_cap, hr_bpm, ppg_i, ppg_sum, ppg_seq[4];
UINT16 hr_end_t;
UINT8 RI, SBUF;
uint16_t g_ms, g_ms_hi;
unsigned sent;
UINT8 last[20], last_len;
void bm_notify_send(UINT8 *p, UINT8 n){memcpy(last, p, n); last_len = n; sent++;}
'''
checks = r'''
void frame(unsigned ms, UINT8 st, UINT8 v, int valid){
  UINT8 b[15]={0x55,0xb0,0,98,72,0,0,0,0,0,0,0,0,0,0xaa};
  int i; b[2]=st; b[9]=v; b[10]=v+1; b[11]=v+2; b[12]=v+3;
  g_ms=ms; for(i=1;i<13;i++) b[13]+=b[i]; if(!valid) b[13]++;
  for(i=0;i<15;i++){SBUF=b[i]; RI=1; SerialPort0_ISR();}
  hr_new=0; ppg_ready=0;   /* main consumed the other hand-overs */
}
int main(void){
  frame(28,1,0x10,1); assert(raw_n==1);
  frame(56,1,0xEE,0); assert(raw_n==1);            /* bad checksum: slot 1 not finished */
  frame(84,2,0x20,1); assert(raw_n==RAW_N);        /* overwrites the failed frame's bytes */
  frame(112,2,0x30,1); assert(raw_n==RAW_N);       /* batch full: sample dropped, sequence still counts */
  raw_send();
  assert(sent==1 && last_len==18 && raw_n==0);
  assert(last[4]==0x15 && last[8]==0 && last[9]==2 && last[3]==84);       /* first seq, last status / time */
  assert(last[10]==0x10 && last[13]==0x13 && last[14]==0x20 && last[17]==0x23);
  frame(140,0,0x40,1); frame(168,0,0x50,1); raw_send();
  assert(last[8]==3 && last[9]==0 && last[3]==168 && last[10]==0x40);    /* gap 2 -> 3 visible */
  return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    c, exe = Path(directory) / 'raw.c', Path(directory) / 'raw.exe'
    c.write_text(prefix + raw + isr + send + checks)
    subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Actual 0x15 ISR/raw_send checks passed: batching, checksum retry, full-batch drop, sequence gap.')
