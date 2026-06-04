// Smoke test for authored voices: can we build a voice from raw style floats
// and play it through Kokoro cleanly? Three checks:
//   1. round-trip   — createVoice(pack.data) must reproduce the file voice
//   2. broadcast    — a single style row broadcast must match using that row
//   3. interpolate  — a 50/50 blend of two voices must render finite, non-silent
// All paths are synchronous (loadKokoro/loadVoice sync forms).

const dir = 'D:/projects/brosoundml/weights/kokoro';
bro.tts.setAssetRoot('D:/projects/brosoundml');

const k = bro.tts.loadKokoro(dir);
const vHeart = k.loadVoice(dir + '/voices/af_heart.bin');
const ids = bro.tts.phonemize('Hello there. This is a test of the pipeline.');
console.log('phonemes', ids.length, '· voice', vHeart.rows + 'x' + vHeart.cols);

const data = vHeart.data;                 // Float32Array, rows*cols
const cols = vHeart.cols;
const a = k.synthesize(ids, vHeart).samples;   // reference render

// 1) round-trip the entire pack table
const vCopy = k.createVoice(data, 'heart_copy');
const b = k.synthesize(ids, vCopy).samples;
let maxd = 0;
for (let i = 0; i < a.length; i++) { const d = Math.abs(a[i] - b[i]); if (d > maxd) maxd = d; }
console.log('[1] roundtrip   len', a.length, b.length, '· maxAbsDiff', maxd);

// 2) single style point (the row pick_for would select), broadcast
const L = ids.length + 2;                  // BOS/EOS wrap
const row = data.slice((L - 1) * cols, L * cols);
const vPoint = k.createVoice(row, 'heart_point');
const c = k.synthesize(ids, vPoint).samples;
let maxd2 = 0;
for (let i = 0; i < a.length; i++) { const d = Math.abs(a[i] - c[i]); if (d > maxd2) maxd2 = d; }
console.log('[2] broadcast   len', c.length, '· maxAbsDiff vs full', maxd2);

// 3) 50/50 interpolation of two real voices
const vAdam = k.loadVoice(dir + '/voices/am_adam.bin');
const d2 = vAdam.data;
const blend = new Float32Array(data.length);
for (let i = 0; i < blend.length; i++) blend[i] = 0.5 * data[i] + 0.5 * d2[i];
const vMix = k.createVoice(blend, 'heart_adam_50');
const mix = k.synthesize(ids, vMix).samples;
let peak = 0, nonFinite = 0, rms = 0;
for (let i = 0; i < mix.length; i++) {
  const v = mix[i];
  if (!isFinite(v)) nonFinite++;
  const av = Math.abs(v); if (av > peak) peak = av; rms += v * v;
}
rms = Math.sqrt(rms / mix.length);
console.log('[3] blend       len', mix.length, '· peak', peak.toFixed(4),
            '· rms', rms.toFixed(4), '· nonFinite', nonFinite);

console.log('DONE');
