// node apps/bmh08002_ppg_analyzer/test_watch_hr.js   (needs gcc on PATH)
// Compiles the real firmware headers (ppg_hr.h, ppg_motion_hr.h) on the host, replays the same events through
// them and through watch_hr.js, and requires identical results: synthetic clean / motion / spikes / drift /
// gap / flat runs plus the hr1 / hr2 module recordings.
const fs = require('fs'), os = require('os'), path = require('path'), assert = require('assert'), { execFileSync } = require('child_process');
const { HrAlgo, MotionHr } = require('./watch_hr.js');
const SRC = path.join(__dirname, '../../firmware/n76e003/application/ble_sensor_hr/source');
const REC = path.join(__dirname, '../../firmware/hub8735-ultra/examples/uart/bmh08002_4_control');

const WRAPPER = `#include <stdio.h>
#include "ppg_motion_hr.h"
#include "ppg_hr.h"
int main(void) {
    motion_hr h; hr_t old; unsigned k, t, a, b, c, st; uint8_t bpm = 0;
    mh_reset(&h, 1); hr_reset(&old);
    while (scanf("%u %u %u %u %u %u", &k, &t, &a, &b, &c, &st) == 6) {
        if (k) { mh_acc(&h, (int16_t)a, (int16_t)b, (int16_t)c, (uint16_t)t); continue; }
        if (hr_feed(&old, (uint16_t)a, (uint16_t)b, (uint16_t)t, &bpm)) printf("hr %u %u %u %u\\n", t, bpm, old.last_b1, old.last_b2);
        if (mh_ppg(&h, (uint16_t)a, (uint16_t)b, (uint16_t)t, (uint8_t)st))
            printf("mh %u %u %u %u %u %u\\n", t, h.bpm, h.raw_bpm, h.quality, h.reason, h.channel_bpm[0] * 256 + h.channel_bpm[1]);
    }
    return 0;
}
`;

function js(events) {
  const hr = new HrAlgo(), mh = new MotionHr(1), out = [];
  for (const [k, t, a, b, c, st] of events) {
    if (k) { mh.accSample(a, b, c, t); continue; }
    const bpm = hr.feed(a, b, t);
    if (bpm !== null) out.push(`hr ${t} ${bpm} ${hr.last_b1} ${hr.last_b2}`);
    if (mh.ppgSample(a, b, t, st)) out.push(`mh ${t} ${mh.bpm} ${mh.raw_bpm} ${mh.quality} ${mh.reason} ${mh.channel_bpm[0] * 256 + mh.channel_bpm[1]}`);
  }
  return out;
}

let seed = 8735;
const rnd = () => (seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648;
const gauss = () => Math.sqrt(-2 * Math.log(rnd() + 1e-12)) * Math.cos(2 * Math.PI * rnd());
function synthetic(scenario, bpm = 72, motion = 126, seconds = 60, amp = 800, noise0 = 15, accAmp = 2500) { // shapes from ble_sensor_hr/test_motion_hr.py
  const ev = [], moving = ['one_channel', 'both_channels', 'same_frequency'].includes(scenario);
  for (let i = 0; i < Math.round(seconds / 0.028); i++) {
    const t = i * 28, s = t / 1000, pulse = amp * Math.sin(2 * Math.PI * bpm / 60 * s), mv = 1800 * Math.sin(2 * Math.PI * motion / 60 * s);
    let ir = pulse, red = 0.8 * pulse;
    if (scenario === 'one_channel') ir += mv;
    if (scenario === 'both_channels' || scenario === 'same_frequency') { ir += mv; red += mv; }
    if (scenario === 'flat') ir = red = 0;
    if (scenario === 'spikes' && i % 71 === 0) ir += 6000;
    if (scenario === 'drift') { ir += 1500 * Math.sin(2 * Math.PI * 0.08 * s); red += 1000 * Math.sin(2 * Math.PI * 0.08 * s); }
    if (scenario === 'gain_step' && s >= 25) { ir *= 2; red *= 2; }
    const noise = scenario === 'flat' ? 0 : noise0;
    if (scenario === 'gap' && s >= 25 && s < 26) continue;
    ev.push([0, t & 0xffff, Math.max(0, Math.min(65535, Math.round(12000 + ir + gauss() * noise))), Math.max(0, Math.min(65535, Math.round(15000 + red + gauss() * noise))), 0, 1]);
  }
  for (let i = 0; i < Math.round(seconds * 26); i++) {
    const t = Math.round(i * 1000 / 26), a = moving ? Math.round(accAmp * Math.sin(2 * Math.PI * motion / 60 * t / 1000)) : 0;
    ev.push([1, t & 0xffff, a & 0xffff, 0, 4098, 0]);
  }
  return ev.sort((x, y) => x[1] - y[1] || y[0] - x[0]);
}
function recording(name) { // module samples at the measured ~28 ms, still ACC so motion has its input
  const rows = fs.readFileSync(path.join(REC, name), 'utf8').trim().split(/\r?\n/).slice(1).map(l => l.split(','));
  const ev = rows.map((r, i) => [0, (i * 28) & 0xffff, +r[13], +r[14], 0, +r[2]]);
  for (let t = 0; t < rows.length * 28; t += 38) ev.push([1, t & 0xffff, 0, 0, 4098, 0]);
  return ev.sort((x, y) => x[1] - y[1] || y[0] - x[0]);
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'watch_hr_'));
const exe = path.join(tmp, 'replay.exe');
fs.writeFileSync(path.join(tmp, 'replay.c'), WRAPPER);
execFileSync('gcc', ['-std=c99', '-O2', '-Wall', '-I' + SRC, path.join(tmp, 'replay.c'), '-o', exe]);
const cases = { hr1: recording('hr1.csv'), hr2: recording('hr2.csv') };
for (const s of ['clean', 'one_channel', 'both_channels', 'same_frequency', 'spikes', 'drift', 'gain_step', 'gap', 'flat']) cases[s] = synthetic(s);
cases.fast = synthetic('clean', 150, 126, 40);
// near the detection / motion thresholds, where a wrong constant or integer width changes the result
for (const [amp, noise] of [[80, 20], [48, 16], [32, 12], [24, 10]]) cases[`weak_${amp}`] = synthetic('clean', 66, 126, 60, amp, noise);
for (const acc of [300, 340, 400, 600]) cases[`small_motion_${acc}`] = synthetic('one_channel', 72, 120, 60, 800, 15, acc);
cases.cadence_double = synthetic('both_channels', 70, 140, 60, 800, 15, 2500);
for (const m of [80, 81, 82, 83, 84]) cases[`cadence_edge_${m}`] = synthetic('one_channel', 72, m, 60, 800, 15, 2500); // motion period 12.5-14 % off the pulse
let windows = 0, valid = 0;
for (const [name, ev] of Object.entries(cases)) {
  const c = execFileSync(exe, { input: ev.map(e => e.join(' ')).join('\n') + '\n' }).toString().trim().split(/\r?\n/);
  const j = js(ev);
  assert.deepStrictEqual(j, c, name);
  windows += j.length; valid += j.filter(l => +l.split(' ')[2] > 0).length;
}
fs.rmSync(tmp, { recursive: true, force: true });
assert(valid > 10, 'the cases must also produce real heart rates, not only zeros');
console.log(`watch_hr.js matches ppg_hr.h / ppg_motion_hr.h: ${Object.keys(cases).length} runs, ${windows} windows (${valid} with a BPM).`);
