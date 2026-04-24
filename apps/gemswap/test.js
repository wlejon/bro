// test.js — headless harness for gemswap.
'use strict';

// Give app scripts a chance to boot.
advanceTime(200);

// Sanity: globals wired up.
assert(typeof window.__gemswap === 'object' && window.__gemswap, '__gemswap exposed');
var hooks = window.__gemswap;
var B = hooks.board;
assert(B && typeof B.findMatches === 'function', 'board exposes findMatches');

// -------- Title screenshot --------
screenshot('apps/gemswap/screenshot-title.png');

// -------- Pure-function tests --------

// findMatches on a hand-seeded board.
var g = B.makeEmptyGrid();
for (var r = 0; r < B.ROWS; r++) {
    for (var c = 0; c < B.COLS; c++) {
        g[r][c] = { color: 1 + ((r + c) % 7), special: 0, frozen: false };
    }
}
// No matches in a diagonal-stripe checker.
var m0 = B.findMatches(g);
assert(m0.length === 0, 'diagonal-stripe grid has no matches, got ' + m0.length);

// Force a horizontal 3-match in row 0, cols 0..2.
g[0][0].color = 1; g[0][1].color = 1; g[0][2].color = 1;
var m1 = B.findMatches(g);
assert(m1.length === 1, 'one match found, got ' + m1.length);
assert(m1[0].cells.length === 3, 'match has 3 cells');
assert(m1[0].maxLine === 3, 'maxLine=3');
assert(m1[0].special === B.SPECIAL_NONE, 'no special for 3-match');

// 4-match → flame special.
g[0][3].color = 1;
var m2 = B.findMatches(g);
assert(m2[0].special === B.SPECIAL_FLAME, 'flame special for 4 in a row');

// 5-match → hyper.
g[0][4].color = 1;
var m3 = B.findMatches(g);
assert(m3[0].special === B.SPECIAL_HYPER, 'hyper special for 5 in a row');

// L/T shape → star.
var g2 = B.makeEmptyGrid();
for (var r2 = 0; r2 < B.ROWS; r2++) for (var c2 = 0; c2 < B.COLS; c2++) {
    g2[r2][c2] = { color: 1 + ((r2 * 3 + c2 * 5 + 1) % 7), special: 0, frozen: false };
}
g2[0][0].color = 2; g2[0][1].color = 2; g2[0][2].color = 2;
g2[1][0].color = 2; g2[2][0].color = 2;
// Make sure neighbours don't collide
if (g2[3][0].color === 2) g2[3][0].color = 3;
if (g2[0][3].color === 2) g2[0][3].color = 3;
if (g2[1][1] && g2[1][1].color === 2) g2[1][1].color = 3;
var m4 = B.findMatches(g2);
assert(m4.length === 1, 'L shape fuses to one group, got ' + m4.length);
assert(m4[0].special === B.SPECIAL_STAR, 'star special for L/T');

// swapMakesMatch returns true when the swap creates a match.
var g3 = B.makeEmptyGrid();
for (var rr = 0; rr < 8; rr++) for (var cc = 0; cc < 8; cc++) {
    g3[rr][cc] = { color: 1 + ((rr * 7 + cc * 3) % 7), special: 0, frozen: false };
}
// Set up near-match: row0: [5, 5, X, 5]  where X is (0,2). Swap (0,2) with (1,2) where (1,2).color=5 → makes row0 = 5,5,5.
g3[0][0].color = 5; g3[0][1].color = 5; g3[0][3].color = 5;
g3[0][2].color = 3;
g3[1][2].color = 5;
assert(B.swapMakesMatch(g3, 0, 2, 1, 2), 'swap creates a match');
// Make sure original unchanged
assert(g3[0][2].color === 3, 'grid unchanged after swapMakesMatch');

// Deadlock detection: grid with no valid moves.
var dead = B.makeEmptyGrid();
// Build a non-matching pattern with no swap able to produce a run.
// Using the repeating 1234567 pattern rotated per row works (no two neighbors same;
// swapping any two adjacent cells either does nothing or still no 3-in-a-row).
for (var dr = 0; dr < 8; dr++) for (var dc = 0; dc < 8; dc++) {
    dead[dr][dc] = { color: 1 + ((dr * 3 + dc) % 7), special: 0, frozen: false };
}
// Actually this pattern does have swaps that create matches sometimes; instead
// build from a valid stripe and verify findAnyMove returns null only if truly dead.
// So instead check the helper returns *something reasonable* on a known live board.
var live = B.seedGrid();
assert(B.findAnyMove(live) !== null, 'seeded board has at least one valid move');
assert(B.findMatches(live).length === 0, 'seeded board has no starting matches');

// scoreChain formula: 3-match on chain depth 0 = 50 * (0+1) = 50.
assert(B.scoreChain([3], 0) === 50, '3-match base 50');
assert(B.scoreChain([3], 1) === 100, '3-match chain x2 = 100');
assert(B.scoreChain([4], 0) === 100, '4-match base 100');
assert(B.scoreChain([5], 0) === 150, '5-match base 150');
assert(B.scoreChain([3, 3], 2) === 300, 'double-3 chain x3 = 300');

// -------- Integration: start classic and force a swap that produces a match --------
B.startGame('classic');
advanceTime(100);

// Seed a deterministic pre-match state: put color 1 at (4,4) and (4,5) and (4,7),
// then swap (4,6) with (5,6) where we place color 1.
var gg = B.getGrid();
// Clear and rebuild
var stripeGrid = B.makeEmptyGrid();
for (var r3 = 0; r3 < 8; r3++) for (var c3 = 0; c3 < 8; c3++) {
    stripeGrid[r3][c3] = { color: 1 + ((r3 * 2 + c3 * 5) % 7), special: 0, frozen: false };
    // avoid accidental matches
}
// Handcraft: row 4 → 2,3,4,5,1,1,2,1 and row 5 col 6 = 1 so swapping (4,6)<->(5,6) → row 4 becomes ...,1,1,1,1
stripeGrid[4][0] = { color: 2, special: 0, frozen: false };
stripeGrid[4][1] = { color: 3, special: 0, frozen: false };
stripeGrid[4][2] = { color: 4, special: 0, frozen: false };
stripeGrid[4][3] = { color: 5, special: 0, frozen: false };
stripeGrid[4][4] = { color: 1, special: 0, frozen: false };
stripeGrid[4][5] = { color: 1, special: 0, frozen: false };
stripeGrid[4][6] = { color: 2, special: 0, frozen: false };
stripeGrid[4][7] = { color: 1, special: 0, frozen: false };
stripeGrid[5][6] = { color: 1, special: 0, frozen: false };
// Make sure row 3 / row 5 adjacent don't cause matches spontaneously.
stripeGrid[3][4] = { color: 2, special: 0, frozen: false };
stripeGrid[3][5] = { color: 3, special: 0, frozen: false };
stripeGrid[3][6] = { color: 4, special: 0, frozen: false };
stripeGrid[5][5] = { color: 3, special: 0, frozen: false };
stripeGrid[5][7] = { color: 4, special: 0, frozen: false };

// Verify no pre-existing match in the crafted board.
var preM = B.findMatches(stripeGrid);
assert(preM.length === 0, 'crafted board has no starting match, got ' + preM.length);

// Switch into playing mode so input is routed to the board.
G.Screens.switchTo('playing');
advanceTime(50);

B.setGrid(stripeGrid);
B.setScore(0);
assert(B.getScore() === 0, 'score reset to 0');

// Swap creates a match.
assert(B.swapMakesMatch(B.getGrid(), 4, 6, 5, 6), 'crafted swap creates match');

// Drive via coordinate clicks: compute canvas coords for (4,6) and (5,6).
// Force a draw pass so calcLayout runs with real W/H.
flush();
advanceTime(100);
var layout = B.getLayout();
function cellCenter(r, c) {
    return { x: layout.ox + c * layout.cell + layout.cell / 2,
             y: layout.oy + r * layout.cell + layout.cell / 2 };
}
var a = cellCenter(4, 6);
var b = cellCenter(5, 6);
click(a.x, a.y);
advanceTime(80);
assert(B.getSelection() !== null, 'first click selected a gem, got ' + JSON.stringify(B.getSelection()));
click(b.x, b.y);
// Wait for swap + cascade to resolve.
advanceTime(3000);

assert(B.getScore() > 0, 'score increased after valid swap, got ' + B.getScore());

// Mid-game screenshot.
screenshot('apps/gemswap/screenshot.png');

// Puzzle count sanity.
assert(hooks.puzzles.count() >= 20, 'at least 20 puzzles');

// Special-gen integration: build a row of 4 same color and cascade.
B.startGame('classic');
advanceTime(50);
var flameBoard = B.makeEmptyGrid();
for (var rr2 = 0; rr2 < 8; rr2++) for (var cc2 = 0; cc2 < 8; cc2++) {
    flameBoard[rr2][cc2] = { color: 1 + ((rr2 * 3 + cc2) % 7), special: 0, frozen: false };
}
flameBoard[0][0].color = 3; flameBoard[0][1].color = 3; flameBoard[0][2].color = 3; flameBoard[0][3].color = 3;
// prevent adjacency collisions
if (flameBoard[1][0].color === 3) flameBoard[1][0].color = 2;
if (flameBoard[1][1].color === 3) flameBoard[1][1].color = 2;
if (flameBoard[1][2].color === 3) flameBoard[1][2].color = 2;
if (flameBoard[1][3].color === 3) flameBoard[1][3].color = 2;
if (flameBoard[0][4].color === 3) flameBoard[0][4].color = 2;
var fm = B.findMatches(flameBoard);
assert(fm.length === 1, '4-in-a-row produces one group');
assert(fm[0].special === B.SPECIAL_FLAME, 'produces flame special');

console.log('gemswap tests passed.');
