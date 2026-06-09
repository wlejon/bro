// A/B the voice-identity bases on REAL Base synthesis: CAMEO (real actors) vs the
// Qwen-rendered VoiceDesign corpus. For the top axes of each, reconstruct the
// x-vector at the basis mean ±2σ (x = mean + z·std·comp), synthesize on Base, and
// measure how far the voice actually travels (f0 + spectral centroid) and whether
// identity stays coherent (cos to the mean render). Wider clean span = more
// impactful slider. Gate the "broader manifold" claim on the model, not geometry.
//
// Run:  bro-headless tests/_smoke_app tests/_qwen_basis_ab.js

const fs = require('fs');
const BASE_DIR = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base';
const CAMEO = BASE_DIR + '/qwen_voice_basis.cameo.json';
const CORPUS = BASE_DIR + '/qwen_voice_basis.json';
const TEXT = 'The quick brown fox jumps over the lazy dog while she sells sea shells.';
const NAXES = 4, ZS = [-2, 0, 2];

function pumpUntil(p, b) { const s = Date.now(); while (!p() && Date.now() - s < b) sleep(20); return p(); }
function f0mean(s, sr) {
  const N = Math.floor(0.04 * sr), hop = Math.floor(0.02 * sr), lmin = Math.floor(sr / 400), lmax = Math.floor(sr / 50), vals = [];
  for (let st = 0; st + N < s.length; st += hop) {
    let e = 0; for (let i = 0; i < N; i++) e += s[st + i] * s[st + i]; if (e < 1e-4) continue;
    let bl = -1, bv = 0; for (let lag = lmin; lag <= lmax; lag++) { let r = 0; for (let i = 0; i + lag < N; i++) r += s[st + i] * s[st + i + lag]; if (r > bv) { bv = r; bl = lag; } }
    if (bl > 0 && bv / e > 0.3) vals.push(sr / bl);
  }
  if (!vals.length) return 0; vals.sort((a, b) => a - b); return vals[vals.length >> 1];
}
function centroid(s, sr) {
  const N = 2048, hop = 1024; let acc = 0, fr = 0;
  for (let st = 0; st + N < s.length; st += hop) {
    let e = 0; for (let i = 0; i < N; i++) e += s[st + i] * s[st + i]; if (e < 1e-4) continue;
    const re = new Float64Array(N), im = new Float64Array(N);
    for (let i = 0; i < N; i++) re[i] = s[st + i] * (0.5 - 0.5 * Math.cos(2 * Math.PI * i / (N - 1)));
    for (let i = 1, j = 0; i < N; i++) { let bit = N >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) { const t = re[i]; re[i] = re[j]; re[j] = t; } }
    for (let len = 2; len <= N; len <<= 1) { const ang = -2 * Math.PI / len, wr = Math.cos(ang), wi = Math.sin(ang); for (let i = 0; i < N; i += len) { let cr = 1, ci = 0; for (let k = 0; k < len / 2; k++) { const ur = re[i + k], ui = im[i + k], vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci, vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr; re[i + k] = ur + vr; im[i + k] = ui + vi; re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi; const ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr; } } }
    let num = 0, den = 0; const binHz = sr / N; for (let k = 1; k < N / 2; k++) { const m = Math.hypot(re[k], im[k]); num += k * binHz * m; den += m; }
    if (den > 0) { acc += num / den; fr++; }
  }
  return fr ? acc / fr : 0;
}
function cos(a, b) { let d = 0, na = 0, nb = 0; for (let i = 0; i < a.length; i++) { d += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; } return d / (Math.sqrt(na * nb) || 1); }

function xAt(basis, k, z) {
  const x = Float32Array.from(basis.mean); const c = z * basis.std[k], v = basis.comps[k];
  for (let d = 0; d < x.length; d++) x[d] += c * v[d]; return x;
}

bro.tts.setAssetRoot('D:/projects/brosoundml');
let q = null, qErr = null;
bro.tts.loadQwen(BASE_DIR, { onReady: (m) => { q = m; }, onError: (m) => { qErr = m; } });
if (!pumpUntil(() => q || qErr, 240000) || qErr) throw new Error('Base load: ' + (qErr || 'timeout'));
console.log('Base loaded');

function measure(x) { const b = q.synthesizeFromXvector(TEXT, x, { language: 'english' }); return { x, s: b.samples, sr: b.sampleRate }; }

for (const [tag, path] of [['CAMEO', CAMEO], ['CORPUS', CORPUS]]) {
  const basis = JSON.parse(fs.readFileSync(path, 'utf8'));
  const meanRender = measure(xAt(basis, 0, 0));
  const meanXv = q.embedSpeaker(meanRender.s, { sampleRate: meanRender.sr });
  console.log('\n=== ' + tag + ' basis (n=' + basis.n + ') ===');
  console.log('axis   f0 @ -2σ → +2σ      Δf0    centroid -2σ→+2σ   idCos@±2σ   name');
  for (let k = 0; k < NAXES; k++) {
    const r = {};
    for (const z of ZS) { const m = measure(xAt(basis, k, z)); r[z] = { f0: f0mean(m.s, m.sr), c: centroid(m.s, m.sr), id: cos(q.embedSpeaker(m.s, { sampleRate: m.sr }), meanXv) }; }
    const df0 = Math.abs(r[2].f0 - r[-2].f0);
    console.log('  V' + String(k + 1).padEnd(3) +
      r[-2].f0.toFixed(0).padStart(5) + ' → ' + r[2].f0.toFixed(0).padStart(4) + ' Hz' +
      ('  ' + df0.toFixed(0)).padStart(7) + '    ' + r[-2].c.toFixed(0).padStart(5) + ' → ' + r[2].c.toFixed(0).padStart(5) +
      '    ' + r[-2].id.toFixed(2) + '/' + r[2].id.toFixed(2) +
      '   ' + (basis.axisName ? basis.axisName[k] : ''));
  }
}
console.log('\nDONE');
