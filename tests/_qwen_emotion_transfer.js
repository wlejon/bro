// Does the CAMEO-derived emotion basis still move the voice when applied on top of
// an identity from the NEW Qwen-VoiceDesign manifold, vs a CAMEO-region identity?
// Emotion is added as a FIXED direction: xvec = identity + α·full[e]. Its audible
// effect depends on how sensitive Qwen's conditioning is at that identity point, so
// a different identity region can read emotion more or less strongly. Measure the
// emotion-induced Δf0 / Δrms at the CAMEO basis mean vs the new basis mean.
//
// Run:  bro-headless tests/_smoke_app tests/_qwen_emotion_transfer.js

const fs = require('fs');
const BASE = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base';
const EMO = JSON.parse(fs.readFileSync(BASE + '/emotion_basis.json', 'utf8'));
const NEW = JSON.parse(fs.readFileSync(BASE + '/qwen_voice_basis.json', 'utf8'));
const CAMEO = JSON.parse(fs.readFileSync(BASE + '/qwen_voice_basis.cameo.json', 'utf8'));
const TEXT = 'The quick brown fox jumps over the lazy dog while she sells sea shells.';
const EMOS = EMO.emotions.slice(0, 4);   // a few representative emotions

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
function rms(s) { let e = 0; for (let i = 0; i < s.length; i++) e += s[i] * s[i]; return Math.sqrt(e / s.length); }

bro.tts.setAssetRoot('D:/projects/brosoundml');
let q = null, qErr = null;
bro.tts.loadQwen(BASE, { onReady: (m) => { q = m; }, onError: (m) => { qErr = m; } });
if (!pumpUntil(() => q || qErr, 240000) || qErr) throw new Error('Base load: ' + (qErr || 'timeout'));
console.log('Base loaded · emotions:', EMOS.join(','));

function render(x) { const b = q.synthesizeFromXvector(TEXT, x, { language: 'english' }); return { f0: f0mean(b.samples, b.sampleRate), rms: rms(b.samples) }; }
function withEmo(mean, e, a) { const x = Float32Array.from(mean); const f = EMO.full[e]; const n = Math.min(x.length, f.length); for (let d = 0; d < n; d++) x[d] += a * f[d]; return x; }

for (const [tag, basis] of [['CAMEO mean', CAMEO], ['NEW (Qwen) mean', NEW]]) {
  const mean = Float32Array.from(basis.mean);
  const base = render(mean);
  console.log('\n=== ' + tag + ' ===  neutral f0 ' + base.f0.toFixed(1) + ' Hz · rms ' + base.rms.toFixed(3));
  console.log('  emotion       α     f0 (Δ)         rms (Δ)');
  for (const e of EMOS) {
    const a = EMO.defaultAlpha[e] || 2;
    const m = render(withEmo(mean, e, a));
    const lab = (EMO.label && EMO.label[e]) || e;
    console.log('  ' + lab.padEnd(12) + a.toFixed(2).padStart(5) + '   ' +
      m.f0.toFixed(1).padStart(6) + ' (' + (m.f0 - base.f0 >= 0 ? '+' : '') + (m.f0 - base.f0).toFixed(1) + ')   ' +
      m.rms.toFixed(3) + ' (' + (m.rms - base.rms >= 0 ? '+' : '') + (m.rms - base.rms).toFixed(3) + ')');
  }
}
console.log('\nDONE');
