// Build the Kokoro EMOTION basis — categorical emotion directions in the 256-D
// style space, the Tier-1 companion to voice_basis.json's perceptual axes. Pure
// Node: reshapes embeddings we already produced into one artifact beside the
// Kokoro model (kokoro/ in brosoundml-data + the dev weights dir).
//
// Source: CAMEO (amu-cai/CAMEO on HuggingFace), the PERMISSIVELY-licensed,
// redistributable subset only — so the derived artifact can ship in the dataset
// repo paired with its attribution. ESD/IEMOCAP/MSP (signed research agreements)
// and CREMA-D (clipped anger) are excluded; mesd is dropped here (50% of its
// clips clip — recorded hot). Kept corpora, multilingual:
//   emozionalmente (it, CC BY 4.0), subesco (bn, CC BY 4.0),
//   oreau (fr, CC BY 4.0), jl_corpus (en, CC0)
// Pooling across languages with speaker-centering yields language-invariant
// emotion directions. Provenance (GPU step, run once): see cameo/_probe/
// export.py (parquet -> FLAC + manifest) and embed.js (decode + quality + embed)
//   -> cameo/_probe/styles.jsonl {file,dataset,lang,emo,actor,license,clip,...,style}
//
// Per emotion e: within-speaker neutral->e centroid shift (per-actor mean removed):
//   full[e]  = mean_sc(style | e) - mean_sc(style | NEU)
//   resid[e] = full[e] with the voice_basis prosody axes projected out (timbre).
// The lab applies `full` (its prosody-correlated components drive Kokoro's
// duration/F0/energy predictor → emotional pitch/energy/pace + timbre); resid
// alone left predicted prosody flat, so it didn't read as the emotion.
//
// Run:  node bro/tests/_emotion_basis.js

const fs = require('fs');

const STYLES = 'D:/projects/cameo/_probe/styles.jsonl';
const DATA_DIR  = 'D:/projects/brosoundml-data/kokoro';
const MODEL_DIR = 'D:/projects/brosoundml/weights/kokoro';
const SOURCE = 'CAMEO (it/bn/fr/en, permissive subset)';
const DROP_DATASETS = ['mesd'];   // recorded hot — 50% of clips clip
const CLIP_MAX = 0.01;            // drop individual clips with any clipping
const LABELS = { ANG:'angry', SAD:'sad', HAP:'happy', FEA:'fearful', DIS:'disgust', SUR:'surprise', NEU:'neutral' };
const TARGET_SIGMA = 0.55;        // σ of the FULL shift the default intensity aims for

function writeBoth(name, data) {
  for (const dir of [DATA_DIR, MODEL_DIR]) {
    try { fs.writeFileSync(dir + '/' + name, data); }
    catch (e) { console.log('  (skip ' + dir + ': ' + e.message + ')'); }
  }
}

// ── voice basis (prosody axes + the σ metric) ────────────────────────────────
const basis = JSON.parse(fs.readFileSync(MODEL_DIR + '/voice_basis.json', 'utf-8'));
const DIM = basis.dim;
const attrAxes = [];
for (let k = 0; k < basis.comps.length; k++) if (basis.axisKind && basis.axisKind[k] === 'attr') attrAxes.push(basis.comps[k]);
function sigmaOf(vec) {
  let s = 0;
  for (let i = 0; i < basis.k; i++) { const a = basis.comps[i]; let p = 0; for (let d = 0; d < DIM; d++) p += vec[d] * a[d]; const z = p / (basis.std[i] || 1); s += z * z; }
  return Math.sqrt(s / basis.k);
}
function projOutProsody(v) {
  const out = Float64Array.from(v);
  for (const a of attrAxes) { let p = 0; for (let d = 0; d < DIM; d++) p += out[d] * a[d]; for (let d = 0; d < DIM; d++) out[d] -= p * a[d]; }
  return out;
}

// ── load + quality-filter embeddings ─────────────────────────────────────────
const all = fs.readFileSync(STYLES, 'utf-8').trim().split('\n').map((l) => JSON.parse(l));
const rows = all.filter((r) => DROP_DATASETS.indexOf(r.dataset) < 0 && (r.clip == null || r.clip <= CLIP_MAX));
console.log('embeddings:', all.length, 'total ->', rows.length, 'kept (dropped', DROP_DATASETS.join('/'), '+ clipped)');

const EMOS = [...new Set(rows.map((r) => r.emo))].filter((e) => e !== 'NEU').sort();
const LABEL = {}; for (const e of EMOS) LABEL[e] = LABELS[e] || e.toLowerCase();

// per-actor mean (actors are corpus-namespaced) → speaker-centered centroids
const byActor = {};
for (const r of rows) (byActor[r.actor] = byActor[r.actor] || []).push(r);
const actorMean = {};
for (const a in byActor) { const m = new Float64Array(DIM); for (const r of byActor[a]) for (let d = 0; d < DIM; d++) m[d] += r.style[d]; for (let d = 0; d < DIM; d++) m[d] /= byActor[a].length; actorMean[a] = m; }
function centroidSC(emo) {
  const m = new Float64Array(DIM); let n = 0;
  for (const r of rows) { if (r.emo !== emo) continue; const am = actorMean[r.actor]; for (let d = 0; d < DIM; d++) m[d] += r.style[d] - am[d]; n++; }
  for (let d = 0; d < DIM; d++) m[d] /= n; return { m, n };
}
const neu = centroidSC('NEU');

const round = (arr, p) => Array.from(arr).map((v) => +v.toFixed(p));
const full = {}, resid = {}, sigmaFull = {}, sigmaResid = {}, defaultAlpha = {}, count = {};
for (const e of EMOS) {
  const c = centroidSC(e); count[e] = c.n;
  const f = new Float64Array(DIM); for (let d = 0; d < DIM; d++) f[d] = c.m[d] - neu.m[d];
  const r = projOutProsody(f);
  full[e] = round(f, 6); resid[e] = round(r, 6);
  sigmaFull[e] = +sigmaOf(f).toFixed(4); sigmaResid[e] = +sigmaOf(r).toFixed(4);
  defaultAlpha[e] = +Math.max(0.5, Math.min(4, TARGET_SIGMA / (sigmaFull[e] || 1))).toFixed(2);
}

// ── per-source attribution (for the dataset-repo license pairing) ────────────
const srcAgg = {};
for (const r of rows) { const s = srcAgg[r.dataset] = srcAgg[r.dataset] || { dataset: r.dataset, language: r.lang, license: r.license, clips: 0 }; s.clips++; }
const sources = Object.values(srcAgg).sort((a, b) => b.clips - a.clips);

console.log('\nlanguages:', [...new Set(rows.map((r) => r.lang))].join(', '));
console.log('sources:'); for (const s of sources) console.log('  ' + s.dataset.padEnd(16) + s.language.padEnd(10) + s.license.padEnd(20) + s.clips + ' clips');
console.log('\nemotion        full σ   resid σ   default α (full)    clips');
for (const e of EMOS) console.log('  ' + (LABEL[e] + ' (' + e + ')').padEnd(16) + String(sigmaFull[e]).padEnd(8) + String(sigmaResid[e]).padEnd(10) + String(defaultAlpha[e]).padEnd(18) + count[e]);

const out = {
  dim: DIM, source: SOURCE, license: 'CC BY 4.0',
  method: 'within-speaker neutral->emotion centroid, pooled across languages; resid = prosody-axes projected out',
  emotions: EMOS, label: LABEL,
  resid, full, sigmaResid, sigmaFull, defaultAlpha, alphaMax: 5,
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
let notice = '# emotion_basis.json — attribution\n\n' +
  'Derived emotion *direction vectors* (per-emotion centroids in Kokoro style space;\n' +
  'no source audio is included or recoverable). Built by bro/tests/_emotion_basis.js\n' +
  'from the permissively-licensed subset of CAMEO (amu-cai/CAMEO). Artifact: CC BY 4.0.\n\n' +
  'Sources used:\n';
for (const s of sources) notice += `- ${s.dataset} (${s.language}, ${s.license}, ${s.clips} clips) — ${cites[s.dataset] || ''}\n`;
notice += '\nCAMEO: Tracz et al., "CAMEO: Collection of Multilingual Emotional Speech Corpora", arXiv:2505.11051.\n';
writeBoth('emotion_basis.ATTRIBUTION.md', notice);

console.log('\nwrote emotion_basis.json + emotion_basis.ATTRIBUTION.md to', DATA_DIR, '+', MODEL_DIR);
console.log('DONE');
