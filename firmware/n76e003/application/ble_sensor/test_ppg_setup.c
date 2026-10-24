/* PC check of source/ppg_setup.h against a fake BMH08002-4 on UART0 (streaming 55 B0 frames in between).
 *   gcc -Wall -o test_ppg_setup test_ppg_setup.c && ./test_ppg_setup */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
typedef uint8_t UINT8; typedef uint16_t UINT16;
#define __code const
#define __bit int
#define PPG_MODE_VALUE 0x04
#define clr_ES
#define set_ES

static UINT16 clock_ms, eeprom[16];
static int silent, writes, order[8];
static UINT8 rx_byte, tx_byte, tx_pending, ti = 1, ri, tx[8], ntx, rxq[4096];
static int nrx, prx;
static UINT16 now_ms(void) { return clock_ms++; }
static void queue(const UINT8 *b, int n) { while (n--) rxq[nrx++ % 4096] = *b++; }
static void module_rx(UINT8 c)              /* a complete 8-byte command -> act like the module */
{
    static const UINT8 noise[15] = { 0x55, 0xB0, 2, 98, 70, 1, 2, 3, 4, 5, 6, 7, 8, 0x12, 0xAA };
    tx[ntx++] = c;
    if (ntx < 8) return;
    ntx = 0;
    assert(tx[0] == 0x55 && tx[1] == 0xB1 && tx[7] == 0xAA && tx[6] == (UINT8)(0xB1 + tx[2] + tx[3] + tx[4] + tx[5]));
    if (silent) return;
    queue(noise, 15);                       /* streaming frame before the reply */
    if (tx[2] == 0x04) { eeprom[tx[3]] = (tx[4] << 8) | tx[5]; order[writes++] = tx[3]; }
    if (tx[2] == 0x05)
    {
        UINT16 v = eeprom[tx[3]];
        UINT8 r[8] = { 0x55, 0xB1, 0x05, tx[3], v >> 8, v, 0, 0xAA };
        r[6] = 0xB1 + 0x05 + tx[3] + r[4] + r[5];
        queue(r, 8);
    }
}
static UINT8 *ti_ref(void) { if (tx_pending) { module_rx(tx_byte); tx_pending = 0; ti = 1; } return &ti; }
static UINT8 *ri_ref(void) { if (!ri && prx < nrx) { rx_byte = rxq[prx++ % 4096]; ri = 1; } return &ri; }
static UINT8 *sbuf_ref(void) { if (ri) return &rx_byte; tx_pending = 1; return &tx_byte; }   /* read only after RI */
#define TI (*ti_ref())
#define RI (*ri_ref())
#define SBUF (*sbuf_ref())
#include "source/ppg_setup.h"

int main(void)
{
    eeprom[0x01] = 0x00; eeprom[0x02] = 100; eeprom[0x04] = 200; eeprom[0x06] = 30000; eeprom[0x08] = 17550;
    ppg_start();
    assert(ppg_mode_ch == 'W' && writes == 5 && eeprom[0x01] == 0x04);
    assert(eeprom[0x02] == PPG_IR_MIN && eeprom[0x04] == PPG_IR_MAX && eeprom[0x06] == PPG_RED_MIN && eeprom[0x08] == PPG_RED_MAX);
    assert(order[1] == 0x04 && order[2] == 0x02 && order[3] == 0x08 && order[4] == 0x06);   /* max before min */

    writes = 0;
    ppg_start();                                                  /* already right: read only */
    assert(ppg_mode_ch == '=' && writes == 0);

    eeprom[0x06] = 18000;
    ppg_start();                                                  /* only the wrong one is written */
    assert(ppg_mode_ch == 'W' && writes == 1 && eeprom[0x06] == PPG_RED_MIN);

    silent = 1; writes = 0;
    ppg_start();                                                  /* no answer: still writes, does not hang */
    assert(ppg_mode_ch == 'W');
    puts("ppg_setup checks passed");
    return 0;
}
