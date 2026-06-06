// Build the Kokoro EMOTION basis — categorical emotion directions in the 256-D
// style space, the Tier-1 (timbre) companion to voice_basis.json's perceptual
// (prosody) axes. Pure Node: it reshapes embeddings we already produced into one
// artifact that lives beside the Kokoro model (kokoro/ in brosoundml-data, and
// the dev weights dir so kokoro-lab finds it next to the model it loads).
//
// Provenance (the expensive GPU step, run once — see ESD/_probe/embed.js):
//   ESD English wav -> Qwen-Base ECAPA x(1024) -> voice_bridge -> style(256)
//   -> ESD/_probe/styles.jsonl  {file, actor, emo, x, style}
// ESD (studio-recorded) replaced CREMA-D, whose crowd-recorded anger CLIPPED
// (0.11% full-scale vs ESD's 0.00%) — that clipping taught the anger direction
// broadband HF "static". ESD's anger harshness is real vocal effort, not ADC
// distortion. Emotion set is whatever the data carries (ESD: Angry/Happy/Sad/
// Surprise vs the Neutral baseline).
//
// For each emotion e the direction is the WITHIN-SPEAKER neutral->e centroid
// shift (per-actor mean removed, so it's emotion not identity):
//   full[e]  = mean_sc(style | e) - mean_sc(style | NEU)
//   resid[e] = full[e] with the voice_basis attribute (prosody) axes projected
//              out — timbre only.
// The lab applies `full` (style += alpha * full[e]): its prosody-correlated
// components feed Kokoro's duration/F0/energy predictor, so the model renders
// emotional PITCH/ENERGY/PACE *and* timbre in one move — the legible signal.
// resid alone (prosody projected out) moves timbre but the predicted prosody
// barely budges (measured: f0/energy within a few %), so it doesn't read as the
// emotion; it's kept in the artifact for experiments. defaultAlpha is calibrated
// against `full` (~0.55σ), since that's what's applied.
//
// Run:  node bro/tests/_emotion_basis.js

const fs = require('fs');

const STYLES = 'D:/projects/ESD/_probe/styles.jsonl';
const DATA_DIR  = 'D:/projects/brosoundml-data/kokoro';
const MODEL_DIR = 'D:/projects/brosoundml/weights/kokoro';
const SOURCE = 'ESD (English, studio)';
const LABELS = { ANG: 'angry', SAD: 'sad', HAP: 'happy', FEA: 'fearful', DIS: 'disgust', SUR: 'surprise', NEU: 'neutral' };
const TARGET_SIGMA = 0.55;    // σ of the FULL emotion shift that the default intensity aims for
                              // (full carries the audible prosody; calibrated per-emotion below)

function writeBoth(name, data) {
  for (const dir of [DATA_DIR, MODEL_DIR]) {
    try { fs.writeFileSync(dir + '/' + name, data); }
    catch (e) { console.log('  (skip ' + dir + ': ' + e.message + ')'); }
  }
}

// ── load the voice basis (for its prosody axes + the σ metric) ───────────────
const basis = JSON.parse(fs.readFileSync(MODEL_DIR + '/voice_basis.json', 'utf-8'));
const DIM = basis.dim;
const attrAxes = [];
for (let k = 0; k < basis.comps.length; k++) if (basis.axisKind && basis.axisKind[k] === 'attr') attrAxes.push(basis.comps[k]);
console.log('voice_basis: dim', DIM, '·', attrAxes.length, 'prosody axes');

// magnitude of a 256-style step in σ-units: RMS over the k basis axes of (proj/std)
function sigmaOf(vec) {
  let s = 0;
  for (let i = 0; i < basis.k; i++) {
    const a = basis.comps[i]; let p = 0;
    for (let d = 0; d < DIM; d++) p += vec[d] * a[d];
    const z = p / (basis.std[i] || 1); s += z * z;
  }
  return Math.sqrt(s / basis.k);
}
function projOutProsody(v) {     // v - Σ (v·a) a  over the orthonormal attr axes
  const out = Float64Array.from(v);
  for (const a of attrAxes) { let p = 0; for (let d = 0; d < DIM; d++) p += out[d] * a[d]; for (let d = 0; d < DIM; d++) out[d] -= p * a[d]; }
  return out;
}

// ── load embeddings; emotion set is whatever the data carries (minus NEU) ─────
const rows = fs.readFileSync(STYLES, 'utf-8').trim().split('\n').map((l) => JSON.parse(l));
const actors = new Set(rows.map((r) => r.actor));
const EMOS = [...new Set(rows.map((r) => r.emo))].filter((e) => e !== 'NEU').sort();
const LABEL = {}; for (const e of EMOS) LABEL[e] = LABELS[e] || e.toLowerCase();
console.log('embeddings:', rows.length, 'clips ·', actors.size, 'actors · emotions', EMOS.join(','));

// per-actor mean style → speaker-centered emotion centroids
const byActor = {};
for (const r of rows) (byActor[r.actor] = byActor[r.actor] || []).push(r);
const actorMean = {};
for (const a in byActor) {
  const m = new Float64Array(DIM);
  for (const r of byActor[a]) for (let d = 0; d < DIM; d++) m[d] += r.style[d];
  for (let d = 0; d < DIM; d++) m[d] /= byActor[a].length;
  actorMean[a] = m;
}
function centroidSC(emo) {
  const m = new Float64Array(DIM); let n = 0;
  for (const r of rows) { if (r.emo !== emo) continue; const am = actorMean[r.actor]; for (let d = 0; d < DIM; d++) m[d] += r.style[d] - am[d]; n++; }
  for (let d = 0; d < DIM; d++) m[d] /= n; return { m, n };
}
const neu = centroidSC('NEU');

const round = (arr, p) => Array.from(arr).map((v) => +v.toFixed(p));
const full = {}, resid = {}, sigmaFull = {}, sigmaResid = {}, defaultAlpha = {}, count = {};
for (const e of EMOS) {
  const c = centroidSC(e); count[e] = c.n;
  const f = new Float64Array(DIM); for (let d = 0; d < DIM; d++) f[d] = c.m[d] - neu.m[d];
  const r = projOutProsody(f);
  full[e] = round(f, 6); resid[e] = round(r, 6);
  sigmaFull[e]  = +sigmaOf(f).toFixed(4);
  sigmaResid[e] = +sigmaOf(r).toFixed(4);
  defaultAlpha[e] = +Math.max(0.5, Math.min(4, TARGET_SIGMA / (sigmaFull[e] || 1))).toFixed(2);
}

console.log('\nemotion        full σ   resid σ   default α (full)    clips');
for (const e of EMOS)
  console.log('  ' + (LABEL[e] + ' (' + e + ')').padEnd(16) + String(sigmaFull[e]).padEnd(8) + String(sigmaResid[e]).padEnd(10) + String(defaultAlpha[e]).padEnd(18) + count[e]);

const out = {
  dim: DIM, source: SOURCE, method: 'within-speaker neutral->emotion centroid; resid = prosody-axes projected out',
  emotions: EMOS, label: LABEL,
  resid, full, sigmaResid, sigmaFull, defaultAlpha, alphaMax: 5,
  neutralClips: neu.n, actors: actors.size,
};
const json = JSON.stringify(out);
writeBoth('emotion_basis.json', json);
console.log('\nwrote emotion_basis.json (' + (json.length / 1024).toFixed(0) + ' KB) to', DATA_DIR, '+', MODEL_DIR);
console.log('DONE');
