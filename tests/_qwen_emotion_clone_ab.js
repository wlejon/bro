// Reproduce the lab's reported regression: clone theraputic.wav (faithful identity),
// dial Happy, Render. The CAMEO emotion basis used to give "a happy version of the
// cloned voice"; the Qwen-synthetic basis gives "just a different voice". Decide WHY
// by measuring two things the symptom separates:
//   • is it HAPPIER  — Δf0 / Δf0-range vs the neutral clone render
//   • is it STILL THE SAME PERSON — re-embed the rendered audio and cosine it back to
//     the neutral render's speaker embedding (identity drift). "different voice" = this
//     collapses; "happy version of the SAME voice" = it stays high.
// Compares emotion_basis.json (new, synthetic) vs emotion_basis.cameo.json (old) on the
// SAME cloned identity, at each basis's default α and at the slider max (α=4).
//
// Run:  bro-headless tests/_smoke_app tests/_qwen_emotion_clone_ab.js

const fs = require('fs');
const BASE = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base';
const CLIP = 'D:/projects/brosoundml/weights/theraputic.wav';
const NEW = JSON.parse(fs.readFileSync(BASE + '/emotion_basis.json', 'utf8'));
const CAMEO = JSON.parse(fs.readFileSync(BASE + '/emotion_basis.cameo.json', 'utf8'));
const TEXT = 'Hello there. This is a test of the pipeline.';

function pumpUntil(p, b) { const s = Date.now(); while (!p() && Date.now() - s < b) sleep(20); return p(); }
function toMono(s, ch) { if (!ch || ch === 1) return s; const n = (s.length / ch) | 0, o = new Float32Array(n); for (let i = 0; i < n; i++) { let a = 0; for (let c = 0; c < ch; c++) a += s[i * ch + c]; o[i] = a / ch; } return o; }
function f0track(s, sr) {
  const N = Math.floor(0.04 * sr), hop = Math.floor(0.02 * sr), lmin = Math.floor(sr / 400), lmax = Math.floor(sr / 50), vals = [];
  for (let st = 0; st + N < s.length; st += hop) {
    let e = 0; for (let i = 0; i < N; i++) e += s[st + i] * s[st + i]; if (e < 1e-4) continue;
    let bl = -1, bv = 0; for (let lag = lmin; lag <= lmax; lag++) { let r = 0; for (let i = 0; i + lag < N; i++) r += s[st + i] * s[st + i + lag]; if (r > bv) { bv = r; bl = lag; } }
    if (bl > 0 && bv / e > 0.3) vals.push(sr / bl);
  }
  vals.sort((a, b) => a - b);
  const q = (p) => vals.length ? vals[Math.min(vals.length - 1, Math.max(0, Math.round(p * (vals.length - 1))))] : 0;
  return { f0: q(0.5), range: vals.length ? q(0.9) - q(0.1) : 0 };
}
function rms(s) { let e = 0; for (let i = 0; i < s.length; i++) e += s[i] * s[i]; return Math.sqrt(e / s.length); }
function cos(a, b) { let d = 0, na = 0, nb = 0, n = Math.min(a.length, b.length); for (let i = 0; i < n; i++) { d += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; } return d / (Math.sqrt(na * nb) || 1); }

bro.tts.setAssetRoot('D:/projects/brosoundml');
let q = null, qErr = null;
bro.tts.loadQwen(BASE, { onReady: (m) => { q = m; }, onError: (m) => { qErr = m; } });
if (!pumpUntil(() => q || qErr, 240000) || qErr) throw new Error('Base load: ' + (qErr || 'timeout'));

const ctx = new AudioContext();
const dec = ctx.decodeAudioFile(CLIP);
const ident = q.embedSpeaker(toMono(dec.samples, dec.channels), { sampleRate: dec.sampleRate });
console.log('cloned theraputic.wav · identity ‖x‖=' + Math.sqrt(ident.reduce((s, v) => s + v * v, 0)).toFixed(2));

// also report how aligned each basis's HAPPY direction is with the identity itself
// (a direction that points along identity will move "who", not "how").
function unit(a) { const n = Math.sqrt(a.reduce((s, v) => s + v * v, 0)) || 1; return a.map((v) => v / n); }
console.log('cos(happy dir, identity):  new ' + cos(NEW.full.HAP, ident).toFixed(3) + '   cameo ' + cos(CAMEO.full.HAP, ident).toFixed(3));
console.log('‖happy dir‖:               new ' + Math.sqrt(NEW.full.HAP.reduce((s, v) => s + v * v, 0)).toFixed(3) + '   cameo ' + Math.sqrt(CAMEO.full.HAP.reduce((s, v) => s + v * v, 0)).toFixed(3));

function render(x) { const b = q.synthesizeFromXvector(TEXT, x, { language: 'english' }); const t = f0track(b.samples, b.sampleRate); const sp = q.embedSpeaker(b.samples, { sampleRate: b.sampleRate }); return { f0: t.f0, range: t.range, rms: rms(b.samples), spk: sp }; }
function withHappy(basis, a) { const x = Float32Array.from(ident); const f = basis.full.HAP; const n = Math.min(x.length, f.length); for (let d = 0; d < n; d++) x[d] += a * f[d]; return x; }

const neutral = render(Float32Array.from(ident));
console.log('\nneutral clone render · f0 ' + neutral.f0.toFixed(1) + ' Hz · range ' + neutral.range.toFixed(1) + ' · rms ' + neutral.rms.toFixed(3));
console.log('\n basis   α      f0(Δ)         range(Δ)      idCos(vs neutral render)');
for (const [tag, basis] of [['new  ', NEW], ['cameo', CAMEO]]) {
  for (const a of [basis.defaultAlpha.HAP || 2, 4]) {
    const m = render(withHappy(basis, a));
    const sgn = (v) => (v >= 0 ? '+' : '') + v.toFixed(1);
    console.log('  ' + tag + ' ' + a.toFixed(2).padStart(5) + '  ' +
      m.f0.toFixed(1).padStart(6) + ' (' + sgn(m.f0 - neutral.f0) + ')   ' +
      m.range.toFixed(1).padStart(5) + ' (' + sgn(m.range - neutral.range) + ')   ' +
      cos(m.spk, neutral.spk).toFixed(3));
  }
}
console.log('\nDONE');
