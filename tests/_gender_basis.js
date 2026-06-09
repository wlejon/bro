// Build the Qwen-TTS GENDER basis — a masculine↔feminine direction in the 1024-D
// speaker x-vector space, the voice-design companion to emotion_basis.json. The lab
// nudges a designed x-vector along it:  xvector += alpha · full[g]   (g ∈ {M,F}).
//
// Gender differs from emotion in ONE structural way that changes the recipe:
//   - emotion VARIES WITHIN a speaker → center per speaker, isolate the residual.
//   - gender is CONSTANT within a speaker → per-speaker centering would erase it.
// So instead we contrast WITHIN each corpus (male-minus-female inside one language /
// recording channel), which cancels the corpus/channel/language mean the same way
// per-speaker centering cancelled the speaker for emotion, then average the four
// per-language directions (equal corpus weight). The result is a language-invariant
// gender axis pooled across Italian / Bengali / French / English:
//
//   centroid[c][g] = mean over SPEAKERS of gender g in corpus c   (equal speaker weight)
//   dev[c][g]      = centroid[c][g] − mean_g centroid[c][g]       (within-corpus contrast)
//   full[g]        = mean over corpora of dev[c][g]               (equal corpus weight)
//
// For the binary M/F set this is antisymmetric (full[M] = −full[F]), i.e. a single
// bipolar axis; the artifact carries both so the lab can render one signed slider.
// mesd is dropped (recorded hot → clips), which also removes the only `child` data,
// so this is a clean two-pole masculine↔feminine axis.
//
// Reuses the x-vectors already embedded by cameo/_probe/embed_xvec.js, joined to
// per-clip gender by cameo/_probe/gender.py (genders.json). σ is the same per-
// dimension z-scored magnitude as the emotion basis (x-vector space has no learned
// basis). TARGET_SIGMA / ALPHA_MAX are calibrated by cameo/_probe/sweep_gender.js
// against real Base synthesis (gate on the model's f0, not this script's geometry).
//
// Run:  node bro/tests/_gender_basis.js

const fs = require('fs');

const XVECS = 'D:/projects/cameo/_probe/xvecs.jsonl';
const GENDERS = 'D:/projects/cameo/_probe/genders.json';
const MODEL_DIR = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base';
const DATA_DIR  = 'D:/projects/brosoundml-data/qwen-tts';
const SOURCE = 'CAMEO (it/bn/fr/en, permissive subset)';
const DROP_DATASETS = ['mesd'];   // recorded hot — 50% of clips clip (also the only `child` source)
const CLIP_MAX = 0.01;            // drop individual clips with any clipping
const LABELS = { M: 'masculine', F: 'feminine' };
const TARGET_SIGMA = 0.45;        // z-scored magnitude the default intensity aims for (calibrated via sweep_gender.js)
const ALPHA_MAX = 3;              // slider ceiling + default-alpha cap

function writeBoth(name, data) {
  for (const dir of [DATA_DIR, MODEL_DIR]) {
    try { fs.writeFileSync(dir + '/' + name, data); }
    catch (e) { console.log('  (skip ' + dir + ': ' + e.message + ')'); }
  }
}

// ── load + quality-filter x-vectors, join gender ─────────────────────────────
const genderOf = JSON.parse(fs.readFileSync(GENDERS, 'utf-8'));
const all = fs.readFileSync(XVECS, 'utf-8').trim().split('\n').map((l) => JSON.parse(l));
const rows = all.filter((r) => DROP_DATASETS.indexOf(r.dataset) < 0 && (r.clip == null || r.clip <= CLIP_MAX) && genderOf[r.file]);
for (const r of rows) r.g = genderOf[r.file];
const DIM = rows[0].x.length;
const GENDERS_SET = [...new Set(rows.map((r) => r.g))].sort();   // ['F','M']
const LABEL = {}; for (const g of GENDERS_SET) LABEL[g] = LABELS[g] || g;
console.log('x-vectors:', all.length, 'total ->', rows.length, 'kept (dropped', DROP_DATASETS.join('/'), '+ clipped + genderless) · dim', DIM);
console.log('genders:', GENDERS_SET.join('/'));

// per-dimension population std → the σ metric (raw x-vector spread)
const popMean = new Float64Array(DIM);
for (const r of rows) for (let d = 0; d < DIM; d++) popMean[d] += r.x[d];
for (let d = 0; d < DIM; d++) popMean[d] /= rows.length;
const popStd = new Float64Array(DIM);
for (const r of rows) for (let d = 0; d < DIM; d++) { const dd = r.x[d] - popMean[d]; popStd[d] += dd * dd; }
for (let d = 0; d < DIM; d++) popStd[d] = Math.sqrt(popStd[d] / rows.length) || 1;
function sigmaOf(vec) { let s = 0; for (let d = 0; d < DIM; d++) { const z = vec[d] / popStd[d]; s += z * z; } return Math.sqrt(s / DIM); }

// ── within-corpus contrast, pooled across corpora ────────────────────────────
// per (corpus, speaker) mean x-vector → equal-speaker-weight gender centroids per corpus
const CORPORA = [...new Set(rows.map((r) => r.dataset))].sort();
function vadd(m, x, s) { for (let d = 0; d < DIM; d++) m[d] += s * x[d]; }
function vscale(m, s) { for (let d = 0; d < DIM; d++) m[d] *= s; }

// devByCorpus[c][g] = within-corpus deviation of gender g from that corpus's gender mean
const devByCorpus = {};
const speakersUsed = {}; const clipsByCorpusGender = {};
for (const c of CORPORA) {
  // group this corpus's clips by speaker, then by gender
  const spk = {};
  for (const r of rows) { if (r.dataset !== c) continue; (spk[r.actor] = spk[r.actor] || []).push(r); }
  const cen = {}; const nspk = {};
  for (const g of GENDERS_SET) { cen[g] = new Float64Array(DIM); nspk[g] = 0; }
  for (const a in spk) {
    const clips = spk[a];
    const g = clips[0].g;
    if (!cen[g]) continue;
    const sm = new Float64Array(DIM);
    for (const r of clips) vadd(sm, r.x, 1);
    vscale(sm, 1 / clips.length);                 // per-speaker mean
    vadd(cen[g], sm, 1); nspk[g]++;                // accumulate speakers
    clipsByCorpusGender[c + '/' + g] = (clipsByCorpusGender[c + '/' + g] || 0) + clips.length;
  }
  // only corpora with BOTH genders contribute a within-corpus contrast
  if (!GENDERS_SET.every((g) => nspk[g] > 0)) { console.log('  skip', c, '(missing a gender)'); continue; }
  for (const g of GENDERS_SET) vscale(cen[g], 1 / nspk[g]);  // equal speaker weight
  const cgrand = new Float64Array(DIM);
  for (const g of GENDERS_SET) vadd(cgrand, cen[g], 1 / GENDERS_SET.length);
  devByCorpus[c] = {};
  for (const g of GENDERS_SET) { const dv = new Float64Array(DIM); for (let d = 0; d < DIM; d++) dv[d] = cen[g][d] - cgrand[d]; devByCorpus[c][g] = dv; }
  speakersUsed[c] = nspk;
}
const usedCorpora = Object.keys(devByCorpus).sort();

// full[g] = equal-corpus-weight average of within-corpus deviations
const full = {}, sigmaFull = {}, defaultAlpha = {};
for (const g of GENDERS_SET) {
  const f = new Float64Array(DIM);
  for (const c of usedCorpora) vadd(f, devByCorpus[c][g], 1 / usedCorpora.length);
  const round = Array.from(f).map((v) => +v.toFixed(6));
  full[g] = round;
  sigmaFull[g] = +sigmaOf(f).toFixed(4);
  defaultAlpha[g] = +Math.max(0.5, Math.min(ALPHA_MAX, TARGET_SIGMA / (sigmaFull[g] || 1))).toFixed(2);
}

// ── cross-language agreement: does each corpus's male-minus-female point the same
// way as the pooled axis? high positive cosine = a real, language-invariant gender
// direction (the gender analog of emotion's opponent-structure sanity check).
function cos(a, b) { let d = 0, na = 0, nb = 0; for (let i = 0; i < DIM; i++) { d += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; } return d / (Math.sqrt(na * nb) || 1); }
const ref = GENDERS_SET[GENDERS_SET.length - 1];   // 'M'
console.log('\ncross-language agreement (cos of each corpus male-axis vs pooled, 1.0 = perfect):');
for (const c of usedCorpora) {
  const nspk = speakersUsed[c];
  console.log('  ' + c.padEnd(16) + (+cos(devByCorpus[c][ref], full[ref]).toFixed(3)).toFixed(3).padStart(6) +
    '   speakers ' + GENDERS_SET.map((g) => g + ':' + nspk[g]).join(' '));
}

console.log('\ngender        full σ   default α    clips');
for (const g of GENDERS_SET) {
  let n = 0; for (const c of usedCorpora) n += clipsByCorpusGender[c + '/' + g] || 0;
  console.log('  ' + (LABEL[g] + ' (' + g + ')').padEnd(16) + String(sigmaFull[g]).padEnd(9) + String(defaultAlpha[g]).padEnd(13) + n);
}
console.log('cos(M,F) =', +cos(full[GENDERS_SET[0]], full[GENDERS_SET[1]]).toFixed(3), '(≈ -1 for a clean bipolar axis)');

// ── per-source attribution ───────────────────────────────────────────────────
const srcAgg = {};
for (const r of rows) { const s = srcAgg[r.dataset] = srcAgg[r.dataset] || { dataset: r.dataset, language: r.lang, license: r.license, clips: 0 }; s.clips++; }
const sources = Object.values(srcAgg).sort((a, b) => b.clips - a.clips);
console.log('\nlanguages:', [...new Set(rows.map((r) => r.lang))].join(', '));

const out = {
  dim: DIM, space: 'qwen-xvector', source: SOURCE, license: 'CC BY 4.0',
  method: 'between-speaker, within-corpus contrast: per-corpus (male-female)/2 deviation from the corpus gender mean, equal speaker weight, averaged equally across languages, in ECAPA x-vector space',
  bipolar: true,
  genders: GENDERS_SET, label: LABEL,
  full, sigmaFull, defaultAlpha, alphaMax: ALPHA_MAX,
  corpora: usedCorpora, sources,
};
writeBoth('gender_basis.json', JSON.stringify(out));

const cites = {
  emozionalmente: 'Catania et al., "Emozionalmente: A Crowdsourced Italian Speech Emotional Corpus" (CC BY 4.0)',
  subesco: 'Sultana et al., "SUBESCO: Bangla Speech Emotion Corpus" (CC BY 4.0)',
  oreau: 'Kerkeni et al., "Oréau French emotional speech" (CC BY 4.0)',
  jl_corpus: 'James et al., "JL-Corpus (NZ English)" (CC0 Public Domain)',
};
let notice = '# gender_basis.json (Qwen-TTS x-vector) — attribution\n\n' +
  'Derived masculine↔feminine *direction vectors* (within-corpus male-vs-female\n' +
  'contrast in Qwen-TTS ECAPA speaker x-vector space, pooled across four languages;\n' +
  'no source audio is included or recoverable). Built by bro/tests/_gender_basis.js\n' +
  'from the permissively-licensed subset of CAMEO (amu-cai/CAMEO). Artifact: CC BY 4.0.\n\nSources used:\n';
for (const s of sources) notice += `- ${s.dataset} (${s.language}, ${s.license}, ${s.clips} clips) — ${cites[s.dataset] || ''}\n`;
notice += '\nCAMEO: Tracz et al., "CAMEO: Collection of Multilingual Emotional Speech Corpora", arXiv:2505.11051.\n';
writeBoth('gender_basis.ATTRIBUTION.md', notice);

console.log('\nwrote gender_basis.json + gender_basis.ATTRIBUTION.md to', DATA_DIR, '+', MODEL_DIR);
console.log('DONE');
