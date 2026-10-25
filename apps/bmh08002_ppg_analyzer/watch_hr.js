// Watch heart-rate algorithms for the capture page, line-by-line ports of the firmware headers:
//   HrAlgo    <- firmware/n76e003/application/ble_sensor_hr/source/ppg_hr.h        (ble_sensor_hr, 10 s)
//   MotionHr  <- firmware/n76e003/application/ble_sensor_hr/source/ppg_motion_hr.h (ble_sensor_motion, 5 s)
// The C integer widths are reproduced (int16 / uint16 wrap, C truncating division) so the page gives the
// same BPM as the watch. test_watch_hr.js compiles the real headers with gcc and compares every result;
// change both sides together.
'use strict';
const i16 = v => (v << 16) >> 16, u16 = v => v & 0xffff, div = (a, b) => Math.trunc(a / b);
const dist = (a, b) => (a = u16(a), b = u16(b), a > b ? a - b : b - a);
const sortSmall = iv => iv.sort((a, b) => a - b);

class HrAlgo { // ppg_hr.h
  constructor() { this.ir = HrAlgo.channel(); this.red = HrAlgo.channel(); this.start = 0; this.started = 0; this.cycles = 0; this.last_b1 = this.last_b2 = 0; }
  static channel() { return { b: 0, s: 0, mp: 0, mb: 0, mean: 0, best: 0, best_t: 0, blk_t: 0, last_t: 0, first: 1, in_blk: 0, skip: 0, have_last: 0, w: 0, iv: [] }; }
  static beat(c, t) {
    if (c.have_last) {
      const iv = u16(t - c.last_t);
      if (iv < 300) return;
      if (iv >= 300 && iv <= 2000 && c.iv.length < 16) c.iv.push(iv);
    }
    c.last_t = t; c.have_last = 1;
  }
  static feed(c, raw, t) {
    const x = i16(raw >> 1);
    if (c.first) { c.b = x; c.first = 0; }
    c.b = i16(c.b + (i16(x - c.b) >> 5));
    const a = i16(x - c.b);
    c.s = i16(c.s + (i16(a - c.s) >> 1));
    const p = c.s > 0 ? c.s : 0;
    c.mp = i16(c.mp + (i16(p - c.mp) >> 2));
    c.mb = i16(c.mb + (i16(p - c.mb) >> 3));
    c.mean = i16(c.mean + (i16(p - c.mean) >> 6));
    const thr = i16(c.mb + (c.mean >> 3));
    if (c.mp > thr && c.mp > 4) {
      if (c.skip) return;
      if (!c.in_blk) { c.in_blk = 1; c.w = 0; c.best = p; c.best_t = c.blk_t = t; }
      if (c.w < 255) c.w++;
      if (p > c.best) { c.best = p; c.best_t = t; }
      if (u16(t - c.blk_t) > 600) { c.in_blk = 0; c.skip = 1; c.have_last = 0; }
    } else {
      c.skip = 0;
      if (c.in_blk) { c.in_blk = 0; if (c.w >= 3) HrAlgo.beat(c, c.best_t); }
    }
  }
  static finish(c) {
    const iv = sortSmall(c.iv), n = iv.length; c.iv = [];
    if (n < 3) return 0;
    const med = n & 1 ? iv[n >> 1] : (iv[(n >> 1) - 1] + iv[n >> 1]) >> 1;
    let total = 0, cnt = 0;
    for (const v of iv) if (u16(dist(v, med) * 5) <= med) { total = u16(total + v); cnt++; }
    if (cnt < 5 || u16(cnt * 100) < u16(n * 60)) return 0;
    const mean = u16(Math.floor((total + (cnt >> 1)) / cnt));
    return Math.floor((60000 + (mean >> 1)) / mean) & 0xff;
  }
  static cross(b1, b2) {
    if (!b1 || !b2) return 0;
    const d = b1 > b2 ? b1 - b2 : b2 - b1;
    return u16(d * 10) > (b1 + b2) >> 1 ? 0 : (b1 + b2 + 1) >> 1;
  }
  // t: watch milliseconds (any width, only the low 16 bits are used). Returns null, or the BPM of the
  // 10 s cycle that just ended (0 = invalid).
  feed(ppg1, ppg2, t) {
    t = u16(t);
    if (!this.started) { this.start = t; this.started = 1; }
    HrAlgo.feed(this.ir, ppg1, t);
    HrAlgo.feed(this.red, ppg2, t);
    if (u16(t - this.start) < 10000) return null;
    this.start = t;
    const b1 = HrAlgo.finish(this.ir), b2 = HrAlgo.finish(this.red);
    this.last_b1 = b1; this.last_b2 = b2;
    if (this.cycles < 255) this.cycles++;
    return this.cycles <= 1 ? 0 : HrAlgo.cross(b1, b2);
  }
}

const MH = { OK: 0, WARMUP: 1, SIGNAL: 2, MOTION: 3, DISAGREE: 4, ACC_MISSING: 5, GAP: 6 };
class MotionHr { // ppg_motion_hr.h
  constructor(requireAcc = 1) { this.reset(requireAcc); }
  reset(requireAcc) {
    const ch = () => ({ base: 0, smooth: 0, short_mean: 0, long_mean: 0, mean: 0, peak: 0, peak_t: 0, block_t: 0, last_t: 0, iv: [], initialized: 0, block: 0, skip: 0, have_last: 0, width: 0, bad: 0 });
    Object.assign(this, { ppg: [ch(), ch()], acc: [0, 1, 2].map(MotionHr.axis), start: 0, last_ppg: 0, last_acc: 0, started: 0, warmed: 0,
      require_acc: requireAcc, have_acc: 0, acc_count: 0, acc_bad: 0, bpm: 0, raw_bpm: 0, quality: 0, reason: MH.WARMUP, previous: 0,
      channel_bpm: [0, 0], channel_quality: [0, 0] });
  }
  static axis() { return { base: 0, amplitude: 0, crossing: 0, period: 0, initialized: 0, positive: 0, have_crossing: 0, stable: 0 }; }
  static accAxis(a, raw, t) {
    const x = div(i16(raw), 8);
    if (!a.initialized) { a.base = x; a.initialized = 1; }
    a.base = i16(a.base + div(x - a.base, 16));
    const ac = i16(x - a.base), magnitude = u16(ac < 0 ? -ac : ac);
    a.amplitude = u16(i16(a.amplitude) + div(i16(magnitude) - i16(a.amplitude), 8));
    if (!a.positive && ac > i16(Math.floor(a.amplitude / 4) + 4)) {
      if (a.have_crossing) {
        const interval = u16(t - a.crossing);
        if (interval >= 250 && interval <= 2000) {
          if (a.period && dist(interval, a.period) <= Math.floor(a.period / 5)) {
            if (a.stable < 255) a.stable++;
            a.period = u16(Math.floor((a.period + interval) / 2));
          } else { a.period = interval; a.stable = 0; }
        } else { a.period = 0; a.stable = 0; }
      }
      a.crossing = t; a.have_crossing = a.positive = 1;
    } else if (ac < -i16(Math.floor(a.amplitude / 4) + 4)) a.positive = 0;
  }
  // Raw signed +/-8 g LSM6DS3 counts, as the capture firmware sends them.
  accSample(x, y, z, t) {
    t = u16(t);
    if (this.have_acc && u16(t - this.last_acc) > 100) { this.acc = [0, 1, 2].map(MotionHr.axis); this.acc_bad = 1; }
    this.last_acc = t; this.have_acc = 1;
    if (this.acc_count < 255) this.acc_count++;
    MotionHr.accAxis(this.acc[0], x, t);
    MotionHr.accAxis(this.acc[1], y, t);
    MotionHr.accAxis(this.acc[2], z, t);
  }
  static channelFeed(c, raw, t) {
    const x = i16(raw >> 3);
    if (!c.initialized) { c.base = x; c.initialized = 1; }
    c.base = i16(c.base + div(x - c.base, 32));
    const ac = i16(x - c.base);
    c.smooth = i16(c.smooth + div(ac - c.smooth, 2));
    const positive = c.smooth > 0 ? c.smooth : 0;
    c.short_mean = i16(c.short_mean + div(positive - c.short_mean, 4));
    c.long_mean = i16(c.long_mean + div(positive - c.long_mean, 8));
    c.mean = i16(c.mean + div(positive - c.mean, 64));
    const threshold = i16(c.long_mean + div(c.mean, 8));
    if (c.short_mean > threshold && c.short_mean > 2) {
      if (c.skip) return;
      if (!c.block) { c.block = 1; c.width = 0; c.peak = positive; c.peak_t = c.block_t = t; }
      if (c.width < 255) c.width++;
      if (positive > c.peak) { c.peak = positive; c.peak_t = t; }
      if (u16(t - c.block_t) > 700) { c.block = 0; c.skip = 1; c.have_last = 0; if (c.bad < 255) c.bad++; }
    } else {
      c.skip = 0;
      if (c.block) {
        c.block = 0;
        if (c.width < 2) return;
        if (c.have_last) {
          const interval = u16(c.peak_t - c.last_t);
          if (interval < 300) return;
          if (interval <= 1500 && c.iv.length < 16) c.iv.push(interval);
          else if (c.bad < 255) c.bad++;
        }
        c.last_t = c.peak_t; c.have_last = 1;
      }
    }
  }
  static finish(c) { // [bpm, quality]
    const iv = sortSmall(c.iv), n = iv.length, bad = c.bad;
    c.iv = []; c.bad = 0;
    if (n < 2 || bad > 1) return [0, 0];
    const median = iv[n >> 1];
    let total = 0, kept = 0;
    for (const v of iv) if (dist(v, median) <= Math.floor(median / 6)) { total = u16(total + v); kept++; }
    if (kept < 2 || kept * 100 < n * 70) return [0, 0];
    const value = u16(Math.floor((total + (kept >> 1)) / kept));
    return [Math.floor((60000 + (value >> 1)) / value) & 0xff, Math.floor(kept * 100 / n)];
  }
  motionMatch(bpm, t) {
    if (!bpm || !this.have_acc) return 0;
    const period = Math.floor(60000 / bpm);
    for (const a of this.acc) {
      if (a.amplitude < 40 || a.stable < 2 || u16(t - a.crossing) > 2000) continue;
      if (dist(period, a.period) <= Math.floor(a.period / 8) ||
          dist(period, Math.floor(a.period / 2)) <= Math.floor(a.period / 24) ||
          dist(period, a.period * 2) <= Math.floor(a.period / 6)) return 1;
    }
    return 0;
  }
  // Returns 1 when a 5 s window just ended (this.bpm / reason / quality then hold its result).
  ppgSample(ir, red, t, status) {
    t = u16(t);
    if (ir <= 1 || red <= 1 || ir >= 65534 || red >= 65534) { this.reset(this.require_acc); this.reason = MH.SIGNAL; return 0; }
    if (status === 0 || status > 2 || (this.started && (u16(t - this.last_ppg) > 100 || t === this.last_ppg))) {
      this.reset(this.require_acc); this.reason = MH.GAP; return 0;
    }
    if (!this.started) { this.start = t; this.started = 1; }
    this.last_ppg = t;
    MotionHr.channelFeed(this.ppg[0], ir, t);
    MotionHr.channelFeed(this.ppg[1], red, t);
    if (u16(t - this.start) < 5000) return 0;
    this.start = t; this.bpm = this.raw_bpm = this.quality = 0;
    [this.channel_bpm[0], this.channel_quality[0]] = MotionHr.finish(this.ppg[0]);
    [this.channel_bpm[1], this.channel_quality[1]] = MotionHr.finish(this.ppg[1]);
    const [b0, b1] = this.channel_bpm, [q0, q1] = this.channel_quality;
    const m0 = this.motionMatch(b0, t), m1 = this.motionMatch(b1, t);
    if (!this.warmed) { this.warmed = 1; this.reason = MH.WARMUP; }
    else if (this.require_acc && (!this.have_acc || this.acc_bad || this.acc_count < 80 || u16(t - this.last_acc) > 100)) this.reason = MH.ACC_MISSING;
    else if (b0 && b1 && !m0 && !m1 && dist(b0, b1) <= Math.floor(u16(b0 + b1) / 20)) {
      this.raw_bpm = (b0 + b1 + 1) >> 1;
      this.quality = q0 < q1 ? q0 : q1;
    } else if (this.require_acc && b0 && !m0 && (!b1 || m1) && q0 >= 85) { this.raw_bpm = b0; this.quality = 60; }
    else if (this.require_acc && b1 && !m1 && (!b0 || m0) && q1 >= 85) { this.raw_bpm = b1; this.quality = 60; }
    else this.reason = (m0 || m1) ? MH.MOTION : (b0 && b1) ? MH.DISAGREE : MH.SIGNAL;
    if (this.raw_bpm) {
      this.bpm = this.raw_bpm;
      if (this.previous && dist(this.previous, this.raw_bpm) <= 12) this.bpm = Math.floor((2 * this.raw_bpm + this.previous + 1) / 3);
      this.reason = MH.OK;
    }
    this.previous = this.bpm;
    this.acc_count = this.acc_bad = 0;
    return 1;
  }
}
MotionHr.REASONS = ['OK', '暖機', '訊號不足', '動作干擾', '雙通道不一致', '缺 ACC', '中斷或無手指'];

if (typeof module !== 'undefined') module.exports = { HrAlgo, MotionHr };
