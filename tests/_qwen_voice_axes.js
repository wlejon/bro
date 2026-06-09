// DIAGNOSTIC (not the lab's control surface — see the finding below). Fits
// supervised per-attribute directions in the 1024-D ECAPA x-vector space from the
// self-rendered VoiceDesign corpus (_qwen_voicedesign_corpus.js), one per prompted
// attribute (pitch/age/gender/brightness/weight/rough/breath), to ask: are those
// attributes SEPARABLE directions you could dial independently?
//
// FINDING (gated on Base synth via _qwen_voice_axes_sweep.js): no. pitch / age /
// gender / brightness / weight are nearly collinear (|cos| 0.66–0.95) and all just
// move f0 — they're one axis (≈ the masc/fem direction). Only rough/breath are
// even partly distinct. This is a property of the x-vector (a speaker-ID embedding
// is low-rank for voice), so named per-attribute sliders would be fake granularity.
// The lab's real control surface is the ORTHOGONAL PCA basis (_qwen_voice_basis_vd.js)
// rebuilt over this same corpus — broad, impactful, distinct axes. Keep this script
// for the analysis, not for an artifact the lab loads.
//
// Method — ridge regression of the x-vector on the attribute matrix:
//   A : N×P  standardized prompted attribute levels (one column per attribute)
//   X : N×D  ECAPA x-vectors,  Xc = X − mean(X)
//   W = (AᵀA + λI)⁻¹ Aᵀ Xc       (P×D)
// Row W[p] is the PARTIAL direction for attribute p with the others held fixed —
// disentangled because the corpus samples each attribute independently (AᵀA is
// near-diagonal), and ridge cleans up the residual leakage. Contrast this with
// masc_fem_basis (a single diff-of-means contrast) and qwen_voice_basis (PCA over
// CAMEO variance): regression gives one clean axis PER attribute at once.
//
// σ-scaling + defaultAlpha mirror masc_fem_basis so the lab consumes every axis
// through the same `xvector += alpha·dir` path. TARGET_SIGMA / ALPHA_MAX are
// provisional — calibrate them against REAL Base synthesis with
// _qwen_voice_axes_sweep.js (gate on the model's f0 / centroid, not this geometry).
//
// Run:  node bro/tests/_qwen_voice_axes.js

const fs = require('fs');

const CORPUS = 'D:/projects/brosoundml-data/qwen-tts/voicedesign-corpus';
const MODEL_DIR = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base';
const DATA_DIR  = 'D:/projects/brosoundml-data/qwen-tts';
const LAMBDA = 1e-3;             // ridge strength, scaled by N below
const TARGET_SIGMA = 0.45;       // default-intensity z-magnitude (matches masc_fem; calibrate via sweep)
const ALPHA_MAX = 3;             // slider ceiling

// axis presentation: which corpus attribute, label, poles, slider shape.
const AXES = [
  { key: 'gender', label: 'masc · fem', poles: { neg: 'feminine', pos: 'masculine' }, bipolar: true },
  { key: 'age',    label: 'age',        poles: { neg: 'younger',  pos: 'older' },     bipolar: true },
  { key: 'pitch',  label: 'pitch',      poles: { neg: 'low',      pos: 'high' },      bipolar: true },
  { key: 'bright', label: 'brightness', poles: { neg: 'warm/dark',pos: 'bright' },    bipolar: true },
  { key: 'weight', label: 'weight',     poles: { neg: 'light',    pos: 'deep' },      bipolar: true },
  { key: 'rough',  label: 'texture',    poles: { neg: 'smooth',   pos: 'gravelly' },  bipolar: true },
  { key: 'breath', label: 'breathiness',poles: { neg: 'clear',    pos: 'breathy' },   bipolar: false },
];

function writeBoth(name, data) {
  for (const dir of [DATA_DIR, MODEL_DIR]) {
    try { fs.writeFileSync(dir + '/' + name, data); }
    catch (e) { console.log('  (skip ' + dir + ': ' + e.message + ')'); }
  }
}

// ── load corpus ───────────────────────────────────────────────────────────────
const rows = fs.readFileSync(CORPUS + '/xvecs.jsonl', 'utf8').trim().split('\n').map((l) => JSON.parse(l));
const N = rows.length, D = rows[0].x.length, P = AXES.length;
console.log('corpus:', N, 'rows · dim', D, '· attributes', P);

// X (N×D) and per-dim mean / population std (the σ metric, as in masc_fem_basis).
const mean = new Float64Array(D);
for (const r of rows) for (let d = 0; d < D; d++) mean[d] += r.x[d];
for (let d = 0; d < D; d++) mean[d] /= N;
const popStd = new Float64Array(D);
for (const r of rows) for (let d = 0; d < D; d++) { const dd = r.x[d] - mean[d]; popStd[d] += dd * dd; }
for (let d = 0; d < D; d++) popStd[d] = Math.sqrt(popStd[d] / N) || 1;
function sigmaOf(vec) { let s = 0; for (let d = 0; d < D; d++) { const z = vec[d] / popStd[d]; s += z * z; } return Math.sqrt(s / D); }

// A (N×P) standardized attribute levels.
const aMean = new Float64Array(P), aStd = new Float64Array(P);
const Araw = rows.map((r) => AXES.map((ax) => +r.attrs[ax.key]));
for (let p = 0; p < P; p++) { let m = 0; for (let i = 0; i < N; i++) m += Araw[i][p]; aMean[p] = m / N; }
for (let p = 0; p < P; p++) { let v = 0; for (let i = 0; i < N; i++) { const d = Araw[i][p] - aMean[p]; v += d * d; } aStd[p] = Math.sqrt(v / N) || 1; }
const A = Araw.map((row) => row.map((v, p) => (v - aMean[p]) / aStd[p]));

// ── ridge: W = (AᵀA + λI)⁻¹ Aᵀ Xc ────────────────────────────────────────────
// AtA (P×P) and AtXc (P×D). Xc computed on the fly (X − mean).
const AtA = Array.from({ length: P }, () => new Float64Array(P));
for (let i = 0; i < N; i++) for (let p = 0; p < P; p++) { const a = A[i][p]; for (let q = 0; q < P; q++) AtA[p][q] += a * A[i][q]; }
const lam = LAMBDA * N;
for (let p = 0; p < P; p++) AtA[p][p] += lam;
const AtXc = Array.from({ length: P }, () => new Float64Array(D));
for (let i = 0; i < N; i++) { const xr = rows[i].x; for (let p = 0; p < P; p++) { const a = A[i][p]; for (let d = 0; d < D; d++) AtXc[p][d] += a * (xr[d] - mean[d]); } }

// Gauss-Jordan inverse of the small P×P (AᵀA + λI).
function inv(M) {
  const n = M.length;
  const aug = M.map((row, i) => { const r = Array.from(row); for (let j = 0; j < n; j++) r.push(i === j ? 1 : 0); return r; });
  for (let c = 0; c < n; c++) {
    let piv = c; for (let r = c + 1; r < n; r++) if (Math.abs(aug[r][c]) > Math.abs(aug[piv][c])) piv = r;
    const tmp = aug[c]; aug[c] = aug[piv]; aug[piv] = tmp;
    const d = aug[c][c] || 1e-12; for (let j = 0; j < 2 * n; j++) aug[c][j] /= d;
    for (let r = 0; r < n; r++) { if (r === c) continue; const f = aug[r][c]; for (let j = 0; j < 2 * n; j++) aug[r][j] -= f * aug[c][j]; }
  }
  return aug.map((row) => row.slice(n));
}
const AtAinv = inv(AtA);

// W[p] = Σ_q AtAinv[p][q] · AtXc[q]
const W = [];
for (let p = 0; p < P; p++) {
  const w = new Float64Array(D);
  for (let q = 0; q < P; q++) { const c = AtAinv[p][q]; if (!c) continue; const row = AtXc[q]; for (let d = 0; d < D; d++) w[d] += c * row[d]; }
  W.push(w);
}

// ── diagnostics: how cleanly does each attribute track ITS OWN axis, and how
// independent are the axes from each other / from the prior masc_fem axis? ─────
function dot(a, b) { let s = 0; for (let d = 0; d < D; d++) s += a[d] * b[d]; return s; }
function norm(a) { return Math.sqrt(dot(a, a)) || 1; }
function cos(a, b) { return dot(a, b) / (norm(a) * norm(b)); }
// corr between the standardized attribute and the x-vector's projection on dir.
function trackCorr(p) {
  const w = W[p], nrm = norm(w);
  const proj = new Float64Array(N); let pm = 0;
  for (let i = 0; i < N; i++) { let s = 0; const xr = rows[i].x; for (let d = 0; d < D; d++) s += (xr[d] - mean[d]) * w[d]; proj[i] = s / nrm; pm += proj[i]; }
  pm /= N;
  let cv = 0, pv = 0, av = 0;
  for (let i = 0; i < N; i++) { const dp = proj[i] - pm, da = A[i][p]; cv += dp * da; pv += dp * dp; av += da * da; }
  return cv / (Math.sqrt(pv * av) || 1);
}

let mascFem = null;
try { const mf = JSON.parse(fs.readFileSync(MODEL_DIR + '/masc_fem_basis.json', 'utf8')); if (mf && mf.full && mf.full.M) mascFem = mf.full.M; } catch (e) {}

console.log('\naxis          track-r   σ(dir)   default α   cos(prior masc/fem)');
const axesOut = [];
for (let p = 0; p < P; p++) {
  const ax = AXES[p], w = W[p];
  const sig = sigmaOf(w);
  const defAlpha = +Math.max(0.5, Math.min(ALPHA_MAX, TARGET_SIGMA / (sig || 1))).toFixed(2);
  const tr = +trackCorr(p).toFixed(3);
  const cmf = mascFem ? +cos(w, mascFem).toFixed(3) : null;
  console.log('  ' + ax.label.padEnd(14) + String(tr).padStart(6) + '   ' + sig.toFixed(4) + '   ' +
              String(defAlpha).padEnd(9) + '   ' + (cmf == null ? '—' : cmf));
  axesOut.push({
    key: ax.key, label: ax.label, poles: ax.poles, bipolar: ax.bipolar,
    full: Array.from(w, (v) => +v.toFixed(6)),
    sigmaFull: +sig.toFixed(4), defaultAlpha: defAlpha, alphaMax: ALPHA_MAX,
    trackCorr: tr, cosMascFem: cmf,
  });
}

// cross-axis independence (cosine between learned directions)
console.log('\ncross-axis cosine (≈0 = independent):');
let hdr = '       '; for (const ax of AXES) hdr += ax.key.slice(0, 6).padStart(8); console.log(hdr);
for (let p = 0; p < P; p++) {
  let line = AXES[p].key.slice(0, 6).padEnd(7);
  for (let q = 0; q < P; q++) line += (p === q ? '   1.00' : (+cos(W[p], W[q]).toFixed(2)).toFixed(2).padStart(8)).padStart(8);
  console.log(line);
}

// ── write voice_axes.json ─────────────────────────────────────────────────────
const out = {
  dim: D, space: 'qwen-xvector',
  source: 'Qwen3-TTS 1.7B VoiceDesign self-rendered corpus (' + N + ' recipes)',
  method: 'ridge regression of ECAPA x-vector on independently-sampled VoiceDesign attribute levels; per-attribute PARTIAL direction (others held fixed), σ-scaled. xvector += alpha·full[axis].',
  lambda: LAMBDA, targetSigma: TARGET_SIGMA, alphaMax: ALPHA_MAX, n: N,
  axes: axesOut,
};
writeBoth('voice_axes.json', JSON.stringify(out));
console.log('\nwrote voice_axes.json (' + (JSON.stringify(out).length / 1024).toFixed(0) + ' KB) to', DATA_DIR, '+', MODEL_DIR);
console.log('DONE');
