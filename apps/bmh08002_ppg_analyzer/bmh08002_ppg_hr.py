"""Live BMH08002-4 red-PPG heart-rate viewer; requires pyserial."""

import argparse
import csv
import statistics
import time
import tkinter as tk
from collections import deque

try:
    import serial
except ImportError as exc:
    raise SystemExit("請先安裝 pyserial：python -m pip install pyserial") from exc


BAUD = 38400
SAMPLE_HZ = 50
PACKET_LEN = 15


class PacketParser:
    def __init__(self):
        self.buffer = bytearray()
        self.bad_packets = 0

    def feed(self, data):
        self.buffer.extend(data)
        packets = []
        while True:
            start = self.buffer.find(b"\x55\xB0")
            if start < 0:
                self.buffer[:] = b"\x55" if self.buffer[-1:] == b"\x55" else b""
                break
            if start:
                del self.buffer[:start]
            if len(self.buffer) < PACKET_LEN:
                break
            packet = bytes(self.buffer[:PACKET_LEN])
            if packet[14] != 0xAA or (sum(packet[1:13]) & 0xFF) != packet[13]:
                del self.buffer[0]
                self.bad_packets += 1
                continue
            del self.buffer[:PACKET_LEN]
            packets.append(packet)
        return packets


def command(opcode, address=0, value=0):
    hi, lo = (value >> 8) & 0xFF, value & 0xFF
    body = bytes((0xB1, opcode, address, hi, lo))
    return b"\x55" + body + bytes((sum(body) & 0xFF, 0xAA))


class HeartRate:
    def __init__(self):
        self.baseline = None
        self.history = deque(maxlen=5 * SAMPLE_HZ)
        self.local = deque(maxlen=3)
        self.intervals = deque(maxlen=5)
        self.last_peak = None
        self.bpm = None
        self.filtered = 0.0

    def reset(self):
        self.baseline = None
        self.history.clear()
        self.local.clear()
        self.intervals.clear()
        self.last_peak = None
        self.bpm = None
        self.filtered = 0.0

    def add(self, timestamp, red, finger_status):
        if finger_status == 0:
            self.reset()
            return 0.0, None, False

        if self.baseline is None:
            self.baseline = float(red)
        # ponytail: 2 s baseline time constant; add a real band-pass filter if hardware data needs it.
        self.baseline += (red - self.baseline) * 0.01
        ac = red - self.baseline
        self.filtered = (2 * self.filtered + ac) / 3
        self.history.append((timestamp, self.filtered))
        self.local.append((timestamp, self.filtered))

        peak = False
        if len(self.history) >= 2 * SAMPLE_HZ and len(self.local) == 3:
            raw_values = [value for _, value in self.history]
            ordered = sorted(raw_values)
            n = len(ordered)
            low, mid, high = ordered[n // 10], ordered[n // 2], ordered[(9 * n) // 10]
            polarity = 1 if high - mid >= mid - low else -1
            oriented = sorted(polarity * value for value in raw_values)
            threshold = oriented[n // 10] + 0.55 * (oriented[(9 * n) // 10] - oriented[n // 10])
            (t0, v0), (t1, v1), (t2, v2) = self.local
            if polarity * v1 >= threshold and polarity * v1 > polarity * v0 and polarity * v1 >= polarity * v2:
                min_interval, max_interval = 60 / 220, 60 / 30
                interval = None if self.last_peak is None else t1 - self.last_peak
                if interval is None or interval > max_interval:
                    self.last_peak = t1
                    self.intervals.clear()
                    peak = True
                elif interval >= min_interval:
                    self.intervals.append(interval)
                    self.last_peak = t1
                    self.bpm = 60 / statistics.median(self.intervals)
                    peak = True

        if self.last_peak is None or timestamp - self.last_peak > 2.2:
            self.bpm = None
        return self.filtered, self.bpm, peak


class Viewer:
    def __init__(self, root, port, csv_file):
        self.root = root
        self.port = port
        self.csv_file = csv_file
        self.csv_writer = csv.writer(csv_file) if csv_file else None
        self.parser = PacketParser()
        self.hr = HeartRate()
        self.samples = deque(maxlen=500)
        self.sample_index = 0
        self.last_packet = None
        self.module_status = 0
        if self.csv_writer:
            self.csv_writer.writerow(("time_s", "finger_status", "red_ppg", "filtered", "bpm", "peak"))

        root.title("BMH08002-4 紅光 PPG 心率分析")
        root.geometry("900x500")
        self.info = tk.Label(root, text="等待 BMH UART PPG 資料…", anchor="w", font=("Segoe UI", 14))
        self.info.pack(fill="x", padx=12, pady=8)
        self.canvas = tk.Canvas(root, background="#101820", highlightthickness=0)
        self.canvas.pack(fill="both", expand=True, padx=12, pady=8)
        self.help = tk.Label(root, text="顯示紅光 PPG；BPM 由外部峰值間隔估算。關閉視窗會停止量測。", anchor="w")
        self.help.pack(fill="x", padx=12, pady=(0, 8))
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.bind("<Escape>", lambda _event: self.close())
        self.poll()
        self.draw()

    def poll(self):
        try:
            waiting = self.port.in_waiting
            data = self.port.read(waiting) if waiting else b""
            for packet in self.parser.feed(data):
                payload = packet[2:13]
                self.module_status = payload[0]
                red = (payload[9] << 8) | payload[10]  # D10/D11: red PPG, high byte first
                t = self.sample_index / SAMPLE_HZ
                self.sample_index += 1
                filtered, bpm, peak = self.hr.add(t, red, self.module_status)
                self.samples.append((t, filtered))
                self.last_packet = time.monotonic()
                if self.csv_writer:
                    self.csv_writer.writerow((
                        f"{t:.3f}", self.module_status, red, f"{filtered:.2f}",
                        "" if bpm is None else f"{bpm:.1f}", int(peak),
                    ))
        except (serial.SerialException, OSError) as exc:
            self.info.config(text=f"UART 讀取錯誤：{exc}")
            self.root.after(500, self.poll)
            return

        if self.last_packet is None:
            text = "等待封包；確認 38400 baud，且模組已設為 Continuous Upload"
        elif time.monotonic() - self.last_packet > 2:
            text = "超過 2 秒沒有新封包"
        else:
            bpm = "--" if self.hr.bpm is None else f"{self.hr.bpm:.1f}"
            states = ("未偵測手指", "手指已放置／訊號收集中", "模組訊號有效")
            state = states[self.module_status] if self.module_status in (0, 1, 2) else f"狀態 {self.module_status}"
            text = f"心率 {bpm} BPM    {state}    校驗錯誤 {self.parser.bad_packets}"
        self.info.config(text=text)
        self.root.after(10, self.poll)

    def draw(self):
        self.canvas.delete("all")
        width, height = max(self.canvas.winfo_width(), 2), max(self.canvas.winfo_height(), 2)
        now = self.samples[-1][0] if self.samples else 0.0
        visible = [(t, value) for t, value in self.samples if now - t <= 8]
        if len(visible) > 1:
            values = [value for _, value in visible]
            low, high = min(values), max(values)
            if high - low < 1:
                low, high = low - 1, high + 1
            pad = (high - low) * 0.1
            low, high = low - pad, high + pad
            points = []
            for t, value in visible:
                x = width * (1 - (now - t) / 8)
                y = height - 20 - (value - low) / (high - low) * (height - 40)
                points.extend((x, y))
            self.canvas.create_line(*points, fill="#59d8a5", width=2)
        self.root.after(40, self.draw)

    def close(self):
        try:
            self.port.write(command(0x01, 0, 1))
        except (serial.SerialException, OSError):
            pass
        self.port.close()
        if self.csv_file:
            self.csv_file.close()
        self.root.destroy()


def main():
    parser = argparse.ArgumentParser(description="即時讀取 BMH08002-4 紅光 PPG 並以峰值間隔估算心率")
    parser.add_argument("--port", required=True, help="序列埠，例如 COM5")
    parser.add_argument("--baud", type=int, default=BAUD, help=f"UART baud（預設 {BAUD}）")
    parser.add_argument("--csv", help="另存取樣資料；若檔案已存在會拒絕覆寫")
    parser.add_argument("--configure", action="store_true", help="將模組 EEPROM 設為 Continuous Upload (0x04)，再啟動量測")
    args = parser.parse_args()

    try:
        port = serial.Serial(args.port, args.baud, timeout=0, bytesize=8, parity="N", stopbits=1)
    except (serial.SerialException, OSError) as exc:
        raise SystemExit(f"無法開啟 {args.port}: {exc}") from exc

    csv_file = None
    try:
        if args.csv:
            csv_file = open(args.csv, "x", newline="", encoding="utf-8-sig")
        if args.configure:
            # This mode is stored in the module EEPROM; write only when explicitly requested.
            port.write(command(0x04, 0x01, 0x0004))
            port.flush()
            time.sleep(0.06)  # BMH specifies 50 ms for an EEPROM write.
        port.write(command(0x01, 0x00, 0x0000))
        port.flush()
    except (OSError, serial.SerialException, FileExistsError) as exc:
        port.close()
        if csv_file:
            csv_file.close()
        raise SystemExit(f"無法啟動分析：{exc}") from exc

    root = tk.Tk()
    Viewer(root, port, csv_file)
    root.mainloop()


if __name__ == "__main__":
    main()
