// Rebuild the Qwen voice-identity basis over QWEN'S OWN manifold — PCA on the
// self-rendered VoiceDesign corpus (_qwen_voicedesign_corpus.js) instead of the
// CAMEO actors. Same artifact + same math as _qwen_voice_basis.js (orthonormal,
// σ-scaled PCA axes via the N×N Gram matrix), so it's a DROP-IN replacement for
// qwen_voice_basis.json: the lab's 2-D voice map + fine-tune sliders work exactly
// as before, but now span the range Qwen can actually produce — wider coverage,
// more impact per slider — rather than a narrow slice fit to real recordings.
//
// The axes stay ORTHOGONAL (each a distinct, independent identity direction — the
// thing that made the existing sliders feel impactful). We additionally ANNOTATE
// each dominant PC with the prompted attribute it correlates with (pitch / age /
// masc-fem / rasp / breath …), so the orthogonal axes also get friendly names.
// (The earlier supervised regression showed those attribute LABELS are redundant —
// several load on one PC — which is about the labels, not the axes; the manifold
// is rich and PC2, PC3… are real orthogonal timbre directions.)
//
// Run:  node bro/tests/_qwen_voice_basis_vd.js

const fs = require('fs');

// Default to the diverse corpus (balanced PCA); BASIS_CORPUS overrides.
const CORPUS = process.env.BASIS_CORPUS || 'D:/projects/brosoundml-data/qwen-tts/voicedesign-corpus-diverse/xvecs.jsonl';
const DATA_DIR  = 'D:/projects/brosoundml-data/qwen-tts';
const MODEL_DIR = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base';
const K = 24;                              // exported principal axes (sliders)
const N_ANCHORS = 16;                      // named seed voices (farthest-point spread)

function writeBoth(name, data) {
  for (const dir of [DATA_DIR, MODEL_DIR]) {
    try { fs.writeFileSync(dir + '/' + name, data); }
    catch (e) { console.log('  (skip ' + dir + ': ' + e.message + ')'); }
  }
}

// ── load the corpus; each row is one rendered voice (no actor aggregation) ────
const rows = fs.readFileSync(CORPUS, 'utf8').trim().split('\n').map((s) => JSON.parse(s));
const DIM = rows[0].x.length, N = rows.length;
// gender attr → color/label slot: -1 feminine, +1 masculine, 0 androgynous
const gSlot = (g) => (g < 0 ? 0 : g > 0 ? 1 : 2);
const voices = rows.map((r) => ({ x: r.x.map(Number), gslot: gSlot(+r.attrs.gender), attrs: r.attrs }));
// numeric attributes present in every row (grid: 7; diverse: gender/age/pitch) —
// used only to ANNOTATE the orthogonal PCA axes with a friendly name.
const ATTRS = Object.keys(rows[0].attrs).filter((k) => rows.every((r) => isFinite(+r.attrs[k])));
console.log('loaded', N, 'rendered voices x', DIM, 'dims · numeric attrs:', ATTRS.join(','));

// ── mean + centered matrix ───────────────────────────────────────────────────
const mean = new Float64Array(DIM);
for (const v of voices) for (let d = 0; d < DIM; d++) mean[d] += v.x[d];
for (let d = 0; d < DIM; d++) mean[d] /= N;
const Xc = voices.map((v) => { const r = new Float64Array(DIM); for (let d = 0; d < DIM; d++) r[d] = v.x[d] - mean[d]; return r; });

// ── Gram matrix G = Xc·Xcᵀ (N×N) and its Jacobi eigendecomposition ───────────
const G = new Float64Array(N * N);
for (let i = 0; i < N; i++) {
  for (let j = i; j < N; j++) {
    let s = 0; const xi = Xc[i], xj = Xc[j];
    for (let d = 0; d < DIM; d++) s += xi[d] * xj[d];
    G[i * N + j] = s; G[j * N + i] = s;
  }
}
function jacobiEigen(Ain, n) {
  const A = Float64Array.from(Ain);
  const V = new Float64Array(n * n);
  for (let i = 0; i < n; i++) V[i * n + i] = 1;
  const off = () => { let s = 0; for (let i = 0; i < n; i++) for (let j = i + 1; j < n; j++) { const a = A[i * n + j]; s += a * a; } return s; };
  for (let sweepN = 0; sweepN < 100 && off() > 1e-12; sweepN++) {
    for (let p = 0; p < n; p++) for (let q = p + 1; q < n; q++) {
      const apq = A[p * n + q];
      if (Math.abs(apq) < 1e-300) continue;
      const app = A[p * n + p], aqq = A[q * n + q];
      const phi = 0.5 * Math.atan2(2 * apq, aqq - app);
      const c = Math.cos(phi), s = Math.sin(phi);
      for (let k = 0; k < n; k++) { const akp = A[k * n + p], akq = A[k * n + q]; A[k * n + p] = c * akp - s * akq; A[k * n + q] = s * akp + c * akq; }
      for (let k = 0; k < n; k++) { const apk = A[p * n + k], aqk = A[q * n + k]; A[p * n + k] = c * apk - s * aqk; A[q * n + k] = s * apk + c * aqk; }
      for (let k = 0; k < n; k++) { const vkp = V[k * n + p], vkq = V[k * n + q]; V[k * n + p] = c * vkp - s * vkq; V[k * n + q] = s * vkp + c * vkq; }
    }
  }
  const eig = new Float64Array(n);
  for (let i = 0; i < n; i++) eig[i] = A[i * n + i];
  return { eig, V };
}
console.log('eigendecomposing the', N + 'x' + N, 'Gram matrix (Jacobi)…');
const { eig, V } = jacobiEigen(G, N);
const order = Array.from({ length: N }, (_, i) => i).sort((a, b) => eig[b] - eig[a]);
const totalVar = eig.reduce((a, b) => a + Math.max(0, b), 0) || 1;

// ── map each Gram eigenvector back to a feature-space axis; σ-unit coords ─────
const kEff = Math.min(K, N - 1);
const comps = [], std = [], range = [], varExplained = [];
const coordsOf = []; for (let i = 0; i < N; i++) coordsOf.push(new Float64Array(kEff));
for (let k = 0; k < kEff; k++) {
  const q = order[k];
  const lam = Math.max(0, eig[q]);
  const v = new Float64Array(DIM);
  for (let i = 0; i < N; i++) { const u = V[i * N + q]; if (!u) continue; const xc = Xc[i]; for (let d = 0; d < DIM; d++) v[d] += u * xc[d]; }
  let nrm = 0; for (let d = 0; d < DIM; d++) nrm += v[d] * v[d]; nrm = Math.sqrt(nrm) || 1;
  for (let d = 0; d < DIM; d++) v[d] /= nrm;
  const sd = Math.sqrt(lam / N) || 1;
  let lo = Infinity, hi = -Infinity;
  for (let i = 0; i < N; i++) {
    let s = 0; const xc = Xc[i]; for (let d = 0; d < DIM; d++) s += xc[d] * v[d];
    const z = s / sd; coordsOf[i][k] = z; if (z < lo) lo = z; if (z > hi) hi = z;
  }
  comps.push(v); std.push(+sd.toFixed(6)); range.push([+lo.toFixed(3), +hi.toFixed(3)]);
  varExplained.push(+(lam / totalVar).toFixed(4));
}

// ── annotate axes by their dominant prompted-attribute correlation ────────────
// Standardize each attribute across rows, correlate with each axis's σ-coords;
// the strongest |r|>0.35 names the axis (e.g. V1·pitch). Orthogonal axes, friendly
// names. Also report the full per-axis/per-attribute correlation table.
const aZ = {};
for (const a of ATTRS) {
  const vals = voices.map((v) => +v.attrs[a]); let m = 0; for (const x of vals) m += x; m /= N;
  let sd = 0; for (const x of vals) sd += (x - m) * (x - m); sd = Math.sqrt(sd / N) || 1;
  aZ[a] = vals.map((x) => (x - m) / sd);
}
function corrWithAxis(k, a) {
  let cm = 0; for (let i = 0; i < N; i++) cm += coordsOf[i][k]; cm /= N;
  let cv = 0, pv = 0, av = 0; const za = aZ[a];
  for (let i = 0; i < N; i++) { const dc = coordsOf[i][k] - cm; cv += dc * za[i]; pv += dc * dc; av += za[i] * za[i]; }
  return cv / (Math.sqrt(pv * av) || 1);
}
const axisName = [], axisAttr = [];
for (let k = 0; k < kEff; k++) {
  let best = '', bestR = 0;
  for (const a of ATTRS) { const r = corrWithAxis(k, a); if (Math.abs(r) > Math.abs(bestR)) { bestR = r; best = a; } }
  axisAttr.push({ attr: best, r: +bestR.toFixed(3) });
  axisName.push(Math.abs(bestR) > 0.35 ? 'V' + (k + 1) + '·' + best : 'V' + (k + 1));
}

// masc/fem alignment hint (reuse the validated axis)
let genderCos = null;
try {
  const mf = JSON.parse(fs.readFileSync(MODEL_DIR + '/masc_fem_basis.json', 'utf8'));
  if (mf && mf.full && mf.full.M) {
    const m = mf.full.M; let mn = 0; for (let d = 0; d < DIM; d++) mn += m[d] * m[d]; mn = Math.sqrt(mn) || 1;
    genderCos = comps.map((v) => { let dp = 0; for (let d = 0; d < DIM; d++) dp += v[d] * m[d]; return +(dp / mn).toFixed(3); });
  }
} catch (e) {}

// ── named anchors: farthest-point spread across the coord cloud ──────────────
function dist2(a, b) { let s = 0; for (let k = 0; k < kEff; k++) { const d = a[k] - b[k]; s += d * d; } return s; }
const picked = [0];
const far = new Float64Array(N);
for (let i = 0; i < N; i++) far[i] = dist2(coordsOf[i], coordsOf[0]);
while (picked.length < Math.min(N_ANCHORS, N)) {
  let bi = -1, bd = -1; for (let i = 0; i < N; i++) { if (far[i] > bd) { bd = far[i]; bi = i; } }
  picked.push(bi); far[bi] = -1;
  for (let i = 0; i < N; i++) { const d = dist2(coordsOf[i], coordsOf[bi]); if (d < far[i]) far[i] = d; }
}
const GLAB = ['fem', 'masc', 'andro'];
const gCount = { 0: 0, 1: 0, 2: 0 };
const names = [], anchors = [], label = {};
for (const i of picked) {
  const g = voices[i].gslot; gCount[g]++;
  const nm = GLAB[g] + ' ' + gCount[g];
  names.push(nm); label[nm] = ['F', 'M', 'A'][g];
  anchors.push(Array.from(coordsOf[i], (z) => +z.toFixed(4)));
}

// all voices as map points: [genderSlot, c0..c_{k-1}] in σ units
const points = voices.map((v, i) => [v.gslot].concat(Array.from(coordsOf[i], (z) => +z.toFixed(3))));

// ── report + compare with the CAMEO basis ────────────────────────────────────
console.log('\naxis   varExpl   σ(std)   range-span   ↔attr (r)        cos(masc/fem)');
for (let k = 0; k < Math.min(12, kEff); k++) {
  const sp = (range[k][1] - range[k][0]).toFixed(2);
  console.log('  V' + String(k + 1).padEnd(3) + (varExplained[k] * 100).toFixed(1).padStart(6) + '%  ' +
    std[k].toFixed(3).padStart(7) + '   ' + sp.padStart(8) + '   ' +
    (axisAttr[k].attr + ' (' + axisAttr[k].r + ')').padEnd(16) + '  ' + (genderCos ? genderCos[k] : '—'));
}
let cum = order.slice(0, kEff).reduce((a, q) => a + Math.max(0, eig[q]), 0) / totalVar;
console.log('cumulative variance over', kEff, 'axes:', (100 * cum).toFixed(1) + '%');
console.log('std V1..V4:', std.slice(0, 4).map((s) => s.toFixed(3)).join(' '),
            '(CAMEO was 1.145 0.893 0.698 0.601)');

// ── write (back up the CAMEO basis first) ─────────────────────────────────────
const round = (arr, p) => Array.from(arr).map((v) => +v.toFixed(p));
const basis = {
  dim: DIM, k: kEff, n: N, space: 'qwen-xvector',
  source: 'Qwen3-TTS 1.7B VoiceDesign self-rendered corpus (' + N + ' voices)', license: 'generated',
  method: 'PCA over self-rendered VoiceDesign x-vectors via the N×N Gram matrix; axes orthonormal, σ-scaled; spans Qwen\'s producible manifold',
  mean: round(mean, 6),
  comps: comps.map((v) => round(v, 6)),
  std: round(std, 6),
  range, varExplained,
  axisName, axisAttr, genderCos,
  names, anchors, label,
  points,
};
const json = JSON.stringify(basis);
try {
  const old = MODEL_DIR + '/qwen_voice_basis.json';
  if (fs.existsSync(old) && !fs.existsSync(MODEL_DIR + '/qwen_voice_basis.cameo.json'))
    fs.writeFileSync(MODEL_DIR + '/qwen_voice_basis.cameo.json', fs.readFileSync(old));
} catch (e) { console.log('  (backup skip: ' + e.message + ')'); }
writeBoth('qwen_voice_basis.json', json);
console.log('\nwrote qwen_voice_basis.json (' + (json.length / 1024).toFixed(0) + ' KB) to', DATA_DIR, '+', MODEL_DIR,
            '· CAMEO backed up as qwen_voice_basis.cameo.json');
console.log('DONE');
