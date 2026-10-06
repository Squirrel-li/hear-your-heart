/*---------------------------------------------------------------------------------------------------------*/
/*  N76E003 UART1  <->  BM7701-00-1 (BLE UART module)                                                       */
/*                                                                                                         */
/*  Flow:                                                                                                  */
/*    1. Power-up wait 100 ms                                                                              */
/*    2. Feature: enable AUTO_SEND_STATUS (RAM only)  77 07 10 10 00 80 00 00 00  -> 78 03 10 10 00     */
/*    3. Advertise ON                                 77 04 00 07 00 01           -> 78 03 00 07 00     */
/*    4. Wait for connection                                                                               */
/*         status event / query reply                 78 LEN xx 00 00 S ...   (S bit0 = 1 connected)      */
/*         poll every 1 s                             77 03 00 00 00                                       */
/*    5. Connected: wait for peer to enable FFF1 notify                                                    */
/*         N76E003 cannot enable notify itself; the phone/PC writes the CCCD.                              */
/*         Probe every 500 ms:                        77 LEN 20 F1 FF DATA                                 */
/*              78 03 20 F1 FF  -> notify ON (data delivered to BLE stack)                                 */
/*              78 03 26 F1 FF  -> notify not enabled yet (status low nibble != 0)                         */
/*         Optional app handshake: peer writes 0x01 to FFF2 -> 78 04 00 F2 FF 01                          */
/*    6. Notify ON: send one packet every SEND_PERIOD_MS                                                   */
/*         Packet (10 bytes, same as scripts\bm7701.ps1 counter): 42 4D seq_lo seq_hi 00 01 02 03 04 05   */
/*         ACK error -> peer unsubscribed -> back to step 5                                                */
/*    7. Disconnect (S bit0 = 0) -> advertise ON again -> step 4                                           */
/*                                                                                                         */
/*  P1.2 LED: slow blink = advertising, fast blink = connected (no notify), ON = notify streaming          */
/*                                                                                                         */
/*  !!! UART1 pins are shared with the Nu-Link debug pins; disconnect Nu-Link / do not run in debug mode. */
/*  !!! 16 MHz HIRC + Timer3 gives 111.1 kbps for "115200" (-3.5 %). If bytes get lost, trim HIRC to      */
/*      16.6 MHz (BSP sample IAP_MoidfyHIRC + define FOSC_166000) or lower both sides' baud rate.          */
/*---------------------------------------------------------------------------------------------------------*/
#include "N76E003.h"
#include "Common.h"
#include "Delay.h"
#include "SFR_Macro.h"
#include "Function_define.h"

/*============================== configuration ==============================*/
#define BM_BAUD             115200
#define BOOT_WAIT_MS        100     /* BM7701 needs >= 60 ms after power-up/reset */
#define CMD_TIMEOUT_MS      300     /* ACK timeout (first RF calibration can be slow) */
#define STATUS_POLL_MS      1000    /* poll connection status while waiting */
#define NOTIFY_PROBE_MS     500     /* try FFF1 send while waiting for subscription */
#define SEND_PERIOD_MS      1000    /* data period once notify is on */
#define ADV_RETRY_MS        500
#define FRAME_GAP_MS        20      /* drop an incomplete frame after this gap */

/*============================== BM7701 protocol ============================*/
#define BM_CMD              0x77
#define BM_EVT              0x78
#define BM_TYPE_STATUS      0x0000
#define BM_TYPE_ADV_EN      0x0007
#define BM_TYPE_FEATURE     0x0010
#define BM_TYPE_FFF1        0xFFF1      /* notify: MCU -> peer      */
#define BM_TYPE_FFF2        0xFFF2      /* write w/o rsp: peer -> MCU */
#define BM_FLAG_NONE        0x00
#define BM_FLAG_FEATURE_OR  0x10
#define BM_FLAG_SEND        0x20
#define BM_FEATURE_AUTO_STATUS  0x80    /* 0x00000080, little-endian byte 0 */

/*============================== state ======================================*/
#define ST_BOOT             0
#define ST_FEATURE          1
#define ST_ADV_ON           2
#define ST_ADV_RETRY        3
#define ST_WAIT_CONN        4
#define ST_WAIT_NOTIFY      5
#define ST_NOTIFY           6

#define CMD_IDLE            0
#define CMD_WAIT            1
#define CMD_OK              2
#define CMD_FAIL            3

/*============================== 1 ms tick (Timer0) =========================*/
/* Timer3 is the UART1 baud generator, so Timer0 is used for timing.          */
/* Fsys/12 = 1.333 MHz, 1 ms = 1333 counts -> reload 65536-1333 = 0xFACB      */
#define T0_RELOAD_H         0xFA
#define T0_RELOAD_L         0xCB

volatile UINT16 g_ms = 0;

void Timer0_ISR(void) interrupt 1
{
    TH0 = T0_RELOAD_H;
    TL0 = T0_RELOAD_L;
    g_ms++;
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

UINT8  cmd_state  = CMD_IDLE;
UINT16 cmd_type   = 0;
UINT8  cmd_flag   = 0;
UINT8  cmd_status = 0;                      /* low nibble of ACK status */
UINT16 cmd_t0     = 0;

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

/* send: 77 LEN FLAG TYPE_L TYPE_H DATA... */
static void bm_send_cmd(UINT16 type, UINT8 flag, UINT8 *dat, UINT8 len)
{
    UINT8 i;
    bm_putc(BM_CMD);
    bm_putc(len + 3);
    bm_putc(flag);
    bm_putc((UINT8)(type & 0xFF));
    bm_putc((UINT8)(type >> 8));
    for (i = 0; i < len; i++) bm_putc(dat[i]);

    cmd_type   = type;
    cmd_flag   = flag;
    cmd_status = 0;
    cmd_t0     = now_ms();
    cmd_state  = CMD_WAIT;
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

static void bm_handle_frame(void)
{
    UINT8  st   = frm[2];
    UINT16 type = (UINT16)frm[3] | ((UINT16)frm[4] << 8);
    UINT8  dlen = frm[1] - 3;

    /* unsolicited data from peer, never an ACK */
    if (type == BM_TYPE_FFF2)
    {
        on_ble_write(&frm[5], dlen);
        return;
    }

    /* status event (auto) or status query reply: data[0] bit0 = connected */
    if (type == BM_TYPE_STATUS && dlen >= 1)
    {
        g_connected = (frm[5] & 0x01) ? 1 : 0;
        if (!g_connected)
        {
            g_notify    = 0;
            g_app_ready = 0;
        }
    }

    /* ACK for the pending command */
    if (cmd_state == CMD_WAIT && type == cmd_type && (st & 0xF0) == (cmd_flag & 0xF0))
    {
        cmd_status = st & 0x0F;
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

    if (cmd_state == CMD_WAIT && (UINT16)(t - cmd_t0) > CMD_TIMEOUT_MS)
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
    bm_send_cmd(BM_TYPE_FEATURE, BM_FLAG_FEATURE_OR, d, 4);        /* RAM only, not saved to flash */
}

static void bm_adv(UINT8 on)
{
    UINT8 d[1];
    d[0] = on ? 1 : 0;
    bm_send_cmd(BM_TYPE_ADV_EN, BM_FLAG_NONE, d, 1);
}

static void bm_query_status(void)
{
    bm_send_cmd(BM_TYPE_STATUS, BM_FLAG_NONE, 0, 0);
}

static void bm_notify_send(UINT8 *dat, UINT8 len)                  /* len <= 20 recommended */
{
    bm_send_cmd(BM_TYPE_FFF1, BM_FLAG_SEND, dat, len);
}

/*============================== application data ===========================*/
UINT16 g_seq = 0;
xdata UINT8 tx_pkt[10];

/* 10-byte packet, same format as scripts\bm7701.ps1 counter: 42 4D seq_lo seq_hi 00..05 */
/* Replace with your sensor data later.                                                   */
static void build_packet(void)
{
    UINT8 i;
    tx_pkt[0] = 0x42;
    tx_pkt[1] = 0x4D;
    tx_pkt[2] = (UINT8)(g_seq & 0xFF);
    tx_pkt[3] = (UINT8)(g_seq >> 8);
    for (i = 0; i < 6; i++) tx_pkt[4 + i] = i;
}

/*============================== main =======================================*/
void main(void)
{
    UINT8  state = ST_BOOT;
    UINT16 t_state, t_led, t;

    P12_PushPull_Mode;
    P12 = 0;

    InitialUART1_Timer3(BM_BAUD);
    Timer0_Tick_Init();
    set_ES_1;                                   /* UART1 interrupt */
    set_EA;

    t_state = now_ms();
    t_led   = t_state;

    while (1)
    {
        bm_poll();
        t = now_ms();

        /* disconnect handling (any state after advertising starts) */
        if (state >= ST_WAIT_NOTIFY && !g_connected && cmd_state != CMD_WAIT)
        {
            g_notify = 0;
            bm_adv(1);                          /* restart advertising */
            state = ST_ADV_ON;
            t_state = t;
        }

        switch (state)
        {
        case ST_BOOT:
            if ((UINT16)(t - t_state) >= BOOT_WAIT_MS)
            {
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

        case ST_ADV_ON:
            if (cmd_state == CMD_WAIT) break;
            if (cmd_state == CMD_OK || g_connected)     /* already connected also OK */
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

        case ST_ADV_RETRY:
            if ((UINT16)(t - t_state) >= ADV_RETRY_MS)
            {
                bm_adv(1);
                state = ST_ADV_ON;
            }
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
                g_seq++;
                state = ST_NOTIFY;
                t_state = t;
                break;
            }
            cmd_state = CMD_IDLE;                       /* 0x26 = not subscribed yet */
            if (g_app_ready || (UINT16)(t - t_state) >= NOTIFY_PROBE_MS)
            {
                g_app_ready = 0;
                build_packet();
                bm_notify_send(tx_pkt, sizeof(tx_pkt));
                t_state = t;
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
            if (cmd_state == CMD_OK && cmd_type == BM_TYPE_FFF1) g_seq++;
            cmd_state = CMD_IDLE;
            if ((UINT16)(t - t_state) >= SEND_PERIOD_MS)
            {
                build_packet();
                bm_notify_send(tx_pkt, sizeof(tx_pkt));
                t_state = t;
            }
            break;

        default:
            state = ST_BOOT;
            t_state = t;
            break;
        }

        /* handle data written by the peer to FFF2 */
        if (fff2_new)
        {
            fff2_new = 0;
            /* TODO: parse fff2_buf[0..fff2_len-1] (commands from phone) */
        }

        /* LED */
        if (g_notify)
        {
            P12 = 1;
        }
        else if ((UINT16)(t - t_led) >= (g_connected ? 150 : 600))
        {
            P12 = ~P12;
            t_led = t;
        }
    }
}
