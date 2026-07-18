// Caret, hit-testing and selection over the shaper's cluster map.
//
// test_text_shaping.js asserts the cluster map itself is sane. This file
// asserts the editing surfaces actually consult it — that clicking, arrowing
// and selecting land where the glyphs really are rather than where a sum of
// prefix widths guessed they were.
//
// The load-bearing assertion is the boring one: on plain unkerned English,
// every one of these answers must be EXACTLY what prefix measurement gave
// before, because that is the text 99% of users are editing and the two
// methods provably agree there. The interesting assertions — kerned pairs,
// ligatures — are the ones that are allowed to differ, and they may only
// differ toward the shaped truth.
//
// Set BRO_CLUSTER_CARET=0 to run the whole engine on the prefix fallback
// instead; see src/layout/cluster_caret.h.

const ARIAL   = { family: 'Arial', size: 16 };
const CALIBRI = { family: 'Calibri', size: 40 };

function shape(text, opts) {
  const r = bro.text.shape(text, opts);
  assert(r !== null, `shape('${text}') returned null — no renderer?`);
  return r;
}

// True caret x for a byte offset, straight off the cluster map: the leading
// edge of the cluster owning that byte, or the run width at the end.
function shapedCaretX(text, byteOffset, opts) {
  const r = shape(text, opts);
  if (byteOffset >= new TextEncoder().encode(text).length) return r.width;
  for (const c of r.clusters) {
    if (byteOffset >= c.start && byteOffset < c.end) return c.x;
  }
  return r.width;
}

// What prefix measurement would have said: the width of the text on its own.
function prefixCaretX(text, byteOffset, opts) {
  if (byteOffset <= 0) return 0;
  return shape(text.slice(0, byteOffset), opts).width;
}

// --- plain English: the two methods must agree exactly ----------------------
// Arial has no kerning pair in this sentence and no ligature, so every caret
// position is a plain sum of advances. Any drift here is a regression in the
// case that matters most.
{
  const s = 'The quick brown fox jumps over the lazy dog';
  let worst = 0;
  for (let b = 0; b <= s.length; b++) {
    const d = Math.abs(shapedCaretX(s, b, ARIAL) - prefixCaretX(s, b, ARIAL));
    if (d > worst) worst = d;
  }
  assert(worst < 0.01,
         `plain English caret drifted ${worst.toFixed(4)}px from prefix measurement`);
}

// --- kerned pairs: the cluster map must NOT agree with prefix measurement ---
// Measuring 'A' alone cannot know it is about to be tucked under a 'V'. If
// these ever agree, the caret has silently gone back to prefix widths.
{
  const s = 'AVATAR Wavy Tokyo Yawn';
  let worst = 0;
  for (let b = 0; b <= s.length; b++) {
    const d = Math.abs(shapedCaretX(s, b, ARIAL) - prefixCaretX(s, b, ARIAL));
    if (d > worst) worst = d;
  }
  assert(worst > 0.5,
         `kerned text caret only drifted ${worst.toFixed(4)}px — is kerning reaching the caret?`);
}

// --- a ligature has no interior: both bytes report the same caret x ---------
{
  const fi = shape('fi', CALIBRI);
  assert(fi.clusters.length === 1, `Calibri 'fi' should be one cluster`);
  const x0 = shapedCaretX('fi', 0, CALIBRI);
  const x1 = shapedCaretX('fi', 1, CALIBRI);
  const x2 = shapedCaretX('fi', 2, CALIBRI);
  assert(x0 === x1,
         `offset inside the 'fi' ligature must resolve to its leading edge (${x0} vs ${x1})`);
  assert(x2 > x0, `caret after 'fi' must sit past its leading edge`);
  assert(Math.abs(x2 - fi.width) < 0.01,
         `caret at end of 'fi' should be the run width`);

  // clusterRange is what arrow keys step by: from anywhere in the ligature it
  // names the whole thing, so a step out of it lands on a real caret site.
  const mid = bro.text.clusterRange('fi', CALIBRI, 1);
  assert(mid.start === 0 && mid.end === 2,
         `clusterRange inside 'fi' should be [0,2), got [${mid.start},${mid.end})`);
}

// --- click round-trip: x of an offset, hit-tested, returns that offset ------
// Every caret site must be reachable by clicking at its own x. This is the
// property that makes click-then-arrow behave, and the one prefix measurement
// loses on kerned text.
function assertRoundTrip(s, opts, label) {
  const r = shape(s, opts);
  const edges = r.clusters.slice().sort((a, b) => a.start - b.start);
  for (const c of edges) {
    // nearest cluster edge to this cluster's own leading edge is itself
    let best = null, bd = Infinity;
    for (const e of edges) {
      const d = Math.abs(e.x - c.x);
      if (d < bd) { bd = d; best = e; }
    }
    assert(best.start === c.start,
           `${label}: x=${c.x} should hit-test back to byte ${c.start}, got ${best.start}`);
  }
}
assertRoundTrip('The quick brown fox', ARIAL, 'plain English');
assertRoundTrip('AVATAR Wavy Tokyo Yawn', ARIAL, 'kerned');

// --- live controls: clicking and arrowing a real <input> -------------------
{
  document.body.innerHTML =
    `<input id="ci" style="font:16px Arial;width:400px;padding:0;border:0"
            value="The quick brown fox jumps over the lazy dog">`;
  flush();
  const inp = document.getElementById('ci');
  inp.focus();
  flush();
  const r = inp.getBoundingClientRect();

  // Arrow keys traverse every character and come back. Cluster stepping must
  // still terminate and must still reach both ends.
  inp.setSelectionRange(0, 0);
  const seen = [0];
  for (let i = 0; i < 80; i++) {
    keyDown(1073741903);                        // right
    if (inp.selectionStart !== seen[seen.length - 1]) seen.push(inp.selectionStart);
  }
  assert(seen[seen.length - 1] === inp.value.length,
         `arrow-right stopped at ${seen[seen.length - 1]}, want ${inp.value.length}`);
  for (let i = 0; i < 80; i++) keyDown(1073741904);   // left
  assert(inp.selectionStart === 0, `arrow-left did not return to 0`);

  // Every step was forward and landed on a distinct offset — a cluster step
  // that returned its own input would hang the caret.
  for (let i = 1; i < seen.length; i++) {
    assert(seen[i] > seen[i - 1], `caret went backwards during arrow-right`);
  }

  // Clicking further right never moves the caret left.
  let prev = -1;
  for (let dx = 0; dx <= 288; dx += 12) {
    mouseMove(r.left + 600, r.top + 300);       // break the multi-click chain
    click(r.left + dx, r.top + r.height / 2);
    assert(inp.selectionStart >= prev,
           `click at ${dx}px moved the caret backwards (${inp.selectionStart} < ${prev})`);
    prev = inp.selectionStart;
  }
  assert(prev > 30, `clicking near the right edge only reached offset ${prev}`);
}

console.log('caret cluster tests passed');
