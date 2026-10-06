/*---------------------------------------------------------------------------------------------------------*/
/*  ble_sensor : N76E003 UART1  <->  BM7701-00-1 (BLE UART module, BC7701)                                 */
/*                                                                                                         */
/*  Wiring (same as application\UART1 test)                                                               */
/*    N76E003 P1.6 TXD_1  ->  BM7701 pin 22 UART1_RX                                                       */
/*    N76E003 P0.2 RXD_1  <-  BM7701 pin 23 UART1_TX                                                       */
/*    N76E003 P1.3        ->  BM7701 pin 6  RST_N  (active low)                                            */
/*    3.3 V, common GND                                                                                    */
/*                                                                                                         */
/*  PPG  BMH08002-4 / BMH83M002 (oximeter module) on UART0, 38400 8N1                                      */
/*    N76E003 P0.6 TXD    ->  PPG RX                                                                       */
/*    N76E003 P0.7 RXD    <-  PPG TX                                                                       */
/*    PPG EN pin must be HIGH (module enabled); not driven by this firmware                                */
/*    module frame (15 B): 55 B0 ST D0..D9 CS AA     CS = sum(B0, ST, D0..D9) & 0xFF                       */
/*    ST/D0..D9 map 1:1 to PpgSensorElement status..ppg2Low (ble-notify-data-format.md section 4)          */
/*                                                                                                         */
/*  Timing (BLE API User's Guide V1.00)                                                                    */
/*    - RST_N low > 5 ms for a full hardware reset                      (1.2 BLE States)                   */
/*    - after RST_N 0->1 the BC7701 needs 60 ms; during that time it samples its UART TX/RX levels.        */
/*      Our TX (P1.6) must stay HIGH and nothing may be sent -> 115200 mode0 (default)                     */
/*    - AdvCtrl 0->1: 2 ms reset + 600~800 ms RF calibration, no response meanwhile   (4.7 API_AdvCtrl)    */
/*    - after a disconnection the BC7701 soft-resets for 5 ms, no command in that time (1.2 BLE States)    */
/*                                                                                                         */
/*  Flow                                                                                                   */
/*    1. RST_N low RESET_LOW_MS, release, wait BOOT_WAIT_MS                                                */
/*    2. Feature OR autoSendStatus (RAM only)  77 07 10 10 00 80 00 00 00  -> 78 03 00 10 00              */
/*    3. Advertise ON                          77 04 00 07 00 01           -> 78 03 00 07 00 (<= ~1 s)     */
/*    4. Wait for connection                                                                               */
/*         status event / query reply          78 07 00 00 00 S0 S1 S2 S3  (S0 bit0 = 1 connected)        */
/*         fallback poll every 1 s             77 03 00 00 00                                              */
/*    5. Connected: wait for peer to enable FFF1 notify (the phone/PC writes the CCCD)                     */
/*         probe every 500 ms                  77 LEN 20 F1 FF DATA                                        */
/*              78 03 20 F1 FF  -> notify ON                                                               */
/*              78 03 26 F1 FF  -> not subscribed yet (_ST_NOT_ENABLE_)                                    */
/*         optional app handshake: peer writes 0x01 to FFF2 -> 78 04 00 F2 FF 01                           */
/*         the probe is a real PPG frame (only sent when a new PPG sample is ready)                        */
/*    6. Notify ON: one 22-byte frame per PPG sample (ble-notify-data-format.md, all Big-endian)           */
/*         [0..1] length = 20   [2..5] sensor_time_ms          [6] msgType = 0x01                          */
/*         [7..10] PPG sequence  [11] status  [12..21] SpO2 HR bpi hrv amp systole ppg1H ppg1L ppg2H ppg2L */
/*         needs the enlarged MTU (> 20 B notify payload), no fragmentation                                */
/*         a new sample is sent after the previous one is ACKed; samples arriving meanwhile are dropped   */
/*         (the hub sees the gap in sequence)                                                              */
/*         error ACK -> peer unsubscribed -> back to step 5                                                */
/*    7. Disconnect (S0 bit0 = 0) -> wait DISC_WAIT_MS -> step 2                                           */
/*                                                                                                         */
/*  FFF1 send uses the official V1.0.3 sendData format (77 LEN 20 F1 FF DATA), NOT the older HT32          */
/*  writeData() which inserts an extra 0xB1 byte before the data.                                          */
/*  A UART ACK only means the BM7701 accepted the data, not that the peer received it.                     */
/*                                                                                                         */
/*  !!! UART1 pins are shared with the Nu-Link ICP pins; unplug Nu-Link / do not run in debug mode.        */
/*  !!! Baud: 16 MHz HIRC + Timer3 can only give 111.1 kbps for "115200" (-3.5 %) -> RX from BM7701 fails. */
/*      USE_HIRC_166 = 1 trims HIRC to 16.6 MHz (BSP IAP_MoidfyHIRC) -> 115278 bps (+0.07 %).             */
/*      The trim is applied on power-on reset only (as in the BSP): power-cycle the board after flashing.  */
/*---------------------------------------------------------------------------------------------------------*/
/*  Code size: Keil C51 Eval is limited to 2 KB, so Common.c / Delay.c are NOT in the project.          */
/*  UART1 is initialised here with a compile-time Timer3 reload (no 32-bit division library).          */
/*---------------------------------------------------------------------------------------------------------*/
#include "N76E003.h"
#include "Common.h"                         /* UINT8/UINT16 typedefs only */
#include "SFR_Macro.h"
#include "Function_define.h"

/*============================== configuration ==============================*/
#define BM_BAUD             115200
#define USE_HIRC_166        1       /* 1 = Fsys 16.6 MHz (accurate 115200), 0 = 16 MHz */
#define RESET_LOW_MS        100     /* RST_N low time, spec > 5 ms (UART1 test uses 100 ms) */
#define BOOT_WAIT_MS        100     /* spec: 60 ms after RST_N 0->1                          */
#define CMD_TIMEOUT_MS      300     /* normal ACK timeout (HT32 driver waits 150 ms)         */
#define ADV_TIMEOUT_MS      1500    /* AdvCtrl 0->1: RF calibration 600~800 ms               */
#define DISC_WAIT_MS        20      /* spec: 5 ms soft reset after disconnection             */
#define STATUS_POLL_MS      1000    /* poll connection status while waiting                  */
#define NOTIFY_PROBE_MS     500     /* try FFF1 send while waiting for subscription          */
#define ADV_RETRY_MS        500
#define FRAME_GAP_MS        20      /* drop an incomplete frame after this gap               */

/* PPG on UART0: Timer1 mode 2, SMOD = 1, T1M = 1 -> baud = Fsys / (16 * (256 - TH1)) */
/* 16.6 MHz: 256 - 27 -> 38426 bps (+0.07 %)    16.0 MHz: 256 - 26 -> 38462 bps (+0.16 %) */
#if USE_HIRC_166
#define PPG_TH1             (256 - 27)
#else
#define PPG_TH1             (256 - 26)
#endif
#define PPG_SEND_START      1       /* 1 = send "start measure" (55 B1 01 00 00 00 B2 AA) at boot */
                                    /* The module must be in timed / continuous upload mode       */
                                    /* (EEPROM 0x01 mode 0x00/01/04/05/06/07). In ask-response     */
                                    /* mode (0x02/03) it does not send 55 B0 frames by itself.     */

/* BM7701 reset pin */
#define BM_RST              P13
#define BM_RST_INIT()       P13_PushPull_Mode

/* Status LED on P1.2 (1 = on). Blink rate shows the state:                */
/*   init, nothing received on P0.2 yet       10 Hz   (50 ms on / 50 ms off)    */
/*   init, bytes received but no valid frame  ON      (baud / wiring problem)   */
/*   init, valid frames received (normal)     5 Hz    (100 ms on / 100 ms off)  */
/*   advertising, waiting for connection      0.5 Hz  (1 s on / 1 s off)        */
/*   connected, waiting for FFF1 subscribe    2 Hz    (250 ms on / 250 ms off)  */
/*   streaming                                one 50 ms flash per packet ACKed  */
#define USE_LED             1
#if USE_LED
#define LED                 P12
#define LED_INIT()          P12_PushPull_Mode
#define LED_INIT_MS         50      /* half period */
#define LED_INIT_OK_MS      100
#define LED_ADV_MS          1000
#define LED_CONN_MS         250
#define LED_PULSE_MS        50
#endif

/*============================== BM7701 protocol ============================*/
#define BM_CMD              0x77
#define BM_EVT              0x78
#define BM_TYPE_STATUS      0x0000
#define BM_TYPE_ADV_EN      0x0007
#define BM_TYPE_FEATURE     0x0010
#define BM_TYPE_FFF1        0xFFF1      /* notify: MCU -> peer        */
#define BM_TYPE_FFF2        0xFFF2      /* write w/o rsp: peer -> MCU */
#define BM_FLAG_NONE        0x00
#define BM_FLAG_FEATURE_OR  0x10        /* CmdType3 orAction          */
#define BM_FLAG_NOTIFY      0x20        /* CmdType2 notifyIndicate    */
#define BM_FEATURE_AUTO_STATUS  0x80    /* codeFeature b[7], little-endian byte 0 */

/*============================== state ======================================*/
#define ST_RESET            0
#define ST_BOOT             1
#define ST_FEATURE          2
#define ST_ADV_ON           3
#define ST_ADV_RETRY        4
#define ST_ADV_READ         5
#define ST_WAIT_CONN        6
#define ST_WAIT_NOTIFY      7
#define ST_NOTIFY           8
#define ST_DISC_WAIT        9

#define CMD_IDLE            0
#define CMD_WAIT            1
#define CMD_OK              2
#define CMD_FAIL            3

/*============================== 1 ms tick (Timer0) =========================*/
/* Timer3 is the UART1 baud generator, so Timer0 is used for timing.          */
/* 16.6 MHz: Fsys/12 = 1.3833 MHz, 1 ms = 1383 counts -> 65536-1383 = 0xFA99  */
/* 16.0 MHz: Fsys/12 = 1.3333 MHz, 1 ms = 1333 counts -> 65536-1333 = 0xFACB  */
/* (Delay.c Timer0_Delay1ms() is not used: it would reprogram Timer0.)        */
#if USE_HIRC_166
#define T0_RELOAD_H         0xFA
#define T0_RELOAD_L         0x99
#else
#define T0_RELOAD_H         0xFA
#define T0_RELOAD_L         0xCB
#endif

/* 32-bit ms counter = g_ms_hi:g_ms (wraps after ~49.7 days).                 */
/* State timing only uses the low 16 bits (now_ms), the BLE sensor_time_ms   */
/* uses all 32 bits.                                                          */
volatile UINT16 g_ms    = 0;
volatile UINT16 g_ms_hi = 0;

void Timer0_ISR(void) interrupt 1
{
    TH0 = T0_RELOAD_H;
    TL0 = T0_RELOAD_L;
    if (++g_ms == 0) g_ms_hi++;
}

static UINT16 now_ms(void)
{
    UINT16 t;
    clr_ET0;
    t = g_ms;
    set_ET0;
    return t;
}

static void Timer0_Tick_Init(void)
{
    clr_T0M;                                /* Timer0 clock = Fsys/12 */
    TMOD = (TMOD & 0xF0) | 0x01;            /* Timer0 mode 1, 16-bit  */
    TH0 = T0_RELOAD_H;
    TL0 = T0_RELOAD_L;
    set_ET0;
    set_TR0;
}

/*============================== UART1 RX ring buffer =======================*/
#define RX_SIZE             64              /* power of 2 */
#define RX_MASK             (RX_SIZE - 1)

xdata UINT8 rx_buf[RX_SIZE];
volatile UINT8 rx_head = 0;                 /* written by ISR  */
volatile UINT8 rx_tail = 0;                 /* written by main */
bit   g_rx_any    = 0;                      /* diag: any byte received on P0.2   */
bit   g_frm_any   = 0;                      /* diag: any valid 78 frame parsed   */

/* RX only. TI_1 is NOT touched here; bm_putc() polls it. */
void SerialPort1_ISR(void) interrupt 15
{
    UINT8 c, next;
    if (RI_1)
    {
        c = SBUF_1;
        RI_1 = 0;
        next = (rx_head + 1) & RX_MASK;
        if (next != rx_tail)                /* drop byte if buffer full */
        {
            rx_buf[rx_head] = c;
            rx_head = next;
            g_rx_any = 1;
        }
    }
}

static bit rx_get(UINT8 *c)
{
    if (rx_tail == rx_head) return 0;
    *c = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) & RX_MASK;
    return 1;
}

static void rx_flush(void)
{
    clr_ES_1;
    rx_tail = rx_head;
    set_ES_1;
}

/* Same register setup as Common.c InitialUART1_Timer3(), reload computed at compile time. */
#if USE_HIRC_166
#define T3_RELOAD           (65536UL - (1037500UL / BM_BAUD))          /* 16.6 MHz */
#else
#define T3_RELOAD           (65536UL - (1000000UL / BM_BAUD) - 1)      /* 16 MHz   */
#endif

static void UART1_Init(void)
{
    P02_Quasi_Mode;                         /* RXD_1 */
    P16_Quasi_Mode;                         /* TXD_1, idle high */
    SCON_1 = 0x50;                          /* mode 1, REN_1 = 1 */
    T3CON  = 0x08;                          /* prescale 1, Timer3 run */
    clr_BRCK;
    RH3    = (UINT8)(T3_RELOAD >> 8);
    RL3    = (UINT8)(T3_RELOAD & 0xFF);
    set_TR3;
}

#if USE_HIRC_166
/* BSP IAP_MoidfyHIRC: HIRC 16 MHz -> 16.6 MHz, only after power-on reset (POF = PCON bit4) */
static void HIRC_Trim_166(void)
{
    UINT8  map0, map1;
    UINT16 trim;
    if ((PCON & SET_BIT4) == SET_BIT4)
    {
        map0 = RCTRIM0;
        map1 = RCTRIM1;
        trim = ((UINT16)map0 << 1) + (map1 & 0x01);
        trim = trim - 15;
        map1 = trim & 0x01;
        map0 = trim >> 1;
        TA = 0xAA;
        TA = 0x55;
        RCTRIM0 = map0;
        TA = 0xAA;
        TA = 0x55;
        RCTRIM1 = map1;
        PCON &= CLR_BIT4;                   /* clear power-on flag */
    }
}
#endif

static void bm_putc(UINT8 c)
{
    TI_1 = 0;
    SBUF_1 = c;
    while (TI_1 == 0);
    TI_1 = 0;
}

/*============================== BM7701 link layer ==========================*/
bit   g_connected = 0;
bit   g_notify    = 0;
bit   g_app_ready = 0;                      /* peer wrote 0x01 to FFF2 */

UINT8  cmd_state   = CMD_IDLE;
UINT16 cmd_type    = 0;
UINT8  cmd_flag    = 0;
UINT8  cmd_status  = 0;
UINT8  cmd_val     = 0xFF;                  /* first data byte of the ACK, 0xFF = none */                     /* low nibble of ACK status, 0xFF = timeout */
UINT16 cmd_t0      = 0;
UINT16 cmd_timeout = CMD_TIMEOUT_MS;

/* received FFF2 data (peer -> MCU), for the application */
#define FFF2_MAX            20
xdata UINT8 fff2_buf[FFF2_MAX];
UINT8 fff2_len = 0;
bit   fff2_new = 0;

/* frame: 78 LEN STAT TYPE_L TYPE_H DATA... , total = LEN + 2 */
#define FRM_MAX             40
xdata UINT8 frm[FRM_MAX];
UINT8  frm_idx  = 0;
UINT8  frm_need = 0;
UINT16 frm_t    = 0;

static void bm_parser_reset(void)
{
    frm_idx   = 0;
    cmd_state = CMD_IDLE;
    rx_flush();
}

/* send: 77 LEN FLAG TYPE_L TYPE_H DATA...   (LEN = data + 3) */
static void bm_send_cmd(UINT16 type, UINT8 flag, UINT8 *dat, UINT8 len, UINT16 timeout)
{
    UINT8 i;
    bm_putc(BM_CMD);
    bm_putc(len + 3);
    bm_putc(flag);
    bm_putc((UINT8)(type & 0xFF));
    bm_putc((UINT8)(type >> 8));
    for (i = 0; i < len; i++) bm_putc(dat[i]);

    cmd_type    = type;
    cmd_flag    = flag;
    cmd_status  = 0;
    cmd_val     = 0xFF;
    cmd_timeout = timeout;
    cmd_t0      = now_ms();
    cmd_state   = CMD_WAIT;
}

/* called when a peer writes to FFF2 */
static void on_ble_write(UINT8 *dat, UINT8 len)
{
    UINT8 i;
    if (len > FFF2_MAX) len = FFF2_MAX;
    for (i = 0; i < len; i++) fff2_buf[i] = dat[i];
    fff2_len = len;
    fff2_new = 1;

    /* optional handshake: app writes 0x01 after enabling FFF1 notify */
    if (len >= 1 && dat[0] == 0x01) g_app_ready = 1;
}

/* Is this frame the ACK of the pending command?                                  */
/* CmdType3 (0x00xx): EvtStatus b[7:4] is not an echo of CmdFlag (guide 4.14:      */
/*   77 07 10 1000 .. -> 78 03 00 1000), so only Type is compared.                */
/* CmdType2 FFF1 notify: bench result 78 03 20 F1 FF / 78 03 26 F1 FF, b[7:4] = 2. */
static bit bm_is_ack(UINT8 st, UINT16 type)
{
    if (cmd_state != CMD_WAIT || type != cmd_type) return 0;
    if (cmd_flag == BM_FLAG_NOTIFY) return ((st & 0xF0) == BM_FLAG_NOTIFY);
    return 1;
}

static void bm_handle_frame(void)
{
    UINT8  st   = frm[2];
    UINT16 type = (UINT16)frm[3] | ((UINT16)frm[4] << 8);
    UINT8  dlen = frm[1] - 3;

    g_frm_any = 1;

    /* unsolicited data from peer, never an ACK */
    if (type == BM_TYPE_FFF2)
    {
        on_ble_write(&frm[5], dlen);
        return;
    }

    /* status event (auto) or status query reply: statusFlag b[0] = connected */
    if (type == BM_TYPE_STATUS && dlen >= 1)
    {
        g_connected = (frm[5] & 0x01) ? 1 : 0;
        if (!g_connected)
        {
            g_notify    = 0;
            g_app_ready = 0;
        }
    }

    if (bm_is_ack(st, type))
    {
        cmd_status = st & 0x0F;
        if (dlen >= 1) cmd_val = frm[5];
        cmd_state  = (cmd_status == 0) ? CMD_OK : CMD_FAIL;
    }
}

/* parse all received bytes into frames; check command timeout */
static void bm_poll(void)
{
    UINT8 c;
    UINT16 t = now_ms();

    if (frm_idx != 0 && (UINT16)(t - frm_t) > FRAME_GAP_MS) frm_idx = 0;   /* stale partial frame */

    while (rx_get(&c))
    {
        frm_t = t;
        if (frm_idx == 0)
        {
            if (c == BM_EVT) frm[frm_idx++] = c;                 /* sync on 0x78 */
        }
        else if (frm_idx == 1)
        {
            if (c < 3 || (UINT16)c + 2 > FRM_MAX)
            {
                frm_idx = (c == BM_EVT) ? 1 : 0;                  /* bad length: resync */
            }
            else
            {
                frm[frm_idx++] = c;
                frm_need = c + 2;
            }
        }
        else
        {
            frm[frm_idx++] = c;
            if (frm_idx >= frm_need)
            {
                bm_handle_frame();
                frm_idx = 0;
            }
        }
    }

    if (cmd_state == CMD_WAIT && (UINT16)(t - cmd_t0) > cmd_timeout)
    {
        cmd_status = 0xFF;                                         /* timeout */
        cmd_state  = CMD_FAIL;
    }
}

/*============================== BM7701 commands ============================*/
static void bm_set_auto_status(void)
{
    UINT8 d[4];
    d[0] = BM_FEATURE_AUTO_STATUS; d[1] = 0; d[2] = 0; d[3] = 0;
    bm_send_cmd(BM_TYPE_FEATURE, BM_FLAG_FEATURE_OR, d, 4, CMD_TIMEOUT_MS);   /* RAM only, not saved to flash */
}

static void bm_adv(UINT8 on)
{
    UINT8 d[1];
    d[0] = on ? 1 : 0;
    bm_send_cmd(BM_TYPE_ADV_EN, BM_FLAG_NONE, d, 1, ADV_TIMEOUT_MS);
}

static void bm_read_adv(void)                                     /* 77 03 00 07 00 -> 78 04 00 07 00 advCtrl */
{
    bm_send_cmd(BM_TYPE_ADV_EN, BM_FLAG_NONE, 0, 0, CMD_TIMEOUT_MS);
}

static void bm_query_status(void)
{
    bm_send_cmd(BM_TYPE_STATUS, BM_FLAG_NONE, 0, 0, CMD_TIMEOUT_MS);
}

static void bm_notify_send(UINT8 *dat, UINT8 len)                  /* len > 20 needs the enlarged MTU */
{
    bm_send_cmd(BM_TYPE_FFF1, BM_FLAG_NOTIFY, dat, len, CMD_TIMEOUT_MS);
}

/*============================== PPG (UART0) -> BLE frame ===================*/
/* BLE frame (ble-notify-data-format.md), Big-endian, one record per frame:  */
/*   [0..1] length  [2..5] sensor_time_ms    [6] msgType                     */
/*   [7..10] sequence  [11] status  [12..21] SpO2 .. ppg2Low                 */
#define SENSOR_PPG          0x01
#define PKT_LEN             22
#define PKT_TS              2
#define PKT_SEQ             7
#define PKT_ST              11              /* status, first byte copied from the module */

xdata UINT8 tx_pkt[PKT_LEN] = { 0x00, PKT_LEN - 2, 0, 0, 0, 0, SENSOR_PPG };

/* The UART0 ISR writes tx_pkt only while ppg_ready = 0; main sends tx_pkt   */
/* and then clears ppg_ready. ppg_cap is latched at the start of each module */
/* frame, so a frame is either captured completely or not at all.            */
bit   ppg_ready = 0;
bit   ppg_cap   = 0;
UINT8 ppg_i     = 0;                        /* byte index in the 15-byte module frame */
UINT8 ppg_sum   = 0;
UINT8 ppg_seq[4] = { 0, 0, 0, 0 };          /* Big-endian, counts every valid module frame */

void SerialPort0_ISR(void) interrupt 4
{
    UINT8 c;
    if (RI)
    {
        c  = SBUF;
        RI = 0;
        if (ppg_i == 0)
        {
            if (c == 0x55) ppg_i = 1;
        }
        else if (ppg_i == 1)
        {
            if (c == 0xB0)
            {
                ppg_sum = 0xB0;
                ppg_cap = !ppg_ready;
                ppg_i   = 2;
            }
            else if (c != 0x55)
            {
                ppg_i = 0;                  /* 55 55 B0: stay synced on the second 55 */
            }
        }
        else if (ppg_i < 13)                /* ST, D0..D9 */
        {
            ppg_sum += c;
            if (ppg_cap) tx_pkt[PKT_ST - 2 + ppg_i] = c;
            ppg_i++;
        }
        else if (ppg_i == 13)               /* checksum */
        {
            ppg_i = (c == ppg_sum) ? 14 : 0;
        }
        else                                /* tail 0xAA */
        {
            ppg_i = 0;
            if (c == 0xAA)
            {
                if (ppg_cap)
                {
                    /* Timer0 has the same priority, so g_ms cannot change here */
                    tx_pkt[PKT_TS + 0] = (UINT8)(g_ms_hi >> 8);
                    tx_pkt[PKT_TS + 1] = (UINT8)g_ms_hi;
                    tx_pkt[PKT_TS + 2] = (UINT8)(g_ms >> 8);
                    tx_pkt[PKT_TS + 3] = (UINT8)g_ms;
                    tx_pkt[PKT_SEQ + 0] = ppg_seq[0];
                    tx_pkt[PKT_SEQ + 1] = ppg_seq[1];
                    tx_pkt[PKT_SEQ + 2] = ppg_seq[2];
                    tx_pkt[PKT_SEQ + 3] = ppg_seq[3];
                    ppg_ready = 1;
                }
                if (++ppg_seq[3] == 0)
                    if (++ppg_seq[2] == 0)
                        if (++ppg_seq[1] == 0)
                            ++ppg_seq[0];
            }
        }
    }
    /* TI is not touched here; ppg_start() polls it with ES off */
}

static void UART0_Init(void)
{
    P06_Quasi_Mode;                         /* TXD */
    P07_Quasi_Mode;                         /* RXD */
    SCON = 0x50;                            /* mode 1, REN = 1, TI = 0 */
    TMOD = (TMOD & 0x0F) | 0x20;            /* Timer1 mode 2, auto reload */
    set_SMOD;                               /* double rate */
    set_T1M;                                /* Timer1 clock = Fsys */
    clr_BRCK;                               /* UART0 baud source = Timer1 (Timer3 is UART1's) */
    TH1 = PPG_TH1;
    set_TR1;
}

#if PPG_SEND_START
code UINT8 ppg_start_cmd[8] = { 0x55, 0xB1, 0x01, 0x00, 0x00, 0x00, 0xB2, 0xAA };

static void ppg_start(void)                 /* reply 55 B1 .. is ignored by the parser */
{
    UINT8 i;
    clr_ES;
    for (i = 0; i < 8; i++)
    {
        TI = 0;
        SBUF = ppg_start_cmd[i];
        while (TI == 0);
    }
    TI = 0;
    set_ES;
}
#endif

static void ppg_send(void)
{
    bm_notify_send(tx_pkt, PKT_LEN);        /* blocking: tx_pkt is fully sent on return */
    ppg_ready = 0;
}

/*============================== main =======================================*/
void main(void)
{
    UINT8  state = ST_RESET;
    UINT16 t_state, t;
#if USE_LED
    UINT16 t_led, t_pkt, half;
#endif

#if USE_HIRC_166
    HIRC_Trim_166();                            /* must run before UART / Timer0 setup */
#endif

    /* hold BM7701 in reset first; TX pin stays high (quasi, latch = 1) */
    BM_RST_INIT();
    BM_RST = 0;
#if USE_LED
    LED_INIT();
    LED = 0;
#endif

    UART1_Init();                               /* TX idle = high, required during BM7701 boot */
    Timer0_Tick_Init();
    UART0_Init();                               /* PPG, after UART1_Init (both touch BRCK) */
    set_ES_1;                                   /* UART1 interrupt */
    set_ES;                                     /* UART0 interrupt */
    set_EA;

    t_state = now_ms();
#if USE_LED
    t_led   = t_state;
    t_pkt   = t_state;
#endif

    while (1)
    {
        bm_poll();
        t = now_ms();

        /* disconnect: BC7701 soft-resets 5 ms, then re-init (feature may be lost) */
        if (state >= ST_WAIT_NOTIFY && state != ST_DISC_WAIT && !g_connected && cmd_state != CMD_WAIT)
        {
            g_notify = 0;
            state = ST_DISC_WAIT;
            t_state = t;
        }

        /* not streaming: drop the sample so the first frame after subscription is fresh */
        if (state != ST_WAIT_NOTIFY && state != ST_NOTIFY) ppg_ready = 0;

        switch (state)
        {
        case ST_RESET:                              /* RST_N low */
            if ((UINT16)(t - t_state) >= RESET_LOW_MS)
            {
                BM_RST = 1;                         /* release, BC7701 boots (60 ms) */
                state = ST_BOOT;
                t_state = t;
            }
            break;

        case ST_BOOT:                               /* do not send while BC7701 samples TX/RX */
            if ((UINT16)(t - t_state) >= BOOT_WAIT_MS)
            {
                bm_parser_reset();                  /* drop noise from reset */
                g_connected = 0;
                g_rx_any    = 0;
                g_frm_any   = 0;
#if PPG_SEND_START
                ppg_start();                    /* ~200 ms after power-up */
#endif
                bm_set_auto_status();
                state = ST_FEATURE;
            }
            break;

        case ST_FEATURE:
            if (cmd_state == CMD_WAIT) break;
            /* OK or fail: continue anyway, status is also polled */
            cmd_state = CMD_IDLE;
            bm_adv(1);
            state = ST_ADV_ON;
            break;

        case ST_ADV_ON:                             /* first time: RF calibration 600~800 ms */
            if (cmd_state == CMD_WAIT) break;
            if (cmd_state == CMD_OK || g_connected) /* already connected also OK */
            {
                cmd_state = CMD_IDLE;
                state = ST_WAIT_CONN;
            }
            else
            {
                cmd_state = CMD_IDLE;
                state = ST_ADV_RETRY;
            }
            t_state = t;
            break;

        /* No ACK for AdvCtrl: do NOT blindly resend it. AdvCtrl=1 while connected resets the RF      */
        /* and drops the link (guide 4.7). Read advCtrl back and only write 1 if it is really 0.       */
        case ST_ADV_RETRY:
            if (g_connected)
            {
                state = ST_WAIT_CONN;
            }
            else if ((UINT16)(t - t_state) >= ADV_RETRY_MS)
            {
                bm_read_adv();
                state = ST_ADV_READ;
            }
            break;

        case ST_ADV_READ:
            if (cmd_state == CMD_WAIT) break;
            if (g_connected || (cmd_state == CMD_OK && cmd_val != 0x00))
            {
                /* advertising (1), or a late ACK of the AdvCtrl write (no value) */
                state = ST_WAIT_CONN;
            }
            else if (cmd_state == CMD_OK)
            {
                bm_adv(1);                              /* advCtrl really 0 */
                state = ST_ADV_ON;
                t_state = t;
                break;
            }
            else
            {
                state = ST_ADV_RETRY;                   /* no reply: read again later */
            }
            cmd_state = CMD_IDLE;
            t_state = t;
            break;

        case ST_WAIT_CONN:
            if (cmd_state == CMD_WAIT) break;
            cmd_state = CMD_IDLE;
            if (g_connected)
            {
                g_notify = 0;
                g_app_ready = 0;
                state = ST_WAIT_NOTIFY;
                t_state = t - NOTIFY_PROBE_MS;          /* probe right away */
            }
            else if ((UINT16)(t - t_state) >= STATUS_POLL_MS)
            {
                bm_query_status();                      /* fallback if auto status is off */
                t_state = t;
            }
            break;

        case ST_WAIT_NOTIFY:
            if (cmd_state == CMD_WAIT) break;
            if (cmd_state == CMD_OK && cmd_type == BM_TYPE_FFF1)
            {
                /* first packet accepted -> peer has subscribed FFF1 */
                cmd_state = CMD_IDLE;
                g_notify = 1;
#if USE_LED
                t_pkt = t;                          /* LED flash */
#endif
                state = ST_NOTIFY;
                t_state = t;
                break;
            }
            cmd_state = CMD_IDLE;                       /* 0x26 = not subscribed yet (sample lost) */
            if (ppg_ready)
            {
                if (g_app_ready || (UINT16)(t - t_state) >= NOTIFY_PROBE_MS)
                {
                    g_app_ready = 0;
                    ppg_send();                         /* probe with a real PPG frame */
                    t_state = t;
                }
                else
                {
                    ppg_ready = 0;                      /* between probes: keep the next sample fresh */
                }
            }
            break;

        case ST_NOTIFY:
            if (cmd_state == CMD_WAIT) break;
            if (cmd_state == CMD_FAIL && cmd_type == BM_TYPE_FFF1)
            {
                /* peer unsubscribed (or timeout) */
                cmd_state = CMD_IDLE;
                g_notify = 0;
                state = ST_WAIT_NOTIFY;
                t_state = t;
                break;
            }
#if USE_LED
            if (cmd_state == CMD_OK && cmd_type == BM_TYPE_FFF1)
            {
                t_pkt = t;                          /* LED flash */
            }
#endif
            cmd_state = CMD_IDLE;
            if (ppg_ready) ppg_send();              /* one frame per PPG sample */
            break;

        case ST_DISC_WAIT:                              /* no command for 5 ms after disconnect */
            if ((UINT16)(t - t_state) >= DISC_WAIT_MS)
            {
                bm_parser_reset();
                bm_set_auto_status();                   /* re-enable in case the soft reset cleared it */
                state = ST_FEATURE;
            }
            break;

        default:
            BM_RST = 0;
            state = ST_RESET;
            t_state = t;
            break;
        }

        /* handle data written by the peer to FFF2 */
        if (fff2_new)
        {
            fff2_new = 0;
            /* TODO: parse fff2_buf[0..fff2_len-1] (commands from phone) */
        }

#if USE_LED
        if (state == ST_NOTIFY)
        {
            LED = ((UINT16)(t - t_pkt) < LED_PULSE_MS) ? 1 : 0;   /* flash per packet */
        }
        else
        {
            if      (state == ST_WAIT_CONN)   half = LED_ADV_MS;
            else if (state == ST_WAIT_NOTIFY) half = LED_CONN_MS;
            else if (g_frm_any)               half = LED_INIT_OK_MS;
            else if (g_rx_any)                half = 0;            /* solid ON below */
            else                              half = LED_INIT_MS;
            if (half == 0)
            {
                LED = 1;
            }
            else
            if ((UINT16)(t - t_led) >= half)
            {
                LED = !LED;
                t_led = t;
            }
        }
#endif
    }
}
