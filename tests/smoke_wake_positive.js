// Feed a known "computer" positive WAV from brosoundml-data through the
// bro.wake binding and report whether the detector fires.
//
// Run from bro repo root:
//   ./build/Debug/bro-headless.exe ../broworkshop tests/smoke_wake_positive.js
//
// Picks up a small selection of clean and noisy positives. Each is replayed
// in 10 ms chunks at 16 kHz (the wake model's native rate). bro.wake.feed runs
// the binding's per-call AGC + WakeWord::feed — the wake-rate path. The SDL
// resampler that the live mic uses is exercised separately, against broaudio's
// shared tap, by tests/smoke_mic_chunks.js.

const FS = require('node:fs');

const WEIGHTS = '../brosoundml/weights/wake/computer.bw';
const ROOT    = '../brosoundml-data/wake/computer/positives';

const CLIPS = [
    'pos_af_bella_sp095_clean.wav',
    'pos_af_bella_sp085_clean.wav',
    'pos_af_bella_sp095_chan_pink_snr10.wav',
    'pos_af_bella_sp095_chan_white_snr0.wav',
];

function assert(cond, msg) { if (!cond) throw new Error('assert: ' + msg); }

function readWav16Mono(path) {
    const buf = FS.readFileSync(path);
    const ab  = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
    const dv  = new DataView(ab);
    assert(dv.getUint32(0, false) === 0x52494646, path + ': not RIFF');
    assert(dv.getUint16(22, true) === 1, path + ': not mono');
    const sampleRate = dv.getUint32(24, true);
    const bps        = dv.getUint16(34, true);
    assert(bps === 16, path + ': expected 16-bit PCM, got ' + bps);
    let off = 12;
    while (off < ab.byteLength) {
        const id = String.fromCharCode(dv.getUint8(off),   dv.getUint8(off+1),
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

const wakeRate = 16000;

function runClip(name, sampleRate, samples, chunkSize) {
    let fired_total = 0;
    let first_fire_ms = -1;
    let peak = 0;
    // stop + listen is the public reset (clears smoothing/refractory and
    // rebuilds the SDL_AudioStream against the current engine mic rate).
    bro.wake.stop();
    bro.wake.listen({
        weights:      WEIGHTS,
        threshold:    0.85,
        refractoryMs: 500,
        onFire:       () => {},
    });
    for (let off = 0; off < samples.length; off += chunkSize) {
        const end = Math.min(off + chunkSize, samples.length);
        const chunk = samples.subarray(off, end);
        const fired = bro.wake.feed(chunk);
        if (fired) {
            fired_total++;
            if (first_fire_ms < 0) {
                first_fire_ms = Math.round(1000 * end / sampleRate);
            }
        }
        const s = bro.wake.lastScore();
        if (s > peak) peak = s;
    }
    return { fired_total, first_fire_ms, peak,
             durMs: Math.round(1000 * samples.length / sampleRate) };
}

console.log('--- 16 kHz native (skips SDL resampler) ---');
for (const name of CLIPS) {
    const wav = readWav16Mono(ROOT + '/' + name);
    assert(wav.sampleRate === wakeRate, name + ': expected 16 kHz');
    const r = runClip(name, wakeRate, wav.samples, 160);
    console.log(name.padEnd(48),
        'fires=' + r.fired_total,
        'peak=' + r.peak.toFixed(3),
        'first_fire=' + (r.first_fire_ms < 0 ? '—' : r.first_fire_ms + 'ms'),
        'len=' + (r.durMs/1000).toFixed(2) + 's');
}

console.log('--- 16 kHz native at 0.10 peak (simulates quiet mic; AGC must lift) ---');
for (const name of CLIPS) {
    const wav = readWav16Mono(ROOT + '/' + name);
    const quiet = new Float32Array(wav.samples.length);
    let cur_peak = 0;
    for (let i = 0; i < wav.samples.length; i++) {
        const a = Math.abs(wav.samples[i]);
        if (a > cur_peak) cur_peak = a;
    }
    const scale = 0.10 / Math.max(cur_peak, 1e-9);
    for (let i = 0; i < wav.samples.length; i++) quiet[i] = wav.samples[i] * scale;
    const r = runClip(name, wakeRate, quiet, 160);
    console.log(name.padEnd(48),
        'fires=' + r.fired_total,
        'peak=' + r.peak.toFixed(3),
        'first_fire=' + (r.first_fire_ms < 0 ? '—' : r.first_fire_ms + 'ms'),
        'len=' + (r.durMs/1000).toFixed(2) + 's');
}

bro.wake.stop();
