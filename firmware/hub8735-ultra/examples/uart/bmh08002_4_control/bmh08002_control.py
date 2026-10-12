"""Control BMH08002-4 through the HUB8735 UART3 bridge; requires pyserial.

Examples:
  python bmh08002_control.py COM5 start
  python bmh08002_control.py COM5 mode 4        # continuous upload, red light off
  python bmh08002_control.py COM5 monitor
  python bmh08002_control.py COM5 monitor --csv log.csv   # 另存 CSV 供後續分析
  python bmh08002_control.py COM5 plot --csv log.csv      # 即時波形（類似心電圖）
  python bmh08002_control.py COM5 measure                 # 量 10 秒，去極端值後輸出平均心率
  python bmh08002_control.py COM5 hr                      # 每 10 秒連續輸出心率（MCU 用的整數演算法）
"""

import argparse
import csv
import sys
import time
from collections import deque
from pathlib import Path

try:
    import serial
except ImportError as exc:
    raise SystemExit("請先安裝 pyserial：python -m pip install pyserial") from exc

HOST_BAUD = 115200
EEPROM_MODE = 0x01
EEPROM_INTERVAL = 0x0A  # unit: 4 ms
CONFIG_ADDRS = (
    ("mode", 0x01),
    ("ir_min", 0x02),
    ("ir_max", 0x04),
    ("red_min", 0x06),
    ("red_max", 0x08),
    ("interval(x4ms)", 0x0A),
)
WORK = {"start": 0, "stop": 1, "sleep": 2}


def frame(opcode, address=0, value=0):
    body = bytes((0xB1, opcode, address, (value >> 8) & 0xFF, value & 0xFF))
    return b"\x55" + body + bytes((sum(body) & 0xFF, 0xAA))


class Bridge:
    def __init__(self, port):
        self.ser = serial.Serial(port, HOST_BAUD, timeout=0.05)
        self.buf = bytearray()

    def read_frame(self, timeout):
        """Return next valid frame: 15B info (55 B0 ...) or 8B reply (55 B1 ...)."""
        end = time.monotonic() + timeout
        first = True
        while first or time.monotonic() < end:
            if not first:
                self.buf.extend(self.ser.read(self.ser.in_waiting or 1))
            first = False
            while True:
                start = self.buf.find(b"\x55")
                if start < 0:
                    self.buf.clear()
                    break
                del self.buf[:start]
                if len(self.buf) < 2:
                    break
                size = {0xB0: 15, 0xB1: 8}.get(self.buf[1])
                if size is None:
                    del self.buf[0]
                    continue
                if len(self.buf) < size:
                    break
                pkt = bytes(self.buf[:size])
                if pkt[-1] == 0xAA and (sum(pkt[1:-2]) & 0xFF) == pkt[-2]:
                    del self.buf[:size]
                    return pkt
                del self.buf[0]
        return None

    def request(self, opcode, address=0, value=0):
        self.ser.reset_input_buffer()
        self.buf.clear()
        self.ser.write(frame(opcode, address, value))
        deadline = time.monotonic() + 0.5
        while time.monotonic() < deadline:
            pkt = self.read_frame(deadline - time.monotonic())
            if pkt and pkt[1] == 0xB1 and pkt[2] == opcode:
                return pkt
        raise TimeoutError("模組無回應（檢查接線、UART3 與 38400 baud）")

    def read_eeprom(self, address):
        pkt = self.request(0x05, address)
        return (pkt[4] << 8) | pkt[5]

    def write_eeprom(self, address, value):
        self.request(0x04, address, value)


def ir_ppg(pkt):
    # PPG1 (infrared) = D8/D9 = packet[9:11], high byte first
    return (pkt[9] << 8) | pkt[10]


def red_ppg(pkt):
    # PPG2 (red) = D10/D11 = packet[11:13]; same field the PPG analyzer uses
    return (pkt[11] << 8) | pkt[12]


class CsvLog:
    """Append info packets to CSV: elapsed_s, unix_time, status, d0..d9, ir_ppg, red_ppg, raw_hex."""

    def __init__(self, path):
        new = not path.exists() or path.stat().st_size == 0
        self.file = open(path, "a", newline="", encoding="utf-8")
        self.writer = csv.writer(self.file)
        self.t0 = time.monotonic()
        if new:
            self.writer.writerow(["elapsed_s", "unix_time", "status"] + [f"d{i}" for i in range(10)] + ["ir_ppg", "red_ppg", "raw_hex"])

    def add(self, pkt):
        self.writer.writerow(
            [f"{time.monotonic() - self.t0:.3f}", f"{time.time():.3f}", pkt[2]] + list(pkt[3:13]) + [ir_ppg(pkt), red_ppg(pkt), hexs(pkt)]
        )
        self.file.flush()

    def close(self):
        self.file.close()


def run_plot(bridge, log, channel="red", fixed_range=None, window_s=8.0):
    pick = ir_ppg if channel == "ir" else red_ppg
    label = "紅外 PPG1" if channel == "ir" else "紅光 PPG2"
    import tkinter as tk

    bridge.ser.timeout = 0
    try:
        bridge.request(0x01, 0, WORK["start"])
    except TimeoutError:
        pass  # continuous-upload mode may already be streaming

    root = tk.Tk()
    root.title("BMH08002-4 即時波形（" + label + "）")
    root.geometry("900x480")
    info = tk.Label(root, text="等待資料…", anchor="w", font=("Segoe UI", 13))
    info.pack(fill="x", padx=12, pady=8)
    canvas = tk.Canvas(root, background="#101820", highlightthickness=0)
    canvas.pack(fill="both", expand=True, padx=12, pady=(0, 12))

    t0 = time.monotonic()
    samples = deque()  # (t, red)
    state = {"status": None, "last": None, "count": 0, "mean": None, "span": None}

    def poll():
        while True:
            pkt = bridge.read_frame(0.001)
            if not pkt:
                break
            if pkt[1] != 0xB0:
                continue
            now = time.monotonic() - t0
            red = pick(pkt)
            state["status"], state["last"] = pkt[2], now
            state["count"] += 1
            # slow baseline so the pulse wave sits around zero like an ECG trace
            state["mean"] = red if state["mean"] is None else state["mean"] + (red - state["mean"]) * 0.02
            samples.append((now, red - state["mean"]))
            if log:
                log.add(pkt)
        now = time.monotonic() - t0
        while samples and now - samples[0][0] > window_s:
            samples.popleft()
        root.after(10, poll)

    def draw():
        canvas.delete("all")
        w, h = max(canvas.winfo_width(), 2), max(canvas.winfo_height(), 2)
        now = time.monotonic() - t0
        for k in range(int(window_s) + 1):
            x = w * (1 - k / window_s)
            canvas.create_line(x, 0, x, h, fill="#1c2b36")
        if len(samples) > 1:
            # Y axis is symmetric around the baseline: fixed with --range, otherwise it
            # grows at once but shrinks slowly so the trace does not "breathe".
            if fixed_range:
                span = float(fixed_range)
            else:
                need = max(abs(v) for _, v in samples) * 1.1 or 1.0
                span = state["span"] or need
                span = need if need > span else span + (need - span) * 0.01
                state["span"] = span
            lo, hi = -span, span
            pts = []
            for t, v in samples:
                pts += [w * (1 - (now - t) / window_s), h - 10 - (v - lo) / (hi - lo) * (h - 20)]
            canvas.create_line(*pts, fill="#59d8a5", width=2)
        if state["last"] is None:
            text = "等待封包；確認模組為連續上傳模式（mode 4）"
        elif now - state["last"] > 2:
            text = "超過 2 秒沒有新封包"
        else:
            names = {0: "未偵測手指", 1: "手指已放置／收集中", 2: "訊號有效"}
            rate = state["count"] / max(now, 1e-3)
            text = f"{names.get(state['status'], state['status'])}    封包 {state['count']}（約 {rate:.0f}/s）"
        info.config(text=text)
        root.after(40, draw)

    root.bind("<Escape>", lambda _e: root.destroy())
    poll()
    draw()
    root.mainloop()
    try:
        bridge.ser.timeout = 0.05
        bridge.request(0x01, 0, WORK["stop"])
    except (TimeoutError, serial.SerialException, OSError):
        pass


def run_measure(bridge, log, seconds, invert):
    """Record `seconds` of continuous-upload PPG, then print an outlier-trimmed mean heart rate."""
    from ppg_hr_from_csv import find_beats, robust_bpm

    try:
        bridge.request(0x01, 0, WORK["start"])
    except TimeoutError:
        pass  # continuous-upload mode may already be streaming
    bridge.ser.reset_input_buffer()
    bridge.buf.clear()
    print("量測 %g 秒，請保持手指穩定…" % seconds)
    ir, red = [], []
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        pkt = bridge.read_frame(0.2)
        if pkt and pkt[1] == 0xB0:
            ir.append(ir_ppg(pkt))
            red.append(red_ppg(pkt))
            if log:
                log.add(pkt)
    wall = time.monotonic() - t0
    n = len(ir)
    if n < 10:
        print("只收到 %d 筆，無法計算" % n)
        return 1
    # the module's real packet rate is measured, not assumed (datasheet says 20 ms)
    sample_s = wall / n
    print("收到 %d 筆，實測取樣間隔 %.1f ms（約 %.1f Hz）" % (n, sample_s * 1000, 1 / sample_s))
    for label, vals in (("PPG1 紅外", ir), ("PPG2 紅光", red)):
        peaks = find_beats(vals, invert, sample_s)
        ivs = [(peaks[k] - peaks[k - 1]) * sample_s for k in range(1, len(peaks))]
        res = robust_bpm(ivs)
        if res is None:
            print("%s：脈搏不足，無法計算（%d 個脈搏）" % (label, len(peaks)))
        else:
            bpm, kept, valid = res
            print("%s：%.1f BPM（%d 個間隔，剔除 %d 個極端值）" % (label, bpm, kept, valid - kept))
    return 0


def run_hr(bridge, log, cycles):
    """Continuous 10 s cycles with the integer algorithm that will be ported to the N76E003."""
    from ppg_hr_int import HeartRate, WINDOW_MS

    try:
        bridge.request(0x01, 0, WORK["start"])
    except TimeoutError:
        pass
    bridge.ser.reset_input_buffer()
    bridge.buf.clear()
    print("每 %d 秒輸出一次（0 = 無效）；Ctrl+C 結束" % (WINDOW_MS // 1000))
    hr = HeartRate()
    t0 = time.monotonic()
    n = done = 0
    try:
        while cycles is None or done < cycles:
            pkt = bridge.read_frame(0.5)
            if not pkt or pkt[1] != 0xB0:
                continue
            n += 1
            # smoothed clock: packets x measured period, so host timer jitter does not reach the beat intervals
            period = (time.monotonic() - t0) / n if n >= 30 else 0.028
            res = hr.feed(ir_ppg(pkt), red_ppg(pkt), int(n * period * 1000) & 0xFFFF)
            if log:
                log.add(pkt)
            if res:
                done += 1
                bpm, b1, b2 = res
                print("%s  心率 %s    (PPG1 %d / PPG2 %d)  取樣 %.1f ms" % (
                    time.strftime("%H:%M:%S"), bpm if bpm else "0（無效）", b1, b2, period * 1000), flush=True)
    except KeyboardInterrupt:
        pass
    return 0


def hexs(data):
    return " ".join(f"{b:02X}" for b in data)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in WORK:
        sub.add_parser(name)
    sub.add_parser("cal", help="校正手指靈敏度")
    sub.add_parser("wave", help="更新波形 (0xB1 0x02)")
    for name, text in (
        ("info", "詢問式模式：要一筆資料"),
        ("monitor", "持續列印模組輸出的 15B 封包，Ctrl+C 結束"),
        ("plot", "開始量測並顯示即時波形（紅光 PPG），Esc 或關閉視窗結束"),
        ("hr", "連續每 10 秒輸出一次心率（整數演算法，PPG1/PPG2 交叉檢查）"),
        ("measure", "量測一次（預設 10 秒），去掉極端值後輸出平均心率"),
    ):
        sp = sub.add_parser(name, help=text)
        sp.add_argument("--csv", type=Path, help="將收到的資料附加寫入 CSV")
        if name == "hr":
            sp.add_argument("--cycles", type=int, help="量測幾個週期後結束（預設持續到 Ctrl+C）")
        if name == "measure":
            sp.add_argument("--seconds", type=float, default=10.0, help="量測秒數（預設 10）")
            sp.add_argument("--invert", action="store_true", help="波形上下顛倒時使用")
        if name == "plot":
            sp.add_argument("--range", type=float, dest="yrange", help="固定 Y 軸為 ±N（不自動縮放）")
            sp.add_argument("--channel", choices=("red", "ir"), default="red", help="PPG2 紅光（預設）或 PPG1 紅外")
    sub.add_parser("config", help="讀出模式、紅外/紅光光強上下限、定時間隔")
    m = sub.add_parser("mode", help="寫入 ModeConfig (0x00-0x0F；bit3=回應更新滿量程，0x0C=連續上傳+回應更新)")
    m.add_argument("code", type=lambda x: int(x, 0))
    i = sub.add_parser("interval", help="定時傳送間隔，單位 4 ms")
    i.add_argument("units", type=lambda x: int(x, 0))
    r = sub.add_parser("read", help="讀 EEPROM")
    r.add_argument("addr", type=lambda x: int(x, 0))
    w = sub.add_parser("write", help="寫 EEPROM")
    w.add_argument("addr", type=lambda x: int(x, 0))
    w.add_argument("value", type=lambda x: int(x, 0))
    a = p.parse_args()

    if hasattr(a, "code") and not 0 <= a.code <= 0x0F:
        p.error("mode 需為 0x00-0x0F（bit4 為硬體佈局，勿設）")
    if hasattr(a, "units") and not 0 <= a.units <= 0xFFFF:
        p.error("interval 需為 0-65535")

    bridge = Bridge(a.port)
    log = CsvLog(a.csv) if getattr(a, "csv", None) else None
    try:
        if a.cmd in WORK:
            pkt = bridge.request(0x01, 0, WORK[a.cmd])
            ok = pkt[5] == WORK[a.cmd]
            print("OK" if ok else "FAIL", hexs(pkt))
            return 0 if ok else 1
        if a.cmd == "cal":
            print(hexs(bridge.request(0x06)))
        elif a.cmd == "wave":
            print(hexs(bridge.request(0x02)))
        elif a.cmd == "info":
            bridge.ser.reset_input_buffer()
            bridge.buf.clear()
            bridge.ser.write(frame(0x03))
            pkt = bridge.read_frame(1.0)
            if not pkt or pkt[1] != 0xB0:
                raise TimeoutError("沒有收到資料封包")
            print(f"status={pkt[2]} data={hexs(pkt[3:13])}")
            if log:
                log.add(pkt)
        elif a.cmd == "monitor":
            while True:
                pkt = bridge.read_frame(1.0)
                if pkt and pkt[1] == 0xB0:
                    print(f"{time.strftime('%H:%M:%S')} status={pkt[2]} data={hexs(pkt[3:13])}", flush=True)
                    if log:
                        log.add(pkt)
        elif a.cmd == "hr":
            return run_hr(bridge, log, a.cycles)
        elif a.cmd == "measure":
            return run_measure(bridge, log, a.seconds, a.invert)
        elif a.cmd == "plot":
            run_plot(bridge, log, a.channel, a.yrange)
        elif a.cmd == "config":
            for name, addr in CONFIG_ADDRS:
                print("%-15s EEPROM[0x%02X] = %d" % (name, addr, bridge.read_eeprom(addr)))
        elif a.cmd == "mode":
            bridge.write_eeprom(EEPROM_MODE, a.code)
            print("mode =", f"0x{bridge.read_eeprom(EEPROM_MODE) & 0xFF:02X}")
        elif a.cmd == "interval":
            bridge.write_eeprom(EEPROM_INTERVAL, a.units)
            print("interval =", bridge.read_eeprom(EEPROM_INTERVAL), "x 4 ms")
        elif a.cmd == "read":
            print(f"EEPROM[0x{a.addr:02X}] = 0x{bridge.read_eeprom(a.addr):04X}")
        elif a.cmd == "write":
            bridge.write_eeprom(a.addr, a.value)
            print("written")
    except (TimeoutError, KeyboardInterrupt) as e:
        print(e if isinstance(e, TimeoutError) else "", file=sys.stderr)
        return 1 if isinstance(e, TimeoutError) else 0
    finally:
        bridge.ser.close()
        if log:
            log.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
