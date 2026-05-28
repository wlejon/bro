// Probe whether bro.wake fires on non-speech audio. If a plain sine tone or
// band-limited noise at speech-ish volume triggers "computer", the model
// itself has learned a volume/energy detector rather than a phoneme pattern —
// and that's a training data issue in brosoundml, not a wiring issue here.
//
// Run from bro repo root:
//   ./build/Debug/bro-headless.exe ../broworkshop tests/smoke_wake_tone.js

const FS = require('node:fs');
const WEIGHTS = '../brosoundml/weights/wake/computer.bw';

function assert(cond, msg) { if (!cond) throw new Error('assert: ' + msg); }

const wakeRate = 16000;

function makeSine(durSec, hz, rate, peak) {
    const n = Math.floor(durSec * rate);
    const out = new Float32Array(n);
    const w = 2 * Math.PI * hz / rate;
    for (let i = 0; i < n; i++) out[i] = peak * Math.sin(w * i);
    return out;
}

function makeWhiteNoise(durSec, rate, peak, seed = 1) {
    // Cheap deterministic PRNG.
    let s = seed >>> 0;
    function rng() {
        s = (s * 1664525 + 1013904223) >>> 0;
        return s / 0xffffffff;
    }
    const n = Math.floor(durSec * rate);
    const out = new Float32Array(n);
    for (let i = 0; i < n; i++) out[i] = peak * (rng() * 2 - 1);
    return out;
}

function makeAmModulated(durSec, carrierHz, modHz, rate, peak) {
    // Carrier modulated by a slower envelope — fakes "speech-like" bursts.
    const n = Math.floor(durSec * rate);
    const out = new Float32Array(n);
    const w  = 2 * Math.PI * carrierHz / rate;
    const wm = 2 * Math.PI * modHz / rate;
    for (let i = 0; i < n; i++) {
        const env = 0.5 + 0.5 * Math.sin(wm * i);
        out[i] = peak * env * Math.sin(w * i);
    }
    return out;
}

function runStream(samples, sampleRate, chunkSize) {
    bro.wake.stop();
    bro.wake.listen({
        weights:      WEIGHTS,
        threshold:    0.85,
        refractoryMs: 500,
        onFire:       () => {},
    });
    let fires = 0;
    let firstFireMs = -1;
    let peak = 0;
    for (let off = 0; off < samples.length; off += chunkSize) {
        const end = Math.min(off + chunkSize, samples.length);
        const chunk = samples.subarray(off, end);
        const fired = bro.wake.feed(chunk);
        if (fired) {
            fires++;
            if (firstFireMs < 0) firstFireMs = Math.round(1000 * end / sampleRate);
        }
        const s = bro.wake.lastScore();
        if (s > peak) peak = s;
    }
    return { fires, peak, firstFireMs };
}

const CASES = [
    { label: 'silence (zeros)',                samples: new Float32Array(2 * wakeRate) },
    { label: '440 Hz sine @ 0.10 peak',        samples: makeSine(2.0, 440,  wakeRate, 0.10) },
    { label: '440 Hz sine @ 0.30 peak',        samples: makeSine(2.0, 440,  wakeRate, 0.30) },
    { label: '440 Hz sine @ 0.95 peak',        samples: makeSine(2.0, 440,  wakeRate, 0.95) },
    { label: '880 Hz sine @ 0.30 peak',        samples: makeSine(2.0, 880,  wakeRate, 0.30) },
    { label: '2 kHz sine @ 0.30 peak',         samples: makeSine(2.0, 2000, wakeRate, 0.30) },
    { label: 'white noise @ 0.30 peak',        samples: makeWhiteNoise(2.0, wakeRate, 0.30) },
    { label: 'white noise @ 0.95 peak',        samples: makeWhiteNoise(2.0, wakeRate, 0.95) },
    { label: 'AM 500 Hz / 5 Hz @ 0.30 peak',   samples: makeAmModulated(2.0, 500, 5, wakeRate, 0.30) },
];

console.log('--- 16 kHz native (skips SDL resampler, exercises AGC + model only) ---');
for (const c of CASES) {
    const r = runStream(c.samples, wakeRate, 160);
    console.log(c.label.padEnd(38),
        'fires=' + r.fires,
        'peak=' + r.peak.toFixed(3),
        'first_fire=' + (r.firstFireMs < 0 ? '—' : r.firstFireMs + 'ms'));
}

bro.wake.stop();
