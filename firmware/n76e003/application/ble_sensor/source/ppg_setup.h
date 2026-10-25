/* BMH08002-4 boot setup, shared by ble_sensor (and its capture / motion builds) and ble_sensor_hr.    */
/* The settings live in the module's EEPROM and a module may move between boards, so every register   */
/* is read first and written only when it differs, then "start measure" is sent. A write takes 50 ms  */
/* and no command may follow during that time; other commands need 6 ms.                            */
/* LED intensity limits (datasheet 0..30000, min < max, no documented factory default) were tuned on   */
/* the capture page (2026-10-25): IR 24000..28000, red 29000..30000 gave status 2 and a clean pulse;  */
/* 18000..30000 for both left the module at status 1 (finger, no data). Max is written before min so  */
/* a module holding min > max never sees a worse pair.                                               */
/* ../test_ppg_setup.c checks it on a PC.                                                             */
/* Include after: UINT8/16, __code, __bit, PPG_MODE_VALUE, now_ms(), SBUF, TI, RI, clr_ES, set_ES.     */
#define PPG_REPLY_MS        100
#define PPG_IR_MIN          24000           /* EEPROM 0x02 */
#define PPG_IR_MAX          28000           /* EEPROM 0x04 */
#define PPG_RED_MIN         29000           /* EEPROM 0x06 */
#define PPG_RED_MAX         30000           /* EEPROM 0x08 */
#define PPG_REGS            5
__code UINT8  ppg_reg_addr[PPG_REGS] = { 0x01, 0x04, 0x02, 0x08, 0x06 };
__code UINT16 ppg_reg_val[PPG_REGS]  = { PPG_MODE_VALUE, PPG_IR_MAX, PPG_IR_MIN, PPG_RED_MAX, PPG_RED_MIN };
UINT8 ppg_mode_ch = '?';                    /* shown on the OLED: '=' all already right, 'W' something was written */

static void ppg_putc(UINT8 c)
{
    TI = 0;
    SBUF = c;
    while (TI == 0);
}

static void ppg_cmd(UINT8 op, UINT8 addr, UINT16 val, UINT16 wait_ms)   /* UART0 interrupt must be off */
{
    UINT16 t0;
    ppg_putc(0x55);
    ppg_putc(0xB1);
    ppg_putc(op);
    ppg_putc(addr);
    ppg_putc((UINT8)(val >> 8));
    ppg_putc((UINT8)val);
    ppg_putc((UINT8)(0xB1 + op + addr + (UINT8)(val >> 8) + (UINT8)val));
    ppg_putc(0xAA);
    TI = 0;
    t0 = now_ms();
    while ((UINT16)(now_ms() - t0) < wait_ms);
}

/* 1 = EEPROM addr holds val. No answer within PPG_REPLY_MS counts as "not right". The reply is      */
/* 55 B1 05 addr hi lo CS AA; a streaming module sends 55 B0 frames in between, so look for the       */
/* header byte by byte.                                                                              */
static __bit ppg_reg_ok(UINT8 addr, UINT16 val)
{
    UINT8 st = 0, c, hi = 0, lo = 0, want;
    UINT16 t0;

    RI = 0;
    ppg_cmd(0x05, addr, 0, 0);
    t0 = now_ms();
    while ((UINT16)(now_ms() - t0) < PPG_REPLY_MS)
    {
        if (!RI) continue;
        c = SBUF;
        RI = 0;
        if (st < 4)
        {
            want = st == 0 ? 0x55 : st == 1 ? 0xB1 : st == 2 ? 0x05 : addr;
            st = (c == want) ? st + 1 : (c == 0x55 ? 1 : 0);
        }
        else if (st == 4) { hi = c; st = 5; }
        else if (st == 5) { lo = c; st = 6; }
        else if (st == 6) st = (c == (UINT8)(0xB1 + 0x05 + addr + hi + lo)) ? 7 : 0;
        else if (c == 0xAA) return hi == (UINT8)(val >> 8) && lo == (UINT8)val;
        else st = 0;
    }
    return 0;
}

static void ppg_start(void)
{
    UINT8 i;
    clr_ES;
    ppg_mode_ch = '=';
    for (i = 0; i < PPG_REGS; i++)
        if (!ppg_reg_ok(ppg_reg_addr[i], ppg_reg_val[i]))
        {
            ppg_cmd(0x04, ppg_reg_addr[i], ppg_reg_val[i], 60);
            ppg_mode_ch = 'W';
        }
    if (ppg_mode_ch == 'W') ppg_cmd(0x01, 0x00, 0x0001, 10);   /* stop, then start with the new settings */
    ppg_cmd(0x01, 0x00, 0x0000, 10);
    RI = 0;                                 /* drop the replies, the parser starts clean */
    set_ES;
}
