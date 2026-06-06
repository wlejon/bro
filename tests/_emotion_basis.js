// Build the Kokoro EMOTION basis — categorical emotion directions in the 256-D
// style space, the Tier-1 (timbre) companion to voice_basis.json's perceptual
// (prosody) axes. Pure Node: it reshapes embeddings we already produced into one
// artifact that lives beside the Kokoro model (kokoro/ in brosoundml-data, and
// the dev weights dir so kokoro-lab finds it next to the model it loads).
//
// Provenance (the expensive GPU step, run once — see crema-d/_probe/embed.js):
//   CREMA-D wav -> Qwen-Base ECAPA x(1024) -> voice_bridge -> style(256)
//   -> crema-d/_probe/styles.jsonl  {file,actor,sent,emo,inten,x,style}
// Emotion label = the ACTED emotion from the filename code (balanced), not the
// skewed crowd-perceived CSV classname.
//
// For each emotion e the direction is the WITHIN-SPEAKER neutral->e centroid
// shift (per-actor mean removed, so it's emotion not identity):
//   full[e]  = mean_sc(style | e) - mean_sc(style | NEU)
//   resid[e] = full[e] with the voice_basis attribute (prosody) axes projected
//              out — the NOVEL timbre Tier-0's pitch/energy/rate can't reach.
// Probe B (closed-loop re-embed) confirmed Kokoro's decoder renders BOTH; resid
// is the Tier-1 primitive (full double-drives prosody via the predictor, so it
// saturates — that's Tier-0's job). Applied as: style += alpha * resid[e].
//
// Run:  node bro/tests/_emotion_basis.js

const fs = require('fs');

const STYLES = 'D:/projects/crema-d/_probe/styles.jsonl';
const DATA_DIR  = 'D:/projects/brosoundml-data/kokoro';
const MODEL_DIR = 'D:/projects/brosoundml/weights/kokoro';
const EMOS  = ['ANG', 'SAD', 'HAP', 'FEA', 'DIS'];
const LABEL = { ANG: 'angry', SAD: 'sad', HAP: 'happy', FEA: 'fearful', DIS: 'disgust' };
const TARGET_SIGMA = 0.7;     // timbre-σ that intensity 1.0 aims for (per-emotion default alpha)

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

// ── within-speaker emotion centroids ─────────────────────────────────────────
const rows = fs.readFileSync(STYLES, 'utf-8').trim().split('\n').map((l) => JSON.parse(l));
const actors = new Set(rows.map((r) => r.actor));
console.log('embeddings:', rows.length, 'clips ·', actors.size, 'actors');

const byActor = {};
for (const r of rows) (byActor[r.actor] = byActor[r.actor] || []).push(r);
const actorMean = {};
for (const a in byActor) {
  const m = new Float64Array(DIM);
  for (const r of byActor[a]) for (let d = 0; d < DIM; d++) m[d] += r.style[d];
  for (let d = 0; d < DIM; d++) m[d] /= byActor[a].length;
  actorMean[a] = m;
}
function centroidSC(emo) {       // speaker-centered centroid for one emotion
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
  // intensity 1.0 -> ~TARGET_SIGMA of resid timbre, clamped to a tasteful range
  defaultAlpha[e] = +Math.max(1, Math.min(5, TARGET_SIGMA / (sigmaResid[e] || 1))).toFixed(2);
}

console.log('\nemotion        full σ   resid σ   default α (resid)   clips');
for (const e of EMOS)
  console.log('  ' + (LABEL[e] + ' (' + e + ')').padEnd(16) + String(sigmaFull[e]).padEnd(8) + String(sigmaResid[e]).padEnd(10) + String(defaultAlpha[e]).padEnd(18) + count[e]);

const out = {
  dim: DIM, source: 'CREMA-D (acted)', method: 'within-speaker neutral->emotion centroid; resid = prosody-axes projected out',
  emotions: EMOS, label: LABEL,
  resid, full, sigmaResid, sigmaFull, defaultAlpha, alphaMax: 5,
  neutralClips: neu.n, actors: actors.size,
};
const json = JSON.stringify(out);
writeBoth('emotion_basis.json', json);
console.log('\nwrote emotion_basis.json (' + (json.length / 1024).toFixed(0) + ' KB) to', DATA_DIR, '+', MODEL_DIR);
console.log('DONE');
