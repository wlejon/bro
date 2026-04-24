// test.js — headless harness for fintank.
'use strict';

// Give scripts a chance to initialize.
advanceTime(300);
for (var w = 0; w < 40 && !window.__fintank; w++) advanceTime(100);
assert(typeof window.__fintank === 'object' && window.__fintank, '__fintank exposed');

var H = window.__fintank;
var G = H.game;
var S = H.screens;
var Eco = H.economy;

// Wipe any persisted slot 1 so test is deterministic.
try { localStorage.removeItem('fintank:slot1'); } catch (e) {}

// ---- Title screenshot ----
S.switchTo('title');
advanceTime(200);
screenshot('apps/fintank/screenshot-title.png');

// ---- Enter slot 1 -> shop ----
G.startGame(1);
S.switchTo('shop');
advanceTime(150);

// Start the day (enterPlayScreen + switch to playing)
G.enterPlayScreen();
S.switchTo('playing');
advanceTime(150);

// ---- Buy fish with addCoins + buy ----
H.addCoins(500);
var st = G._state();
var before = st.fish.length;
var res = H.buy('fish_tier1');
assert(res && res.ok, 'fish_tier1 purchase ok (reason=' + (res && res.reason) + ', coins=' + st.slot.coins + ', slotFish=' + st.slot.fish.length + ')');
var stAfter = G._state();
assert(stAfter.fish.length === before + 1, 'fish inventory grew (before=' + before + ', after=' + stAfter.fish.length + ')');

// ---- Feed + ensure a fish eats ----
H.feed(600);
var s1 = G._state();
var fishIdx = 0;
// Find closest fish to x=600 to shorten wait
var best = 0, bd = 99999;
for (var fi = 0; fi < s1.fish.length; fi++) {
    var d = Math.abs(s1.fish[fi].x - 600);
    if (d < bd) { bd = d; best = fi; }
}
fishIdx = best;
// Give the fish low hunger to guarantee seeking.
s1.fish[fishIdx].hunger = 40;
// Pump time until pellets consumed or timeout
var atePellet = false;
for (var pp = 0; pp < 80; pp++) {
    advanceTime(100);
    // Use the game's current state to see if any fish has hasFood==true
    var s = G._state();
    var anyHas = false;
    for (var ii = 0; ii < s.fish.length; ii++) if (s.fish[ii].hasFood) { anyHas = true; break; }
    if (anyHas) { atePellet = true; break; }
    if (s.pellets.length === 0) break;
}
assert(atePellet, 'a fish ate a pellet (hasFood set)');

// Force a fish to drop a coin quickly and test collection.
H.feedFish(fishIdx);
var s2 = G._state();
var priorCoins = s2.slot.coins;
// Pump until a coin appears
var coinAppeared = false;
for (var cc = 0; cc < 40; cc++) {
    advanceTime(100);
    var s = G._state();
    if (s.coins.length > 0) { coinAppeared = true; break; }
}
assert(coinAppeared, 'a coin dropped after fish was fed');
// Collect all coins via hook
H.collectAllCoins();
advanceTime(50);
var s3 = G._state();
assert(s3.slot.coins > priorCoins, 'coin balance increased (before=' + priorCoins + ', after=' + s3.slot.coins + ')');

// ---- Intruder spawn + kill ----
H.spawnIntruder('snatcher');
advanceTime(50);
var s4 = G._state();
assert(s4.intruders.length >= 1, 'intruder spawned');
H.killAllIntruders();
// Pump time for dying animation to despawn
for (var di = 0; di < 20; di++) advanceTime(100);
var s5 = G._state();
var aliveIntruders = 0;
for (var ai = 0; ai < s5.intruders.length; ai++) if (!s5.intruders[ai].dead) aliveIntruders++;
assert(aliveIntruders === 0, 'intruders cleared (remaining alive=' + aliveIntruders + ')');

// ---- Pet egg purchase ----
H.addCoins(5000);
var petRes = H.buy('pet_bubbler');
assert(petRes && petRes.ok, 'pet egg purchase ok');
var s6 = G._state();
assert(s6.pet && s6.pet.id === 'bubbler', 'pet bubbler active (got ' + (s6.pet && s6.pet.id) + ')');

// ---- End day transition ----
H.endDay();
advanceTime(50);
assert(G.status() === 'dayclear', 'status is dayclear (got ' + G.status() + ')');
S.switchTo('dayclear');
advanceTime(100);
var stats = G.dayStats();
assert(typeof stats.day === 'number', 'day stats populated');
assert(stats.day === 1, 'day stats day = 1 (got ' + stats.day + ')');

// ---- Gameplay screenshot ----
// Reset to a fresh, lively scene for a better screenshot.
G.advanceToNextDay();
S.switchTo('playing');
advanceTime(150);
// Add some visual flavor.
H.feed(400); H.feed(600); H.feed(800);
H.spawnIntruder('snatcher');
H.spawnIntruder('swarmer');
advanceTime(500);
screenshot('apps/fintank/screenshot-play.png');

console.log('Fintank tests passed.');
