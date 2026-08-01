// bro.media — the waveform and the filmstrip a timeline is drawn from.
//
// Both decode the whole file through the media backend registry, so this
// exercises the built-in WebM path; a host that registers its own backend gets
// the same two calls for every format it can open.

const os = require('os');
const path = require('path');
const fs = require('fs');

const W = 96, H = 64, FPS = 10, N = 40;
const RATE = 48000;
const src = path.join(os.tmpdir(), 'bro_media_' + Date.now() + '.webm');

// A clip whose picture and sound both change over time, so "did the analysis
// see the file" is answerable rather than a matter of taste: the picture ramps
// from black to red, and the sound is silent for the first half.
const enc = new VideoEncoder({
    path: src, width: W, height: H, fps: FPS, quality: 'realtime',
    audioSampleRate: RATE, audioChannels: 1, audioBitrateKbps: 64,
});

const px = new Uint8Array(W * H * 4);
const chunk = new Float32Array(RATE / FPS);      // one video frame of audio
for (let f = 0; f < N; ++f) {
    const level = Math.round((f / (N - 1)) * 255);
    for (let i = 0; i < W * H; ++i) {
        px[i * 4] = level; px[i * 4 + 1] = 0; px[i * 4 + 2] = 0; px[i * 4 + 3] = 255;
    }
    enc.addFrameRGBA(px);

    const loud = f >= N / 2;
    for (let i = 0; i < chunk.length; ++i)
        chunk[i] = loud ? 0.6 * Math.sin(i * 0.09) : 0;
    enc.addAudioFramesPCM(chunk);
}
enc.finish();
assert(fs.existsSync(src), 'encoded a test clip');

// ── the waveform ──────────────────────────────────────────────────────────

const peaks = bro.media.peaks(src, { buckets: 200 });
assert(peaks, 'peaks() returned a result');
assert(peaks.sampleRate === RATE, `sampleRate ${peaks.sampleRate} = ${RATE}`);
assert(peaks.channels === 1, `channels ${peaks.channels}`);
assert(Math.abs(peaks.duration - N / FPS) < 0.5,
       `duration ${peaks.duration.toFixed(3)}s ≈ ${N / FPS}s`);
assert(peaks.min.length === peaks.buckets && peaks.max.length === peaks.buckets,
       'min/max are one entry per bucket');
assert(peaks.rms.length === peaks.buckets, 'rms too');

// Quiet first half, loud second half — the whole point of a waveform is that
// it distinguishes them.
const half = peaks.buckets >> 1;
let quiet = 0, loudBuckets = 0;
for (let i = 0; i < half; ++i) if (peaks.rms[i] < 0.05) quiet++;
for (let i = half + 4; i < peaks.buckets - 4; ++i) if (peaks.rms[i] > 0.2) loudBuckets++;
assert(quiet > half * 0.8, `first half reads quiet (${quiet}/${half} buckets)`);
assert(loudBuckets > (peaks.buckets - half) * 0.7,
       `second half reads loud (${loudBuckets} buckets)`);

let lo = 0, hi = 0;
for (let i = 0; i < peaks.buckets; ++i) {
    lo = Math.min(lo, peaks.min[i]);
    hi = Math.max(hi, peaks.max[i]);
}
assert(hi > 0.3 && hi < 1.5, `envelope peaks at ${hi.toFixed(3)}`);
assert(lo < -0.3 && lo > -1.5, `and troughs at ${lo.toFixed(3)}`);

// ── the filmstrip ─────────────────────────────────────────────────────────

const strip = bro.media.thumbnails(src, { count: 8, height: 32 });
assert(strip, 'thumbnails() returned a result');
assert(strip.count === 8, `got ${strip.count} thumbnails`);
assert(strip.height === 32, `height ${strip.height}`);
assert(strip.width === Math.round(32 * W / H),
       `width ${strip.width} follows the frame aspect`);
assert(strip.data.length === strip.width * strip.count * strip.height * 4,
       'one image, count thumbnails wide');
assert(strip.times.length === strip.count, 'a timestamp per thumbnail');
for (let i = 1; i < strip.times.length; ++i)
    assert(strip.times[i] > strip.times[i - 1],
           `times walk forward (${strip.times[i - 1]} -> ${strip.times[i]})`);

// The clip ramps black to red, so the thumbnails must get redder left to
// right. This is what catches a strip that grabbed the same keyframe every
// time — the failure that makes a filmstrip useless.
const stripW = strip.width * strip.count;
const redOf = (i) => {
    let sum = 0, n = 0;
    for (let y = 4; y < strip.height - 4; ++y)
        for (let x = 2; x < strip.width - 2; ++x) {
            sum += strip.data[((y * stripW) + i * strip.width + x) * 4];
            n++;
        }
    return sum / n;
};
const first = redOf(0), last = redOf(strip.count - 1);
assert(last > first + 60,
       `the strip walks the file (red ${first.toFixed(0)} -> ${last.toFixed(0)})`);
let rising = 0;
for (let i = 1; i < strip.count; ++i) if (redOf(i) > redOf(i - 1)) rising++;
assert(rising >= strip.count - 2, `and does it monotonically (${rising}/${strip.count - 1})`);

// ── a window ──────────────────────────────────────────────────────────────
//
// The clip is quiet for its first half and loud for its second, so "did the
// window move" is answerable rather than a matter of taste: a read of the
// first half must be quiet all the way across and a read of the second loud
// all the way across. A window that was ignored would give both of them the
// same shape, which is the failure worth catching — the buckets would still
// come back the right length and full of plausible numbers.

const secs = N / FPS;
const early = bro.media.peaks(src, { buckets: 100, from: 0, to: secs / 2 });
const late = bro.media.peaks(src, { buckets: 100, from: secs / 2 });
assert(early && late, 'a windowed peaks() at each end');
assert(Math.abs(early.from - 0) < 0.01 && Math.abs(early.to - secs / 2) < 0.3,
       `the early window says where it is (${early.from.toFixed(2)}..${early.to.toFixed(2)})`);
assert(Math.abs(late.to - secs) < 0.3,
       `and 'to' omitted runs to the end (${late.to.toFixed(2)}s of ${secs}s)`);
assert(Math.abs(early.duration - secs) < 0.5,
       'duration is still the FILE, not the window');

const loudest = (p) => { let m = 0; for (let i = 0; i < p.buckets; ++i) m = Math.max(m, p.rms[i]); return m; };
let lateQuiet = 0;
for (let i = 4; i < late.buckets - 4; ++i) if (late.rms[i] > 0.2) lateQuiet++;
assert(loudest(early) < 0.05, `the first half is quiet throughout (peak ${loudest(early).toFixed(3)})`);
assert(lateQuiet > (late.buckets - 8) * 0.8,
       `the second half is loud throughout (${lateQuiet}/${late.buckets - 8} buckets)`);

// Whole-file reads report the whole file as their span, so a caller drawing a
// lane needs no branch for "did I ask for a window".
assert(peaks.from === 0 && Math.abs(peaks.to - peaks.duration) < 1e-6,
       'an unwindowed read spans the file');

assert(bro.media.peaks(src, { buckets: 16, from: 3, to: 1 }) === null,
       'a window that ends before it starts is null, not the whole file');
assert(bro.media.peaks(src, { buckets: 16, from: secs + 10 }) === null,
       'and one that starts past the end is too');

// The same for the picture: a strip of the second half is grabbed from the
// second half, and the file ramps black to red so it is visibly redder than
// one of the first.
const lateStrip = bro.media.thumbnails(src, { count: 6, height: 32, from: secs / 2 });
assert(lateStrip && lateStrip.count > 0, 'a windowed thumbnails()');
for (const t of lateStrip.times)
    assert(t >= secs / 2 - 0.25, `every frame is from the window (${t.toFixed(2)}s)`);
const lateW = lateStrip.width * lateStrip.count;
let sum = 0, n = 0;
for (let y = 4; y < lateStrip.height - 4; ++y)
    for (let x = 2; x < lateStrip.width - 2; ++x) { sum += lateStrip.data[((y * lateW) + x) * 4]; n++; }
assert(sum / n > first + 60,
       `and starts where the file is redder (${(sum / n).toFixed(0)} vs ${first.toFixed(0)})`);

// ── failures are reported, not thrown ─────────────────────────────────────

assert(bro.media.peaks(src + '.nope', { buckets: 16 }) === null,
       'peaks() on a missing file returns null');
assert(bro.media.thumbnails(src + '.nope', { count: 2 }) === null,
       'thumbnails() on a missing file returns null');

try { fs.unlinkSync(src); } catch (e) {}
console.log('PASS');
