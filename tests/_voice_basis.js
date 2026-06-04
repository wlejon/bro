// Build the kokoro-lab voice-space basis (PCA over the clean swept Kokoro
// styles) + pack the ECAPA->style bridge into a compact binary. Pure Node — no
// bro/model needed; it only reshapes data we already produced under
// D:/projects/voice-sweep into two app-side assets:
//
//   voicebasis.json  — mean + top-K principal axes of the 256-D Kokoro style
//                      space (orthonormal, std-scaled), the 28 named anchors
//                      expressed in those axes, the realizable per-axis range,
//                      and each axis' strongest correlate among the measured
//                      attributes (f0/energy/rate/...). This is what turns 256
//                      opaque dims into a handful of meaningful sliders.
//   bridge.f32       — the linear ridge x(1024)->style(256): little-endian
//                      [int32 D, int32 M, f32 xm[D], f32 ym[M], f32 B[D*M]].
//
// Run:  node bro/tests/_voice_basis.js
// Writes both into broworkshop/demos/kokoro-lab/ (the app loads them at start).

const fs = require('fs');
const path = require('path');

const SWEEP_DIR = 'D:/projects/voice-sweep';
const OUT_DIR = path.resolve(__dirname, '../../broworkshop/demos/kokoro-lab');
const K = 20;                              // exported principal axes (sliders)
const DIM = 256;

// ── load the clean swept styles + their measured attributes ─────────────────
const sweep = fs.readFileSync(SWEEP_DIR + '/sweep.jsonl', 'utf8')
  .trim().split('\n').map((s) => JSON.parse(s));
const X = sweep.map((r) => r.style);       // N x 256
const N = X.length;
const ATTR_KEYS = ['f0_mean', 'rms', 'energy', 'rate', 'zcr'];
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

// rank eigenpairs by descending eigenvalue; pull each eigenvector (column q of V)
const order = Array.from({ length: DIM }, (_, i) => i).sort((a, b) => eig[b] - eig[a]);
const comps = [];                  // K x 256 unit eigenvectors
const std = [];                    // sqrt(eigenvalue): natural scale of each axis
for (let k = 0; k < K; k++) {
  const q = order[k];
  const v = new Float64Array(DIM);
  let nrm = 0;
  for (let i = 0; i < DIM; i++) { v[i] = V[i * DIM + q]; nrm += v[i] * v[i]; }
  nrm = Math.sqrt(nrm) || 1;
  for (let i = 0; i < DIM; i++) v[i] /= nrm;
  comps.push(v);
  std.push(Math.sqrt(Math.max(0, eig[q])));
}
const totalVar = eig.reduce((a, b) => a + Math.max(0, b), 0) || 1;
const varExplained = order.slice(0, K).map((q) => eig[q] / totalVar);
console.log('top-K var explained:', varExplained.map((v) => (100 * v).toFixed(1) + '%').join(' '));

// project a centered 256-vec onto axis k, in std units (so a slider unit == 1σ)
function proj(xc, k) { const v = comps[k]; let s = 0; for (let i = 0; i < DIM; i++) s += xc[i] * v[i]; return s / (std[k] || 1); }

// ── per-axis range (realizable) + strongest attribute correlate ─────────────
const coordsK = Xc.map((xc) => { const c = new Float64Array(K); for (let k = 0; k < K; k++) c[k] = proj(xc, k); return c; });
function pearson(a, b) {
  let ma = 0, mb = 0, n = 0;
  for (let i = 0; i < a.length; i++) { if (!isFinite(a[i]) || !isFinite(b[i])) continue; ma += a[i]; mb += b[i]; n++; }
  if (n < 3) return 0; ma /= n; mb /= n;
  let cov = 0, va = 0, vb = 0;
  for (let i = 0; i < a.length; i++) { if (!isFinite(a[i]) || !isFinite(b[i])) continue; const da = a[i] - ma, db = b[i] - mb; cov += da * db; va += da * da; vb += db * db; }
  return cov / (Math.sqrt(va * vb) || 1);
}
const range = [], attrHint = [];
for (let k = 0; k < K; k++) {
  let lo = Infinity, hi = -Infinity;
  for (const c of coordsK) { if (c[k] < lo) lo = c[k]; if (c[k] > hi) hi = c[k]; }
  range.push([+lo.toFixed(3), +hi.toFixed(3)]);
  const col = coordsK.map((c) => c[k]);
  let best = { attr: '', r: 0 };
  for (const key of ATTR_KEYS) { const r = pearson(col, attrs[key]); if (Math.abs(r) > Math.abs(best.r)) best = { attr: key, r: +r.toFixed(3) }; }
  attrHint.push(best);
}

// ── the 28 named anchors, expressed in the slider axes ──────────────────────
const anchorsMeta = JSON.parse(fs.readFileSync(SWEEP_DIR + '/anchors.json', 'utf8'));
const names = anchorsMeta.names;
const anchors = anchorsMeta.anchors.map((a) => {
  const xc = new Float64Array(DIM); for (let d = 0; d < DIM; d++) xc[d] = a[d] - mean[d];
  const c = new Float64Array(K); for (let k = 0; k < K; k++) c[k] = +proj(xc, k).toFixed(4);
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
  attrHint,
  names, anchors,
};
fs.writeFileSync(OUT_DIR + '/voicebasis.json', JSON.stringify(basis));
console.log('wrote voicebasis.json (' + (fs.statSync(OUT_DIR + '/voicebasis.json').size / 1024).toFixed(0) + ' KB)');

// ── pack the bridge into bridge.f32 ─────────────────────────────────────────
const br = JSON.parse(fs.readFileSync(SWEEP_DIR + '/bridge.json', 'utf8'));
const D = br.D, M = br.M;
const buf = new ArrayBuffer(8 + 4 * (D + M + D * M));
const iv = new Int32Array(buf, 0, 2); iv[0] = D; iv[1] = M;
let off = 8;
const put = (a, n) => { const f = new Float32Array(buf, off, n); for (let i = 0; i < n; i++) f[i] = a[i]; off += 4 * n; };
put(br.xm, D); put(br.ym, M); put(br.B, D * M);
fs.writeFileSync(OUT_DIR + '/bridge.f32', new Uint8Array(buf));
console.log('wrote bridge.f32 (' + (buf.byteLength / 1048576).toFixed(2) + ' MB) · D=' + D + ' M=' + M);
console.log('DONE');
