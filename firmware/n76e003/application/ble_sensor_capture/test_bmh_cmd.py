"""Host check of the ACTUAL bmh_passthrough() (FFF2 opcode 0x02). Needs gcc on PATH.
python firmware/n76e003/application/ble_sensor_capture/test_bmh_cmd.py
"""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).parent / '../ble_sensor/source/main.c').read_text(encoding='utf-8')
start = source.index('#define CMD_BMH')
code = source[start:source.index('#endif', source.index('static void bmh_passthrough'))]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t UINT8;
typedef uint16_t UINT16;
#define __xdata
#define SENSOR_SRC 0x10
#define clr_ES
#define set_ES
/* UART0 model: RI turns 1 while module bytes are queued, SBUF then holds the byte; TX is not checked */
static UINT8 q[64], qlen, qpos, ri, ti, sbuf, ppg_i;
static UINT16 clock_ms;
static UINT8 *ri_ptr(void){ if (!ri && qpos < qlen) { ri = 1; sbuf = q[qpos++]; } return &ri; }
static UINT8 *ti_ptr(void){ ti = 1; return &ti; }
#define RI (*ri_ptr())
#define TI (*ti_ptr())
#define SBUF sbuf
static UINT16 now_ms(void){ return clock_ms++; }
'''
checks = r'''
static void feed(const UINT8 *b, UINT8 n){ memcpy(q, b, n); qlen = n; qpos = 0; ri = 0; clock_ms = 0; }
int main(void){
  UINT8 req[6] = { 0x02, 7, 0x05, 0x01, 0, 0 };
  /* a streaming 55 B0 frame first, then the reply 55 B1 05 01 00 04 CS AA */
  UINT8 rx[] = { 0x55,0xB0,1,2,3,4,5,6,7,8,9,10,11,12,0xAA, 0x55,0xB1,0x05,0x01,0x00,0x04,(UINT8)(0xB1+5+1+4),0xAA };
  feed(rx, sizeof rx); ppg_i = 9;
  bmh_passthrough(req, 6);
  assert(cmd_rsp_len == 12 && cmd_rsp[4] == 0x10);
  assert(cmd_rsp[5] == 0x82 && cmd_rsp[6] == 7 && cmd_rsp[7] == 0);
  assert(cmd_rsp[8] == 0x05 && cmd_rsp[9] == 0x01 && cmd_rsp[10] == 0 && cmd_rsp[11] == 4 && ppg_i == 0);
  rx[21]++; feed(rx, sizeof rx);                         /* bad checksum -> no reply */
  bmh_passthrough(req, 6); assert(cmd_rsp_len == 8 && cmd_rsp[7] == 0x03);
  feed(rx, 0); bmh_passthrough(req, 6); assert(cmd_rsp[7] == 0x03);   /* silent module */
  req[2] = 0x03; bmh_passthrough(req, 6); assert(cmd_rsp_len == 8 && cmd_rsp[7] == 0x02);  /* info op not allowed */
  req[2] = 0x05; bmh_passthrough(req, 5); assert(cmd_rsp[7] == 0x02);                     /* short request */
  req[0] = 0x09; bmh_passthrough(req, 6); assert(cmd_rsp[5] == 0x89 && cmd_rsp[7] == 0x01);  /* unknown opcode */
  return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    c, exe = Path(directory) / 'cmd.c', Path(directory) / 'cmd.exe'
    c.write_text(prefix + code + checks)
    subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Actual bmh_passthrough checks passed: reply among streaming frames, checksum, timeout, op/length/opcode errors.')
