// Multi-pass wake regression through the REAL mic tap (resample + AGC + the
// rolling streaming window + suspend gate). bro.mic.feed runs injectMicSamples
// synchronously on this thread, fanning out to the wake tap; advanceTime() pumps
// tickWake() so onFire lands. Mirrors the app loop:
//
//   say "computer" -> onFire -> suspend() (record + STT/LLM/TTS) -> resume()
//
// The detector must keep ROLLING while suspended (feeding continues; only fire
// delivery is gated), so on resume there is NO warmup gap and NO stale state.
// The decisive check below feeds the wake word IMMEDIATELY after resume() with
// no warmup audio in between — which the earlier (broken) reset-on-resume build,
// re-arming a ~610 ms warmup each cycle, would fail.
//
//   ./build/Debug/bro-headless.exe ../broworkshop tests/repro_wake_passes.js

const FS = require('node:fs');
const WEIGHTS = '../brosoundml/weights/wake/computer.bw';
const ROOT    = '../brosoundml-data/wake/computer/positives';
const CLIP    = 'pos_af_bella_sp095_clean.wav';

function assert(cond, msg) { if (!cond) throw new Error('FAIL: ' + msg); }

function readWav16Mono(path) {
    const buf = FS.readFileSync(path);
    const ab  = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
    const dv  = new DataView(ab);
    const sampleRate = dv.getUint32(24, true);
    let off = 12;
    while (off < ab.byteLength) {
        const id = String.fromCharCode(dv.getUint8(off), dv.getUint8(off+1),
                                       dv.getUint8(off+2), dv.getUint8(off+3));
        const sz = dv.getUint32(off + 4, true);
        if (id === 'data') {
            const n = sz / 2;
            const out = new Float32Array(n);
            for (let i = 0; i < n; i++) out[i] = dv.getInt16(off + 8 + i*2, true) / 32768;
            return { sampleRate, samples: out };
        }
        off += 8 + sz;
    }
    throw new Error(path + ': no data chunk');
}

function resampleLinear(samples, fromRate, toRate) {
    if (fromRate === toRate) return samples;
    const ratio = fromRate / toRate;
    const n = Math.floor(samples.length / ratio);
    const out = new Float32Array(n);
    for (let i = 0; i < n; i++) {
        const x = i * ratio, i0 = Math.floor(x), i1 = Math.min(i0 + 1, samples.length - 1);
        const f = x - i0;
        out[i] = samples[i0] * (1 - f) + samples[i1] * f;
    }
    return out;
}

const ENGINE_RATE = bro.mic.engineRate();
const wav = readWav16Mono(ROOT + '/' + CLIP);
const posEng     = resampleLinear(wav.samples, wav.sampleRate, ENGINE_RATE);
const respEng    = new Float32Array(Math.floor(2.0 * ENGINE_RATE)); // "response" silence

let fires = 0;
bro.wake.stop();
bro.wake.listen({
    weights: WEIGHTS, threshold: 0.85, refractoryMs: 500,
    device: 'cpu',
    onFire: () => { fires++; },
});
bro.mic.start({ chunkFrames: 0, targetRate: 16000, agc: true, live: false,
                targetPeak: 0.95, halfLifeSec: 1.0, noiseGate: 0.01, maxGain: 10 });

function inject(samples) {
    const before = fires;
    bro.mic.feed(samples, ENGINE_RATE);
    advanceTime(60);
    return fires - before;
}

// Initial warmup once after listen() (the only warmup a rolling detector needs).
inject(new Float32Array(Math.floor(1.2 * ENGINE_RATE)));

const PASSES = 6;
const perPass = [];
let firedDuringSuspend = 0;
for (let p = 0; p < PASSES; p++) {
    const f = inject(posEng);          // say "computer"
    perPass.push(f);

    bro.wake.suspend();                // record + think + speak
    firedDuringSuspend += inject(respEng); // detector keeps rolling; must NOT surface fires
    bro.wake.resume();
    // NOTE: no warmup audio here — the very next thing is the wake word.
}

bro.mic.stop();
bro.wake.stop();

console.log('fires per pass:', JSON.stringify(perPass));
console.log('fires surfaced during suspend:', firedDuringSuspend);
const activated = perPass.filter(n => n >= 1).length;
console.log(`activated on ${activated}/${PASSES} passes`);
assert(perPass[0] >= 1, 'first pass must activate');
assert(activated === PASSES, `every pass must activate immediately after resume (got ${JSON.stringify(perPass)})`);
assert(firedDuringSuspend === 0, 'fires must NOT be surfaced while suspended');
console.log('OK');
