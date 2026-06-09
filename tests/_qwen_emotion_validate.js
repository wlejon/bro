// IN-DISTRIBUTION gate for the Qwen-native emotion basis (emotion_basis.json built by
// _qwen_emotion_basis_vd.js). The transfer test applies the directions at the abstract
// voice-basis MEAN — a "no one" identity that's out of distribution and whose median-f0
// response is degenerate. Here we test where the directions were LEARNED: on real corpus
// voices. Each corpus voice's 12-emotion MEAN is its neutral identity (the contrastive
// center). We add α·dir[e] on the Base model and measure the acoustic SHIFT with a richer
// readout than median f0 alone — emotion lives in f0 RANGE and energy as much as pitch.
//
// Expected signs (averaged over voices):
//   angry     ↑f0  ↑range ↑rms      happy    ↑f0  ↑range
//   surprised ↑↑f0 ↑range           fearful  ↑f0  ↑range (jittery)
//   sad       ↓f0  ↓rms             disgust  ↓f0 (creaky), variable
//
// Run:  bro-headless tests/_smoke_app tests/_qwen_emotion_validate.js
//   BRO_EMO_VOICES=8 bro-headless tests/_smoke_app tests/_qwen_emotion_validate.js

const fs = require('fs');
const BASE = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base';
const CORPUS = 'D:/projects/brosoundml-data/qwen-tts/voicedesign-corpus-emotion/xvecs.jsonl';
const EMO = JSON.parse(fs.readFileSync(BASE + '/emotion_basis.json', 'utf8'));
const TEXT = 'The quick brown fox jumps over the lazy dog while she sells sea shells.';
const NV = parseInt(process.env.BRO_EMO_VOICES || '6', 10);

function pumpUntil(p, b) { const s = Date.now(); while (!p() && Date.now() - s < b) sleep(20); return p(); }
// per-frame f0 track → median + IQR range; plus rms.
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

// corpus identities: per-voice mean over its 12 emotion clips = neutral identity.
const rows = fs.readFileSync(CORPUS, 'utf8').trim().split('\n').map((l) => JSON.parse(l));
const DIM = rows[0].x.length;
const byVoice = new Map();
for (const r of rows) { if (!byVoice.has(r.voice)) byVoice.set(r.voice, []); byVoice.get(r.voice).push(r); }
const voiceKeys = [...byVoice.keys()];
const step = Math.max(1, Math.floor(voiceKeys.length / NV));
const picks = [];
for (let i = 0; i < voiceKeys.length && picks.length < NV; i += step) picks.push(voiceKeys[i]);
function neutralOf(k) { const clips = byVoice.get(k); const m = new Float64Array(DIM); for (const c of clips) for (let d = 0; d < DIM; d++) m[d] += c.x[d]; for (let d = 0; d < DIM; d++) m[d] /= clips.length; return m; }

bro.tts.setAssetRoot('D:/projects/brosoundml');
let q = null, qErr = null;
bro.tts.loadQwen(BASE, { onReady: (m) => { q = m; }, onError: (m) => { qErr = m; } });
if (!pumpUntil(() => q || qErr, 240000) || qErr) throw new Error('Base load: ' + (qErr || 'timeout'));
console.log('Base loaded · ' + picks.length + ' corpus identities · emotions ' + EMO.emotions.join(','));

function render(x) { const b = q.synthesizeFromXvector(TEXT, x, { language: 'english' }); const t = f0track(b.samples, b.sampleRate); return { f0: t.f0, range: t.range, rms: rms(b.samples) }; }
function withEmo(mean, e, a) { const x = Float32Array.from(mean); const f = EMO.full[e]; const n = Math.min(x.length, f.length); for (let d = 0; d < n; d++) x[d] += a * f[d]; return x; }

// accumulate Δ per emotion across voices.
const acc = {}; for (const e of EMO.emotions) acc[e] = { df0: 0, drange: 0, drms: 0, n: 0 };
for (const k of picks) {
  const mean = Float32Array.from(neutralOf(k));
  const base = render(mean);
  for (const e of EMO.emotions) {
    const a = EMO.defaultAlpha[e] || 1.5;
    const m = render(withEmo(mean, e, a));
    const A = acc[e]; A.df0 += m.f0 - base.f0; A.drange += m.range - base.range; A.drms += m.rms - base.rms; A.n++;
  }
}

console.log('\n  mean Δ over ' + picks.length + ' identities (emotion vs that voice\'s neutral):');
console.log('  emotion       α     Δf0(Hz)   Δrange(Hz)  Δrms');
for (const e of EMO.emotions) {
  const A = acc[e], a = EMO.defaultAlpha[e] || 1.5, lab = (EMO.label && EMO.label[e]) || e;
  const f = (v, w) => (v >= 0 ? '+' : '') + v.toFixed(w);
  console.log('  ' + lab.padEnd(12) + a.toFixed(2).padStart(5) + '   ' +
    f(A.df0 / A.n, 1).padStart(7) + '   ' + f(A.drange / A.n, 1).padStart(8) + '   ' + f(A.drms / A.n, 3));
}
console.log('\nDONE');
