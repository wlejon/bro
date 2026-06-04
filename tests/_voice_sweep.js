// Off-anchor sweep of Kokoro's voice (style) space.
//
// Goal 1 — bounds: how far can we move a style vector off the real voices
//   before Kokoro stops rendering cleanly (NaNs / clipping / collapse)?
// Goal 2 — cache training data for the style adapter: (style -> attributes)
//   pairs across the manifold. Synthesis is deterministic, so the 256-D style
//   is the only thing we must store — audio (and later a Qwen ECAPA x-vector)
//   is reproducible on demand. No waveforms cached.
//
// Sampling: anchors (the real voices) · pairwise extrapolation (alpha beyond
// [0,1]) · radial scaling from the centroid · random convex combinations.

const fs = require('fs');
const OUT = 'D:/projects/voice-sweep';
fs.mkdirSync(OUT, { recursive: true });

// seeded xorshift32 — reproducible sampling
let _s = (0x9e3779b9) >>> 0;
function rnd() { _s ^= _s << 13; _s >>>= 0; _s ^= _s >> 17; _s ^= _s << 5; _s >>>= 0; return _s / 4294967296; }
function ri(n) { return Math.floor(rnd() * n); }

const dir = 'D:/projects/brosoundml/weights/kokoro';
bro.tts.setAssetRoot('D:/projects/brosoundml');
const k = bro.tts.loadKokoro(dir);
const text = 'Hello there. This is a test of the pipeline.';
const ids = bro.tts.phonemize(text);
const L = ids.length + 2;                   // BOS/EOS-wrapped length -> pick_for row

const names = fs.readdirSync(dir + '/voices')
  .filter(f => f.endsWith('.bin')).map(f => f.slice(0, -4)).sort();
console.log('voices', names.length, '· phonemes', ids.length, '· pick row', L - 1);

// anchor style points = the row Kokoro will actually select for this utterance
let COLS = 0;
const anchors = names.map(n => {
  const v = k.loadVoice(dir + '/voices/' + n + '.bin');
  COLS = v.cols;
  return Array.from(v.data.slice((L - 1) * COLS, L * COLS));
});
const centroid = new Array(COLS).fill(0);
for (const a of anchors) for (let i = 0; i < COLS; i++) centroid[i] += a[i] / anchors.length;

// ── measurement ────────────────────────────────────────────────────────────
const mean = (d) => { let m = 0; for (let i = 0; i < d.length; i++) m += d[i]; return m / d.length; };
const std = (d, m) => { let v = 0; for (let i = 0; i < d.length; i++) { const x = d[i] - m; v += x * x; } return Math.sqrt(v / d.length); };
const stage = (r, nm) => { const s = r.stages.find(x => x.name === nm); return s ? s.data : new Float32Array(0); };

function measure(style) {
  const v = k.createVoice(Float32Array.from(style), 's');
  const r = k.synthesizeTraced(ids, v);
  const s = r.samples, n = s.length;
  let peak = 0, rms = 0, nf = 0, zc = 0, prev = 0;
  for (let i = 0; i < n; i++) {
    const x = s[i];
    if (!(x === x) || x === Infinity || x === -Infinity) nf++;
    const a = x < 0 ? -x : x; if (a > peak) peak = a; rms += x * x;
    if (i > 0 && ((x < 0) !== (prev < 0))) zc++; prev = x;
  }
  rms = Math.sqrt(rms / n);
  const f0 = stage(r, 'F0_pred'), np = stage(r, 'N_pred');
  const f0m = mean(f0);
  let tot = 0; for (let i = 0; i < r.durations.length; i++) tot += r.durations[i];
  return {
    len: n, peak: +peak.toFixed(5), rms: +rms.toFixed(5), zcr: +(zc / n).toFixed(6),
    f0_mean: +f0m.toFixed(3), f0_std: +std(f0, f0m).toFixed(3),
    energy: +mean(np).toFixed(4), rate: +(tot / Math.max(1, r.durations.length)).toFixed(3),
    nonFinite: nf,
  };
}
// "clean" render: finite, not clipping, not collapsed to silence, not blown up
const degraded = (a) => a.nonFinite > 0 || a.peak > 0.99 || a.rms < 0.004 || a.rms > 0.45;

// ── sample accumulation ──────────────────────────────────────────────────────
const out = fs.openSync ? null : null;          // (placeholder; we batch-write JSONL)
const lines = [];
let idc = 0;
function record(method, params, style) {
  const a = measure(style);
  lines.push(JSON.stringify({
    id: idc++, method, params,
    deg: degraded(a) ? 1 : 0, attrs: a,
    style: style.map(x => +x.toFixed(6)),
  }));
  return a;
}

// 1) anchors
let anchorBad = 0;
const aR = { f0: [1e9, -1e9], rms: [1e9, -1e9], rate: [1e9, -1e9], energy: [1e9, -1e9] };
for (let i = 0; i < anchors.length; i++) {
  const a = record('anchor', { name: names[i] }, anchors[i]);
  if (degraded(a)) { anchorBad++; console.log('  anchor DEGRADED:', names[i], JSON.stringify(a)); }
  aR.f0[0] = Math.min(aR.f0[0], a.f0_mean); aR.f0[1] = Math.max(aR.f0[1], a.f0_mean);
  aR.rms[0] = Math.min(aR.rms[0], a.rms); aR.rms[1] = Math.max(aR.rms[1], a.rms);
  aR.rate[0] = Math.min(aR.rate[0], a.rate); aR.rate[1] = Math.max(aR.rate[1], a.rate);
  aR.energy[0] = Math.min(aR.energy[0], a.energy); aR.energy[1] = Math.max(aR.energy[1], a.energy);
}
console.log('anchors:', anchors.length - anchorBad, '/', anchors.length, 'clean');
console.log('  anchor ranges  f0', aR.f0.map(x => x.toFixed(0)), 'rms', aR.rms.map(x => x.toFixed(3)),
            'rate', aR.rate.map(x => x.toFixed(1)), 'energy', aR.energy.map(x => x.toFixed(2)));

const lerp = (a, b, t) => a.map((x, i) => x + (b[i] - x) * t);

// 2) pairwise extrapolation — alpha runs past both endpoints to find the edge
const ALPHAS = []; for (let a = -0.5; a <= 1.5001; a += 0.1) ALPHAS.push(+a.toFixed(2));
const pairValid = [];  // [alphaMin, alphaMax] clean interval per pair
for (let p = 0; p < 12; p++) {
  let i = ri(anchors.length), j = ri(anchors.length); if (j === i) j = (j + 1) % anchors.length;
  let lo = 99, hi = -99;
  for (const al of ALPHAS) {
    const a = record('pair', { i: names[i], j: names[j], alpha: al }, lerp(anchors[i], anchors[j], al));
    if (!degraded(a)) { lo = Math.min(lo, al); hi = Math.max(hi, al); }
  }
  pairValid.push({ pair: names[i] + '+' + names[j], clean: [lo, hi] });
}

// 3) radial scaling from the centroid: centroid + alpha*(anchor - centroid)
const RAD = []; for (let a = 0; a <= 2.5001; a += 0.125) RAD.push(+a.toFixed(3));
const radMax = [];
for (let p = 0; p < 6; p++) {
  const i = ri(anchors.length);
  let maxClean = 0;
  for (const al of RAD) {
    const style = centroid.map((c, d) => c + (anchors[i][d] - c) * al);
    const a = record('radial', { name: names[i], alpha: al }, style);
    if (!degraded(a)) maxClean = Math.max(maxClean, al);
  }
  radMax.push({ name: names[i], maxCleanAlpha: maxClean });
}

// 4) random convex combinations (interior of the hull) — should all be clean
let convBad = 0; const NCONV = 200;
for (let s = 0; s < NCONV; s++) {
  const kk = 2 + ri(3);                       // mix 2..4 anchors
  const w = []; let sum = 0;
  const pick = [];
  for (let t = 0; t < kk; t++) { pick.push(ri(anchors.length)); const u = rnd(); w.push(u); sum += u; }
  const style = new Array(COLS).fill(0);
  for (let t = 0; t < kk; t++) { const wt = w[t] / sum, A = anchors[pick[t]]; for (let d = 0; d < COLS; d++) style[d] += wt * A[d]; }
  const a = record('convex', { k: kk }, style);
  if (degraded(a)) convBad++;
}

// ── write cache + report ─────────────────────────────────────────────────────
fs.writeFileSync(OUT + '/sweep.jsonl', lines.join('\n') + '\n');
fs.writeFileSync(OUT + '/anchors.json', JSON.stringify({
  text, cols: COLS, pickRow: L - 1, names, anchors, centroid,
}));
fs.writeFileSync(OUT + '/summary.json', JSON.stringify({
  total: idc, voices: names.length, anchorRanges: aR,
  anchorsClean: anchors.length - anchorBad,
  convexCleanFrac: +((NCONV - convBad) / NCONV).toFixed(3),
  pairValid, radMax,
}, null, 2));

console.log('—');
console.log('pairwise clean alpha intervals (1.0 = endpoint j, 0 = endpoint i):');
for (const pv of pairValid) console.log('  ', pv.pair, '->', JSON.stringify(pv.clean));
console.log('radial max-clean alpha (1.0 = the real voice, >1 = past it):');
for (const rm of radMax) console.log('  ', rm.name, '->', rm.maxCleanAlpha);
console.log('convex interior:', NCONV - convBad, '/', NCONV, 'clean');
console.log('CACHED', idc, 'samples ->', OUT);
console.log('DONE');
