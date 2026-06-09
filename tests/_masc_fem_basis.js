// Build the Qwen-TTS MASCULINE↔FEMININE basis — a bipolar vocal masculinity/
// femininity direction in the 1024-D speaker x-vector space, the voice-design
// companion to emotion_basis.json. This is an ACOUSTIC axis (it moves pitch +
// formant/timbre), not an identity label; "masc"/"fem" name the perceptual poles.
// It's *derived from* CAMEO's per-clip gender labels (see gender.py / genders.json),
// but what alpha actually controls is the sound. The lab nudges a designed x-vector
// along it:  xvector += alpha · full[p]   (p ∈ {M,F}, full[M] = −full[F]).
//
// The poles are CONSTANT within a speaker (a speaker has one), which changes the
// recipe vs emotion:
//   - emotion VARIES WITHIN a speaker → center per speaker, isolate the residual.
//   - masc/fem is CONSTANT within a speaker → per-speaker centering would erase it.
// So instead we contrast WITHIN each corpus (male-minus-female inside one language /
// recording channel), cancelling the corpus/channel/language mean the same way
// per-speaker centering cancelled the speaker for emotion, then average the four
// per-language directions (equal corpus weight) into a language-invariant axis
// pooled across Italian / Bengali / French / English:
//
//   centroid[c][p] = mean over SPEAKERS of pole p in corpus c   (equal speaker weight)
//   dev[c][p]      = centroid[c][p] − mean_p centroid[c][p]      (within-corpus contrast)
//   full[p]        = mean over corpora of dev[c][p]              (equal corpus weight)
//
// Binary M/F is antisymmetric (full[M] = −full[F]) — one bipolar axis; the artifact
// carries both poles so the lab can render a single signed slider. mesd is dropped
// (recorded hot → clips), which also drops the only `child` data, so this is a clean
// two-pole masculine↔feminine axis.
//
// Reuses the x-vectors already embedded by cameo/_probe/embed_xvec.js, joined to the
// per-clip CAMEO gender label by cameo/_probe/gender.py (genders.json). σ is the same
// per-dimension z-scored magnitude as the emotion basis. TARGET_SIGMA / ALPHA_MAX are
// calibrated by cameo/_probe/sweep_masc_fem.js against real Base synthesis (gate on
// the model's f0, not this script's geometry).
//
// Run:  node bro/tests/_masc_fem_basis.js

const fs = require('fs');

const XVECS = 'D:/projects/cameo/_probe/xvecs.jsonl';
const GENDERS = 'D:/projects/cameo/_probe/genders.json';
const MODEL_DIR = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base';
const DATA_DIR  = 'D:/projects/brosoundml-data/qwen-tts';
const SOURCE = 'CAMEO (it/bn/fr/en, permissive subset)';
const DROP_DATASETS = ['mesd'];   // recorded hot — 50% of clips clip (also the only `child` source)
const CLIP_MAX = 0.01;            // drop individual clips with any clipping
const LABELS = { M: 'masculine', F: 'feminine' };
const TARGET_SIGMA = 0.45;        // z-scored magnitude the default intensity aims for (calibrated via sweep_masc_fem.js)
const ALPHA_MAX = 3;              // slider ceiling + default-alpha cap

function writeBoth(name, data) {
  for (const dir of [DATA_DIR, MODEL_DIR]) {
    try { fs.writeFileSync(dir + '/' + name, data); }
    catch (e) { console.log('  (skip ' + dir + ': ' + e.message + ')'); }
  }
}

// ── load + quality-filter x-vectors, join the CAMEO gender label ─────────────
const poleOf = JSON.parse(fs.readFileSync(GENDERS, 'utf-8'));   // CAMEO gender label → pole key
const all = fs.readFileSync(XVECS, 'utf-8').trim().split('\n').map((l) => JSON.parse(l));
const rows = all.filter((r) => DROP_DATASETS.indexOf(r.dataset) < 0 && (r.clip == null || r.clip <= CLIP_MAX) && poleOf[r.file]);
for (const r of rows) r.p = poleOf[r.file];
const DIM = rows[0].x.length;
const POLES = [...new Set(rows.map((r) => r.p))].sort();   // ['F','M']
const LABEL = {}; for (const p of POLES) LABEL[p] = LABELS[p] || p;
console.log('x-vectors:', all.length, 'total ->', rows.length, 'kept (dropped', DROP_DATASETS.join('/'), '+ clipped + unlabeled) · dim', DIM);
console.log('poles:', POLES.join('/'));

// per-dimension population std → the σ metric (raw x-vector spread)
const popMean = new Float64Array(DIM);
for (const r of rows) for (let d = 0; d < DIM; d++) popMean[d] += r.x[d];
for (let d = 0; d < DIM; d++) popMean[d] /= rows.length;
const popStd = new Float64Array(DIM);
for (const r of rows) for (let d = 0; d < DIM; d++) { const dd = r.x[d] - popMean[d]; popStd[d] += dd * dd; }
for (let d = 0; d < DIM; d++) popStd[d] = Math.sqrt(popStd[d] / rows.length) || 1;
function sigmaOf(vec) { let s = 0; for (let d = 0; d < DIM; d++) { const z = vec[d] / popStd[d]; s += z * z; } return Math.sqrt(s / DIM); }

// ── within-corpus contrast, pooled across corpora ────────────────────────────
const CORPORA = [...new Set(rows.map((r) => r.dataset))].sort();
function vadd(m, x, s) { for (let d = 0; d < DIM; d++) m[d] += s * x[d]; }
function vscale(m, s) { for (let d = 0; d < DIM; d++) m[d] *= s; }

const devByCorpus = {};
const speakersUsed = {}; const clipsByCorpusPole = {};
for (const c of CORPORA) {
  const spk = {};
  for (const r of rows) { if (r.dataset !== c) continue; (spk[r.actor] = spk[r.actor] || []).push(r); }
  const cen = {}; const nspk = {};
  for (const p of POLES) { cen[p] = new Float64Array(DIM); nspk[p] = 0; }
  for (const a in spk) {
    const clips = spk[a];
    const p = clips[0].p;
    if (!cen[p]) continue;
    const sm = new Float64Array(DIM);
    for (const r of clips) vadd(sm, r.x, 1);
    vscale(sm, 1 / clips.length);                 // per-speaker mean
    vadd(cen[p], sm, 1); nspk[p]++;                // accumulate speakers
    clipsByCorpusPole[c + '/' + p] = (clipsByCorpusPole[c + '/' + p] || 0) + clips.length;
  }
  if (!POLES.every((p) => nspk[p] > 0)) { console.log('  skip', c, '(missing a pole)'); continue; }
  for (const p of POLES) vscale(cen[p], 1 / nspk[p]);  // equal speaker weight
  const cgrand = new Float64Array(DIM);
  for (const p of POLES) vadd(cgrand, cen[p], 1 / POLES.length);
  devByCorpus[c] = {};
  for (const p of POLES) { const dv = new Float64Array(DIM); for (let d = 0; d < DIM; d++) dv[d] = cen[p][d] - cgrand[d]; devByCorpus[c][p] = dv; }
  speakersUsed[c] = nspk;
}
const usedCorpora = Object.keys(devByCorpus).sort();

// full[p] = equal-corpus-weight average of within-corpus deviations
const full = {}, sigmaFull = {}, defaultAlpha = {};
for (const p of POLES) {
  const f = new Float64Array(DIM);
  for (const c of usedCorpora) vadd(f, devByCorpus[c][p], 1 / usedCorpora.length);
  full[p] = Array.from(f).map((v) => +v.toFixed(6));
  sigmaFull[p] = +sigmaOf(f).toFixed(4);
  defaultAlpha[p] = +Math.max(0.5, Math.min(ALPHA_MAX, TARGET_SIGMA / (sigmaFull[p] || 1))).toFixed(2);
}

// ── cross-language agreement: does each corpus's male-minus-female point the same
// way as the pooled axis? high positive cosine = a real, language-invariant axis.
function cos(a, b) { let d = 0, na = 0, nb = 0; for (let i = 0; i < DIM; i++) { d += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; } return d / (Math.sqrt(na * nb) || 1); }
const ref = POLES[POLES.length - 1];   // 'M'
console.log('\ncross-language agreement (cos of each corpus masc-axis vs pooled, 1.0 = perfect):');
for (const c of usedCorpora) {
  const nspk = speakersUsed[c];
  console.log('  ' + c.padEnd(16) + (+cos(devByCorpus[c][ref], full[ref]).toFixed(3)).toFixed(3).padStart(6) +
    '   speakers ' + POLES.map((p) => p + ':' + nspk[p]).join(' '));
}

console.log('\npole          full σ   default α    clips');
for (const p of POLES) {
  let n = 0; for (const c of usedCorpora) n += clipsByCorpusPole[c + '/' + p] || 0;
  console.log('  ' + (LABEL[p] + ' (' + p + ')').padEnd(16) + String(sigmaFull[p]).padEnd(9) + String(defaultAlpha[p]).padEnd(13) + n);
}
console.log('cos(M,F) =', +cos(full[POLES[0]], full[POLES[1]]).toFixed(3), '(≈ -1 for a clean bipolar axis)');

// ── per-source attribution ───────────────────────────────────────────────────
const srcAgg = {};
for (const r of rows) { const s = srcAgg[r.dataset] = srcAgg[r.dataset] || { dataset: r.dataset, language: r.lang, license: r.license, clips: 0 }; s.clips++; }
const sources = Object.values(srcAgg).sort((a, b) => b.clips - a.clips);
console.log('\nlanguages:', [...new Set(rows.map((r) => r.lang))].join(', '));

const out = {
  dim: DIM, space: 'qwen-xvector', source: SOURCE, license: 'CC BY 4.0',
  method: 'between-speaker, within-corpus contrast: per-corpus (male-female)/2 deviation from the corpus mean, equal speaker weight, averaged equally across languages, in ECAPA x-vector space; derived from CAMEO gender labels (an acoustic masc/fem axis, not an identity label)',
  bipolar: true,
  poles: POLES, label: LABEL,
  full, sigmaFull, defaultAlpha, alphaMax: ALPHA_MAX,
  corpora: usedCorpora, sources,
};
writeBoth('masc_fem_basis.json', JSON.stringify(out));

const cites = {
  emozionalmente: 'Catania et al., "Emozionalmente: A Crowdsourced Italian Speech Emotional Corpus" (CC BY 4.0)',
  subesco: 'Sultana et al., "SUBESCO: Bangla Speech Emotion Corpus" (CC BY 4.0)',
  oreau: 'Kerkeni et al., "Oréau French emotional speech" (CC BY 4.0)',
  jl_corpus: 'James et al., "JL-Corpus (NZ English)" (CC0 Public Domain)',
};
let notice = '# masc_fem_basis.json (Qwen-TTS x-vector) — attribution\n\n' +
  'A derived masculine↔feminine *direction vector* (within-corpus male-vs-female\n' +
  'contrast in Qwen-TTS ECAPA speaker x-vector space, pooled across four languages).\n' +
  'It is an acoustic vocal-quality axis derived from CAMEO\'s per-clip gender labels;\n' +
  'no source audio is included or recoverable. Built by bro/tests/_masc_fem_basis.js\n' +
  'from the permissively-licensed subset of CAMEO (amu-cai/CAMEO). Artifact: CC BY 4.0.\n\nSources used:\n';
for (const s of sources) notice += `- ${s.dataset} (${s.language}, ${s.license}, ${s.clips} clips) — ${cites[s.dataset] || ''}\n`;
notice += '\nCAMEO: Tracz et al., "CAMEO: Collection of Multilingual Emotional Speech Corpora", arXiv:2505.11051.\n';
writeBoth('masc_fem_basis.ATTRIBUTION.md', notice);

console.log('\nwrote masc_fem_basis.json + masc_fem_basis.ATTRIBUTION.md to', DATA_DIR, '+', MODEL_DIR);
console.log('DONE');
