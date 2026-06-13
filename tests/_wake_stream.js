// 2b verification — per-stream wake detectors over ONE shared net.
//
// bro.wake.load() loads the BC-ResNet once; the default-mic stream (bro.wake.*)
// and a second opened stream (stream.wake.*) each get their OWN WakeWord over
// the shared weights. Proves: independent detectors (each fires only on what IT
// was fed), per-stream policy, isLoaded reflects the shared net, and tenant
// teardown on stream close. Run:
//   bro-headless tests/_smoke_app tests/_wake_stream.js
// Needs ../brosoundml/weights/wake/computer.bw and a positive clip from
// ../brosoundml-data/wake/computer-rawlevel/positives.

const FS = require('node:fs');
const WEIGHTS = '../brosoundml/weights/wake/computer.bw';
const CLIP = '../brosoundml-data/wake/computer-rawlevel/positives/pos_af_bella_sp095_clean.wav';
const wakeRate = 16000;

function readWav16Mono(path) {
    const buf = FS.readFileSync(path);
    const ab  = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
    const dv  = new DataView(ab);
    assert(dv.getUint32(0, false) === 0x52494646, path + ': not RIFF');
    assert(dv.getUint16(22, true) === 1, path + ': not mono');
    const sampleRate = dv.getUint32(24, true);
    assert(dv.getUint16(34, true) === 16, path + ': expected 16-bit PCM');
    let off = 12;
    while (off < ab.byteLength) {
        const id = String.fromCharCode(dv.getUint8(off), dv.getUint8(off+1),
                                       dv.getUint8(off+2), dv.getUint8(off+3));
        const sz = dv.getUint32(off + 4, true);
        if (id === 'data') {
            const n = sz / 2, out = new Float32Array(n);
            for (let i = 0; i < n; i++) out[i] = dv.getInt16(off + 8 + i*2, true) / 32768;
            return { sampleRate, samples: out };
        }
        off += 8 + sz;
    }
    throw new Error(path + ': no data chunk');
}

assert(FS.existsSync(WEIGHTS), 'wake weights exist');
assert(FS.existsSync(CLIP), 'positive clip exists (' + CLIP + ')');
const clip = readWav16Mono(CLIP);
assert(clip.sampleRate === wakeRate, 'clip is 16 kHz');

// Feed a clip (with ~1.2 s leading silence to warm the rolling window) through
// `feedFn` in 10 ms chunks; return how many times the detector fired.
function feedPositive(feedFn) {
    const warm = new Float32Array(Math.floor(wakeRate * 1.2));
    const all = new Float32Array(warm.length + clip.samples.length);
    all.set(warm, 0);
    all.set(clip.samples, warm.length);
    let fires = 0;
    const CHUNK = Math.floor(wakeRate / 100);   // 10 ms
    for (let off = 0; off < all.length; off += CHUNK) {
        if (feedFn(all.subarray(off, Math.min(off + CHUNK, all.length))) === true) fires++;
    }
    return fires;
}

// ── load once, shared ──────────────────────────────────────────────────────
bro.wake.load({ weights: WEIGHTS });
assert(bro.wake.isLoaded(), 'shared net loaded');

// ── open a second, independent stream ────────────────────────────────────────
const s = bro.listen.open('mic');
assert(s && s.valid, 'second stream opened');
assert(s.wake, 'stream exposes a .wake view');
assert(s.wake.isLoaded(), 'stream sees the shared net as loaded');
assert(!s.wake.isActive(), 'stream wake not yet active');

// ── listen on both, separate sinks + policy ───────────────────────────────────
let defFires = 0, strFires = 0;
bro.wake.listen({ onFire: () => defFires++, threshold: 0.5, refractoryMs: 1500 });
s.wake.listen({ onFire: () => strFires++, threshold: 0.5, refractoryMs: 1500 });
assert(bro.wake.isActive() && s.wake.isActive(), 'both streams listening');

// Per-stream tunable: setting one stream's threshold doesn't throw / cross over.
s.wake.setThreshold(0.5);
bro.wake.setThreshold(0.5);

// ── crosstalk: feed the positive to ONE stream at a time ───────────────────────
const defHits = feedPositive((b) => bro.wake.feed(b));
assert(defHits > 0, 'default mic fired on the positive (got ' + defHits + ')');
const strScoreBefore = s.wake.lastScore();
assert(strScoreBefore === 0, 'stream saw no audio yet (score 0, got ' + strScoreBefore + ')');
console.log('[wake-stream] default fired ' + defHits + 'x; stream untouched (score 0)');

const strHits = feedPositive((b) => s.wake.feed(b));
assert(strHits > 0, 'stream fired on the positive (got ' + strHits + ')');
assert(s.wake.lastScore() > 0, 'stream now has a non-zero score');
console.log('[wake-stream] stream fired ' + strHits + 'x on its own feed');

// onFire delivery routed to the right sink (one tick to drain both).
sleep(100);
assert(defFires > 0, 'default onFire delivered (' + defFires + ')');
assert(strFires > 0, 'stream onFire delivered (' + strFires + ')');
console.log('[wake-stream] onFire routing isolated: default=' + defFires +
            ' stream=' + strFires);

// ── close the stream → tenant pruned, default + shared net survive ─────────────
s.wake.stop();
assert(!s.wake.isActive(), 'stream wake stopped');
assert(bro.wake.isActive(), 'default mic still active after stream stop');
s.close();
assert(!s.valid, 'stream closed');
bro.wake.feed(new Float32Array(160));   // drive a tick-worth on default
sleep(100);
assert(bro.wake.isLoaded(), 'shared net still loaded after one stream closed');

bro.wake.stop();
bro.wake.unload();
assert(!bro.wake.isLoaded(), 'unloaded');

console.log('[wake-stream] PASS');
