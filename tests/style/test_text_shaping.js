// Cluster-map assertions on the render layer's shaper (bro.text — see
// src/js/text_bindings.h). These are the regression net for the caret and
// selection work that consumes the cluster map: they fail loudly if shaping
// stops fusing ligatures, stops kerning, stops joining Arabic, or starts
// reporting byte ranges that don't tile the string.
//
// Offsets here are UTF-8 BYTES — the render layer's own domain. That is
// deliberate; see text_bindings.h.

const CALIBRI = { family: 'Calibri', size: 40 };
const ARIAL   = { family: 'Arial', size: 40 };

function shape(text, opts) {
  const r = bro.text.shape(text, opts);
  assert(r !== null, `shape('${text}') returned null — no renderer?`);
  return r;
}

// --- the cluster map must tile the string exactly ---------------------------
// Every byte belongs to exactly one cluster and clusters cover [0, len).
// This is the invariant caret movement rests on: step by cluster and you can
// never land inside a glyph, and you always eventually reach the end.
function assertTiles(text, opts) {
  const r = shape(text, opts);
  const bytes = new TextEncoder().encode(text).length;
  const sorted = r.clusters.slice().sort((a, b) => a.start - b.start);
  assert(sorted.length > 0, `no clusters for '${text}'`);
  assert(sorted[0].start === 0, `'${text}': first cluster starts at ${sorted[0].start}`);
  for (let i = 0; i < sorted.length; i++) {
    assert(sorted[i].end > sorted[i].start,
           `'${text}': empty cluster at ${sorted[i].start}`);
    if (i + 1 < sorted.length) {
      assert(sorted[i].end === sorted[i + 1].start,
             `'${text}': gap/overlap between ${sorted[i].end} and ${sorted[i + 1].start}`);
    }
  }
  assert(sorted[sorted.length - 1].end === bytes,
         `'${text}': clusters end at ${sorted[sorted.length - 1].end}, want ${bytes}`);
  return r;
}

['hello world', 'AV', 'fi', 'Wavy Text', 'عربي', 'a b  c'].forEach(t => {
  assertTiles(t, ARIAL);
  assertTiles(t, CALIBRI);
});

// --- ligature: several bytes fuse into ONE glyph ----------------------------
// Calibri has an 'fi' ligature. Two source bytes, one cluster, one glyph —
// and that cluster is indivisible, so a byte offset of 1 (between f and i)
// reports the whole ligature's extent, not a position inside it.
{
  const fi = shape('fi', CALIBRI);
  assert(fi.clusters.length === 1,
         `Calibri 'fi' should be 1 cluster, got ${fi.clusters.length}`);
  assert(fi.glyphCount === 1,
         `Calibri 'fi' should shape to 1 glyph, got ${fi.glyphCount}`);
  assert(fi.clusters[0].glyphs === 1, `'fi' cluster should own 1 glyph`);

  const range = bro.text.clusterRange('fi', CALIBRI, 1);
  assert(range.start === 0 && range.end === 2,
         `clusterRange inside 'fi' should be [0,2), got [${range.start},${range.end})`);

  // Control: 'fx' does not ligate, so it stays two clusters and two glyphs.
  const fx = shape('fx', CALIBRI);
  assert(fx.clusters.length === 2, `'fx' should be 2 clusters, got ${fx.clusters.length}`);
  assert(fx.glyphCount === 2, `'fx' should be 2 glyphs, got ${fx.glyphCount}`);
}

// --- letter-spacing suppresses ligatures (CSS behaviour) --------------------
// Letter-spacing puts space BETWEEN characters; a ligature has fused them into
// one glyph with no seam. Browsers turn ligatures off rather than render a
// box whose ink is narrower than its layout width.
{
  const spaced = shape('fi', { ...CALIBRI, letterSpacing: 10 });
  assert(spaced.clusters.length === 2,
         `'fi' with letter-spacing should un-ligate to 2 clusters, got ${spaced.clusters.length}`);
  // n-1 gaps: one 10px gap between the two clusters, none trailing. Compare
  // against the sum of THIS run's own advances — the un-ligated pair is not
  // the same width as the ligature, which is the point of a ligature.
  const sumAdv = spaced.clusters.reduce((a, c) => a + c.advance, 0);
  assert(Math.abs(spaced.width - (sumAdv + 10)) < 0.01,
         `letter-spacing width: got ${spaced.width}, want ${sumAdv + 10}`);
  const plain = shape('fi', CALIBRI);
  assert(plain.width < sumAdv - 0.05,
         `the 'fi' ligature (${plain.width}) should be tighter than f+i (${sumAdv})`);
  assert(Math.abs(spaced.clusters[1].x - spaced.clusters[0].x -
                  (spaced.clusters[0].advance + 10)) < 0.01,
         'letter-spacing must land BETWEEN clusters');
}

// --- kerning: the pair is narrower than the sum of its parts ----------------
{
  const av = shape('AV', ARIAL).width;
  const a = shape('A', ARIAL).width;
  const v = shape('V', ARIAL).width;
  assert(av < a + v - 0.5,
         `'AV' (${av}) should kern tighter than 'A'+'V' (${a + v})`);
}

// --- Arabic joining: the same letters are narrower when they join ----------
// Each letter takes a positional form; the joined word is materially narrower
// than the four isolated forms laid end to end.
{
  const joined = shape('عربي', ARIAL);
  const isolated = ['ع', 'ر', 'ب', 'ي']
    .reduce((sum, ch) => sum + shape(ch, ARIAL).width, 0);
  assert(joined.width < isolated - 1,
         `Arabic joined (${joined.width}) should be narrower than isolated sum (${isolated})`);
  assert(joined.clusters.length === 4,
         `'عربي' should be 4 clusters, got ${joined.clusters.length}`);
  // Two bytes per Arabic letter in UTF-8.
  joined.clusters.forEach(c => {
    assert(c.end - c.start === 2, `Arabic cluster should span 2 bytes, got ${c.end - c.start}`);
  });
}

// --- word-spacing widens the space cluster only ----------------------------
{
  const plain = shape('a b', ARIAL);
  const spaced = shape('a b', { ...ARIAL, wordSpacing: 12 });
  assert(Math.abs(spaced.width - (plain.width + 12)) < 0.01,
         `word-spacing width: got ${spaced.width}, want ${plain.width + 12}`);
  assert(spaced.clusters.length === 3, 'a b is 3 clusters');
  assert(Math.abs(spaced.clusters[1].advance - (plain.clusters[1].advance + 12)) < 0.01,
         'word-spacing must widen the SPACE cluster, not its neighbours');
  assert(Math.abs(spaced.clusters[0].advance - plain.clusters[0].advance) < 0.01,
         "word-spacing must not touch 'a'");
}

// --- byteOffsetToX / xToByteOffset round-trip on cluster boundaries --------
{
  const text = 'Wavy Text';
  const r = shape(text, ARIAL);
  r.clusters.forEach(c => {
    const caret = bro.text.byteOffsetToX(text, ARIAL, c.start);
    assert(Math.abs(caret.x - c.x) < 0.01,
           `byteOffsetToX(${c.start}) = ${caret.x}, cluster x = ${c.x}`);
    // Probe just inside the cluster: the nearest boundary is its own start.
    const back = bro.text.xToByteOffset(text, ARIAL, c.x + c.advance * 0.2);
    assert(back === c.start,
           `xToByteOffset near cluster ${c.start} snapped to ${back}`);
  });
  // Past the end clamps to the full width, on the trailing edge.
  const end = bro.text.byteOffsetToX(text, ARIAL, 999);
  assert(Math.abs(end.x - r.width) < 0.01, 'offset past end should clamp to width');
  assert(end.isLeadingEdge === false, 'the end caret is a trailing edge');
}

// --- x positions are monotonic and total advance adds up -------------------
{
  const r = shape('hello world', ARIAL);
  let sum = 0;
  for (let i = 0; i < r.clusters.length; i++) {
    if (i > 0) {
      assert(r.clusters[i].x >= r.clusters[i - 1].x,
             'LTR cluster x must be non-decreasing');
    }
    sum += r.clusters[i].advance;
  }
  assert(Math.abs(sum - r.width) < 0.01,
         `cluster advances (${sum}) must sum to width (${r.width})`);
}

// --- the shaped-run cache actually caches ----------------------------------
{
  const before = bro.text.cacheStats();
  for (let i = 0; i < 50; i++) shape('cache me', ARIAL);
  const after = bro.text.cacheStats();
  assert(after.hits - before.hits >= 49,
         `repeat shaping should hit the cache: +${after.hits - before.hits} hits`);
  assert(after.misses - before.misses <= 1,
         `repeat shaping should miss once: +${after.misses - before.misses} misses`);
}

console.log('text shaping: cluster map OK');
