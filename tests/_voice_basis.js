// Build the Kokoro voice-space basis (PCA over the clean swept Kokoro styles) +
// pack the ECAPA->style bridge into a compact binary. Pure Node — no bro/model
// needed; it only reshapes data we already produced under D:/projects/voice-sweep
// into two artifacts that live alongside the Kokoro model (kokoro/ in
// brosoundml-data, and the dev weights dir so kokoro-lab finds them next to the
// model it already loads):
//
//   voice_basis.json  — mean + top-K principal axes of the 256-D Kokoro style
//                       space (orthonormal, std-scaled), the 28 named anchors
//                       expressed in those axes, the realizable per-axis range,
//                       and each axis' strongest correlate among the measured
//                       attributes (f0/energy/rate/...). Turns 256 opaque dims
//                       into a handful of meaningful sliders.
//   voice_bridge.bin  — the linear ridge x(1024)->style(256): little-endian
//                       [int32 D, int32 M, f32 xm[D], f32 ym[M], f32 B[D*M]].
//                       Clones a real clip (ECAPA x-vector) into the style space.
//
// Run:  node bro/tests/_voice_basis.js

const fs = require('fs');

const SWEEP_DIR = 'D:/projects/voice-sweep';
// Canonical home (published to the HF dataset) + the dev model dir the app reads.
const DATA_DIR = 'D:/projects/brosoundml-data/kokoro';
const MODEL_DIR = 'D:/projects/brosoundml/weights/kokoro';
const K = 20;                              // exported principal axes (sliders)
const DIM = 256;

// write a file into both the canonical data dir and the dev model dir
function writeBoth(name, data) {
  for (const dir of [DATA_DIR, MODEL_DIR]) {
    try { fs.writeFileSync(dir + '/' + name, data); }
    catch (e) { console.log('  (skip ' + dir + ': ' + e.message + ')'); }
  }
}

// ── load the clean swept styles + their measured attributes ─────────────────
const sweep = fs.readFileSync(SWEEP_DIR + '/sweep.jsonl', 'utf8')
  .trim().split('\n').map((s) => JSON.parse(s));
const X = sweep.map((r) => r.style);       // N x 256
const N = X.length;
const ATTR_KEYS = ['f0_mean', 'rms', 'energy', 'rate', 'zcr', 'f0_std'];
const attrs = {};
for (const k of ATTR_KEYS) attrs[k] = sweep.map((r) => (r.attrs ? r.attrs[k] : NaN));
console.log('loaded', N, 'styles x', X[0].length, 'dims');

// ── mean + centered covariance (256 x 256) ──────────────────────────────────
const mean = new Float64Array(DIM);
for (const x of X) for (let d = 0; d < DIM; d++) mean[d] += x[d];
for (let d = 0; d < DIM; d++) mean[d] /= N;

const Xc = X.map((x) => { const r = new Float64Array(DIM); for (let d = 0; d < DIM; d++) r[d] = x[d] - mean[d]; return r; });

const C = new Float64Array(DIM * DIM);
for (const x of Xc) for (let i = 0; i < DIM; i++) { const xi = x[i]; if (!xi) continue; const row = i * DIM; for (let j = i; j < DIM; j++) C[row + j] += xi * x[j]; }
for (let i = 0; i < DIM; i++) for (let j = i; j < DIM; j++) { const v = C[i * DIM + j] / N; C[i * DIM + j] = v; C[j * DIM + i] = v; }

// ── Jacobi eigendecomposition of the symmetric covariance ───────────────────
// V accumulates eigenvectors (columns); A is rotated toward diagonal.
function jacobiEigen(Ain, n) {
  const A = Float64Array.from(Ain);
  const V = new Float64Array(n * n);
  for (let i = 0; i < n; i++) V[i * n + i] = 1;
  const off = () => { let s = 0; for (let i = 0; i < n; i++) for (let j = i + 1; j < n; j++) { const a = A[i * n + j]; s += a * a; } return s; };
  for (let sweepN = 0; sweepN < 100 && off() > 1e-18; sweepN++) {
    for (let p = 0; p < n; p++) for (let q = p + 1; q < n; q++) {
      const apq = A[p * n + q];
      if (Math.abs(apq) < 1e-300) continue;
      const app = A[p * n + p], aqq = A[q * n + q];
      const phi = 0.5 * Math.atan2(2 * apq, aqq - app);
      const c = Math.cos(phi), s = Math.sin(phi);
      for (let k = 0; k < n; k++) {
        const akp = A[k * n + p], akq = A[k * n + q];
        A[k * n + p] = c * akp - s * akq;
        A[k * n + q] = s * akp + c * akq;
      }
      for (let k = 0; k < n; k++) {
        const apk = A[p * n + k], aqk = A[q * n + k];
        A[p * n + k] = c * apk - s * aqk;
        A[q * n + k] = s * apk + c * aqk;
      }
      for (let k = 0; k < n; k++) {
        const vkp = V[k * n + p], vkq = V[k * n + q];
        V[k * n + p] = c * vkp - s * vkq;
        V[k * n + q] = s * vkp + c * vkq;
      }
    }
  }
  const eig = new Float64Array(n);
  for (let i = 0; i < n; i++) eig[i] = A[i * n + i];
  return { eig, V };               // V[k*n+i] = component i of eigenvector k... col-indexed below
}

console.log('eigendecomposing 256x256 (Jacobi)…');
const { eig, V } = jacobiEigen(C, DIM);

// ── supervised attribute axes + residual character axes ─────────────────────
// Raw variance-ranked PCs put all the audible signal in PC1-3 and leave the
// tail nearly inert. Instead the first axes are *perceptual*: each is the ridge
// direction that moves one measured attribute (pitch/brightness/pace/energy/
// volume/pitch-variation). The remaining axes are PCA of the RESIDUAL — voice
// character the attributes don't explain — so the two banks are disentangled.
// Every axis is orthonormal, so style<->coords stays an exact projection and
// the app's slider math is unchanged.

function pearson(a, b) {
  let ma = 0, mb = 0, n = 0;
  for (let i = 0; i < a.length; i++) { if (!isFinite(a[i]) || !isFinite(b[i])) continue; ma += a[i]; mb += b[i]; n++; }
  if (n < 3) return 0; ma /= n; mb /= n;
  let cov = 0, va = 0, vb = 0;
  for (let i = 0; i < a.length; i++) { if (!isFinite(a[i]) || !isFinite(b[i])) continue; const da = a[i] - ma, db = b[i] - mb; cov += da * db; va += da * da; vb += db * db; }
  return cov / (Math.sqrt(va * vb) || 1);
}

// ridge regression  Xc -> (y - mean_y),  reusing the eigendecomposition of
// C = Xc^T Xc / N:   w = (Xc^T Xc + λI)^-1 Xc^T y = Σ_q v_q (v_q·g)/(N·eig_q+λ)
const LAMBDA = 10;
function ridgeDir(yRaw) {
  let my = 0, cnt = 0; for (const y of yRaw) if (isFinite(y)) { my += y; cnt++; } my /= Math.max(1, cnt);
  const g = new Float64Array(DIM);
  for (let n = 0; n < N; n++) { const y = yRaw[n]; if (!isFinite(y)) continue; const yc = y - my, x = Xc[n]; for (let d = 0; d < DIM; d++) g[d] += x[d] * yc; }
  const w = new Float64Array(DIM);
  for (let q = 0; q < DIM; q++) {
    let dot = 0; for (let d = 0; d < DIM; d++) dot += V[d * DIM + q] * g[d];
    const coef = dot / (N * eig[q] + LAMBDA);
    if (!coef) continue;
    for (let d = 0; d < DIM; d++) w[d] += coef * V[d * DIM + q];
  }
  return w;
}

// priority order: keep the most distinct attributes as near-pure axes; the most
// redundant (pitch-variation, ~0.8 cos with pitch) goes last, so Gram-Schmidt
// leaves it as the *independent* remainder rather than a near-duplicate of pitch.
const ATTR_ORDER = [
  ['f0_mean', 'pitch'], ['zcr', 'brightness'], ['rate', 'pace'],
  ['energy', 'energy'], ['rms', 'volume'], ['f0_std', 'pitch-var'],
];
const attrAxes = [], attrMeta = [];
for (const [key, word] of ATTR_ORDER) {
  if (!attrs[key]) { console.log('  (no attribute ' + key + ', skipped)'); continue; }
  const w = ridgeDir(attrs[key]);
  for (const a of attrAxes) { let p = 0; for (let d = 0; d < DIM; d++) p += w[d] * a[d]; for (let d = 0; d < DIM; d++) w[d] -= p * a[d]; }
  let nrm = 0; for (let d = 0; d < DIM; d++) nrm += w[d] * w[d]; nrm = Math.sqrt(nrm);
  if (nrm < 1e-6) { console.log('  (attribute ' + word + ' redundant, skipped)'); continue; }
  for (let d = 0; d < DIM; d++) w[d] /= nrm;
  attrAxes.push(w); attrMeta.push({ key, word });
}

// residual character axes: project the attribute subspace out of every style,
// then PCA what's left (orthogonal to the attribute axes by construction)
const Xr = Xc.map((x) => {
  const r = Float64Array.from(x);
  for (const a of attrAxes) { let p = 0; for (let d = 0; d < DIM; d++) p += x[d] * a[d]; for (let d = 0; d < DIM; d++) r[d] -= p * a[d]; }
  return r;
});
const Cr = new Float64Array(DIM * DIM);
for (const x of Xr) for (let i = 0; i < DIM; i++) { const xi = x[i]; if (!xi) continue; const row = i * DIM; for (let j = i; j < DIM; j++) Cr[row + j] += xi * x[j]; }
for (let i = 0; i < DIM; i++) for (let j = i; j < DIM; j++) { const v = Cr[i * DIM + j] / N; Cr[i * DIM + j] = v; Cr[j * DIM + i] = v; }
console.log('eigendecomposing residual for character axes…');
const erR = jacobiEigen(Cr, DIM);
const orderR = Array.from({ length: DIM }, (_, i) => i).sort((a, b) => erR.eig[b] - erR.eig[a]);
const K_CHAR = Math.max(0, K - attrAxes.length);
const charAxes = [];
for (let k = 0; k < K_CHAR; k++) {
  const q = orderR[k]; const v = new Float64Array(DIM); let n = 0;
  for (let i = 0; i < DIM; i++) { v[i] = erR.V[i * DIM + q]; n += v[i] * v[i]; }
  n = Math.sqrt(n) || 1; for (let i = 0; i < DIM; i++) v[i] /= n;
  charAxes.push(v);
}

// ── assemble the combined orthonormal basis ─────────────────────────────────
const axes = attrAxes.concat(charAxes);
const totalVar = eig.reduce((a, b) => a + Math.max(0, b), 0) || 1;
const comps = [], std = [], range = [], varExplained = [], attrHint = [], axisName = [], axisKind = [];
for (let k = 0; k < axes.length; k++) {
  const v = axes[k];
  const coord = new Float64Array(N);
  for (let n = 0; n < N; n++) { const x = Xc[n]; let s = 0; for (let d = 0; d < DIM; d++) s += x[d] * v[d]; coord[n] = s; }
  let m = 0; for (let n = 0; n < N; n++) m += coord[n]; m /= N;
  let vv = 0; for (let n = 0; n < N; n++) { const dd = coord[n] - m; vv += dd * dd; } vv /= N;
  const sd = Math.sqrt(vv) || 1;
  let lo = Infinity, hi = -Infinity; for (let n = 0; n < N; n++) { const z = (coord[n] - m) / sd; if (z < lo) lo = z; if (z > hi) hi = z; }
  comps.push(Array.from(v)); std.push(+sd.toFixed(6)); range.push([+lo.toFixed(3), +hi.toFixed(3)]);
  varExplained.push(+(vv / totalVar).toFixed(4));
  if (k < attrMeta.length) {
    attrHint.push({ attr: attrMeta[k].key, r: +pearson(coord, attrs[attrMeta[k].key]).toFixed(3) });
    axisName.push(attrMeta[k].word); axisKind.push('attr');
  } else {
    let best = { attr: '', r: 0 };
    for (const key of ATTR_KEYS) { const r = pearson(coord, attrs[key]); if (Math.abs(r) > Math.abs(best.r)) best = { attr: key, r: +r.toFixed(3) }; }
    attrHint.push(best);
    axisName.push('C' + (k - attrMeta.length + 1)); axisKind.push('char');
  }
}
console.log('attribute axes (orthonormal · post-Gram-Schmidt corr to their attribute):');
for (let k = 0; k < attrMeta.length; k++) console.log('   ', axisName[k].padEnd(11), attrHint[k].attr.padEnd(8), 'r=' + attrHint[k].r);
const attrVar = varExplained.slice(0, attrMeta.length).reduce((a, b) => a + b, 0);
const charVar = varExplained.slice(attrMeta.length).reduce((a, b) => a + b, 0);
console.log('variance: attribute axes ' + (100 * attrVar).toFixed(1) + '% · character axes ' + (100 * charVar).toFixed(1) + '% · total ' + (100 * (attrVar + charVar)).toFixed(1) + '%');

// ── the named anchors, expressed in the combined axes (exact: orthonormal) ──
const anchorsMeta = JSON.parse(fs.readFileSync(SWEEP_DIR + '/anchors.json', 'utf8'));
const names = anchorsMeta.names;
const anchors = anchorsMeta.anchors.map((a) => {
  const xc = new Float64Array(DIM); for (let d = 0; d < DIM; d++) xc[d] = a[d] - mean[d];
  const c = new Float64Array(axes.length);
  for (let k = 0; k < axes.length; k++) { const v = axes[k]; let s = 0; for (let d = 0; d < DIM; d++) s += xc[d] * v[d]; c[k] = +(s / (std[k] || 1)).toFixed(4); }
  return Array.from(c);
});

// ── write voicebasis.json ───────────────────────────────────────────────────
const round = (arr, p) => Array.from(arr).map((v) => +v.toFixed(p));
const basis = {
  dim: DIM, k: K, n: N, text: anchorsMeta.text,
  mean: round(mean, 6),
  comps: comps.map((v) => round(v, 6)),
  std: round(std, 6),
  range, varExplained: varExplained.map((v) => +v.toFixed(4)),
  attrHint, axisName, axisKind,
  names, anchors,
};
const basisJson = JSON.stringify(basis);
writeBoth('voice_basis.json', basisJson);
console.log('wrote voice_basis.json (' + (basisJson.length / 1024).toFixed(0) + ' KB) to', DATA_DIR, '+', MODEL_DIR);

// ── pack the bridge into voice_bridge.bin ───────────────────────────────────
const br = JSON.parse(fs.readFileSync(SWEEP_DIR + '/bridge.json', 'utf8'));
const D = br.D, M = br.M;
const buf = new ArrayBuffer(8 + 4 * (D + M + D * M));
const iv = new Int32Array(buf, 0, 2); iv[0] = D; iv[1] = M;
let off = 8;
const put = (a, n) => { const f = new Float32Array(buf, off, n); for (let i = 0; i < n; i++) f[i] = a[i]; off += 4 * n; };
put(br.xm, D); put(br.ym, M); put(br.B, D * M);
writeBoth('voice_bridge.bin', new Uint8Array(buf));
console.log('wrote voice_bridge.bin (' + (buf.byteLength / 1048576).toFixed(2) + ' MB) · D=' + D + ' M=' + M);
console.log('DONE');
