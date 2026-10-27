"""Render the HUB SH1106 OLED (layout and menu logic copied from UITask.cpp) from boot to standby and wake.

Writes one numbered PNG per step plus hub_ui_operation.gif into the folder given on the command line.
Values (heart rate, clock, battery, songs) are made up.
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

S = 4                                   # OLED pixel -> image pixel
OW, OH = 128 * S, 64 * S
W, H = OW + 120, 130 + OH + 40 + 150
OX, OY = 60, 130
FG, BG = (120, 200, 255), (8, 10, 16)  # blue-ish OLED
mono = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 35)
mono_b = ImageFont.truetype("C:/Windows/Fonts/consolab.ttf", 26)
cjk = ImageFont.truetype("C:/Windows/Fonts/msjhbd.ttc", 26)
cjk_s = ImageFont.truetype("C:/Windows/Fonts/msjh.ttc", 20)

GENRES = ["Any", "Classical", "Electronic", "Hip-Hop", "Rock", "Other"]
PLANS = ["Train 11m", "Train 5m"]
LIBRARY = [["001.mp3", 122], ["002.mp3", 61], ["003.mp3", 0], ["004.mp3", 140]]

# ---- device state, mirrors the UITask globals ----
st = dict(screen="Top", cursor=0, top=0, mode="Enjoy", plan=1, status="", genre=0, audio="stop", song="",
          vol=30, age=25, age_edit=25, bpm=dict(state="Idle", cur=0, total=0, file=""), info=True,
          watch="Scan", chest="Auto", ppg="Watch", imu="Local", custom=True, hr=None, acc="3,-8,1001",
          song_menu=-1, from_list=False, anim=None)


def lib_index():
    if st["audio"] == "stop":
        return -1
    return next((i for i, (f, _) in enumerate(LIBRARY) if f == st["song"]), -1)


def top_items():
    m = st["mode"]
    rows = [("Mode", "Mode: " + (PLANS[st["plan"]] if m == "Train" else m))]
    if m != "Manual":
        rows.append(("Status", st["status"]))
    if m in ("Enjoy", "Train"):
        rows.append(("Genre", "Genre: " + GENRES[st["genre"]]))
    rows.append(("Play", {"play": "Pause", "pause": "Resume", "stop": "Stopped"}[st["audio"]]))
    if lib_index() >= 0:
        bpm = LIBRARY[lib_index()][1]
        rows.append(("SongBpm", f"Song BPM: {bpm or '--'}"))
    if m == "Manual":
        rows.append(("Songs", "Songs"))
    b = st["bpm"]
    rows += [("Stop", "Stop"), ("Volume", f"Volume: {st['vol']}%"), ("Age", f"Age: {st['age']}"),
             ("Bpm", f"BPM: {b['cur']} / {b['total']}" if b["state"] == "Running" else "Rescan BPM"),
             ("Sensors", "Sensors"), ("Custom", "Custom"), ("Info", "Info bar: " + ("On" if st["info"] else "Off")),
             ("Power", "Power off")]
    return rows


def hrmax(age):
    return int(206.9 - 0.67 * age + 0.5)


def scaled(bpm, doubled):
    v = bpm * 2 if doubled else (bpm + 1) // 2
    return v if bpm and 30 <= v <= 300 else 0


def items():
    sc = st["screen"]
    if sc == "Top":
        return [label for _, label in top_items()]
    if sc == "ModeList":
        return [f"{m} mode" + (" *" if m == st["mode"] else "") for m in ("Enjoy", "Train", "Manual", "Demo")]
    if sc == "PlanList":
        return [p + (" *" if st["mode"] == "Train" and i == st["plan"] else "") for i, p in enumerate(PLANS)]
    if sc == "GenreList":
        return [g + (" *" if i == st["genre"] else "") for i, g in enumerate(GENRES)]
    if sc == "AgeEdit":
        return [f"Age {st['age_edit']}  HRmax {hrmax(st['age_edit'])}"]
    if sc == "SongList":
        return [f"{f} {b or '--'}" for f, b in LIBRARY]
    if sc == "SongMenu":
        f, b = LIBRARY[st["song_menu"]]
        rows = [f"Play {f}"] if st["from_list"] else []
        for doubled, tag in ((True, "x2"), (False, "/2")):
            v = scaled(b, doubled)
            rows.append(f"{tag}: BPM unknown" if not b else f"BPM {tag}: {b} -> {v}" if v else f"{tag}: out of range")
        return rows + ["OK: Back"]
    if sc == "SensorList":
        return [f"Watch: {st['watch']}", f"Chest: {st['chest']}",
                "Input: " + ("Custom" if st["custom"] else st["ppg"]), "OK: Back"]
    if sc == "CustomList":
        return [f"PPG: {st['ppg']}", f"IMU: {st['imu']}", "OK: Back"]
    if sc == "PowerConfirm":
        return ["Cancel", "Power off"]
    if sc == "BpmStatus":
        b = st["bpm"]
        rows = ["BPM: Analyzing" if b["state"] == "Running" else "BPM: Complete",
                f"File {b['cur']} / {b['total']}" if b["state"] == "Running" else "OK 5  Fail 0", b["file"], "Back"]
        return rows
    return []


def open_list(screen, cursor=0):
    st.update(screen=screen, cursor=cursor, top=0)


def back_to_top(key):
    st.update(screen="Top", top=0)
    st["cursor"] = [k for k, _ in top_items()].index(key)


def go_top(key):  # move the Top cursor onto an item (several DOWN presses)
    st["cursor"] = [k for k, _ in top_items()].index(key)


# ---- drawing, same coordinates as drawHeader() / draw() ----
def text(d, x, y, s, fill):
    d.text((OX + x * S, OY + y * S - 3), s, font=mono, fill=fill)


def px(d, x0, y0, x1, y1, fill=FG):
    d.rectangle([OX + x0 * S, OY + y0 * S, OX + x1 * S - 1, OY + y1 * S - 1], fill=fill)


def op(x, y):
    return OX + x * S, OY + y * S


def header(d, clock):
    if st["audio"] == "play":
        d.polygon([op(0, 0), op(0, 7), op(6, 3.5)], fill=FG)
    elif st["audio"] == "pause":
        px(d, 0, 0, 2, 7); px(d, 4, 0, 6, 7)
    else:
        px(d, 0, 1, 5, 6)
    for x, ch, link in ((10, "C", st["chest"]), (19, "W", st["watch"])):
        on = link == "Conn" or (link == "Scan" and int(clock[-2:]) % 2 == 0)
        if on:
            px(d, x, 0, x + 7, 7)
        else:
            d.rectangle([*op(x, 0), OX + (x + 7) * S - 1, OY + 7 * S - 1], outline=FG, width=S)
        d.text((OX + (x + 1.3) * S, OY - 2), ch, font=mono_b, fill=BG if on else FG)
    if st["bpm"]["state"] == "Running":  # hourglass
        d.polygon([op(30, 0), op(36, 0), op(33, 3)], fill=FG)
        d.polygon([op(30, 6), op(36, 6), op(33, 3)], fill=FG)
    text(d, 44, 0, clock, FG)
    d.rectangle([*op(115, 0), OX + 126 * S - 1, OY + 7 * S - 1], outline=FG, width=S)
    px(d, 126, 2, 127, 5); px(d, 117, 2, 123, 5)
    text(d, 89, 0, "80%", FG)
    if st["audio"] == "stop":
        song = "Stopped"
    else:
        i = lib_index()
        song = f"{st['song'][:15]}({LIBRARY[i][1] if i >= 0 and LIBRARY[i][1] else '--'})"
    text(d, 0, 8, song, FG)
    px(d, 0, 15, 128, 16)


def menu(d):
    rows = 4 if st["info"] else 6
    if st["screen"] == "Volume":
        text(d, 2, 18, f"Volume {st['vol']}%", FG)
        d.rectangle([*op(2, 30), OX + 126 * S - 1, OY + 42 * S - 1], outline=FG, width=S)
        px(d, 4, 32, 4 + 120 * st["vol"] // 100, 40)
    else:
        lst, cur = items(), st["cursor"]
        if cur < st["top"]:
            st["top"] = cur
        if cur >= st["top"] + rows:
            st["top"] = cur - rows + 1
        for row in range(rows):
            i = st["top"] + row
            if i >= len(lst):
                break
            y = 16 + row * 8
            sel = i == cur
            if sel:
                px(d, 0, y, 128, y + 8)
            text(d, 2, y, lst[i][:21], BG if sel else FG)
            if row == 0 and st["top"] > 0:
                text(d, 120, y, "^", BG if sel else FG)
            elif row == rows - 1 and i + 1 < len(lst):
                text(d, 120, y, "v", BG if sel else FG)
    if st["info"]:
        px(d, 0, 47, 128, 48)
        hr = st["hr"]
        text(d, 0, 48, f"HR:{hr} SpO2:97 St:1" if hr else "HR:-- SpO2:-- St:-", FG)
        text(d, 0, 56, f"A:{st['acc']} G:2,1,0", FG)


def heart(d, cx, cy, r):  # drawHeart() geometry
    if r < 1:
        px(d, cx, cy, cx + 1, cy + 1)
        return
    tx, ty = (8 * r + 2) // 5, cy + (4 * r + 2) // 5
    for c in (cx - r, cx + r):
        d.ellipse([*op(c - r, cy - r), *op(c + r, cy + r)], fill=FG)
    d.polygon([op(cx - tx, ty), op(cx, cy), op(cx + tx, ty), op(cx, cy + 2 * r)], fill=FG)


def note(d, x, y):
    d.ellipse([*op(x - 2, y - 2), *op(x + 2, y + 2)], fill=FG)
    d.line([op(x + 2, y - 8), op(x + 2, y)], fill=FG, width=S)
    d.line([op(x + 2, y - 8), op(x + 5, y - 5)], fill=FG, width=S)


ECG = [(0, 0), (36, 0), (43, -3), (50, 0), (60, 0), (65, 4), (72, -14), (79, 8), (84, 0), (120, 0)]


def ecg(d, base, x_end):
    pts = []
    for x, y in ECG:
        x += 4
        if x > x_end:
            x0, y0 = pts[-1]
            pts.append((x_end, y0 + (base + y - y0) * (x_end - x0) / (x - x0)))
            break
        pts.append((x, base + y))
    d.line([op(*p) for p in pts], fill=FG, width=S)


def centered(d, s, y):
    text(d, (128 - 6 * len(s)) / 2, y, s, FG)


def boot(d, ms):  # drawBootFrame(); bar jumps 20/40/70/90/100 with the init stages, capped by a 3 s pace
    stage = 100 if ms >= 2800 else 90 if ms >= 2400 else 70 if ms >= 1200 else 40 if ms >= 600 else 20
    prog = min(ms * 100 // 3000, stage)
    if ms < 1000:
        ecg(d, 26, 4 + 120 * ms / 1000)
    elif ms < 1600:
        heart(d, 64, 18, int(1 + 6 * (ms - 1000) / 600))
    else:
        beat = (ms - 1600) % 800
        heart(d, 64, 18, 6 + (2 if beat < 120 else 1 if 200 <= beat < 300 else 0))
        note(d, 92, 26); note(d, 102, 20)
        centered(d, "Hear your heart", 38)
    d.rectangle([*op(14, 52), OX + 114 * S - 1, OY + 60 * S - 1], outline=FG, width=S)
    if prog:
        px(d, 16, 54, 16 + 96 * prog // 100, 58)


def goodbye(d, ms):  # playPowerOffAnimation()
    if ms < 800:
        heart(d, 64, 18, int(7 * (800 - ms) / 800))
        centered(d, "Goodbye", 40)
    elif ms < 1100:
        w = 128 * (1100 - ms) / 300
        px(d, (128 - w) / 2, 31, (128 + w) / 2, 32)


def buttons(d, pressed):
    y = OY + OH + 70
    for i, (label, name) in enumerate((("▲", "UP"), ("▼", "DOWN"), ("●", "OK"))):
        cx = W // 2 + (i - 1) * 170
        on = name == pressed
        d.ellipse([cx - 42, y - 42, cx + 42, y + 42], fill=(255, 170, 40) if on else (60, 62, 72),
                  outline=(255, 220, 140) if on else (110, 112, 125), width=4)
        d.text((cx, y), label, font=cjk, fill=(20, 20, 20) if on else (230, 230, 230), anchor="mm")
        d.text((cx, y + 64), name, font=cjk_s, fill=(255, 190, 80) if on else (170, 170, 180), anchor="mm")


def render(step, caption, pressed, clock):
    img = Image.new("RGB", (W, H), (22, 24, 30))
    d = ImageDraw.Draw(img)
    d.text((W // 2, 16), step, font=cjk_s, fill=(160, 165, 180), anchor="mm")
    lines = caption.split("\n")
    for k, s in enumerate(lines):
        d.text((W // 2, 72 + k * 34 - (len(lines) - 1) * 17), s, font=cjk, fill=(240, 240, 245), anchor="mm")
    d.rectangle([OX - 12, OY - 12, OX + OW + 12, OY + OH + 12], fill=(30, 30, 36))
    d.rectangle([OX, OY, OX + OW - 1, OY + OH - 1], fill=BG)
    if st["anim"]:
        st["anim"](d)
    elif st["screen"] != "Off":
        header(d, clock)
        menu(d)
    buttons(d, pressed)
    return img


# ---- storyboard: (caption, [(button or None, action that changes st, ms), ...]) ----
def set_(**kw):
    return lambda: st.update(**kw)


def down_to(key):
    """DOWN presses that move the Top cursor onto key; counted when the step runs."""
    return ("DOWN_TO", key)


def expand(seq):
    for entry in seq:
        if entry[0] != "DOWN_TO":
            yield entry
            continue
        keys = [k for k, _ in top_items()]
        for _ in range((keys.index(entry[1]) - st["cursor"]) % len(keys)):
            yield "DOWN", lambda: st.update(cursor=(st["cursor"] + 1) % len(top_items())), 350


def anim(fn, total, step=100, start=0):
    return [(None, (lambda t: lambda: st.update(anim=lambda d: fn(d, t)))(t), step) for t in range(start, total, step)]


def bpm_step(cur, f):
    return lambda: st["bpm"].update(state="Running", cur=cur, total=5, file=f)


STEPS = []


def step(caption, *seq, snap=None):  # snap: entry index whose frame becomes the PNG (default: last)
    STEPS.append((caption, list(seq), snap))


step("開機動畫：心電圖→愛心跳動\n進度條依SD、曲庫、NPU、喇叭初始化前進",
     *anim(boot, 3000), (None, lambda: st.update(anim=lambda d: boot(d, 3000)), 700))
step("進入主選單，預設Enjoy模式\n手錶連線中（W閃爍），等待有效心率",
     (None, set_(anim=None, status=" Target: wait HR"), 1200),
     (None, lambda: None, 1000))
step("手錶連上、取得心率118\nZone 3→目標124 BPM（心率＋5%），自動選歌播放",
     (None, set_(watch="Conn", hr=118, status=" Z3 Target 124 BPM", audio="play", song="001.mp3"), 2200))
step("DOWN：Status列（唯讀）\n顯示目前心率Zone與目標BPM",
     ("DOWN", set_(cursor=1), 1600))
step("DOWN、OK：開啟Genre期望曲風清單\n*為目前選擇（預設Any）",
     ("DOWN", set_(cursor=2), 500), ("OK", lambda: open_list("GenreList", st["genre"]), 1600))
step("DOWN×4：游標移到Rock",
     *[("DOWN", (lambda c: set_(cursor=c))(c), 350) for c in (1, 2, 3, 4)], (None, lambda: None, 900))
step("OK：只在Rock曲風內依目標BPM選曲\n不打斷目前歌曲，下一首生效",
     ("OK", lambda: (st.update(genre=4), back_to_top("Genre")), 1800))
step("DOWN、OK：暫停播放\n仍維持Enjoy模式，暫停中不換歌",
     ("DOWN", lambda: go_top("Play"), 500), ("OK", set_(audio="pause"), 1800))
step("OK：Resume繼續播放\n暫停期間累積的換歌此時才執行",
     ("OK", set_(audio="play", song="002.mp3"), 1800))
step("DOWN、OK：Song BPM修正頁\n此曲被分析成61，是實際的一半",
     ("DOWN", lambda: go_top("SongBpm"), 500),
     ("OK", lambda: (st.update(song_menu=1, from_list=False), open_list("SongMenu")), 1800))
step("OK：BPM x2，61→122\n修正寫入music_library.json",
     ("OK", lambda: LIBRARY[1].__setitem__(1, 122), 1800))
step("DOWN×2、OK：OK: Back回主選單\n標題列顯示002.mp3(122)",
     ("DOWN", set_(cursor=1), 350), ("DOWN", set_(cursor=2), 400),
     ("OK", lambda: back_to_top("SongBpm"), 1800))
step("DOWN×2、OK：進入音量條",
     down_to("Volume"), ("OK", set_(screen="Volume"), 1500))
step("UP×2：音量30%→50%\n（每按一次±10%）",
     ("UP", set_(vol=40), 600), ("UP", set_(vol=50), 1500))
step("OK：確定並回主選單",
     ("OK", lambda: back_to_top("Volume"), 1500))
step("DOWN、OK：年齡設定\n畫面同時顯示HRmax（Tanaka公式）",
     down_to("Age"), ("OK", lambda: (st.update(age_edit=st["age"]), open_list("AgeEdit")), 1600))
step("UP：年齡25→26，HRmax隨之改變",
     ("UP", set_(age_edit=26), 1600))
step("OK：存入HUB內部Flash\n立即用於Zone與Train",
     ("OK", lambda: (st.update(age=26), back_to_top("Age")), 1800))
step("UP×7：回到Mode，OK開啟模式清單\n*為目前模式",
     *[("UP", (lambda c: set_(cursor=c))(c), 250) for c in range(6, -1, -1)],
     ("OK", lambda: open_list("ModeList", 0), 1600))
step("DOWN、OK：選Train，開啟課表清單",
     ("DOWN", set_(cursor=1), 500), ("OK", lambda: open_list("PlanList", st["plan"]), 1600))
step("OK：開始Train 5m課表（慢-快-慢-更快-慢）\n每段固定時長，時間到立即換段換歌",
     ("OK", lambda: (st.update(mode="Train", status=" Stage 1/5 0:58", song="001.mp3"),
                     back_to_top("Mode")), 1200),
     (None, set_(status=" Stage 1/5 0:57"), 1000))
step("Status列顯示段落與剩餘時間\n實際心率僅供顯示，不影響選曲",
     ("DOWN", set_(cursor=1, status=" Stage 2/5 0:44", song="004.mp3"), 2000))
step("UP、OK、DOWN×2、OK：切到Demo模式\n依G-sensor運動強度0～4播DEMO曲目",
     ("UP", set_(cursor=0), 300), ("OK", lambda: open_list("ModeList", 1), 500),
     *[("DOWN", (lambda c: set_(cursor=c))(c), 300) for c in (2, 3)],
     ("OK", lambda: (st.update(mode="Demo", status=" Level: 1", song="DEMO_01.mp3", acc="40,-30,1010"),
                     back_to_top("Mode")), 1500))
step("搖動HUB：強度升到3並維持0.5秒\n立即改播DEMO_03.mp3",
     ("DOWN", set_(cursor=1), 600),
     (None, set_(status=" Level: 3", song="DEMO_03.mp3", acc="620,-480,1350"), 2000))
step("UP、OK、UP、OK：切到Manual\n目前歌曲繼續播，主選單出現Songs",
     ("UP", set_(cursor=0), 300), ("OK", lambda: open_list("ModeList", 3), 500),
     ("UP", set_(cursor=2), 300),
     ("OK", lambda: (st.update(mode="Manual", acc="3,-8,1001"), back_to_top("Mode")), 1500))
step("DOWN×2、OK：Songs曲庫清單\n右側為BPM，--表示尚未分析",
     down_to("Songs"), ("OK", lambda: open_list("SongList"), 1600))
step("DOWN×3、OK：選004.mp3，開啟單曲選單",
     *[("DOWN", (lambda c: set_(cursor=c))(c), 300) for c in (1, 2, 3)],
     ("OK", lambda: (st.update(song_menu=3, from_list=True), open_list("SongMenu")), 1600))
step("OK：Play播放004.mp3",
     ("OK", lambda: (st.update(audio="play", song="004.mp3"), back_to_top("Songs")), 1800))
step("DOWN、OK：Stop停止播放",
     down_to("Stop"), ("OK", lambda: (st.update(audio="stop"), go_top("Stop")), 1800))
step("DOWN×3、OK：Rescan BPM，開啟進度頁\n掃描SD，分析新檔與未知BPM／曲風的歌",
     down_to("Bpm"),
     ("OK", lambda: (bpm_step(2, "003.mp3")(), st.update(screen="BpmStatus", cursor=3, top=0)), 1800))
step("OK：Back回主選單，分析在背景繼續\n標題列沙漏、選單顯示進度",
     (None, bpm_step(3, "005.mp3"), 1200), ("OK", lambda: back_to_top("Bpm"), 1800),
     (None, bpm_step(4, "006.mp3"), 1200))
step("分析完成，沙漏消失\n新曲目取得BPM後即可參與自動選歌",
     (None, lambda: st["bpm"].update(state="Complete"), 1800))
step("DOWN、OK：Sensors感測連線\nWatch已連線，Chest綁帶Auto重試中",
     down_to("Sensors"), ("OK", lambda: open_list("SensorList"), 1800))
step("DOWN×2、OK：Input切成Watch\nPPG與IMU一起改用手錶",
     ("DOWN", set_(cursor=1), 350), ("DOWN", set_(cursor=2), 500),
     ("OK", set_(ppg="Watch", imu="Watch", custom=False), 1800))
step("DOWN、OK：Back，再DOWN、OK進Custom\nPPG／IMU可各自選來源",
     ("DOWN", set_(cursor=3), 350), ("OK", lambda: back_to_top("Sensors"), 400),
     ("DOWN", lambda: go_top("Custom"), 350), ("OK", lambda: open_list("CustomList"), 1600))
step("DOWN、OK：IMU改回Local（HUB本機）\nSensors的Input會顯示Custom",
     ("DOWN", set_(cursor=1), 500), ("OK", set_(imu="Local", custom=True), 1800))
step("DOWN、OK：Back，再DOWN、OK\nInfo bar關閉，選單多顯示2列",
     ("DOWN", set_(cursor=2), 350), ("OK", lambda: back_to_top("Custom"), 400),
     ("DOWN", lambda: go_top("Info"), 400), ("OK", set_(info=False), 1800))
step("OK：Info bar再打開，顯示心率與IMU",
     ("OK", set_(info=True), 1600))
step("DOWN、OK：Power off確認\n預設停在Cancel，避免誤觸",
     ("DOWN", lambda: go_top("Power"), 400), ("OK", lambda: open_list("PowerConfirm"), 1600))
step("DOWN、OK：關機動畫Goodbye\n畫面收成一條線",
     ("DOWN", set_(cursor=1), 600),
     ("OK", lambda: st.update(anim=lambda d: goodbye(d, 0)), 100), *anim(goodbye, 1100, start=100), snap=5)
step("軟體待機：停止播放、取消分析、\n感測器斷電、BLE斷線、OLED關閉",
     (None, set_(anim=None, screen="Off", audio="stop"), 2500))
step("長按OK約2秒喚醒", ("OK", lambda: None, 300, 2000))
step("喚醒：再次播放開機動畫", *anim(boot, 3000), (None, lambda: st.update(anim=lambda d: boot(d, 3000)), 500))
step("回到Enjoy，保留音量50%、年齡26、感測來源\n目標BPM已清除，等新的有效心率",
     (None, lambda: (st.update(anim=None, mode="Enjoy", status=" Target: wait HR", watch="Scan", hr=None,
                               bpm=dict(state="Idle", cur=0, total=0, file="")), open_list("Top")), 2500))


def main(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for f in os.listdir(out_dir):
        if f.endswith(".png"):
            os.remove(os.path.join(out_dir, f))
    frames, durations, clock_ms = [], [], 0
    for n, (cap, seq, snap_at) in enumerate(STEPS, 1):
        label = f"步驟 {n}/{len(STEPS)}"
        last_btn, snap = None, None
        for k, (btn, action, ms, *hold) in enumerate(expand(seq)):
            clock = f"{clock_ms // 60000 % 100:02d}:{clock_ms // 1000 % 60:02d}"
            if btn:  # press frame: old screen, button lit
                frames.append(render(label, cap, btn, clock)); durations.append(hold[0] if hold else 250)
                last_btn = btn
                clock_ms += durations[-1]
            action()
            frames.append(render(label, cap, None, clock)); durations.append(ms)
            clock_ms += ms
            if snap_at is None or k == snap_at:
                snap = render(label, cap, last_btn, clock)
        snap.save(os.path.join(out_dir, f"{n:02d}.png"))
    gif = os.path.join(out_dir, "hub_ui_operation.gif")
    q = [f.quantize(colors=64, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE) for f in frames]
    q[0].save(gif, save_all=True, append_images=q[1:], duration=durations, loop=0, optimize=True)
    out = Image.open(gif)  # identical neighbours get merged, so check total time instead of frame count
    total = 0
    for i in range(out.n_frames):
        out.seek(i); total += out.info["duration"]
    assert total == sum(durations), (total, sum(durations))
    print(len(STEPS), "steps,", len(frames), "frames,", sum(durations) / 1000, "s")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__)))
