"""Host checks of the ACTUAL ISR/handoff/ring code. Needs gcc on PATH.
python firmware/n76e003/application/ble_sensor_capture/test_capture.py
"""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).parent / '../ble_sensor/source/main.c').read_text(encoding='utf-8')
ring = source[source.index('__xdata UINT8 capture_ppg'):source.index('\n#endif', source.index('__xdata UINT8 capture_ppg'))]
isr = source[source.index('void SerialPort0_ISR'):source.index('\n/* TI is not touched') if '\n/* TI is not touched' in source else source.index('\nstatic void UART0_Init')]
isr = re.sub(r'__interrupt\s*\(4\)', '', isr)
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t UINT8;
#define __xdata
#define __bit uint8_t
#define PKT_LEN 20
#define PKT_ST 9
#define PKT_TS 0
#define PKT_SEQ 5
UINT8 tx_pkt[20]={0,0,0,0,0x11}, ppg_ready,ppg_cap,ppg_i,ppg_sum,ppg_seq[4];
UINT8 RI,SBUF;
uint16_t g_ms,g_ms_hi;
unsigned sent;
UINT8 last[20];
void bm_notify_send(UINT8 *p, UINT8 n){assert(n==20);memcpy(last,p,n);sent++;}
'''
checks = r'''
void frame(unsigned ms, int valid){
  UINT8 b[15]={0x55,0xb0,1,98,72,0,0,0,0,0x12,0x34,0x56,0x78,0,0xaa};
  int i;g_ms=ms;for(i=1;i<13;i++)b[13]+=b[i];if(!valid)b[13]++;
  for(i=0;i<15;i++){SBUF=b[i];RI=1;SerialPort0_ISR();}
}
int main(void){
  int i;
  frame(28,0);assert(!ppg_ready && ppg_seq[3]==0);
  for(i=0;i<4;i++){frame(56+28*i,1);capture_collect();}
  assert(capture_count==4 && !ppg_ready);
  frame(168,1);capture_collect();assert(capture_count==4 && !ppg_ready);
  for(i=0;i<4;i++){capture_send();assert(last[8]==i && last[16]==0x12 && last[19]==0x78);}
  frame(196,1);capture_collect();capture_send();assert(last[8]==5); /* overflow gap visible */
  frame(224,1);frame(252,1); /* busy ISR slot: second valid sample still counts */
  capture_collect();capture_send();assert(last[8]==6);
  frame(280,1);capture_collect();capture_send();assert(last[8]==8);
  assert(sent==7 && capture_count==0);return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    c, exe = Path(directory)/'capture.c', Path(directory)/'capture.exe'
    c.write_text(prefix + ring + isr + checks)
    subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Actual capture ISR/ring checks passed: checksum, ordering, overflow, busy slot, sequence gaps.')
