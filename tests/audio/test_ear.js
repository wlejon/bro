// bro.ear (docs/ear-api.js): measure / compare / spectrogram driven from a
// script, on clips synthesized here with known answers, in every clip form
// (bare Float32Array, {samples, sampleRate, channels}, AudioBuffer, a WAV
// path at another rate), plus a side-by-side spectrogram written as a PNG.
// No playback: bro.ear is offline analysis and needs no audio device.

const fs = require('fs');
const os = require('os');
const path = require('path');

assert(bro.ear && typeof bro.ear.measure === 'function' &&
       typeof bro.ear.compare === 'function' && typeof bro.ear.spectrogram === 'function',
       'bro.ear.measure / compare / spectrogram are installed');

const sr = 48000;

// A modal "bar": inharmonic partials 1 : 2.756 : 5.404 that ring on, the
// signature of a synthetic xylophone.
function bar(f0, secs, delay) {
    const n = Math.floor(secs * sr), d = Math.floor((delay || 0) * sr);
    const s = new Float32Array(n + d);
    const modes = [[1, 0.5, 0.8], [2.756, 0.3, 0.5], [5.404, 0.2, 0.3]];
    for (let i = 0; i < n; i++) {
        const t = i / sr;
        let v = 0;
        for (const [r, a, tau] of modes) v += a * Math.exp(-t / tau) * Math.sin(2 * Math.PI * f0 * r * t);
        s[i + d] = v;
    }
    return s;
}

// A short noise hit: fast-decaying filtered noise, deterministic.
function knock(secs) {
    const s = new Float32Array(Math.floor(secs * sr));
    let seed = 7, y = 0;
    for (let i = 0; i < s.length; i++) {
        seed = (seed * 1103515245 + 12345) % 2147483648;
        const x = seed / 1073741824 - 1;
        y = 0.7 * y + 0.3 * x;                        // gentle low-pass
        s[i] = 0.9 * Math.exp(-i / sr / 0.04) * y;
    }
    return s;
}

// ── measure ──────────────────────────────────────────────────────────────
const barClip = { samples: bar(523.25, 2.0), sampleRate: sr };
const mb = bro.ear.measure(barClip);
console.log('bar:', JSON.stringify({ tonality: mb.tonality, lufs: mb.lufs, tailTime: mb.tailTime,
    tailEnd: mb.tailEnd, t60: mb.t60, ringing: mb.ringing }));
assert(mb.tonality > 0.9, 'bar is tonal, got ' + mb.tonality);
assert(mb.ringing.count === 3, 'three partials, got ' + mb.ringing.count);
assert(Math.abs(mb.partials[0].freqHz - 523.25) < 2, 'strongest partial at 523 Hz, got ' + mb.partials[0].freqHz);
assert(Math.abs(mb.partials[1].ratio - 2.756) < 0.02, 'second mode ratio 2.756, got ' + mb.partials[1].ratio);
assert(mb.ringing.inharmonicity > 0.2, 'bar modes are inharmonic, got ' + mb.ringing.inharmonicity);
assert(mb.ringing.ringScore > 0.8, 'bar rings like a synthetic tone, got ' + mb.ringing.ringScore);
assert(mb.partials[0].ringTime > 1.5, 'fundamental rings > 1.5 s, got ' + mb.partials[0].ringTime);
assert(typeof mb.lufs === 'number' && mb.lufs < 0, 'LUFS is a number');
assert(mb.timeline.length === 8, 'eight timeline slices');

const knockClip = { samples: knock(0.5), sampleRate: sr };
const mk = bro.ear.measure(knockClip);
console.log('knock:', JSON.stringify({ tonality: mk.tonality, flatness: mk.flatness, tailTime: mk.tailTime,
    t60: mk.t60, centroidHz: mk.centroidHz, ringScore: mk.ringing.ringScore }));
assert(mk.tonality < 0.1, 'noise hit is not tonal, got ' + mk.tonality);
assert(mk.ringing.ringScore < 0.1, 'noise hit does not ring, got ' + mk.ringing.ringScore);
assert(mk.tailTime < 0.4, 'noise hit has a short tail, got ' + mk.tailTime);
assert(mk.envelopePeakTime < 0.02, 'noise hit peaks at the start, got ' + mk.envelopePeakTime);

// Deterministic: the same report twice.
assert(JSON.stringify(bro.ear.measure(barClip)) === JSON.stringify(mb), 'measure is deterministic');

// AudioBuffer and bare Float32Array forms agree with the object form.
const ctx = new AudioContext();
const buf = new AudioBuffer({ length: barClip.samples.length, numberOfChannels: 1, sampleRate: sr });
buf.copyToChannel(barClip.samples, 0);
assert(bro.ear.measure(buf).tonality === mb.tonality, 'AudioBuffer form');
assert(bro.ear.measure(barClip.samples, { sampleRate: sr }).tonality === mb.tonality, 'Float32Array form');

// A stereo WAV at 44.1 kHz on disk, by path.
const dir = path.join(os.tmpdir(), 'bro_test_ear');
fs.mkdirSync(dir, { recursive: true });
const wavPath = path.join(dir, 'bar_44k.wav').replace(/\\/g, '/');
{
    const n = Math.floor(2.0 * 44100);
    const st = new Float32Array(n * 2);
    const modes = [[1, 0.5, 0.8], [2.756, 0.3, 0.5], [5.404, 0.2, 0.3]];
    for (let i = 0; i < n; i++) {
        const t = i / 44100;
        let v = 0;
        for (const [r, a, tau] of modes) v += a * Math.exp(-t / tau) * Math.sin(2 * Math.PI * 523.25 * r * t);
        st[2 * i] = v;
        st[2 * i + 1] = v;
    }
    ctx.saveWav(wavPath, st, 2, 44100);
}
assert(fs.existsSync(wavPath), 'wrote ' + wavPath);
const mw = bro.ear.measure(wavPath);
assert(mw.sampleRate === 44100 && mw.channels === 2, 'path clip keeps its rate and channel count');
assert(Math.abs(mw.partials[0].freqHz - 523.25) < 2, 'path clip partial');

// ── compare ──────────────────────────────────────────────────────────────
const self = bro.ear.compare(barClip, barClip);
assert(self.score === 0, 'a clip against itself scores 0, got ' + self.score);
const late = bro.ear.compare({ samples: bar(523.25, 2.0, 0.025), sampleRate: sr }, barClip);
assert(late.score < 0.03 && Math.abs(late.offsetTime - 0.025) < 0.006,
       'a 25 ms late copy is close and its offset found: ' + JSON.stringify(late));
const crossRate = bro.ear.compare(wavPath, barClip);
console.log('44.1k file vs 48k clip:', JSON.stringify(crossRate));
assert(crossRate.sampleRate === 44100 && crossRate.score < 0.05, 'resampled copy is close, got ' + crossRate.score);
const far = bro.ear.compare(knockClip, barClip);
console.log('knock vs bar:', JSON.stringify(far));
assert(far.score > 0.4, 'a noise hit is far from the bar, got ' + far.score);
assert(far.tonality > 0.5 && far.ringTimeRatio < 0.5, 'and the tonality component says why');

// ── spectrogram, side by side, written as a PNG ─────────────────────────
const pngPath = path.join(dir, 'ear_side_by_side.png').replace(/\\/g, '/');
try { fs.unlinkSync(pngPath); } catch (e) {}
const img = bro.ear.spectrogram([barClip, knockClip], {
    layout: 'side', width: 480, height: 240, labels: ['modal bar', 'noise knock'], path: pngPath,
});
console.log('spectrogram:', img.width + 'x' + img.height, 'dB', img.minDb.toFixed(1), '..', img.maxDb.toFixed(1), '->', pngPath);
assert(img.data instanceof Uint8ClampedArray && img.data.length === img.width * img.height * 4, 'RGBA pixels');
assert(img.panels.length === 2 && img.panels[0].y === img.panels[1].y, 'two panels side by side');
assert(img.panels[1].x > img.panels[0].x + img.panels[0].width, 'second panel to the right');
assert(Math.abs(img.duration - 2.0) < 1e-9, 'shared time axis spans the longer clip');
assert(img.path === pngPath && fs.existsSync(pngPath), 'PNG written');
const head = fs.readFileSync(pngPath);
assert(head[0] === 0x89 && head[1] === 0x50 && head[2] === 0x4e && head[3] === 0x47, 'file is a PNG');

// Same pixels every time.
const again = bro.ear.spectrogram([barClip, knockClip], { layout: 'side', width: 480, height: 240, labels: ['modal bar', 'noise knock'] });
let same = again.data.length === img.data.length;
for (let i = 0; same && i < img.data.length; i++) same = img.data[i] === again.data[i];
assert(same, 'spectrogram is deterministic');

// The knock panel is hatched past its 0.5 s end: grey (r == g == b-ish, dark).
const p1 = img.panels[1];
const px = (p1.y + 20) * img.width + (p1.x + Math.floor(p1.width * 0.8));
assert(img.data[4 * px] === img.data[4 * px + 1] && img.data[4 * px] < 80, 'hatched after the shorter clip ends');

// Errors are TypeErrors.
let threw = false;
try { bro.ear.measure(new Float32Array(100)); } catch (e) { threw = e instanceof TypeError; }
assert(threw, 'a bare Float32Array without sampleRate throws TypeError');

console.log('bro.ear OK');
