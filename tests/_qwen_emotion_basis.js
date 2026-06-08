// Build the Qwen-TTS EMOTION basis — categorical emotion directions in the 1024-D
// speaker x-vector space, the Base-variant companion to Kokoro's emotion_basis.json.
// Where Kokoro learns directions in its 256-D STYLE space (which drives its F0/
// energy/duration predictor), Qwen's only continuous voice seam is the ECAPA
// x-vector fed to synthesize_with_xvector — so the basis lives there, and the lab
// nudges the designed x-vector along an emotion's `full` direction:
//
//   xvector += Σ alphaₑ · full[e]
//
// Pure Node: reshapes the 1024-D x-vectors we already embedded (cameo/_probe/
// embed_xvec.js — decode + quality + q.embedSpeaker) into one artifact beside the
// Base model. Same permissive CAMEO subset and CONTRASTIVE within-speaker recipe
// as _emotion_basis.js (per-actor mean removed throughout; each direction measured
// against the grand EMOTION mean, not neutral — neutral-relative shifts collapse
// onto one shared arousal axis):
//   grand   = mean_e mean_sc(x | e)              (equal weight per emotion)
//   full[e] = mean_sc(x | e) - grand  +  EXPRESS_BETA · (grand - mean_sc(x | NEU))
//
// NOTE: ECAPA x-vectors are trained for speaker verification — emotion is partly
// factored out by design. Per-actor centering isolates the within-speaker emotion
// residual; an alpha sweep through real Base synthesis (cameo/_probe/sweep_emotion.js)
// confirms it DOES render: anger rises in f0 + energy, sadness drops in both, both
// monotonic and coherent. Happiness reads but saturates by α≈2; fear/disgust/
// surprise are subtler and drag the AR loop (durations balloon) past α≈3. So the
// usable band is ~1.5–3: TARGET_SIGMA + ALPHA_MAX below put each emotion's default
// in it (strong emotions have larger σ → smaller α; the cap reins in the weak ones).
// σ is a per-dimension z-scored magnitude (no learned prosody axes exist here).
//
// Run:  node bro/tests/_qwen_emotion_basis.js

const fs = require('fs');

const XVECS = 'D:/projects/cameo/_probe/xvecs.jsonl';
const MODEL_DIR = 'D:/projects/brosoundml/weights/qwen-tts/0.6B-Base';
const DATA_DIR  = 'D:/projects/brosoundml-data/qwen-tts';
const SOURCE = 'CAMEO (it/bn/fr/en, permissive subset)';
const DROP_DATASETS = ['mesd'];   // recorded hot — 50% of clips clip
const CLIP_MAX = 0.01;            // drop individual clips with any clipping
const LABELS = { ANG:'angry', SAD:'sad', HAP:'happy', FEA:'fearful', DIS:'disgust', SUR:'surprise', NEU:'neutral' };
const TARGET_SIGMA = 0.33;        // z-scored magnitude the default intensity aims for (calibrated via sweep_emotion.js)
const EXPRESS_BETA = 0;           // 0 = pure contrastive; up to 1 re-adds shared expressivity (grand->neutral)
const ALPHA_MAX = 4;              // slider ceiling + default-alpha cap (past ~4 the AR loop drags off-manifold)

function writeBoth(name, data) {
  for (const dir of [DATA_DIR, MODEL_DIR]) {
    try { fs.writeFileSync(dir + '/' + name, data); }
    catch (e) { console.log('  (skip ' + dir + ': ' + e.message + ')'); }
  }
}

// ── load + quality-filter x-vectors ──────────────────────────────────────────
const all = fs.readFileSync(XVECS, 'utf-8').trim().split('\n').map((l) => JSON.parse(l));
const rows = all.filter((r) => DROP_DATASETS.indexOf(r.dataset) < 0 && (r.clip == null || r.clip <= CLIP_MAX));
const DIM = rows[0].x.length;
console.log('x-vectors:', all.length, 'total ->', rows.length, 'kept (dropped', DROP_DATASETS.join('/'), '+ clipped) · dim', DIM);

const EMOS = [...new Set(rows.map((r) => r.emo))].filter((e) => e !== 'NEU').sort();
const LABEL = {}; for (const e of EMOS) LABEL[e] = LABELS[e] || e.toLowerCase();

// per-dimension population std → the σ metric (raw x-vector spread, the analog of
// Kokoro's per-axis std; x-vector space has no learned basis to project onto).
const popMean = new Float64Array(DIM);
for (const r of rows) for (let d = 0; d < DIM; d++) popMean[d] += r.x[d];
for (let d = 0; d < DIM; d++) popMean[d] /= rows.length;
const popStd = new Float64Array(DIM);
for (const r of rows) for (let d = 0; d < DIM; d++) { const dd = r.x[d] - popMean[d]; popStd[d] += dd * dd; }
for (let d = 0; d < DIM; d++) popStd[d] = Math.sqrt(popStd[d] / rows.length) || 1;
function sigmaOf(vec) {
  let s = 0; for (let d = 0; d < DIM; d++) { const z = vec[d] / popStd[d]; s += z * z; }
  return Math.sqrt(s / DIM);
}

// per-actor mean (actors are corpus-namespaced) → speaker-centered centroids
const byActor = {};
for (const r of rows) (byActor[r.actor] = byActor[r.actor] || []).push(r);
const actorMean = {};
for (const a in byActor) { const m = new Float64Array(DIM); for (const r of byActor[a]) for (let d = 0; d < DIM; d++) m[d] += r.x[d]; for (let d = 0; d < DIM; d++) m[d] /= byActor[a].length; actorMean[a] = m; }
function centroidSC(emo) {
  const m = new Float64Array(DIM); let n = 0;
  for (const r of rows) { if (r.emo !== emo) continue; const am = actorMean[r.actor]; for (let d = 0; d < DIM; d++) m[d] += r.x[d] - am[d]; n++; }
  for (let d = 0; d < DIM; d++) m[d] /= n; return { m, n };
}
const neu = centroidSC('NEU');

// Per-emotion speaker-centered centroids + the grand emotion mean (equal weight per
// emotion, so a large corpus can't pull the reference). `shared` is the common
// expressivity offset every emotion has over neutral, re-added only in EXPRESS_BETA.
const cen = {}; for (const e of EMOS) cen[e] = centroidSC(e);
const grand = new Float64Array(DIM);
for (const e of EMOS) for (let d = 0; d < DIM; d++) grand[d] += cen[e].m[d] / EMOS.length;
const shared = new Float64Array(DIM); for (let d = 0; d < DIM; d++) shared[d] = grand[d] - neu.m[d];

const round = (arr, p) => Array.from(arr).map((v) => +v.toFixed(p));
const full = {}, sigmaFull = {}, defaultAlpha = {}, count = {};
for (const e of EMOS) {
  count[e] = cen[e].n;
  const f = new Float64Array(DIM); for (let d = 0; d < DIM; d++) f[d] = (cen[e].m[d] - grand[d]) + EXPRESS_BETA * shared[d];
  full[e] = round(f, 6);
  sigmaFull[e] = +sigmaOf(f).toFixed(4);
  defaultAlpha[e] = +Math.max(0.5, Math.min(ALPHA_MAX, TARGET_SIGMA / (sigmaFull[e] || 1))).toFixed(2);
}

// pairwise cosine between emotion directions — a sanity check that the contrast
// restored opponent structure (near-zero / negative off-diagonals) rather than the
// one-shared-axis collapse (all ~+0.9) the neutral-relative version produced.
function cos(a, b) { let d = 0, na = 0, nb = 0; for (let i = 0; i < DIM; i++) { d += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; } return d / (Math.sqrt(na * nb) || 1); }

// ── per-source attribution (for the dataset-repo license pairing) ────────────
const srcAgg = {};
for (const r of rows) { const s = srcAgg[r.dataset] = srcAgg[r.dataset] || { dataset: r.dataset, language: r.lang, license: r.license, clips: 0 }; s.clips++; }
const sources = Object.values(srcAgg).sort((a, b) => b.clips - a.clips);

console.log('\nlanguages:', [...new Set(rows.map((r) => r.lang))].join(', '));
console.log('sources:'); for (const s of sources) console.log('  ' + s.dataset.padEnd(16) + String(s.language).padEnd(10) + s.license.padEnd(20) + s.clips + ' clips');
console.log('\nemotion        full σ   default α    clips');
for (const e of EMOS) console.log('  ' + (LABEL[e] + ' (' + e + ')').padEnd(16) + String(sigmaFull[e]).padEnd(9) + String(defaultAlpha[e]).padEnd(13) + count[e]);
console.log('\npairwise cosine (off-diagonal near 0 = opponent structure restored):');
process.stdout.write('       ' + EMOS.map((e) => e.padStart(7)).join('') + '\n');
for (const a of EMOS) { let line = '  ' + a.padEnd(5); for (const b of EMOS) line += (+cos(full[a], full[b]).toFixed(2)).toFixed(2).padStart(7); console.log(line); }

const out = {
  dim: DIM, space: 'qwen-xvector', source: SOURCE, license: 'CC BY 4.0',
  method: 'within-speaker contrastive: emotion centroid - grand emotion mean (+ ' + EXPRESS_BETA + '·shared expressivity), pooled across languages, in ECAPA x-vector space',
  emotions: EMOS, label: LABEL,
  full, sigmaFull, defaultAlpha, alphaMax: ALPHA_MAX,
  neutralClips: neu.n, actors: Object.keys(byActor).length, sources,
};
writeBoth('emotion_basis.json', JSON.stringify(out));

// attribution sidecar paired with the artifact in the dataset repo
const cites = {
  emozionalmente: 'Catania et al., "Emozionalmente: A Crowdsourced Italian Speech Emotional Corpus" (CC BY 4.0)',
  subesco: 'Sultana et al., "SUBESCO: Bangla Speech Emotion Corpus" (CC BY 4.0)',
  oreau: 'Kerkeni et al., "Oréau French emotional speech" (CC BY 4.0)',
  jl_corpus: 'James et al., "JL-Corpus (NZ English)" (CC0 Public Domain)',
};
let notice = '# emotion_basis.json (Qwen-TTS x-vector) — attribution\n\n' +
  'Derived emotion *direction vectors* (per-emotion centroids in Qwen-TTS ECAPA\n' +
  'speaker x-vector space; no source audio is included or recoverable). Built by\n' +
  'bro/tests/_qwen_emotion_basis.js from the permissively-licensed subset of CAMEO\n' +
  '(amu-cai/CAMEO). Artifact: CC BY 4.0.\n\nSources used:\n';
for (const s of sources) notice += `- ${s.dataset} (${s.language}, ${s.license}, ${s.clips} clips) — ${cites[s.dataset] || ''}\n`;
notice += '\nCAMEO: Tracz et al., "CAMEO: Collection of Multilingual Emotional Speech Corpora", arXiv:2505.11051.\n';
writeBoth('emotion_basis.ATTRIBUTION.md', notice);

console.log('\nwrote emotion_basis.json + emotion_basis.ATTRIBUTION.md to', DATA_DIR, '+', MODEL_DIR);
console.log('DONE');
