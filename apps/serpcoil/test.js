// test.js — headless harness for Serpcoil.
'use strict';

// Let the app boot.
advanceTime(300);

assert(typeof window.__serpcoil === 'object' && window.__serpcoil, '__serpcoil exposed');
var H = window.__serpcoil;

// Title screenshot.
screenshot('apps/serpcoil/screenshot-title.png');

assert(H.currentScreen() === 'title', 'starts on title, got ' + H.currentScreen());

// Navigate to level select via arrow-down then Enter.
// (title menu index 0 = PLAY; index 1 = LEVEL SELECT)
// We'll just click "PLAY" to seed level 1.
keyDown(13); // not ideal — use the JS API directly
// Just start level 0 directly via seedLevel hook.
H.seedLevel(0, 12345);
H.switchTo('play');
advanceTime(100);

// Prevent chain from reaching goal during the test (slow, but moving).
H.setChainSpeed(5);

var chain = H.chain();
var path = H.path();
assert(path.length() > 0, 'path has length');
assert(chain, 'chain created');

// Let a few orbs spawn.
advanceTime(2500);
var orbCount = chain.count();
assert(orbCount >= 3, 'at least 3 orbs spawned (got ' + orbCount + ')');

// -------- Pure match detection --------
// Seed deterministic chain: force colors of orbs and insert a matching one.
var orbs = chain.orbs();
var ORB_DIAM = chain.ORB_DIAM;
// Set first three orbs same color.
orbs[0].color = 1; orbs[1].color = 1; orbs[2].color = 1;
// Ensure there are at least 4 orbs so we can detect properly.
var matches0 = H.detectMatches(0);
assert(matches0.length === 1, 'detect one match after seeding, got ' + matches0.length);
assert(matches0[0][1] - matches0[0][0] >= 3, 'match is 3+');

// Reset colors to something that won't match and insert a matching color.
for (var i = 0; i < orbs.length; i++) orbs[i].color = (i % 2) + 1;
// Now force positions: orbs at d = 80, 110, 140 same color.
// First clear chain and build a known state manually.
orbs.length = 0;
orbs.push({ color: 1, d: 100, phase: 0 });
orbs.push({ color: 1, d: 130, phase: 0 });
orbs.push({ color: 2, d: 160, phase: 0 });
orbs.push({ color: 3, d: 190, phase: 0 });

var scoreBefore = H.score();
// Insert a color 1 orb between index 1 and 2 — should make 3 in a row.
H.insertAt(145, 1);
advanceTime(50);

var scoreAfter = H.score();
assert(scoreAfter > scoreBefore, 'score advanced after match (before=' + scoreBefore + ' after=' + scoreAfter + ')');
// The three color-1 orbs should be popped.
var c1Left = 0;
for (var j = 0; j < orbs.length; j++) if (orbs[j].color === 1) c1Left++;
assert(c1Left === 0, 'all matching orbs popped (left=' + c1Left + ')');

// -------- Combo escalation --------
// Build a cascading chain: color pattern [A A B A A] with an inserted A
// that breaks up the B, then cascade. We craft it so inserting color A
// in the middle triggers two pops.
orbs.length = 0;
// After insert: A A A A A? Not great. Use: [A A][X][A A] where removing
// the X (via a match around a fresh insert) would bring the two A-A pairs
// together. Using colorshift is cleaner, but we can do it with a direct
// pattern: [A A A B B B] where inserting A into the AAA triggers a pop,
// then the BBB are still there unmatched. For cascade, we need removal
// to bring same-colors together.
// Pattern: [1 1 2 2 2 1] — insert a 2 into the 222 run? That already
// matches. Better: [1 1 2 1 1] — insert a 2 into the 2 (makes 22 only).
// We need: [1 1 X 2 2 2 X 1 1] where X is the hit region. Removing the
// 222 in the middle joins 1 1 with 1 1 = 1111 → second pop.
orbs.push({ color: 1, d: 100, phase: 0 });
orbs.push({ color: 1, d: 130, phase: 0 });
orbs.push({ color: 2, d: 160, phase: 0 });
orbs.push({ color: 2, d: 190, phase: 0 });
orbs.push({ color: 1, d: 220, phase: 0 });
orbs.push({ color: 1, d: 250, phase: 0 });

H.setScore(0);
// Insert a color 2 so middle becomes 2 2 2 — pops, then the 1s join.
var idx = H.insertAt(175, 2);
advanceTime(50);

var cascScore = H.score();
assert(cascScore > 0, 'cascade produced score (' + cascScore + ')');
// After cascade: the 1-1-1-1 should have popped too. Very few orbs left.
assert(orbs.length <= 1, 'cascade removed most orbs, left=' + orbs.length);
// Combo tracking recorded depth >= 2 at some point.
var comboHit = H.combo();
assert(comboHit >= 2, 'combo escalated to >=2, got ' + comboHit);

// -------- Danger state --------
// Advance head past the danger threshold.
orbs.length = 0;
orbs.push({ color: 3, d: path.length() * 0.9, phase: 0 });
orbs.push({ color: 3, d: path.length() * 0.85, phase: 0 });
// Swap so "head" (last) is at 0.9.
orbs.sort(function (a, b) { return a.d - b.d; });
advanceTime(50);
assert(H.danger() === true, 'danger state is active near goal');

// Clear danger by removing those orbs.
orbs.length = 0;
advanceTime(50);
assert(H.danger() === false, 'danger clears when chain empty');

// -------- Level clear transition --------
// Force all orbs gone AND spawn count maxed.
H.chain().forceEmpty();
advanceTime(100);
assert(H.currentScreen() === 'levelclear', 'screen transitioned to levelclear, got ' + H.currentScreen());

// -------- Play-state screenshot: restart and let it render --------
H.seedLevel(2, 98765);
H.switchTo('play');
advanceTime(600);
H.setChainSpeed(10);
advanceTime(1200);
screenshot('apps/serpcoil/screenshot-play.png');

// -------- Powerup activation smoke test --------
var S = H.shooter();
// Manually apply a BLASTER to current slot
S.setCurrent(S.current(), 2); // 2 = PU_BLASTER
// fire straight at center of chain
var testOrbs = H.chain().orbs();
if (testOrbs.length > 0) {
    var p = H.path().pointAt(testOrbs[0].d);
    S.aimAt(p.x, p.y);
    H.fire();
    advanceTime(400);
    // Can't assert anything strong here (projectile may miss), but no crash.
}

console.log('Serpcoil tests passed.');
