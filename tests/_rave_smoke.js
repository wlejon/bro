// Smoke test for the bro.rave bindings: loadRave (sync + async-load convention),
// the Rave handle properties, encode -> latent, decode -> waveform, and an
// edit-the-latent morph round-trip.
// Run (GPU) against the minimal smoke app, pointing at a converted RAVE model:
//   RAVE_DIR=/tmp/rave/out_z8  bro-headless tests/_smoke_app tests/_rave_smoke.js
// The model dir holds config.json + model.safetensors (scripts/convert-rave.py).

function pumpUntil(pred, budgetMs) {
    const start = Date.now();
    while (!pred() && (Date.now() - start) < budgetMs) { sleep(20); }
    return pred();
}

const RAVE_DIR = (typeof process !== 'undefined' && process.env && process.env.RAVE_DIR)
    ? process.env.RAVE_DIR
    : '/tmp/rave/out_z8';

// ── 1. async-load rave ───────────────────────────────────────────────────────
let rave = null, raveErr = null;
const lh = bro.rave.loadRave(RAVE_DIR, {
    onReady: (r) => { rave = r; },
    onError: (m) => { raveErr = m; },
});
assert(lh && typeof lh.cancel === 'function', 'loadRave async returns a handle');
assert(pumpUntil(() => rave || raveErr, 120000), 'rave load finished');
assert(!raveErr, 'rave load did not error: ' + raveErr);
assert(rave.loaded, 'rave reports loaded');
assert(rave.sampleRate > 0, 'rave has a sample rate: ' + rave.sampleRate);
assert(rave.nLatent > 0, 'rave has nLatent: ' + rave.nLatent);
assert(rave.totalRatio > 0, 'rave has totalRatio: ' + rave.totalRatio);
console.log(`rave: sr=${rave.sampleRate} nLatent=${rave.nLatent} ` +
            `nBand=${rave.nBand} totalRatio=${rave.totalRatio}`);

// ── 2. make a 0.5 s test tone at the model rate ──────────────────────────────
const sr = rave.sampleRate;
const n = (sr / 2) | 0;
const audio = new Float32Array(n);
for (let i = 0; i < n; i++) audio[i] = 0.3 * Math.sin(2 * Math.PI * 220 * i / sr);

// ── 3. encode -> latent grid ─────────────────────────────────────────────────
const enc = rave.encode(audio);
assert(enc.nLatent === rave.nLatent, 'encode nLatent matches handle');
assert(enc.frames > 0, 'encode produced frames: ' + enc.frames);
assert(enc.latent.length === enc.nLatent * enc.frames, 'latent grid size');
let finiteL = true;
for (let i = 0; i < enc.latent.length; i++) if (!isFinite(enc.latent[i])) finiteL = false;
assert(finiteL, 'latent is all finite');
console.log(`encode: frames=${enc.frames} latent=${enc.latent.length}`);

// ── 4. decode -> waveform ────────────────────────────────────────────────────
const dec = rave.decode(enc.latent, enc.frames);
assert(dec.sampleRate === sr, 'decode sampleRate matches model');
assert(dec.samples.length === enc.frames * rave.totalRatio, 'decode length');
let finiteW = true, peak = 0;
for (let i = 0; i < dec.samples.length; i++) {
    const v = dec.samples[i];
    if (!isFinite(v)) finiteW = false;
    if (Math.abs(v) > peak) peak = Math.abs(v);
}
assert(finiteW, 'decoded waveform is all finite');
assert(peak > 0, 'decoded waveform is non-silent: peak=' + peak);
console.log(`decode: samples=${dec.samples.length} peak=${peak.toFixed(4)}`);

// ── 5. edit a latent curve and re-decode (the morph path) ────────────────────
const z = Float32Array.from(enc.latent);
for (let t = 0; t < enc.frames; t++) z[0 * enc.frames + t] += 1.5;   // boost dim 0
const morph = rave.decode(z, enc.frames);
assert(morph.samples.length === dec.samples.length, 'morph length matches');
let diff = 0;
for (let i = 0; i < morph.samples.length; i++) diff += Math.abs(morph.samples[i] - dec.samples[i]);
assert(diff > 0, 'editing the latent changed the output');
console.log(`morph: total abs delta=${diff.toFixed(2)}`);

console.log('rave smoke: PASS');
