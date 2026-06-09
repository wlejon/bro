// Build the Qwen voice-identity basis: PCA over the CAMEO speaker x-vectors we
// already embedded for the emotion / masc-fem bases. Pure Node — no bro/model
// needed; it reshapes cameo/_probe/xvecs.jsonl into one artifact that lives
// beside the Base checkpoint (and the shared qwen-tts data dir), so the lab finds
// it next to the model it already loads:
//
//   qwen_voice_basis.json — mean + top-K principal axes of the 1024-D Qwen ECAPA
//                           x-vector space (orthonormal, std-scaled), a spread of
//                           named anchor voices expressed in those axes, and the
//                           realizable per-axis range. Turns 9 fixed presets into
//                           a continuous, slider-navigable speaker manifold over
//                           ~hundreds of real voices.
//
// The basis is built from PER-ACTOR MEAN x-vectors (one identity point per
// speaker), so the axes capture BETWEEN-speaker identity variation, not the
// within-speaker emotion / channel noise the emotion basis isolates. Because
// there are far fewer speakers (N) than dimensions (1024), PCA is done via the
// N×N Gram matrix (Xc·Xcᵀ) and the eigenvectors are mapped back to feature space
// — the standard small-sample trick — instead of a 1024×1024 eigensolve.
//
// Same 1024-D space as synthesize_with_xvector (Base) and the voiceSteer slot, so
// a designed point renders natively on Base and can nudge a CustomVoice preset.
//
// Run:  node bro/tests/_qwen_voice_basis.js

const fs = require('fs');

const PROBE_DIR = 'D:/projects/cameo/_probe';
// Canonical home (published to the HF dataset) + the dev model dir the lab reads.
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

// ── load the x-vectors + per-clip gender; aggregate to per-actor identity ─────
const rows = fs.readFileSync(PROBE_DIR + '/xvecs.jsonl', 'utf8')
  .trim().split('\n').map((s) => JSON.parse(s));
const DIM = rows[0].x.length;
let genders = {};
try { genders = JSON.parse(fs.readFileSync(PROBE_DIR + '/genders.json', 'utf8')); } catch (e) {}

// group clips by actor → mean x-vector (the identity point) + majority gender.
const byActor = new Map();
for (const r of rows) {
  let a = byActor.get(r.actor);
  if (!a) { a = { sum: new Float64Array(DIM), n: 0, g: { F: 0, M: 0, C: 0 } }; byActor.set(r.actor, a); }
  const x = r.x; for (let d = 0; d < DIM; d++) a.sum[d] += x[d];
  a.n++;
  const gv = genders[r.file]; if (gv && a.g[gv] != null) a.g[gv]++;
}
const actors = [];          // { id, x: Float64Array(DIM), gender }
for (const [id, a] of byActor) {
  const x = new Float64Array(DIM); for (let d = 0; d < DIM; d++) x[d] = a.sum[d] / a.n;
  const gender = a.g.F >= a.g.M && a.g.F >= a.g.C ? 'F' : a.g.M >= a.g.C ? 'M' : 'C';
  actors.push({ id, x, gender });
}
const N = actors.length;
console.log('loaded', rows.length, 'clips ·', N, 'actors x', DIM, 'dims');

// ── mean + centered actor matrix ─────────────────────────────────────────────
const mean = new Float64Array(DIM);
for (const a of actors) for (let d = 0; d < DIM; d++) mean[d] += a.x[d];
for (let d = 0; d < DIM; d++) mean[d] /= N;
const Xc = actors.map((a) => { const r = new Float64Array(DIM); for (let d = 0; d < DIM; d++) r[d] = a.x[d] - mean[d]; return r; });

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
  for (let sweepN = 0; sweepN < 100 && off() > 1e-14; sweepN++) {
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
  return { eig, V };               // V[i*n+q] = component i of eigenvector q
}
console.log('eigendecomposing the', N + 'x' + N, 'Gram matrix (Jacobi)…');
const { eig, V } = jacobiEigen(G, N);
const order = Array.from({ length: N }, (_, i) => i).sort((a, b) => eig[b] - eig[a]);
const totalVar = eig.reduce((a, b) => a + Math.max(0, b), 0) || 1;

// ── map each Gram eigenvector u_q back to a feature-space axis v ──────────────
// v = Xcᵀ u / ‖Xcᵀ u‖ (unit); var of the coordinate along v across actors = λ/N,
// so std = sqrt(λ/N) and the σ-unit coord of a point is (x-mean)·v / std.
const kEff = Math.min(K, N - 1);
const comps = [], std = [], range = [], varExplained = [], axisName = [];
const coordsOf = [];      // per-actor σ-unit coords (for anchors + range)
for (let i = 0; i < N; i++) coordsOf.push(new Float64Array(kEff));
for (let k = 0; k < kEff; k++) {
  const q = order[k];
  const lam = Math.max(0, eig[q]);
  // u_q (Gram eigenvector) -> feature axis v
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
  axisName.push('V' + (k + 1));
}

// ── optional hint: how each axis aligns with the masc↔fem direction ──────────
let genderCos = null;
try {
  const mf = JSON.parse(fs.readFileSync(MODEL_DIR + '/masc_fem_basis.json', 'utf8'));
  if (mf && mf.full && mf.full.M) {
    const m = mf.full.M; let mn = 0; for (let d = 0; d < DIM; d++) mn += m[d] * m[d]; mn = Math.sqrt(mn) || 1;
    genderCos = comps.map((v) => { let dp = 0; for (let d = 0; d < DIM; d++) dp += v[d] * m[d]; return +(dp / mn).toFixed(3); });
    const best = genderCos.map((c, k) => [Math.abs(c), k]).sort((a, b) => b[0] - a[0])[0];
    if (best && best[0] > 0.3) axisName[best[1]] = 'V' + (best[1] + 1) + '·m/f';
    console.log('masc/fem alignment: strongest on', axisName[best[1]], '(cos ' + genderCos[best[1]] + ')');
  }
} catch (e) { /* no masc/fem basis → skip the hint */ }

// ── named anchors: a farthest-point spread across the coord cloud ────────────
// Greedy max-min in σ-space so the seeds sample distinct regions of the manifold,
// not 16 near-duplicates. Labeled by majority gender + a running index.
function dist2(a, b) { let s = 0; for (let k = 0; k < kEff; k++) { const d = a[k] - b[k]; s += d * d; } return s; }
const picked = [0];
const far = new Float64Array(N);
for (let i = 0; i < N; i++) far[i] = dist2(coordsOf[i], coordsOf[0]);
while (picked.length < Math.min(N_ANCHORS, N)) {
  let bi = -1, bd = -1; for (let i = 0; i < N; i++) { if (far[i] > bd) { bd = far[i]; bi = i; } }
  picked.push(bi); far[bi] = -1;
  for (let i = 0; i < N; i++) { const d = dist2(coordsOf[i], coordsOf[bi]); if (d < far[i]) far[i] = d; }
}
const gCount = { F: 0, M: 0, C: 0 };
const names = [], anchors = [], label = {};
for (const i of picked) {
  const g = actors[i].gender; gCount[g] = (gCount[g] || 0) + 1;
  const nm = (g === 'F' ? 'fem' : g === 'M' ? 'masc' : 'child') + ' ' + gCount[g];
  names.push(nm); label[nm] = g;
  anchors.push(Array.from(coordsOf[i], (z) => +z.toFixed(4)));
}

// ── all actors as map points: [genderCode, c0..c_{k-1}] in σ units ───────────
// The full coords per actor (not just the 2 map axes) so clicking a point on the
// 2-D voice map snaps to that real speaker's COMPLETE identity, not a flattened one.
const gCode = { F: 0, M: 1, C: 2 };
const points = actors.map((a, i) => [gCode[a.gender]].concat(Array.from(coordsOf[i], (z) => +z.toFixed(3))));

// ── write qwen_voice_basis.json ──────────────────────────────────────────────
const round = (arr, p) => Array.from(arr).map((v) => +v.toFixed(p));
const basis = {
  dim: DIM, k: kEff, n: N, space: 'qwen-xvector',
  source: 'CAMEO (per-actor mean ECAPA x-vectors)', license: 'CC BY 4.0',
  method: 'PCA over per-actor mean x-vectors via the N×N Gram matrix; axes orthonormal, σ-scaled',
  mean: round(mean, 6),
  comps: comps.map((v) => round(v, 6)),
  std: round(std, 6),
  range, varExplained,
  axisName, genderCos,
  names, anchors, label,
  points,
};
const json = JSON.stringify(basis);
writeBoth('qwen_voice_basis.json', json);
const cum = order.slice(0, kEff).reduce((a, q) => a + Math.max(0, eig[q]), 0) / totalVar;
console.log('axes', kEff, '· cumulative variance ' + (100 * cum).toFixed(1) + '% · anchors', names.length,
            '(' + gCount.F + 'F/' + gCount.M + 'M/' + (gCount.C || 0) + 'C)');
console.log('wrote qwen_voice_basis.json (' + (json.length / 1024).toFixed(0) + ' KB) to', DATA_DIR, '+', MODEL_DIR);
console.log('DONE');
