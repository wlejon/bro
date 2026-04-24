// test.js — headless harness for blockpop.
'use strict';

// Give app scripts a chance to boot.
advanceTime(200);

assert(typeof window.__blockpop === 'object' && window.__blockpop, '__blockpop exposed');
var hooks = window.__blockpop;
var B = hooks.board;
assert(B && typeof B.findChains === 'function', 'board exposes findChains');
assert(typeof B.popChains === 'function', 'board exposes popChains');
assert(typeof B.spawnRow === 'function', 'board exposes spawnRow');
assert(typeof B.settle === 'function', 'board exposes settle');

// -------- Title screenshot --------
screenshot('apps/blockpop/screenshot-title.png');

// -------- Unit tests: findChains / popChains ---------------------------

function mk(color, special) { return { color: color, special: special || 0 }; }

// Build an empty board, then stack columns by hand.
var eb = B.makeEmptyBoard();
// col 0..2 each have a single block of color 1 on top (bottom) — 3 adjacent tops.
eb[0].push(mk(1));
eb[1].push(mk(1));
eb[2].push(mk(1));
// col 3 has color 2 (different)
eb[3].push(mk(2));
var chains = B.findChains(eb);
assert(chains.length >= 1, 'found at least one chain, got ' + chains.length);
var total = 0;
for (var i = 0; i < chains.length; i++) total += chains[i].cells.length;
assert(total === 3, 'chain total is 3 cells, got ' + total);
assert(chains[0].color === 1, 'chain color is 1');

// popChains removes them.
var res = B.popChains(eb, chains);
assert(res.removed === 3, 'popChains removed 3 blocks, got ' + res.removed);
assert(eb[0].length === 0 && eb[1].length === 0 && eb[2].length === 0, 'columns emptied');
assert(eb[3].length === 1, 'col 3 untouched');

// Vertical chain within a single column: stack 3 of color 5 in col 0.
var eb2 = B.makeEmptyBoard();
eb2[0].push(mk(5)); eb2[0].push(mk(5)); eb2[0].push(mk(5));
var cc = B.findChains(eb2);
assert(cc.length === 1, 'single-column stack = 1 chain');
assert(cc[0].cells.length === 3, 'single-column chain has 3 cells');

// settle compacts null holes.
var eb3 = B.makeEmptyBoard();
eb3[0].push(mk(1)); eb3[0].push(null); eb3[0].push(mk(2));
B.settle(eb3);
assert(eb3[0].length === 2, 'settle removed the null');
assert(eb3[0][0].color === 1 && eb3[0][1].color === 2, 'settle preserved order');

// spawnRow prepends to every column.
var eb4 = B.makeEmptyBoard();
B.setSeed(42);
B.spawnRow(eb4, Math.random);
for (var c = 0; c < B.COLS; c++) assert(eb4[c].length === 1, 'spawnRow added a block to col ' + c);

// No starting matches in a seeded board (guard-loop scrubbing).
var seeded = B.seedBoard(5, B.makeRng(7));
assert(B.findChains(seeded).length === 0, 'seedBoard has no starting chains');

// Specials: star expands group to all matching color; bomb 3x3.
var eb5 = B.makeEmptyBoard();
// tops: col0=1*, col1=1, col2=1 → flood finds 3 tops; star at col0 should add
// any other color-1 blocks board-wide.
eb5[0].push(mk(1, B.SPECIAL_STAR));
eb5[1].push(mk(1));
eb5[2].push(mk(1));
eb5[4].push(mk(1)); // lonely color-1 far away — should be swept by star
var chains5 = B.findChains(eb5);
assert(chains5.length >= 1, 'star case produces a chain');
// expand specials on first group
var ex = B.expandSpecials(eb5, chains5[0]);
var keys = {};
for (var k = 0; k < ex.cells.length; k++) keys[ex.cells[k][0] + ',' + ex.cells[k][1]] = true;
assert(keys['4,0'] === true, 'star sweeps lonely color-1 at col 4');

// Bomb: single bomb cell pops its 3x3 (anything in range).
var eb6 = B.makeEmptyBoard();
for (var cx = 0; cx < 3; cx++) eb6[cx].push(mk(2));
eb6[0].push(mk(3, B.SPECIAL_BOMB)); // bomb is now the top of col 0
eb6[1].push(mk(3));
eb6[2].push(mk(3));
// group is {col0 top, col1 top, col2 top} of color 3
var g6 = B.findChains(eb6);
assert(g6.length >= 1, 'bomb group formed');
var ex6 = B.expandSpecials(eb6, g6[0]);
var popKeys = {};
for (var kk = 0; kk < ex6.cells.length; kk++) popKeys[ex6.cells[kk].join(',')] = true;
// Bomb at col0,row1 → 3x3 includes col1,row0 (the color-2 layer).
assert(popKeys['1,0'] === true, 'bomb reaches col1 row0');

// Rainbow counts as its own color (wildcard matches the chain color).
var eb7 = B.makeEmptyBoard();
eb7[0].push(mk(1));
eb7[1].push(mk(5, B.SPECIAL_RAINBOW));
eb7[2].push(mk(1));
var g7 = B.findChains(eb7);
assert(g7.length >= 1, 'rainbow joins color-1 chain');
assert(g7[0].cells.length >= 3, 'rainbow chain has 3+ cells, got ' + g7[0].cells.length);

// -------- Integration: start classic, navigate screens -----------------
B.startGame('classic');
advanceTime(100);
G.Screens.switchTo('playing');
advanceTime(100);

// -------- Seeded chain via pick-and-place ------------------------------
// Set up: cols 1 and 3 have color X on top. Col 2 has color X exposed after
// picking its current top. Move carrier to col 2, pick, move to col 3, place.
// After placement: col 3 top = X, col 1 top = X, col 2 top = X → 3 tops all X.
// But our findChains treats adjacency as columns next to each other, so we
// need cols 1,2,3 all with X on top. Simpler: make cols 1,2,3 each have X on
// top after the operation.
var craft = B.makeEmptyBoard();
// Fill 4 rows of varied non-matching colors.
function fillCol(col, arr) { for (var i = 0; i < arr.length; i++) col.push(mk(arr[i])); }
fillCol(craft[0], [5, 6, 7, 4]);
fillCol(craft[1], [5, 6, 7, 3]); // top = 3
fillCol(craft[2], [5, 6, 3, 7]); // top = 7, and below-top is 3
fillCol(craft[3], [5, 6, 7, 3]); // top = 3
fillCol(craft[4], [5, 6, 7, 4]);
fillCol(craft[5], [5, 6, 7, 2]);
fillCol(craft[6], [5, 6, 7, 4]);
fillCol(craft[7], [5, 6, 7, 2]);

// Sanity: no existing chain.
assert(B.findChains(craft).length === 0, 'crafted board has no starting chain, got ' + B.findChains(craft).length);

B.setBoard(craft);
B.setScore(0);
assert(B.getScore() === 0, 'score reset');

// Move to col 2 and pick (removes top=7). Now col 2 top = 3.
// cols 1,2,3 all now have 3 on top → chain fires immediately after? No,
// chains only fire in resolveChains which runs only after place(). Good.
B.moveTo(2);
assert(hooks.pick(), 'pick top of col 2');
// Carrier holds a color-7 block now. But the board already has cols 1,2,3
// with color-3 on top — we haven't resolved yet, so let's verify.
var afterPick = B.findChains(B.getBoard());
assert(afterPick.length >= 1, 'after picking, cols 1-2-3 tops align (color 3)');

// Move to col 4 (empty territory) and place there to trigger chain resolution.
// Actually placing will drop the color-7 onto col4, then resolveChains() runs
// and will pop the 3-chain on cols 1-2-3.
B.moveTo(4);
hooks.place();
advanceTime(50);

// The 3-chain of color 3 should have popped.
assert(B.getScore() > 0, 'score increased after chain popped, got ' + B.getScore());
var board2 = B.getBoard();
// col 1 had stack [5,6,7,3], losing top → length 3.
assert(board2[1].length === 3, 'col1 lost its top, got len ' + board2[1].length);
assert(board2[3].length === 3, 'col3 lost its top, got len ' + board2[3].length);

// -------- Chain multiplier cascade -------------------------------------
// Set up a board where one pop triggers another (chain depth 2).
// Stack: col 0 = [4,1], col 1 = [1,1], col 2 = [4,1]. Tops are all color 1
// → pops. After popping, cols 0,2 reveal color 4 as tops. Col 1 becomes empty.
// But for cascade we need 3 adjacent color-4 tops. Use cols 0,1,2 more
// carefully: col0=[4,1], col1=[4,1], col2=[4,1] → first pop tops (all 1),
// then reveal 4s → pops again (cascade depth 2).
B.startGame('classic');
advanceTime(50);
var casc = B.makeEmptyBoard();
fillCol(casc[0], [4, 1]);
fillCol(casc[1], [4, 1]);
fillCol(casc[2], [4, 1]);
// Other cols irrelevant but need to not match.
fillCol(casc[3], [6, 7]);
fillCol(casc[4], [6, 7]);
fillCol(casc[5], [6, 7]);
fillCol(casc[6], [6, 7]);
fillCol(casc[7], [6, 7]);
B.setBoard(casc);
B.setScore(0);
// Trigger resolveChains by placing a junk block somewhere innocuous.
// Easier: directly call resolveChains via the board API.
B.resolveChains();
advanceTime(50);

var stats = B.getStats();
assert(stats.bestChain >= 2, 'cascade depth reached 2+, got ' + stats.bestChain);
assert(B.getScore() >= 450, 'cascade score (150 + 150*2 = 450), got ' + B.getScore());

// -------- Top-out triggers GAME OVER -----------------------------------
// Fill every column to ROWS height → tick should report gameOver.
B.startGame('classic');
advanceTime(50);
var full = B.makeEmptyBoard();
for (var cc2 = 0; cc2 < B.COLS; cc2++) {
    for (var rr2 = 0; rr2 < B.ROWS; rr2++) {
        // Alternate colors to avoid accidental matches.
        full[cc2].push(mk(1 + ((cc2 + rr2) % B.NUM_COLORS)));
    }
}
B.setBoard(full);
// Tick once to trigger the top-out check (columns already at ROWS → gameOver).
B.tick(16);
assert(B.isGameOver(), 'top-out sets gameOver');

// -------- Play screenshot (classic mid-game) ---------------------------
B.startGame('classic');
advanceTime(100);
G.Screens.switchTo('playing');
advanceTime(300);
screenshot('apps/blockpop/screenshot-play.png');

console.log('blockpop tests passed.');
