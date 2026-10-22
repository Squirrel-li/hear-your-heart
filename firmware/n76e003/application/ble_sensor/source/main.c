/*---------------------------------------------------------------------------------------------------------*/
/*  ble_sensor : N76E003 UART1  <->  BM7701-00-1 (BLE UART module, BC7701)                                 */
/*                                                                                                         */
/*  Wiring (same as application\UART1 test)                                                               */
/*    N76E003 P1.6 TXD_1  ->  BM7701 pin 22 UART1_RX                                                       */
/*    N76E003 P0.2 RXD_1  <-  BM7701 pin 23 UART1_TX                                                       */
/*    N76E003 P1.5        ->  BM7701 pin 6  RST_N  (active low)                                            */
/*    3.3 V, common GND                                                                                    */
/*                                                                                                         */
/*  OLED  SH1106 128x64 (1.3") on I2C0, 8-bit address 0x78 (7-bit 0x3C), polled master, ~296 kHz                 */
/*    N76E003 P1.3 SCL, P1.4 SDA (open-drain, the OLED module provides the pull-ups)                       */
/*    shows BLE name / link state / PPG values; one text row is refreshed per OLED_ROW_MS                  */
/*                                                                                                         */
/*  IMU  LSM6DS3 on the same I2C0 bus, 7-bit address 0x6B (SA0 = VCC)                                    */
/*    read once per second, sent as one 21 B acc+gyro frame (msgType 0x02), shown on the IMU page */
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
/*    2. Device name BM_NAME (RAM only)        77 11 00 05 00 "BLE_watch_ITAP"  -> 78 03 00 05 00         */
/*       Feature OR autoSendStatus (RAM only)  77 07 10 10 00 80 00 00 00  -> 78 03 00 10 00              */
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
/*    6. Notify ON: one 20-byte frame per PPG sample (ble-notify-data-format.md, all Big-endian)           */
/*         [0..3] sensor_time_ms          [4] msgType = 0x11    [5..8] PPG sequence                        */
/*         [9] status  [10..19] SpO2 HR bpi hrv amp systole ppg1H ppg1L ppg2H ppg2L                        */
/*         no length prefix (the BLE notify carries the length); fits the default 20 B notify payload    */
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
/*  Ported from application/ble_sensor_keil (Keil C51) to SDCC / NuEclipse, behaviour unchanged.         */
/*  UART1 is initialised here with a compile-time Timer3 reload (no 32-bit division library).          */
/*  Only common.c is needed from the BSP library (BIT_TMP used by the SFR page macros).                 */
/*---------------------------------------------------------------------------------------------------------*/
#include "numicro_8051.h"
#include "oled_font.h"

/*============================== startup: XRAM initialised variables =========*/
/* SDCC's __mcs51_genXINIT copies the initial values (code, .XINIT) to the xdata variables (.XISEG)     */
/* with MOVX @R0 and P2 as the upper address byte. The N76E003 has no usable P2 address port, so with   */
/* more than 256 B of XRAM the copy misses and every variable that has an initialiser starts as random  */
/* (seen on air: msgType, sequence upper bytes). This hook runs before the SDCC init and copies .XINIT  */
/* again with DPTR. It returns 0 so the normal init still runs; the clear of .XSEG does not touch      */
/* .XISEG, and the later wrong-page write only lands in page 0, which is cleared afterwards.           */
unsigned char _sdcc_external_startup(void)
{
    __asm
        mov     r1,#l_XINIT
        mov     a,r1
        orl     a,#(l_XINIT >> 8)
        jz      9002$
        mov     r2,#((l_XINIT >> 8) + 1)
        mov     r3,#s_XINIT
        mov     r4,#(s_XINIT >> 8)
        mov     r5,#s_XISEG
        mov     r6,#(s_XISEG >> 8)
    9001$:
        mov     dpl,r3
        mov     dph,r4
        clr     a
        movc    a,@a+dptr
        inc     dptr
        mov     r3,dpl
        mov     r4,dph
        mov     dpl,r5
        mov     dph,r6
        movx    @dptr,a
        inc     dptr
        mov     r5,dpl
        mov     r6,dph
        djnz    r1,9001$
        djnz    r2,9001$
    9002$:
    __endasm;
    return 0;
}

/*============================== configuration ==============================*/
#if defined(ITAP_CAPTURE) || defined(ITAP_MOTION_HR)
#define ITAP_FAST_SENSOR 1
#endif
#define BM_BAUD             115200
#ifdef ITAP_CAPTURE
#define BM_NAME             "BLE_capture_ITAP"
#else
#define BM_NAME             "BLE_watch_ITAP"    /* BLE device name, 1..31 ASCII bytes, set at every boot; the HUB tells chest/watch apart by name */
#endif
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
#define PPG_SEND_START      1       /* 1 = at boot read the module's mode (EEPROM 0x01), write the factory     */
                                    /* default 0x00 (timed upload) if it differs, then send "start measure",  */
                                    /* see ppg_start().                                                         */
                                    /* 0 = leave the module as it is: it must then already be in timed /       */
                                    /* continuous upload mode; in ask-response mode (0x02/03) it sends no 55 B0 */
                                    /* frames by itself.                                                        */

#ifdef ITAP_FAST_SENSOR
#define USE_OLED            0       /* dedicate I2C/main-loop time to acquisition */
#else
#define USE_OLED            1       /* 0 = skip the OLED (to rule it out when debugging)      */
#endif
#define OLED_ADDR           0x78    /* 8-bit I2C address                                     */
#define OLED_I2CLK          13      /* I2C clock = Fsys / (4 * (13 + 1)) ~ 296 kHz           */
#define OLED_TMO_MS         5       /* I2C SI wait limit (bus stuck / no pull-up)            */
#define OLED_ROW_MS         20      /* one text row is refreshed per period (8 rows)         */
#define OLED_RETRY_MS       1000    /* retry init when the OLED did not answer               */
#define KEY_DEBOUNCE_MS     200     /* P3.0 / P1.7 presses closer than this are one press    */
#define PPG_LOST_MS         1500    /* no PPG sample for this long -> NO SIGNAL              */

/* BM7701 reset pin */
#define BM_RST              P15
#define BM_RST_INIT()       P15_PUSHPULL_MODE

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
#define LED_INIT()          P12_PUSHPULL_MODE
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
#define BM_TYPE_NAME        0x0005
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
#define ST_NAME             2
#define ST_FEATURE          3
#define ST_ADV_ON           4
#define ST_ADV_RETRY        5
#define ST_ADV_READ         6
#define ST_WAIT_CONN        7
#define ST_WAIT_NOTIFY      8               /* states from here on need a connection */
#define ST_NOTIFY           9
#define ST_DISC_WAIT        10

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

void Timer0_ISR(void) __interrupt (1)
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

__xdata UINT8 rx_buf[RX_SIZE];
volatile UINT8 rx_head = 0;                 /* written by ISR  */
volatile UINT8 rx_tail = 0;                 /* written by main */
/* __bit globals have no "= 0": SDCC --debug emits bad "$(null)$" labels for initialised bits. */
/* They are still 0 at reset (SDCC startup clears internal RAM, including the bit area).        */
volatile __bit g_rx_any;                           /* diag: any byte received on P0.2   */
__bit   g_frm_any;                           /* diag: any valid 78 frame parsed   */

/* RX only. TI_1 is NOT touched here; bm_putc() polls it. */
void SerialPort1_ISR(void) __interrupt (15)
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

static __bit rx_get(UINT8 *c)
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
    P02_QUASI_MODE;                         /* RXD_1 */
    P16_QUASI_MODE;                         /* TXD_1, idle high */
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
__bit   g_connected;
__bit   g_notify;
__bit   g_app_ready;                           /* peer wrote 0x01 to FFF2 */

UINT8  cmd_state   = CMD_IDLE;
UINT16 cmd_type    = 0;
UINT8  cmd_flag    = 0;
UINT8  cmd_status  = 0;
UINT8  cmd_val     = 0xFF;                  /* first data byte of the ACK, 0xFF = none */                     /* low nibble of ACK status, 0xFF = timeout */
UINT16 cmd_t0      = 0;
UINT16 cmd_timeout = CMD_TIMEOUT_MS;

/* received FFF2 data (peer -> MCU), for the application */
#define FFF2_MAX            20
__xdata UINT8 fff2_buf[FFF2_MAX];
UINT8 fff2_len = 0;
__bit   fff2_new;

/* frame: 78 LEN STAT TYPE_L TYPE_H DATA... , total = LEN + 2 */
#define FRM_MAX             40
__xdata UINT8 frm[FRM_MAX];
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
static __bit bm_is_ack(UINT8 st, UINT16 type)
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
__code UINT8 bm_name[] = BM_NAME;

static void bm_set_name(void)                                     /* RAM only, not saved to flash */
{
    bm_send_cmd(BM_TYPE_NAME, BM_FLAG_NONE, (UINT8 *)bm_name, sizeof(bm_name) - 1, CMD_TIMEOUT_MS);
}

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

static void bm_notify_send(UINT8 *dat, UINT8 len)                  /* len > 20 needs an enlarged MTU */
{
    bm_send_cmd(BM_TYPE_FFF1, BM_FLAG_NOTIFY, dat, len, CMD_TIMEOUT_MS);
}

/*============================== PPG (UART0) -> BLE frame ===================*/
/* BLE frame (ble-notify-data-format.md), Big-endian, one record per frame:  */
/*   [0..3] sensor_time_ms    [4] msgType   [5..8] sequence                  */
/*   [9] status  [10..19] SpO2 .. ppg2Low                                    */
#define SENSOR_CMD          0x00            /* reserved: command frame, payload not defined yet */
#define SENSOR_SRC          0x10            /* high nibble = source: 1 = watch (chest sends 0) */
#define SENSOR_PPG          (SENSOR_SRC | 0x01)
#ifdef ITAP_FAST_SENSOR
#define SENSOR_IMU          (SENSOR_SRC | 0x03) /* capture sends acc only, fits default MTU */
#else
#define SENSOR_IMU          (SENSOR_SRC | 0x02) /* acc + gyro in one frame */
#endif
#define SENSOR_ACC          0x03            /* separate acc / gyro frames: not sent by this firmware, */
#define SENSOR_GYRO         0x04            /* defined in the protocol for other senders              */
#define PKT_LEN             20
#define PKT_TS              0
#define PKT_SEQ             5
#define PKT_ST              9               /* status, first byte copied from the module */

__xdata UINT8 tx_pkt[PKT_LEN] = { 0, 0, 0, 0, SENSOR_PPG };

/* The UART0 ISR writes tx_pkt only while ppg_ready = 0; main sends tx_pkt   */
/* and then clears ppg_ready. ppg_cap is latched at the start of each module */
/* frame, so a frame is either captured completely or not at all.            */
volatile __bit ppg_ready;
__bit   ppg_cap;
UINT8 ppg_i     = 0;                        /* byte index in the 15-byte module frame */
UINT8 ppg_sum   = 0;
UINT8 ppg_seq[4] = { 0, 0, 0, 0 };          /* Big-endian, counts every valid module frame */

#ifdef ITAP_MOTION_HR
#include "../../ble_sensor_hr/source/ppg_motion_hr.h"
__xdata motion_hr motion;
__xdata UINT8 motion_pkt[PKT_LEN];
__bit motion_has, motion_ready;
__bit motion_seen;
UINT8 motion_sequence = 0;
UINT16 motion_acc_sent = 0;
static void motion_task(void)
{
    UINT8 k, previous;
    if (!ppg_ready) return;
    previous = motion.bpm;
    /* Low byte suffices within a <100ms gap; long gaps reset in mh_ppg. */
    if (motion_seen && (UINT8)(tx_pkt[8] - motion_sequence) != 1) mh_reset(&motion, 1);
    motion_sequence = tx_pkt[8]; motion_seen = 1;
    if (mh_ppg(&motion, ((UINT16)tx_pkt[16] << 8) | tx_pkt[17],
               ((UINT16)tx_pkt[18] << 8) | tx_pkt[19],
               ((UINT16)tx_pkt[2] << 8) | tx_pkt[3], tx_pkt[9])) motion_ready = 1;
    if (previous && !motion.bpm) motion_ready = 1; /* report loss of validity promptly */
    for (k = 0; k < PKT_LEN; k++) motion_pkt[k] = tx_pkt[k];
    motion_pkt[11] = motion.bpm;
    motion_pkt[9] = motion.bpm ? 2 : (tx_pkt[9] ? 1 : 0);
    motion_has = 1;
    ppg_ready = 0;
}
#endif

#ifdef ITAP_CAPTURE
/* ponytail: four main-loop slots absorb short ACK stalls, not a slow link.
 * Overflow discards the incoming frame; the transmitted sequence exposes it.
 * ISR keeps its original atomic single-slot handoff. */
__xdata UINT8 capture_ppg[4][PKT_LEN];
UINT8 capture_head = 0, capture_tail = 0, capture_count = 0;
__bit capture_acc_turn;
static void capture_collect(void)
{
    UINT8 k;
    if (!ppg_ready) return;
    if (capture_count < 4) {
        for (k = 0; k < PKT_LEN; k++) capture_ppg[capture_head][k] = tx_pkt[k];
        capture_head = (capture_head + 1) & 3;
        capture_count++;
    }
    ppg_ready = 0;
}
static void capture_send(void)
{
    bm_notify_send(capture_ppg[capture_tail], PKT_LEN);
    capture_tail = (capture_tail + 1) & 3;
    capture_count--;
}
#endif

void SerialPort0_ISR(void) __interrupt (4)
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
    P06_QUASI_MODE;                         /* TXD */
    P07_QUASI_MODE;                         /* RXD */
    SCON = 0x50;                            /* mode 1, REN = 1, TI = 0 */
    TMOD = (TMOD & 0x0F) | 0x20;            /* Timer1 mode 2, auto reload */
    set_SMOD;                               /* double rate */
    set_T1M;                                /* Timer1 clock = Fsys */
    clr_BRCK;                               /* UART0 baud source = Timer1 (Timer3 is UART1's) */
    TH1 = (UINT8)PPG_TH1;
    set_TR1;
}

#if PPG_SEND_START
/* Factory default module state (datasheet: EEPROM 0x01 = 0x0000, timed upload, red LED off) + "start". */
/* ble_sensor_hr writes mode 4 (continuous upload) into the same EEPROM, so a module moved between the  */
/* boards keeps it: the mode is read first and written back only when it differs. The timed upload     */
/* interval (EEPROM 0x0A) is left as it is. A write takes 50 ms and no command may follow during that   */
/* time; other commands need 6 ms.                                                                      */
#ifdef ITAP_FAST_SENSOR
#define PPG_MODE_VALUE      0x04       /* continuous waveform upload */
#else
#define PPG_MODE_VALUE      0x00
#endif
#define PPG_REPLY_MS        100
__code UINT8 ppg_read_cmd[8]  = { 0x55, 0xB1, 0x05, 0x01, 0x00, 0x00, 0xB7, 0xAA };   /* read EEPROM 0x01 */
__code UINT8 ppg_mode_cmd[8]  = { 0x55, 0xB1, 0x04, 0x01, 0x00, PPG_MODE_VALUE, (UINT8)(0xB6 + PPG_MODE_VALUE), 0xAA };
__code UINT8 ppg_stop_cmd[8]  = { 0x55, 0xB1, 0x01, 0x00, 0x00, 0x01, 0xB3, 0xAA };
__code UINT8 ppg_start_cmd[8] = { 0x55, 0xB1, 0x01, 0x00, 0x00, 0x00, 0xB2, 0xAA };
__code UINT8 ppg_read_hdr[4]  = { 0x55, 0xB1, 0x05, 0x01 };
UINT8 ppg_mode_ch = '?';                    /* shown on the OLED: '=' mode was already right, 'W' it was written */

static void ppg_cmd(__code UINT8 *cmd, UINT16 wait_ms)      /* UART0 interrupt must be off */
{
    UINT8 i;
    UINT16 t0;
    for (i = 0; i < 8; i++)
    {
        TI = 0;
        SBUF = cmd[i];
        while (TI == 0);
    }
    TI = 0;
    t0 = now_ms();
    while ((UINT16)(now_ms() - t0) < wait_ms);
}

/* 1 = the module reports mode PPG_MODE_VALUE. No answer within PPG_REPLY_MS counts as "not right".   */
/* The reply is 55 B1 05 01 hi lo CS AA; a streaming module sends 55 B0 frames in between, so look for  */
/* the header byte by byte.                                                                          */
static __bit ppg_mode_ok(void)
{
    UINT8 st = 0, c, hi = 0, lo = 0;
    UINT16 t0;

    RI = 0;
    ppg_cmd(ppg_read_cmd, 0);
    t0 = now_ms();
    while ((UINT16)(now_ms() - t0) < PPG_REPLY_MS)
    {
        if (!RI) continue;
        c = SBUF;
        RI = 0;
        if (st < 4)       st = (c == ppg_read_hdr[st]) ? st + 1 : (c == 0x55 ? 1 : 0);
        else if (st == 4) { hi = c; st = 5; }
        else if (st == 5) { lo = c; st = 6; }
        else if (st == 6) st = (c == (UINT8)(0xB1 + 0x05 + 0x01 + hi + lo)) ? 7 : 0;
        else if (c == 0xAA) return hi == 0 && lo == PPG_MODE_VALUE;
        else st = 0;
    }
    return 0;
}

static void ppg_start(void)
{
    clr_ES;
    if (ppg_mode_ok())
        ppg_mode_ch = '=';
    else
    {
        ppg_cmd(ppg_mode_cmd, 60);
        ppg_cmd(ppg_stop_cmd, 10);
        ppg_mode_ch = 'W';
    }
    ppg_cmd(ppg_start_cmd, 10);
    RI = 0;                                 /* drop the replies, the parser starts clean */
    set_ES;
}
#endif

static void ppg_send(void)
{
#ifdef ITAP_MOTION_HR
    bm_notify_send(motion_pkt, PKT_LEN);
    motion_has = motion_ready = 0;
#else
    bm_notify_send(tx_pkt, PKT_LEN);        /* blocking: tx_pkt is fully sent on return */
    ppg_ready = 0;
#endif
}

/*============================== SH1106 OLED (I2C0, polled) ================*/
/* 21 text columns x 8 rows (6x8 cell). Each row is built as text, compared with the */
/* text already on screen and only sent when it changed (about 4 ms per row).        */
#define OLED_COLS           21
#define OLED_ROWS           8

__xdata UINT8 disp[PKT_LEN];                /* last complete PPG sample, same layout as tx_pkt */
UINT16 disp_t   = 0;
__bit  disp_ok;                             /* a sample has been received            */
__bit  ppg_ok;                              /* ... and it is recent                  */

__xdata char ln[OLED_COLS + 1];             /* row being built                       */
#ifndef ITAP_MOTION_HR
__xdata char ln_cache[OLED_ROWS][OLED_COLS];/* text currently on the screen          */
#endif
UINT8  ln_n;
__xdata char ln_dig[5];
UINT8  oled_row  = 0;
UINT16 t_oled    = 0;
__bit  oled_ok;
__bit  oled_page;                           /* 0 = PPG page, 1 = IMU page                  */
__bit  oled_off;                            /* 1 = panel switched off by the P1.7 key      */

/* Keys, active low to GND, falling edge: P3.0 = INT0 changes the page, P1.7 = INT1 switches */
/* the OLED on / off. The ISRs only set a flag, key_task() debounces in the main loop.      */
volatile __bit key_page;
volatile __bit key_oled;
UINT16 t_key_page = 0;
UINT16 t_key_oled = 0;

void Key_INT0_ISR(void) __interrupt (0)
{
    key_page = 1;
}

void Key_INT1_ISR(void) __interrupt (2)
{
    key_oled = 1;
}

static void key_init(void)
{
    P30_QUASI_MODE;                         /* weak internal pull-up */
    P17_QUASI_MODE;
    P30 = 1;
    P17 = 1;
    set_IT0;                                /* edge triggered */
    set_IT1;
    clr_IE0;
    clr_IE1;
    set_EX0;
    set_EX1;
}

static __bit i2c_wait(void)
{
    UINT16 t0 = now_ms();
    while (!SI)
        if ((UINT16)(now_ms() - t0) > OLED_TMO_MS) return 0;
    return 1;
}

static __bit i2c_start(void)
{
    STA = 1;
    SI  = 0;
    if (!i2c_wait()) return 0;
    return I2STAT == 0x08;
}

static __bit i2c_rstart(void)                    /* repeated start */
{
    STA = 1;
    SI  = 0;
    if (!i2c_wait()) return 0;
    return I2STAT == 0x10;
}

static void i2c_bus_init(void)                   /* shared by the OLED and the LSM6DS3 */
{
    P13_OPENDRAIN_MODE;                     /* SCL */
    P14_OPENDRAIN_MODE;                     /* SDA */
    I2CPX = 0;                              /* I2C0 on P1.3 / P1.4 */
    I2CLK = OLED_I2CLK;
    I2CEN = 0;
    I2CEN = 1;
}

static __bit i2c_tx(UINT8 b, UINT8 ack_status)   /* 0x18 after SLA+W, 0x28 after data, 0x40 after SLA+R */
{
    I2DAT = b;
    STA = 0;
    SI  = 0;
    if (!i2c_wait()) return 0;
    return I2STAT == ack_status;
}

static void i2c_stop(void)
{
    UINT16 t0 = now_ms();
    STO = 1;
    SI  = 0;
    while (STO)
        if ((UINT16)(now_ms() - t0) > OLED_TMO_MS)
        {
            I2CEN = 0;                      /* bus stuck: reset the I2C block */
            I2CEN = 1;
            break;
        }
}

#ifndef ITAP_MOTION_HR
static __bit oled_begin(UINT8 ctrl)                /* ctrl 0x00 = commands, 0x40 = data */
{
    return i2c_start() && i2c_tx(OLED_ADDR, 0x18) && i2c_tx(ctrl, 0x28);
}

__code const UINT8 oled_init_seq[] = {     /* SH1106: page addressing only, no 0x20 / 0x21 / 0x22 / 0x8D */
    0xAE,               /* display off                        */
    0xD5, 0x80,         /* clock                              */
    0xA8, 0x3F,         /* 64 rows                            */
    0xD3, 0x00,         /* no offset                          */
    0x40,               /* start line 0                       */
    0xAD, 0x8B,         /* internal DC-DC on                  */
    0xA1, 0xC8,         /* segment / COM remap (not flipped)  */
    0xDA, 0x12,         /* COM pins                           */
    0x81, 0xCF,         /* contrast                           */
    0xD9, 0xF1,         /* pre-charge                         */
    0xDB, 0x40,         /* VCOMH                              */
    0xA4, 0xA6,         /* follow RAM, normal                 */
    0xAF                /* display on                         */
};

static __bit oled_row_draw(UINT8 row)
{
    UINT8 i, j, c;
    UINT16 g;
    /* page = row, column pointer 2 (the SH1106 RAM is 132 wide, the panel shows columns 2..129) */
    if (!(oled_begin(0x00) && i2c_tx(0xB0 + row, 0x28) && i2c_tx(0x02, 0x28) && i2c_tx(0x10, 0x28)))
    {
        i2c_stop();
        return 0;
    }
    i2c_stop();
    if (!oled_begin(0x40)) { i2c_stop(); return 0; }
    for (i = 0; i < OLED_COLS; i++)
    {
        c = (UINT8)ln[i];
        if (c < FONT_FIRST || c > FONT_LAST) c = ' ';
        g = (UINT16)(c - FONT_FIRST) * 5;
        for (j = 0; j < 5; j++)
            if (!i2c_tx(font5x7[g + j], 0x28)) { i2c_stop(); return 0; }
        if (!i2c_tx(0x00, 0x28)) { i2c_stop(); return 0; }   /* 1 column gap */
    }
    if (!(i2c_tx(0x00, 0x28) && i2c_tx(0x00, 0x28))) { i2c_stop(); return 0; }   /* 21*6 + 2 = 128 */
    i2c_stop();
    return 1;
}

static void oled_hw_init(void)
{
    UINT8 i, j;
    i2c_bus_init();

    oled_ok = 0;
    if (!oled_begin(0x00)) { i2c_stop(); return; }          /* no ACK: no OLED / bus problem */
    for (i = 0; i < sizeof(oled_init_seq); i++)
        if (!i2c_tx(oled_init_seq[i], 0x28)) { i2c_stop(); return; }
    i2c_stop();

    for (i = 0; i < OLED_COLS; i++) ln[i] = ' ';            /* clear the screen */
    for (j = 0; j < OLED_ROWS; j++)
    {
        if (!oled_row_draw(j)) return;
        for (i = 0; i < OLED_COLS; i++) ln_cache[j][i] = ' ';
    }
    oled_ok = 1;
}

static void ln_c(char c)
{
    if (ln_n < OLED_COLS) ln[ln_n++] = c;
}

static void ln_s(__code const char *s)
{
    while (*s) ln_c(*s++);
}

static void ln_u(UINT16 v, UINT8 w)         /* right-aligned decimal, dashes when there is no sample */
{
    UINT8 i = 0;
    if (!ppg_ok)
    {
        while (w--) ln_c('-');
        return;
    }
    do { ln_dig[i++] = '0' + (UINT8)(v % 10); v /= 10; } while (v);
    while (w > i) { ln_c(' '); w--; }
    while (i) ln_c(ln_dig[--i]);
}

static void ln_2d(UINT8 v)                  /* two digits, zero padded */
{
    ln_c('0' + v / 10);
    ln_c('0' + v % 10);
}

static void ln_time(UINT32 ms)              /* "mmMssS" since power-up, minutes wrap at 100 (there is no RTC) */
{
    UINT32 sec = ms / 1000UL;
    ln_2d((UINT8)((sec / 60UL) % 100UL));
    ln_c('m');
    ln_2d((UINT8)(sec % 60UL));
    ln_c('s');
}

#endif
static UINT32 uptime_ms(void)
{
    UINT32 t;
    clr_ET0;
    t = ((UINT32)g_ms_hi << 16) | g_ms;
    set_ET0;
    return t;
}

/*============================== LSM6DS3 (I2C0, shared with the OLED) ======*/
/* Read once per IMU_PERIOD_MS, sent as one acc+gyro frame (msgType 0x12, ble-notify-data-format.md */
/* section 5) and shown on the IMU page. Accel +-8 g, gyro +-1000 dps, both ODR 12.5 Hz.          */
#define IMU_ADDR            0xD6            /* 8-bit address, SA0 = VCC (7-bit 0x6B) */
#ifdef ITAP_FAST_SENSOR
#define IMU_PERIOD_MS       2               /* poll XLDA; new samples at hardware 26 Hz */
#else
#define IMU_PERIOD_MS       1000            /* 1 Hz */
#endif
#define IMU_PKT_LEN         21              /* needs MTU >= 24 (default 23 carries only 20 B) */
#define IMU_PKT_SEQ         5
#define IMU_PKT_ACC         9               /* acc x y z: int16 Big-endian */
#define IMU_PKT_GYRO        15              /* gyro x y z: int16 Big-endian */

#define LSM_WHO_AM_I        0x0F            /* 0x69 LSM6DS3, 0x6A LSM6DS3-C / TR-C */
#define LSM_CTRL1_XL        0x10
#define LSM_CTRL2_G         0x11
#define LSM_CTRL3_C         0x12
#define LSM_CTRL4_C         0x13
#define LSM_CTRL7_G         0x16
#define LSM_CTRL8_XL        0x17
#define LSM_TAP_CFG         0x58
#define LSM_OUTX_L_G        0x22            /* gyro XYZ then accel XYZ, little-endian */

__xdata UINT8 imu_pkt[IMU_PKT_LEN] = { 0, 0, 0, 0, SENSOR_IMU };
__xdata UINT8 imu_raw[12];
UINT32 imu_seq   = 0;
UINT16 t_imu     = 0;
__bit  imu_ok;                              /* sensor answered WHO_AM_I and is configured */
__bit  imu_skip;                            /* init failed: IMU is not used any more      */
__bit  imu_has;                             /* imu_pkt holds a sample                     */
UINT8  imu_ready;                           /* 1 = imu_pkt not sent yet                   */

static __bit imu_write(UINT8 reg, UINT8 val)
{
    __bit ok = i2c_start() && i2c_tx(IMU_ADDR, 0x18) && i2c_tx(reg, 0x28) && i2c_tx(val, 0x28);
    i2c_stop();
    return ok;
}

static __bit imu_read(UINT8 reg, UINT8 *buf, UINT8 n)
{
    UINT8 i;
    if (!(i2c_start() && i2c_tx(IMU_ADDR, 0x18) && i2c_tx(reg, 0x28) &&
          i2c_rstart() && i2c_tx(IMU_ADDR | 1, 0x40)))
    {
        i2c_stop();
        return 0;
    }
    for (i = 0; i < n; i++)
    {
        AA = (i + 1 < n);                   /* NACK the last byte */
        SI = 0;
        if (!i2c_wait()) { i2c_stop(); return 0; }
        buf[i] = I2DAT;
    }
    i2c_stop();
    return 1;
}

static void imu_init(void)
{
    UINT8 id;
    imu_ok  = 0;
    imu_has = 0;
    i2c_bus_init();
    if (!imu_read(LSM_WHO_AM_I, &id, 1) || (id != 0x69 && id != 0x6A)) return;
#ifdef ITAP_FAST_SENSOR
    if (imu_write(LSM_CTRL3_C, 0x44) &&    /* BDU + auto-increment */
        imu_write(LSM_CTRL4_C, 0x00) &&
        imu_write(LSM_TAP_CFG, 0x00) &&    /* retain gravity; host removes baseline */
        imu_write(LSM_CTRL8_XL, 0x00) &&
        imu_write(LSM_CTRL2_G, 0x00) &&    /* gyro off */
        imu_write(LSM_CTRL1_XL, 0x2C))    /* 26 Hz, +/-8 g */
        imu_ok = 1;
#else
    if (imu_write(LSM_CTRL3_C,  0x44) &&    /* BDU, auto-increment */
        imu_write(LSM_CTRL4_C,  0x80) &&    /* XL_BW_SCAL_ODR: anti-alias BW from BW_XL (high-perf mode only) */
        imu_write(LSM_TAP_CFG,  0x10) &&    /* SLOPE_FDS: enable the accel HP / LPF2 filters, without it the HP path outputs 0 */
        imu_write(LSM_CTRL8_XL, 0x24) &&    /* HP_SLOPE_XL_EN + HPCF_XL=01: high-pass ODR/100 = 0.125 Hz (gravity removed) */
        imu_write(LSM_CTRL7_G,  0x50) &&    /* HP_G_EN + HPCF_G=01: gyro high-pass 0.0324 Hz (zero-rate offset removed) */
        imu_write(LSM_CTRL1_XL, 0x1F) &&    /* 12.5 Hz, +-8 g, anti-alias BW 50 Hz */
        imu_write(LSM_CTRL2_G,  0x18))      /* 12.5 Hz, +-1000 dps */
        imu_ok = 1;
#endif
}

static void imu_task(void)
{
    UINT8  i;
    UINT16 t = now_ms();
    UINT32 up;

    if (imu_skip || (UINT16)(t - t_imu) < IMU_PERIOD_MS) return;
    t_imu = t;
    if (!imu_ok)
    {
        imu_init();                         /* first sample one period after init */
        if (!imu_ok) imu_skip = 1;          /* init failed: give up until the next power-up */
        return;
    }
#ifdef ITAP_FAST_SENSOR
    if (!imu_read(0x1E, imu_raw, 1)) { imu_ok = 0; return; }
    if (!(imu_raw[0] & 1)) return;          /* never upload repeated old samples */
    up = uptime_ms();                      /* readout time, <= polling jitter from DRDY */
    if (!imu_read(0x28, &imu_raw[6], 6))
#else
    if (!imu_read(LSM_OUTX_L_G, imu_raw, 12))
#endif
    {
        imu_ok  = 0;                        /* re-init next period */
        imu_has = 0;
        return;
    }

#ifndef ITAP_FAST_SENSOR
    up = uptime_ms();
#endif
#ifdef ITAP_MOTION_HR
    mh_acc(&motion, (int16_t)(((UINT16)imu_raw[7] << 8) | imu_raw[6]),
           (int16_t)(((UINT16)imu_raw[9] << 8) | imu_raw[8]),
           (int16_t)(((UINT16)imu_raw[11] << 8) | imu_raw[10]), (UINT16)up);
#endif
    imu_pkt[0] = (UINT8)(up >> 24);
    imu_pkt[1] = (UINT8)(up >> 16);
    imu_pkt[2] = (UINT8)(up >> 8);
    imu_pkt[3] = (UINT8)up;
    imu_pkt[IMU_PKT_SEQ + 0] = (UINT8)(imu_seq >> 24);
    imu_pkt[IMU_PKT_SEQ + 1] = (UINT8)(imu_seq >> 16);
    imu_pkt[IMU_PKT_SEQ + 2] = (UINT8)(imu_seq >> 8);
    imu_pkt[IMU_PKT_SEQ + 3] = (UINT8)imu_seq;
    imu_seq++;
    for (i = 0; i < 6; i++)
    {
        imu_pkt[IMU_PKT_ACC + i]  = imu_raw[6 + (i ^ 1)];   /* accel, raw[6..11] LE -> BE */
        imu_pkt[IMU_PKT_GYRO + i] = imu_raw[i ^ 1];         /* gyro,  raw[0..5]  LE -> BE */
    }
    imu_has   = 1;
    imu_ready = 1;
}

static void imu_send(void)
{
#ifdef ITAP_FAST_SENSOR
    bm_notify_send(imu_pkt, 15);
#else
    bm_notify_send(imu_pkt, IMU_PKT_LEN);
#endif
    imu_ready = 0;
}

#ifndef ITAP_MOTION_HR
static int imu_val(UINT8 i)                 /* i: 0..2 accel XYZ, 3..5 gyro XYZ */
{
    __xdata UINT8 *p = imu_pkt + (i < 3 ? IMU_PKT_ACC : IMU_PKT_GYRO) + 2 * (i % 3);
    return ((int)p[0] << 8) | p[1];
}

static void ln_i(int v, UINT8 w)            /* right-aligned signed decimal, dashes when there is no sample */
{
    UINT8  i = 0, neg = 0;
    UINT16 u = (UINT16)v;
    if (!imu_has)
    {
        while (w--) ln_c('-');
        return;
    }
    if (v < 0) { neg = 1; u = 0 - u; }
    do { ln_dig[i++] = '0' + (UINT8)(u % 10); u /= 10; } while (u);
    w -= i + neg;
    while (w--) ln_c(' ');
    if (neg) ln_c('-');
    while (i) ln_c(ln_dig[--i]);
}

static void oled_build(UINT8 row, UINT8 state)
{
    UINT8 st = disp[PKT_ST];
    ln_n = 0;
    if (oled_page && row >= 3)              /* IMU page: rows 0..2 (name, BLE, UP) stay */
    {
        switch (row)
        {
        case 3:  ln_s(imu_ok ? "IMU:OK" : "IMU:NO SENSOR"); break;
        case 4:  ln_s("AX:"); ln_i(imu_val(0), 6); ln_s(" GX:"); ln_i(imu_val(3), 6); break;
        case 5:  ln_s("AY:"); ln_i(imu_val(1), 6); ln_s(" GY:"); ln_i(imu_val(4), 6); break;
        case 6:  ln_s("AZ:"); ln_i(imu_val(2), 6); ln_s(" GZ:"); ln_i(imu_val(5), 6); break;
        default: break;
        }
    }
    else
    switch (row)
    {
    case 0:
        ln_s((__code const char *)bm_name);
        ln_s(" BM:"); ln_s(g_frm_any ? "OK" : (g_rx_any ? "RX" : "--"));   /* -- nothing on P0.2, RX bytes but no valid frame */   /* BM7701 answers on UART1 */
        break;
    case 1:
        ln_s("BLE:");
        if      (state <= ST_FEATURE)     ln_s("INIT");
        else if (state <= ST_WAIT_CONN)   ln_s("ADVERTISING");
        else if (state == ST_WAIT_NOTIFY) ln_s("CONNECTED");
        else if (state == ST_NOTIFY)      ln_s("STREAMING");
        else                              ln_s("DISCONNECTED");
        break;
    case 2:
        ln_s("UP  ");   ln_time(uptime_ms());
#if PPG_SEND_START
        ln_s(" M:");    ln_c(ppg_mode_ch);      /* module mode at boot: '=' already default, 'W' written */
#endif
        break;
    case 3:
        ln_s("PPG:");
        if (!disp_ok)      ln_s("NO SIGNAL");
        else
        {
            if (!ppg_ok)       ln_s("LOST");            /* time below = last sample */
            else if (st == 0)  ln_s("FNG-OFF");
            else if (st == 1)  ln_s("FNG-ON");
            else if (st == 2)  ln_s("DATA");
            else { ln_s("ST "); ln_u(st, 3); }
            ln_s(" @");
            ln_time(((UINT32)disp[0] << 24) | ((UINT32)disp[1] << 16) | ((UINT32)disp[2] << 8) | disp[3]);
        }
        break;
    case 4:
        ln_s("HR:");    ln_u(disp[PKT_ST + 2], 3);
        ln_s(" SpO2:"); ln_u(disp[PKT_ST + 1], 3);
        break;
    case 5:
        ln_s("bpi:");   ln_u(disp[PKT_ST + 3], 3);
        ln_s(" hrv:");  ln_u(disp[PKT_ST + 4], 3);
        break;
    case 6:
        ln_s("amp:");   ln_u(disp[PKT_ST + 5], 3);
        ln_s(" sys:");  ln_u(disp[PKT_ST + 6], 3);
        break;
    default:
        ln_s("P1:");  ln_u(((UINT16)disp[PKT_ST + 7] << 8) | disp[PKT_ST + 8], 5);
        ln_s(" P2:"); ln_u(((UINT16)disp[PKT_ST + 9] << 8) | disp[PKT_ST + 10], 5);
        break;
    }
    while (ln_n < OLED_COLS) ln[ln_n++] = ' ';
}

static void oled_power(__bit on)            /* SH1106 display off 0xAE / on 0xAF, RAM is kept */
{
    if (!(oled_begin(0x00) && i2c_tx(on ? 0xAF : 0xAE, 0x28)))
        oled_ok = 0;                        /* re-init after OLED_RETRY_MS (turns the panel on) */
    i2c_stop();
}

static void key_task(void)
{
    UINT16 t = now_ms();

    if (key_page)
    {
        key_page = 0;
        if ((UINT16)(t - t_key_page) >= KEY_DEBOUNCE_MS)
        {
            t_key_page = t;
            oled_page = !oled_page;
        }
    }
    if (key_oled)
    {
        key_oled = 0;
        if ((UINT16)(t - t_key_oled) >= KEY_DEBOUNCE_MS)
        {
            t_key_oled = t;
            oled_off = !oled_off;
            if (oled_ok) oled_power(!oled_off);
        }
    }
}

static void oled_task(UINT8 state)
{
    UINT8 i;
    UINT16 t = now_ms();

    if (oled_off) return;                   /* panel is off: no I2C traffic, no re-init */
    if (!oled_ok)
    {
        if ((UINT16)(t - t_oled) >= OLED_RETRY_MS)
        {
            t_oled = t;
            oled_hw_init();
        }
        return;
    }
    if ((UINT16)(t - t_oled) < OLED_ROW_MS) return;
    t_oled = t;

    ppg_ok = disp_ok && (UINT16)(t - disp_t) < PPG_LOST_MS;
    oled_build(oled_row, state);
    for (i = 0; i < OLED_COLS; i++)
        if (ln[i] != ln_cache[oled_row][i]) break;
    if (i < OLED_COLS)                                      /* changed */
    {
        if (oled_row_draw(oled_row))
            for (i = 0; i < OLED_COLS; i++) ln_cache[oled_row][i] = ln[i];
        else
            oled_ok = 0;                                    /* re-init after OLED_RETRY_MS */
    }
    oled_row = (oled_row + 1) & (OLED_ROWS - 1);
}

#endif
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
    key_init();                                 /* P3.0 page, P1.7 OLED on / off */
    set_EA;

    t_state = now_ms();
#ifdef ITAP_MOTION_HR
    mh_reset(&motion, 1);
#endif
    t_oled  = t_state - (OLED_RETRY_MS - 100);  /* OLED init ~100 ms after power-up */
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

        /* keep the last complete sample for the OLED (ppg_ready means tx_pkt is stable) */
        if (ppg_ready)
        {
            UINT8 k;
            for (k = 0; k < PKT_LEN; k++) disp[k] = tx_pkt[k];   /* incl. sensor_time_ms [0..3] */
            disp_t  = t;
            disp_ok = 1;
        }

#ifdef ITAP_MOTION_HR
        motion_task();
#endif
        /* not streaming: drop the sample so the first frame after subscription is fresh */
        if (state != ST_WAIT_NOTIFY && state != ST_NOTIFY) ppg_ready = 0;
        if (state != ST_NOTIFY) imu_ready = 0;      /* the probe is always a PPG frame */

#ifdef ITAP_CAPTURE
        if (state == ST_NOTIFY) capture_collect();
        else capture_head = capture_tail = capture_count = 0;
#endif
        imu_task();                                 /* also runs without a BLE link: feeds the OLED */

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
                bm_set_name();
                state = ST_NAME;
            }
            break;

        case ST_NAME:
            if (cmd_state == CMD_WAIT) break;
            /* OK or fail: continue anyway, the module keeps its previous name */
            cmd_state = CMD_IDLE;
            bm_set_auto_status();
            state = ST_FEATURE;
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
#ifdef ITAP_MOTION_HR
            if (motion_has)
#else
            if (ppg_ready)
#endif
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
#ifdef ITAP_MOTION_HR
            if (motion_ready) ppg_send();
            else if (imu_ready && (UINT16)(t - motion_acc_sent) >= 1000) {
                imu_send(); motion_acc_sent = t;
            }
#elif defined(ITAP_CAPTURE)
            /* Alternate when both are pending; neither stream can starve. */
            if (imu_ready && (capture_acc_turn || !capture_count)) {
                imu_send();
                capture_acc_turn = 0;
            } else if (capture_count) {
                capture_send();
                capture_acc_turn = 1;
            }
#else
            if (imu_ready) imu_send();              /* 1 Hz, the PPG sample of this round is dropped */
            else if (ppg_ready) ppg_send();         /* one frame per PPG sample */
#endif
            break;

        case ST_DISC_WAIT:                              /* no command for 5 ms after disconnect */
            if ((UINT16)(t - t_state) >= DISC_WAIT_MS)
            {
                bm_parser_reset();
                bm_set_name();                          /* re-apply in case the soft reset cleared it */
                state = ST_NAME;
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

#if USE_OLED
        key_task();
        oled_task(state);
#endif

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
