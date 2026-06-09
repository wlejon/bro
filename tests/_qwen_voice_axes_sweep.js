// Validate the VoiceDesign axes (voice_axes.json) against REAL Base synthesis.
// The axes are LEARNED from prompted labels (_qwen_voice_axes.js), so on their own
// they only prove the geometry is self-consistent — exactly the trap that let a
// "bit-exact" port stay silent in production. This script closes the loop: take a
// fixed neutral identity x-vector, push it ±α along each axis, synthesize on Base,
// and MEASURE the model's output. An axis is real only if its target acoustic
// correlate moves monotonically with α while identity is largely retained:
//
//   pitch / gender / age  → fundamental frequency (autocorrelation f0)
//   brightness / weight   → spectral centroid (brighter = higher) + spectral tilt
//   breathiness           → high-band energy / aperiodicity proxy (ZCR, HF ratio)
//   ALL                   → cos(embed(output), base) stays high  (identity held)
//
// Run (GPU — Base synth):
//   bro-headless tests/_smoke_app tests/_qwen_voice_axes_sweep.js

const fs = require('fs');
const BASE_DIR = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base';
const AXES_JSON = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base/voice_axes.json';
const BASIS_JSON = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base/qwen_voice_basis.json';
const TEXT = 'The quick brown fox jumps over the lazy dog while she sells sea shells.';
const ALPHAS = [-3, -1.5, 0, 1.5, 3];

function pumpUntil(pred, budgetMs) { const s = Date.now(); while (!pred() && Date.now() - s < budgetMs) sleep(20); return pred(); }

// ── signal measures ───────────────────────────────────────────────────────────
// autocorrelation f0 over voiced frames (24 kHz, 40 ms frames, 50–400 Hz search).
function f0mean(s, sr) {
  const N = Math.floor(0.04 * sr), hop = Math.floor(0.02 * sr);
  const lagMin = Math.floor(sr / 400), lagMax = Math.floor(sr / 50);
  const vals = [];
  for (let st = 0; st + N < s.length; st += hop) {
    let energy = 0; for (let i = 0; i < N; i++) energy += s[st + i] * s[st + i];
    if (energy < 1e-4) continue;                       // skip silence
    let bestLag = -1, best = 0, r0 = energy;
    for (let lag = lagMin; lag <= lagMax; lag++) {
      let r = 0; for (let i = 0; i + lag < N; i++) r += s[st + i] * s[st + i + lag];
      if (r > best) { best = r; bestLag = lag; }
    }
    if (bestLag > 0 && best / r0 > 0.3) vals.push(sr / bestLag);   // voiced only
  }
  if (!vals.length) return 0;
  vals.sort((a, b) => a - b); return vals[Math.floor(vals.length / 2)];   // median
}
// radix-2 FFT (in-place, real input → complex). Returns magnitude spectrum half.
function fftMag(frame) {
  let n = 1; while (n < frame.length) n <<= 1;
  const re = new Float64Array(n), im = new Float64Array(n);
  for (let i = 0; i < frame.length; i++) re[i] = frame[i];
  for (let i = 1, j = 0; i < n; i++) { let bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) { const tr = re[i]; re[i] = re[j]; re[j] = tr; const ti = im[i]; im[i] = im[j]; im[j] = ti; } }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = -2 * Math.PI / len, wr = Math.cos(ang), wi = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let cwr = 1, cwi = 0;
      for (let k = 0; k < len / 2; k++) {
        const ur = re[i + k], ui = im[i + k];
        const vr = re[i + k + len / 2] * cwr - im[i + k + len / 2] * cwi;
        const vi = re[i + k + len / 2] * cwi + im[i + k + len / 2] * cwr;
        re[i + k] = ur + vr; im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
        const ncwr = cwr * wr - cwi * wi; cwi = cwr * wi + cwi * wr; cwr = ncwr;
      }
    }
  }
  const mag = new Float64Array(n / 2); for (let i = 0; i < n / 2; i++) mag[i] = Math.hypot(re[i], im[i]);
  return { mag, n };
}
// spectral centroid (Hz) + HF energy ratio (>2 kHz / total), averaged over frames.
function spectrum(s, sr) {
  const N = 2048, hop = 1024; let cAcc = 0, hfAcc = 0, frames = 0;
  for (let st = 0; st + N < s.length; st += hop) {
    let energy = 0; for (let i = 0; i < N; i++) energy += s[st + i] * s[st + i];
    if (energy < 1e-4) continue;
    const frame = new Float64Array(N);
    for (let i = 0; i < N; i++) frame[i] = s[st + i] * (0.5 - 0.5 * Math.cos(2 * Math.PI * i / (N - 1)));  // Hann
    const { mag, n } = fftMag(frame);
    let num = 0, den = 0, hf = 0; const binHz = sr / n;
    for (let k = 1; k < mag.length; k++) { const f = k * binHz, m = mag[k]; num += f * m; den += m; if (f > 2000) hf += m; }
    if (den > 0) { cAcc += num / den; hfAcc += hf / den; frames++; }
  }
  return frames ? { centroid: cAcc / frames, hf: hfAcc / frames } : { centroid: 0, hf: 0 };
}
function zcr(s) { let z = 0; for (let i = 1; i < s.length; i++) if ((s[i] < 0) !== (s[i - 1] < 0)) z++; return z / s.length; }
function rms(s) { let e = 0; for (let i = 0; i < s.length; i++) e += s[i] * s[i]; return Math.sqrt(e / s.length); }
function cos(a, b) { let d = 0, na = 0, nb = 0; for (let i = 0; i < a.length; i++) { d += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; } return d / (Math.sqrt(na * nb) || 1); }

// ── load axes + neutral base identity (the CAMEO centroid the lab seeds) ──────
const AX = JSON.parse(fs.readFileSync(AXES_JSON, 'utf8'));
const basis = JSON.parse(fs.readFileSync(BASIS_JSON, 'utf8'));
const baseX = Float32Array.from(basis.mean);
console.log('axes', AX.axes.length, '· dim', AX.dim, '· base identity = CAMEO centroid');

// ── load Base (GPU) ───────────────────────────────────────────────────────────
bro.tts.setAssetRoot('D:/projects/brosoundml');
let q = null, qErr = null;
bro.tts.loadQwen(BASE_DIR, { onReady: (m) => { q = m; }, onError: (m) => { qErr = m; } });
if (!pumpUntil(() => q || qErr, 240000) || qErr) throw new Error('Base load: ' + (qErr || 'timeout'));
console.log('Base loaded · variant', q.variant);

function render(xvec) {
  const buf = q.synthesizeFromXvector(TEXT, xvec, { language: 'english' });
  const s = buf.samples, sr = buf.sampleRate;
  return { f0: f0mean(s, sr), ...spectrum(s, sr), zcr: zcr(s), rms: rms(s), idCos: cos(q.embedSpeaker(s, { sampleRate: sr }), baseX) };
}

// baseline (α=0, identical for every axis)
const base = render(baseX);
console.log('\nbaseline  f0 ' + base.f0.toFixed(1) + ' Hz · centroid ' + base.centroid.toFixed(0) +
            ' Hz · hf ' + base.hf.toFixed(3) + ' · zcr ' + base.zcr.toFixed(4) + ' · rms ' + base.rms.toFixed(3));

// per-axis α sweep
for (const ax of AX.axes) {
  const dir = ax.full;
  console.log('\n── ' + ax.label + ' (' + ax.poles.neg + ' ↔ ' + ax.poles.pos + ') · σ ' + ax.sigmaFull + ' ──');
  console.log('   α      f0      centroid    hf     zcr      rms    idCos');
  const f0s = [];
  for (const a of ALPHAS) {
    if (a === 0) { console.log('  ' + ('0').padStart(5) + '  ' + base.f0.toFixed(1).padStart(6) + '  ' + base.centroid.toFixed(0).padStart(8) + '  ' + base.hf.toFixed(3) + '  ' + base.zcr.toFixed(4) + '  ' + base.rms.toFixed(3) + '  1.000'); f0s.push([a, base.f0]); continue; }
    const x = Float32Array.from(baseX); for (let d = 0; d < x.length; d++) x[d] += a * dir[d];
    const m = render(x);
    f0s.push([a, m.f0]);
    console.log('  ' + String(a).padStart(5) + '  ' + m.f0.toFixed(1).padStart(6) + '  ' + m.centroid.toFixed(0).padStart(8) + '  ' + m.hf.toFixed(3) + '  ' + m.zcr.toFixed(4) + '  ' + m.rms.toFixed(3) + '  ' + m.idCos.toFixed(3));
  }
}
console.log('\nDONE');
