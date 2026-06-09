// Build a QWEN-NATIVE emotion basis from the self-rendered emotion corpus
// (_qwen_voicedesign_corpus.js MODE=emotion). The CAMEO emotion basis is a fixed
// direction learned in CAMEO's region of x-vector space; it transfers POORLY to
// voices designed on the Qwen-VoiceDesign manifold (measured: half-strength on
// anger/happy, sign-flipped on disgust/fear). This re-derives the 6 basic emotion
// directions in Qwen's OWN region so they read on the new identities.
//
// Recipe (same contrastive idea as the CAMEO/Kokoro emotion bases — contrast vs the
// grand-emotion MEAN, never neutral, which collapses to one shared axis):
//   • each of V base voices was rendered with 12 emotions (6 basic + 6 dyad blends)
//   • per-voice center: residual r = x − mean_e(x over that voice's 12)  (drops identity)
//   • COMPOSITION regression — each emotion is a row over the 6 basics (basic=one-hot,
//     blend=0.5/0.5), column-centered over the 12 so it matches the centered residuals:
//        dirs = (CᵀC + λI)⁻¹ Cᵀ R        (6×1024, pooled over all voices)
//     The 6 BLENDS add cross-terms so each basic direction isn't over-fit to its own
//     pure cluster AND "slider A + slider B" lands where Qwen renders the real blend
//     (validated: cos(real blend residual, 0.5·(dirA+dirB))).
//
// Emits 6 sliders (BASIC6). σ-scale + defaultAlpha mirror the CAMEO emotion basis so
// the lab's emotion.js consumes it unchanged. VERIFY end-to-end on NEW-basis voices
// before trusting it (gate on the model's f0, not this geometry).
//
// Run:  node bro/tests/_qwen_emotion_basis_vd.js

const fs = require('fs');

const CORPUS = 'D:/projects/brosoundml-data/qwen-tts/voicedesign-corpus-emotion/xvecs.jsonl';
const MODEL_DIR = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base';
const DATA_DIR  = 'D:/projects/brosoundml-data/qwen-tts';
const BASIC6 = ['ANG', 'DIS', 'FEA', 'HAP', 'SAD', 'SUR'];
const LABEL = { ANG: 'angry', DIS: 'disgust', FEA: 'fearful', HAP: 'happy', SAD: 'sad', SUR: 'surprised' };
const LAMBDA = 1e-2;             // ridge strength (relative to N·trace, set below)
const TARGET_SIGMA = 0.33;       // default-intensity z-magnitude (matches CAMEO emotion basis)
const ALPHA_MAX = 4;

function writeBoth(name, data) {
  for (const dir of [DATA_DIR, MODEL_DIR]) {
    try { fs.writeFileSync(dir + '/' + name, data); } catch (e) { console.log('  (skip ' + dir + ': ' + e.message + ')'); }
  }
}

// ── load + group by voice ─────────────────────────────────────────────────────
const rows = fs.readFileSync(CORPUS, 'utf8').trim().split('\n').map((l) => JSON.parse(l));
const DIM = rows[0].x.length;
const byVoice = new Map();
for (const r of rows) { if (!byVoice.has(r.voice)) byVoice.set(r.voice, []); byVoice.get(r.voice).push(r); }
const V = byVoice.size;
console.log('emotion corpus:', rows.length, 'rows ·', V, 'voices · dim', DIM);

// ── per-voice contrastive residuals (subtract the voice's 12-emotion mean) ────
const resids = [];   // { code, comp, r:Float64Array }
for (const [, clips] of byVoice) {
  const m = new Float64Array(DIM);
  for (const c of clips) for (let d = 0; d < DIM; d++) m[d] += c.x[d];
  for (let d = 0; d < DIM; d++) m[d] /= clips.length;
  for (const c of clips) { const r = new Float64Array(DIM); for (let d = 0; d < DIM; d++) r[d] = c.x[d] - m[d]; resids.push({ code: c.emotion, comp: c.comp, r }); }
}
const N = resids.length;

// population std over residuals → the σ metric (z-scored magnitude, as in the bases)
const popStd = new Float64Array(DIM);
for (const e of resids) for (let d = 0; d < DIM; d++) popStd[d] += e.r[d] * e.r[d];
for (let d = 0; d < DIM; d++) popStd[d] = Math.sqrt(popStd[d] / N) || 1;
function sigmaOf(v) { let s = 0; for (let d = 0; d < DIM; d++) { const z = v[d] / popStd[d]; s += z * z; } return Math.sqrt(s / DIM); }

// ── composition design, column-centered over the DISTINCT emotion types ───────
const P = BASIC6.length;
function compVec(comp) { const c = new Float64Array(P); for (const k in comp) { const i = BASIC6.indexOf(k); if (i >= 0) c[i] = comp[k]; } return c; }
// mean composition over the distinct emotion rows present (so centering matches the
// per-voice residual centering, whatever the emotion set turns out to be).
const seen = new Map();
for (const e of resids) { const key = e.code; if (!seen.has(key)) seen.set(key, compVec(e.comp)); }
const meanComp = new Float64Array(P);
for (const [, c] of seen) for (let i = 0; i < P; i++) meanComp[i] += c[i];
for (let i = 0; i < P; i++) meanComp[i] /= seen.size;
function centeredComp(comp) { const c = compVec(comp); for (let i = 0; i < P; i++) c[i] -= meanComp[i]; return c; }

// ── ridge: dirs = (CᵀC + λI)⁻¹ Cᵀ R ──────────────────────────────────────────
const CtC = Array.from({ length: P }, () => new Float64Array(P));
const CtR = Array.from({ length: P }, () => new Float64Array(DIM));
for (const e of resids) {
  const c = centeredComp(e.comp);
  for (let i = 0; i < P; i++) { if (!c[i]) continue; for (let j = 0; j < P; j++) CtC[i][j] += c[i] * c[j]; const ri = e.r; const row = CtR[i]; for (let d = 0; d < DIM; d++) row[d] += c[i] * ri[d]; }
}
let tr = 0; for (let i = 0; i < P; i++) tr += CtC[i][i];
const lam = LAMBDA * tr / P;
for (let i = 0; i < P; i++) CtC[i][i] += lam;
function inv(M) {
  const n = M.length, aug = M.map((row, i) => { const r = Array.from(row); for (let j = 0; j < n; j++) r.push(i === j ? 1 : 0); return r; });
  for (let c = 0; c < n; c++) { let p = c; for (let r = c + 1; r < n; r++) if (Math.abs(aug[r][c]) > Math.abs(aug[p][c])) p = r; const t = aug[c]; aug[c] = aug[p]; aug[p] = t; const d = aug[c][c] || 1e-12; for (let j = 0; j < 2 * n; j++) aug[c][j] /= d; for (let r = 0; r < n; r++) { if (r === c) continue; const f = aug[r][c]; for (let j = 0; j < 2 * n; j++) aug[r][j] -= f * aug[c][j]; } }
  return aug.map((row) => row.slice(n));
}
const CtCinv = inv(CtC);
const dir = {};                 // basic code -> Float64Array(DIM)
for (let i = 0; i < P; i++) { const w = new Float64Array(DIM); for (let j = 0; j < P; j++) { const a = CtCinv[i][j]; if (!a) continue; const row = CtR[j]; for (let d = 0; d < DIM; d++) w[d] += a * row[d]; } dir[BASIC6[i]] = w; }

// ── diagnostics ───────────────────────────────────────────────────────────────
function dot(a, b) { let s = 0; for (let d = 0; d < DIM; d++) s += a[d] * b[d]; return s; }
function cos(a, b) { return dot(a, b) / (Math.sqrt(dot(a, a) * dot(b, b)) || 1); }
// real mean residual per emotion code (over all voices) — to check blend consistency.
const meanResid = {};
{ const sum = {}, cnt = {}; for (const e of resids) { if (!sum[e.code]) { sum[e.code] = new Float64Array(DIM); cnt[e.code] = 0; } const s = sum[e.code]; for (let d = 0; d < DIM; d++) s[d] += e.r[d]; cnt[e.code]++; } for (const k in sum) { const m = sum[k]; for (let d = 0; d < DIM; d++) m[d] /= cnt[k]; meanResid[k] = m; } }

console.log('\nbasic     σ(dir)   default α');
const sigmaFull = {}, defaultAlpha = {};
for (const e of BASIC6) {
  const sg = sigmaOf(dir[e]); sigmaFull[e] = +sg.toFixed(4);
  defaultAlpha[e] = +Math.max(0.5, Math.min(ALPHA_MAX, TARGET_SIGMA / (sg || 1))).toFixed(2);
  console.log('  ' + LABEL[e].padEnd(10) + sg.toFixed(4) + '   ' + defaultAlpha[e]);
}

console.log('\nbasic opponent cosines (≈ −1 = opposed, like CAMEO anger↔sad):');
let hdr = '      '; for (const e of BASIC6) hdr += e.padStart(7); console.log(hdr);
for (const a of BASIC6) { let line = a.padEnd(6); for (const b of BASIC6) line += (a === b ? '  1.00' : (+cos(dir[a], dir[b]).toFixed(2)).toFixed(2)).padStart(7); console.log(line); }

console.log('\nblend consistency — cos(real blend residual, slider sum 0.5·(A+B)):');
const blends = [...seen.keys()].filter((k) => BASIC6.indexOf(k) < 0);
for (const code of blends) {
  const comp = compVec([...resids].find((e) => e.code === code).comp);
  const pred = new Float64Array(DIM);
  for (let i = 0; i < P; i++) if (comp[i]) for (let d = 0; d < DIM; d++) pred[d] += comp[i] * dir[BASIC6[i]][d];
  console.log('  ' + code.padEnd(5) + ' cos ' + (+cos(meanResid[code], pred).toFixed(3)));
}

// ── write emotion_basis.json (back up the CAMEO one) ──────────────────────────
const round = (a, p) => Array.from(a).map((v) => +v.toFixed(p));
const out = {
  dim: DIM, space: 'qwen-xvector',
  source: 'Qwen3-TTS 1.7B VoiceDesign self-rendered emotion corpus (' + V + ' voices × 12 emotions)',
  method: 'within-voice contrastive (vs grand-emotion mean) + ridge regression of residual on emotion composition (6 basic one-hot + 6 dyad blends 0.5/0.5); 6 basic directions, sigma-scaled. xvector += alpha·full[e].',
  emotions: BASIC6, label: LABEL,
  full: Object.fromEntries(BASIC6.map((e) => [e, round(dir[e], 6)])),
  sigmaFull, defaultAlpha, alphaMax: ALPHA_MAX, voices: V,
};
try { const old = MODEL_DIR + '/emotion_basis.json'; if (fs.existsSync(old) && !fs.existsSync(MODEL_DIR + '/emotion_basis.cameo.json')) fs.writeFileSync(MODEL_DIR + '/emotion_basis.cameo.json', fs.readFileSync(old)); } catch (e) {}
writeBoth('emotion_basis.json', JSON.stringify(out));
console.log('\nwrote emotion_basis.json (' + BASIC6.length + ' basic sliders) to', DATA_DIR, '+', MODEL_DIR, '· CAMEO backed up as emotion_basis.cameo.json');
console.log('DONE');
