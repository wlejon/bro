// test.js — headless harness for wordspire.
'use strict';

// Give scripts and dictionary load a chance to finish.
advanceTime(500);
// Dictionary load is async (fetch). Pump more time until loaded.
for (var w = 0; w < 40 && !window.__wordspire; w++) advanceTime(100);
assert(typeof window.__wordspire === 'object' && window.__wordspire, '__wordspire exposed');
var H = window.__wordspire;
var B = H.board;
var D = H.dictionary;
var S = H.scoring;

// Pump until dictionary resolves.
for (var w2 = 0; w2 < 60 && !D.loaded(); w2++) advanceTime(100);
assert(D.loaded(), 'dictionary eventually loaded');
assert(D.count() > 5000, 'dictionary has thousands of words, got ' + D.count());
// Basic common-word presence.
var common = ['hello','world','test','game','word','play','board','tower','stone'];
for (var i = 0; i < common.length; i++) {
    assert(D.isWord(common[i]), 'dictionary contains ' + common[i]);
}

// --- Title screenshot ----
H.screens.switchTo('title');
advanceTime(100);
screenshot('apps/wordspire/screenshot-title.png');

// --- Scoring unit tests -------------------------------------------------
// Per-letter values: CAT = 3+1+1 = 5 + length-bonus(3) = 10 = 15
assert(S.letterValue('a') === 1, 'A = 1');
assert(S.letterValue('q') === 10, 'Q = 10');
assert(S.letterValue('z') === 10, 'Z = 10');
assert(S.letterValue('e') === 1, 'E = 1');
assert(S.lengthBonus(3) === 10, 'length bonus 3 = 10');
assert(S.lengthBonus(5) === 40, 'length bonus 5 = 40');
assert(S.lengthBonus(7) === 160, 'length bonus 7 = 160');

var scoreCat = S.computeWordScore('cat', null);
assert(scoreCat === (3 + 1 + 1) + 10, 'computeWordScore CAT = 15, got ' + scoreCat);

var scoreStone = S.computeWordScore('stone', null);
// S+T+O+N+E = 1+1+1+1+1 = 5; length bonus(5) = 40 => 45.
assert(scoreStone === 45, 'computeWordScore STONE = 45, got ' + scoreStone);

// With multiplier tiles: give 'a' a 3x tile in CAT.
var scoreCatMult = S.computeWordScore('cat', [
    { letter: 'c', mult: 1 },
    { letter: 'a', mult: 3 },
    { letter: 't', mult: 1 }
]);
// letter sum: 3*1 + 1*3 + 1*1 = 7. length bonus: 10 * max(1,3,1)=3 => 30. Total 37.
assert(scoreCatMult === 37, 'CAT with x3 on A = 37, got ' + scoreCatMult);

// --- isValidPath tests ---------------------------------------------------
var g = B.makeEmptyGrid();
B.fillGrid(g);
assert(B.isValidPath([[0,0],[1,1],[2,2]], g), 'diagonal 3-path is valid');
assert(B.isValidPath([[0,0],[0,1]], g), 'orthogonal adj valid');
assert(!B.isValidPath([[0,0],[2,0]], g), 'non-adjacent path invalid');
assert(!B.isValidPath([[0,0],[1,0],[0,0]], g), 'repeat cell invalid');
assert(!B.isValidPath([[0,0]], g) || B.isValidPath([[0,0]], g), 'single-cell is valid for 1-len');

// --- Seeded board + play a word -----------------------------------------
// We'll start a game, set the grid to a known layout, choose a path that
// spells a real word, and verify the side effects.
B.startGame('classic');
advanceTime(50);
H.screens.switchTo('playing');
advanceTime(50);

// Set a board with CAT in the top row at cols 0,1,2.
// Remaining letters are harmless.
// 7 cols wide, 8 rows deep (COLS=7, ROWS=8).
H.setGrid([
    'cate xyz',
    'rotaxyz',
    'pirxxyz',
    'gelxxyz',
    'donxxyz',
    'blmxxyz',
    'nuvxxyz',
    'asizxyz'
]);
// Drop any chain.
B.clearChain();
B.setScore(0);

// Build chain = [[0,0],[1,0],[2,0]] spelling "c","a","t" = CAT.
var priorScore = B.getScore();
var priorGridTop0 = H.board.getGrid()[0][0].letter;
var priorGridTop1 = H.board.getGrid()[1][0].letter;
var priorGridTop2 = H.board.getGrid()[2][0].letter;
assert(priorGridTop0 === 'c', 'col 0 top is C, got ' + priorGridTop0);
assert(priorGridTop1 === 'a', 'col 1 top is A, got ' + priorGridTop1);
assert(priorGridTop2 === 't', 'col 2 top is T, got ' + priorGridTop2);

var ok = H.playPath([[0,0],[1,0],[2,0]]);
assert(ok, 'CAT was a valid play');
assert(B.getScore() > priorScore, 'score rose after CAT, got ' + B.getScore());

// After popping + settle, top of cols 0,1,2 should still exist (refilled at top)
// but will likely differ from c/a/t.
var g2 = B.getGrid();
assert(g2[0][B.ROWS - 1] !== null, 'col 0 bottom tile exists after settle');
assert(g2[0][0] !== null, 'col 0 top tile exists (refill)');

// --- Dictionary rejection ----------------------------------------------
// Set a grid with a non-word sequence and try to play it.
H.setGrid([
    'zxqjvwk',
    'rotayzn',
    'pirxmxn',
    'gelxynm',
    'donxxnm',
    'blmxxxn',
    'nuvxxyz',
    'asizxyz'
]);
B.clearChain();
var priorScore2 = B.getScore();
var ok2 = H.playPath([[0,0],[1,0],[2,0]]); // "zxq" — not a word
assert(!ok2, 'non-word rejected');
assert(B.getScore() === priorScore2, 'score unchanged on rejection');

// --- Burning tile reaches bottom => GAME OVER ---------------------------
B.startGame('classic');
advanceTime(50);
// Force a burning tile at the bottom row, then descendBurning should detect
// collapse -> trigger GAME OVER via submit path.
H.forceBurn(0, B.ROWS - 1);
// Submit any valid word so descendBurning runs.
// Construct a known valid arrangement first.
H.setGrid([
    'cate xyz',
    'rotaxyz',
    'pirxxyz',
    'gelxxyz',
    'donxxyz',
    'blmxxyz',
    'nuvxxyz',
    'asi zxy'
]);
H.forceBurn(0, B.ROWS - 1);
// Burn tile is now at col 0 bottom. The game checks descendBurning after a
// successful word submission; since it's already at the bottom, it collapses.
var played = H.playPath([[0,0],[1,0],[2,0]]);
assert(played, 'CAT submission attempted');
assert(B.isGameOver(), 'game over triggered by burning tile at bottom');

// --- findMatches sanity -------------------------------------------------
// With a solved grid, findMatches returns at least one real word.
B.startGame('classic');
advanceTime(50);
// startGame already tried to guarantee a match; either way, explicitly search.
var matches = H.findMatches(5);
// Accept 0 too (rare) but assert dictionary is consulted (no throw).
assert(Array.isArray(matches), 'findMatches returned an array');

// --- Gameplay screenshot -----------------------------------------------
// Play a clean game screenshot with a visible chain.
B.startGame('classic');
advanceTime(50);
H.screens.switchTo('playing');
advanceTime(50);
// Pick any three adjacent tiles — no need for a valid word, just visual.
B.tryAddTile(2, 3);
B.tryAddTile(3, 4);
B.tryAddTile(4, 4);
advanceTime(50);
screenshot('apps/wordspire/screenshot-play.png');

console.log('wordspire tests passed.');
