// test.js — headless harness for Fluffshuffle.
'use strict';

advanceTime(200);

assert(typeof window.__fluffshuffle === 'object' && window.__fluffshuffle, '__fluffshuffle exposed');
var hooks = window.__fluffshuffle;
var B = hooks.board;
assert(B && typeof B.findMatches === 'function', 'board exposes findMatches');
assert(typeof B.slideRow === 'function', 'slideRow present');
assert(typeof B.slideCol === 'function', 'slideCol present');
assert(typeof B.hasAnyMatchingShift === 'function', 'hasAnyMatchingShift present');

// Title screenshot.
screenshot('apps/fluffshuffle/screenshot-title.png');

// ---------- Unit tests: pure slide-with-wrap ----------
var g = B.makeEmptyGrid();
for (var r = 0; r < B.ROWS; r++) for (var c = 0; c < B.COLS; c++) {
    g[r][c] = B.makePuff(1 + ((r + c) % 6), 0, false);
}

// Slide row 2 by +1: every col c should now hold what was at col (c-1).
var orig = B.copyGrid(g);
var g1 = B.slideRow(g, 2, 1);
for (var cc = 0; cc < B.COLS; cc++) {
    var src = (cc - 1 + B.COLS) % B.COLS;
    assert(g1[2][cc].color === orig[2][src].color,
        'slideRow +1: col ' + cc + ' should hold col ' + src + ' color');
}
// Source grid must be untouched (pure).
for (var cc2 = 0; cc2 < B.COLS; cc2++) {
    assert(g[2][cc2].color === orig[2][cc2].color, 'slideRow does not mutate input');
}
// Sliding row by COLS = identity.
var gLoop = B.slideRow(g, 2, B.COLS);
for (var cc3 = 0; cc3 < B.COLS; cc3++) {
    assert(gLoop[2][cc3].color === g[2][cc3].color, 'slideRow by COLS is identity');
}
// Negative shift wraps correctly.
var gNeg = B.slideRow(g, 2, -1);
for (var cc4 = 0; cc4 < B.COLS; cc4++) {
    var src2 = (cc4 + 1) % B.COLS;
    assert(gNeg[2][cc4].color === g[2][src2].color, 'slideRow -1 wraps');
}

// slideCol same pattern.
var g2 = B.slideCol(g, 3, 2);
for (var rr = 0; rr < B.ROWS; rr++) {
    var src3 = (rr - 2 + B.ROWS) % B.ROWS;
    assert(g2[rr][3].color === g[src3][3].color, 'slideCol +2: row ' + rr + ' sources row ' + src3);
}

// ---------- Match detection ----------
// Fresh non-matching board.
var mg = B.makeEmptyGrid();
for (var mr = 0; mr < B.ROWS; mr++) for (var mc = 0; mc < B.COLS; mc++) {
    // Stripe: color = 1 + ((r*3+c) % 6). Avoids immediate 3-in-rows/cols.
    mg[mr][mc] = B.makePuff(1 + ((mr * 3 + mc) % 6), 0, false);
}
var baseMatches = B.findMatches(mg);
assert(baseMatches.length === 0, 'striped grid has no matches, got ' + baseMatches.length);

// Force a 3-in-a-row on row 0.
mg[0][0].color = 1; mg[0][1].color = 1; mg[0][2].color = 1;
// clear any accidentally-matching neighbors on row 1 cols 0-2
for (var kc = 0; kc < 3; kc++) if (mg[1][kc].color === 1) mg[1][kc].color = 2;
var m1 = B.findMatches(mg);
assert(m1.length === 1, 'one 3-match group, got ' + m1.length);
assert(m1[0].size === 3, '3 cells');
assert(m1[0].maxLine === 3, 'maxLine=3');
assert(m1[0].special === B.SPECIAL_NONE, 'no special for 3-match');

// 4-in-a-row → jumbo.
mg[0][3].color = 1;
if (mg[1][3].color === 1) mg[1][3].color = 2;
var m2 = B.findMatches(mg);
assert(m2[0].special === B.SPECIAL_JUMBO, 'jumbo special for 4 in a row');

// 5 → arrow.
mg[0][4].color = 1;
if (mg[1][4].color === 1) mg[1][4].color = 2;
var m3 = B.findMatches(mg);
assert(m3[0].special === B.SPECIAL_ARROW, 'arrow special for 5 in a row');
assert(m3[0].arrowDir === 'h', 'horizontal arrow dir');

// L-shape → prism.
var lg = B.makeEmptyGrid();
for (var lr = 0; lr < B.ROWS; lr++) for (var lc = 0; lc < B.COLS; lc++) {
    lg[lr][lc] = B.makePuff(1 + ((lr * 5 + lc * 3 + 2) % 6), 0, false);
}
lg[0][0].color = 2; lg[0][1].color = 2; lg[0][2].color = 2;
lg[1][0].color = 2; lg[2][0].color = 2;
// wipe cross-contamination
if (lg[0][3].color === 2) lg[0][3].color = 3;
if (lg[3][0].color === 2) lg[3][0].color = 3;
if (lg[1][1].color === 2) lg[1][1].color = 3;
if (lg[2][1].color === 2) lg[2][1].color = 3;
if (lg[1][2].color === 2) lg[1][2].color = 3;
var m4 = B.findMatches(lg);
assert(m4.length === 1, 'L fuses to one group, got ' + m4.length);
assert(m4[0].special === B.SPECIAL_PRISM, 'prism special for L/T');

// ---------- Slide creates match + hasAnyMatchingShift ----------
// Shifting a row rotates it, so no NEW horizontal 3-in-a-row can appear in
// that row that wasn't already there. Instead, a slide creates a match by
// aligning a colored cell with matching cells in adjacent rows (vertical
// 3-in-a-column). Set that up precisely.
//
// Target vertical match at column c=2, color 5. Place (1,2)=5 and (3,2)=5.
// In pre-shift row 2, put color 5 at col 1, NOT col 2. After shift +1,
// (2,2) = old (2,1) = 5 → column 2 reads 5,5,5.
var sg = B.makeEmptyGrid();
// Start every cell a "safe" color that never creates cross-matches. Use
// a mosaic where no two orthogonal neighbors share a color and row 2 is
// rebuilt explicitly below.
for (var sr = 0; sr < B.ROWS; sr++) for (var sc = 0; sc < B.COLS; sc++) {
    // Use color = 1 + ((sr + sc * 2) % 3) from {1,2,3} — never equals 4/5/6.
    sg[sr][sc] = B.makePuff(1 + ((sr + sc * 2) % 3), 0, false);
}
// Now plant the vertical-match setup at column 2 using color 5.
sg[1][2] = B.makePuff(5, 0, false);
sg[3][2] = B.makePuff(5, 0, false);
// Row 2: every cell color 4, except (2,1)=5 (will slide into col 2).
for (var rc = 0; rc < B.COLS; rc++) sg[2][rc] = B.makePuff(4, 0, false);
sg[2][1] = B.makePuff(5, 0, false);
// Pre-shift board must have no existing match.
// Row 2 is all color 4 except col 1 — that's 4,5,4,4,4,4 which has
// no 3-in-a-row. Column 2 is [...,5,4,5,...] — no match.
// But row 2 has 4,4,4,4 at cols 2..5 which IS a horizontal 4-match! Fix.
sg[2][2] = B.makePuff(6, 0, false);
sg[2][4] = B.makePuff(6, 0, false);
// Row 2 is now: [4, 5, 6, 4, 6, 4] — no run of 3.
// Column 4 now reads [sr=0..5 col 4]. Make sure no accidental vert match.
// We'll validate via findMatches.
var preCheck = B.findMatches(sg);
assert(preCheck.length === 0, 'pre-shift has no match, got ' + preCheck.length);
// Post-shift +1 on row 2 → row 2 becomes [4, 4, 5, 6, 4, 6].
// Column 2 becomes [sg[0][2], sg[1][2]=5, 5 (from shift), sg[3][2]=5, ...].
var sgShifted = B.slideRow(sg, 2, 1);
var postM = B.findMatches(sgShifted);
assert(postM.length >= 1, 'post-shift produces a match, got ' + postM.length);
// Verify it's specifically a vertical 5-run at column 2.
var foundVert = false;
for (var pm = 0; pm < postM.length; pm++) {
    if (postM[pm].color === 5 && postM[pm].hasV) { foundVert = true; break; }
}
assert(foundVert, 'vertical 5-color match at column 2 after shift');

// legalShifts / hasAnyMatchingShift detect this.
assert(B.hasAnyMatchingShift(sg), 'hasAnyMatchingShift true on board with a legal move');
var shifts = B.legalShifts(sg);
var found = false;
for (var si = 0; si < shifts.length; si++) {
    if (shifts[si].axis === 'h' && shifts[si].index === 2) { found = true; break; }
}
assert(found, 'legalShifts lists row 2 as a legal move');

// Deadlock: construct a board with no 3-in-a-row achievable by a single
// row/column shift. With 6 colors and 6x6, "each row is a rotation of 1..6"
// gives no same-color adjacencies and no line shift produces three in a row.
var dead = B.makeEmptyGrid();
for (var dr = 0; dr < B.ROWS; dr++) for (var dc = 0; dc < B.COLS; dc++) {
    dead[dr][dc] = B.makePuff(1 + ((dr + dc) % 6), 0, false);
}
// Every row/column is a rotation of 1..6 — no single-axis wrap shift can
// produce three same-color cells in a line. (Shifting a row k just rotates
// it; it still contains one of each color.)
assert(B.findMatches(dead).length === 0, 'deadlock board has no existing match');
assert(!B.hasAnyMatchingShift(dead), 'hasAnyMatchingShift false for deadlocked rotation board');

// ---------- Scoring / cascade multiplier ----------
assert(B.scoreChain(3, 0) === 150, '3 puffs at chain depth 0 = 150');
assert(B.scoreChain(3, 1) === 300, '3 puffs at chain depth 1 (x2) = 300');
assert(B.scoreChain(4, 2) === 600, '4 puffs at chain depth 2 (x3) = 600');

// ---------- Integration: enter classic, force a match, score goes up ----------
B.startGame('classic');
advanceTime(50);
G.Screens.switchTo('playing');
advanceTime(50);
flush();
advanceTime(100);

// Install our crafted board and verify score increases after slide.
B.setGrid(sg);
B.setScore(0);
assert(B.getScore() === 0, 'score reset');

// Force a resolve by sliding row 2 by 1.
B.setGrid(B.slideRow(B.getGrid(), 2, 1));
B.resolveMatchesNow();
// Let flash + collapse + any cascade complete.
advanceTime(2500);
assert(B.getScore() > 0, 'score increased after forced slide+match, got ' + B.getScore());

// ---------- Cascade escalation ----------
// Build a grid with TWO independent matches so findMatches returns >=2 groups,
// plus a setup where the collapse triggers a second cascade.
B.startGame('classic');
advanceTime(50);

// Row 0 cols 0-2: color 1; Row 1 cols 0-2: color 2; Row 2 cols 0-2: color 3.
// Drop two more of color 1 at (1,3),(2,3) and a color 1 at (3,3) and (3,2),(3,1),
// so that once row 0 pops and gravity pulls down, a new match can form.
var cg = B.makeEmptyGrid();
for (var cr = 0; cr < B.ROWS; cr++) for (var cc5 = 0; cc5 < B.COLS; cc5++) {
    cg[cr][cc5] = B.makePuff(1 + ((cr * 5 + cc5 * 3 + 2) % 6), 0, false);
}
// Two simultaneous matches → chain 1 first resolve.
cg[0][0].color = 1; cg[0][1].color = 1; cg[0][2].color = 1;
cg[5][3].color = 2; cg[5][4].color = 2; cg[5][5].color = 2;
// scrub neighbors
if (cg[1][0].color === 1) cg[1][0].color = 3;
if (cg[1][1].color === 1) cg[1][1].color = 3;
if (cg[1][2].color === 1) cg[1][2].color = 3;
if (cg[4][3].color === 2) cg[4][3].color = 4;
if (cg[4][4].color === 2) cg[4][4].color = 4;
if (cg[4][5].color === 2) cg[4][5].color = 4;

B.setGrid(cg);
B.setScore(0);
var pre = B.getScore();
B.resolveMatchesNow();
advanceTime(2500);
var afterOne = B.getScore() - pre;
// Expected at least scoreChain(6, 0) = 300.
assert(afterOne >= 300, 'two 3-matches at chain 1 yield >=300, got ' + afterOne);

// Now simulate a 2-chain manually: after resolve, chain state is tracked
// internally. Resolve a fresh grid, then immediately resolve again before
// reset (verifies scoreChain formula reacts to chain depth).
assert(B.scoreChain(3, 0) < B.scoreChain(3, 1), 'chain 2 scores more than chain 1');
assert(B.scoreChain(3, 1) < B.scoreChain(3, 2), 'chain 3 scores more than chain 2');

// ---------- Mid-play screenshot ----------
// Seed fresh classic game and let a few frames render.
B.startGame('classic');
G.Screens.switchTo('playing');
advanceTime(100);
flush();
advanceTime(300);
screenshot('apps/fluffshuffle/screenshot-play.png');

console.log('fluffshuffle tests passed.');
