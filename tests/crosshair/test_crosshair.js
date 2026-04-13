// tests/crosshair/test_crosshair.js
// Pixel-level validation of the crosshair rendering system.
// Runs via: bro-headless --no-gpu tests/test_app test_crosshair.js

var W = 1920, H = 1080;
var cx = Math.floor(W / 2);  // 960
var cy = Math.floor(H / 2);  // 540

// Background is white (255,255,255,255) for the test app
function isBg(p) { return p.r === 255 && p.g === 255 && p.b === 255; }

// Helper: assert pixel color at (x, y)
function assertPixel(x, y, check, msg) {
    var p = getPixel(x, y);
    assert(check(p), msg + ' — got rgba(' + p.r + ',' + p.g + ',' + p.b + ',' + p.a + ') at (' + x + ',' + y + ')');
}

function isGreen(p) { return p.g > 200 && p.r < 100 && p.b < 100; }

// -----------------------------------------------------------------------
// Test 1: Crosshair hidden by default
// -----------------------------------------------------------------------
flush();
assertPixel(cx, cy, isBg, 'Test 1: center should be background with no crosshair');

// -----------------------------------------------------------------------
// Test 2: Show a basic green cross, no spread, no outline
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'cross',
    size: 20,
    thickness: 2,
    spread: 0,
    color: '#00ff00',
    opacity: 1.0,
    outline: false
});
bro.crosshair.show();
assert(bro.crosshair.visible === true, 'visible getter should be true');
flush();

// Center pixel should be green (spread=0 means arms meet at center)
assertPixel(cx, cy, isGreen, 'Test 2: center pixel should be green');

// Along the horizontal arm (right side)
assertPixel(cx + 10, cy, isGreen, 'Test 2: right arm at +10');
assertPixel(cx + 19, cy, isGreen, 'Test 2: right arm at +19 (near tip)');

// Beyond the arm
assertPixel(cx + 21, cy, isBg, 'Test 2: beyond right arm at +21');

// Along the vertical arm
assertPixel(cx, cy - 10, isGreen, 'Test 2: top arm at -10');
assertPixel(cx, cy - 19, isGreen, 'Test 2: top arm at -19');
assertPixel(cx, cy - 21, isBg, 'Test 2: beyond top arm at -21');

// Diagonal — should be background
assertPixel(cx + 10, cy + 10, isBg, 'Test 2: diagonal should be background');

// -----------------------------------------------------------------------
// Test 3: Cross with spread (gap)
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'cross',
    size: 20,
    thickness: 2,
    spread: 6,
    color: '#00ff00',
    opacity: 1.0,
    outline: false
});
flush();

// Center should be background (gap from spread)
assertPixel(cx, cy, isBg, 'Test 3: center should be background (spread gap)');
assertPixel(cx + 5, cy, isBg, 'Test 3: inside spread at +5');
assertPixel(cx + 7, cy, isGreen, 'Test 3: past spread at +7 should be green');
assertPixel(cx + 19, cy, isGreen, 'Test 3: right arm at +19');
assertPixel(cx + 21, cy, isBg, 'Test 3: beyond arm at +21');

// -----------------------------------------------------------------------
// Test 4: "gap" alias works for "spread"
// -----------------------------------------------------------------------
bro.crosshair.configure({ gap: 10 });
flush();
assertPixel(cx + 9, cy, isBg, 'Test 4: gap alias — inside at +9');
assertPixel(cx + 11, cy, isGreen, 'Test 4: gap alias — past gap at +11');

// -----------------------------------------------------------------------
// Test 5: Dot style
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'dot',
    dotSize: 4,
    color: '#ff0000',
    opacity: 1.0,
    outline: false
});
flush();

var pc = getPixel(cx, cy);
assert(pc.r > 200 && pc.g < 50, 'Test 5: center should be red');
assertPixel(cx + 6, cy, isBg, 'Test 5: outside dot at +6');

// -----------------------------------------------------------------------
// Test 6: CrossDot style
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'crossdot',
    size: 15,
    thickness: 2,
    spread: 4,
    dotSize: 2,
    color: '#00ff00',
    opacity: 1.0,
    outline: false
});
flush();

assertPixel(cx, cy, isGreen, 'Test 6: center should be green (dot)');
assertPixel(cx + 8, cy, isGreen, 'Test 6: arm past spread at +8');

// -----------------------------------------------------------------------
// Test 7: Circle style
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'circle',
    size: 10,
    thickness: 2,
    color: '#ff0000',
    opacity: 1.0,
    outline: false
});
flush();

assertPixel(cx, cy, isBg, 'Test 7: circle center should be background');
var pEdge = getPixel(cx + 10, cy);
assert(pEdge.r > 200 && pEdge.g < 100,
       'Test 7: circle edge should be red — got rgba(' + pEdge.r + ',' + pEdge.g + ',' + pEdge.b + ',' + pEdge.a + ')');
assertPixel(cx + 15, cy, isBg, 'Test 7: outside circle at +15');

// -----------------------------------------------------------------------
// Test 8: Outline
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'cross',
    size: 20,
    thickness: 2,
    spread: 0,
    color: '#00ff00',
    opacity: 1.0,
    outline: true,
    outlineThickness: 2,
    outlineColor: '#ff0000'
});
flush();

assertPixel(cx, cy, isGreen, 'Test 8: center with outline should be green');
assertPixel(cx + 10, cy, isGreen, 'Test 8: arm body at +10');
var pOut = getPixel(cx + 10, cy - 2);
assert(pOut.r > 150, 'Test 8: outline pixel should have red');

// -----------------------------------------------------------------------
// Test 9: Hide
// -----------------------------------------------------------------------
bro.crosshair.hide();
assert(bro.crosshair.visible === false, 'visible should be false after hide');
flush();
assertPixel(cx, cy, isBg, 'Test 9: center should be background after hide');

// -----------------------------------------------------------------------
// Test 10: Color via #RRGGBB
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'dot', dotSize: 4,
    color: '#0000ff', opacity: 1.0, outline: false
});
bro.crosshair.show();
flush();
var pBlue = getPixel(cx, cy);
assert(pBlue.b > 200 && pBlue.r < 50 && pBlue.g < 50,
       'Test 10: dot should be blue');

// -----------------------------------------------------------------------
// Test 11: Spread system — setMoving increases spread
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'cross', size: 30, thickness: 2,
    spread: 4, moveSpread: 16,
    color: '#00ff00', opacity: 1.0, outline: false,
    lerpSpeed: 100  // fast lerp for testing
});
flush();

// Idle: spread=4, so pixel at +5 should be green (arm starts at spread=4)
assertPixel(cx + 5, cy, isGreen, 'Test 11: idle arm at +5');

// Start moving
bro.crosshair.setMoving(true);
advanceTime(500);  // let spread lerp to 4+16=20

// At spread ~20, pixel at +5 should be background (inside gap)
assertPixel(cx + 5, cy, isBg, 'Test 11: moving — +5 should be inside gap');
// Arm should be past spread
assertPixel(cx + 22, cy, isGreen, 'Test 11: moving — arm at +22');

// Stop moving — spread should recover
bro.crosshair.setMoving(false);
advanceTime(500);

assertPixel(cx + 5, cy, isGreen, 'Test 11: stopped — arm at +5 should be green again');

// -----------------------------------------------------------------------
// Test 12: Spread system — addBloom
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'cross', size: 40, thickness: 2,
    spread: 4, fireBloom: 20, bloomDecay: 40,
    color: '#00ff00', opacity: 1.0, outline: false,
    lerpSpeed: 100
});
flush();

// Before bloom: spread ~4
assertPixel(cx + 5, cy, isGreen, 'Test 12: pre-bloom arm at +5');

// Fire!
bro.crosshair.addBloom();
advanceTime(50);  // small step to apply bloom

// Spread should be high (~24), so +5 should be inside gap
assertPixel(cx + 5, cy, isBg, 'Test 12: post-bloom +5 should be gap');

// Wait for bloom to decay
advanceTime(1000);
assertPixel(cx + 5, cy, isGreen, 'Test 12: bloom decayed — arm at +5');

// -----------------------------------------------------------------------
// Test 13: Spread system — ADS tightens spread
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'cross', size: 20, thickness: 2,
    spread: 8, adsSpread: 1,
    color: '#00ff00', opacity: 1.0, outline: false,
    lerpSpeed: 100
});
flush();

// Idle: spread ~8
assertPixel(cx + 7, cy, isBg, 'Test 13: idle — +7 inside spread');

// ADS
bro.crosshair.setAds(true);
advanceTime(500);

// spread ~1, so +2 should be green
assertPixel(cx + 2, cy, isGreen, 'Test 13: ADS — +2 should be arm');

bro.crosshair.setAds(false);
advanceTime(500);

// -----------------------------------------------------------------------
// Test 14: currentSpread getter
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'cross', size: 20, thickness: 2,
    spread: 10, lerpSpeed: 100,
    color: '#00ff00', opacity: 1.0, outline: false
});
flush();
advanceTime(500);

var cs = bro.crosshair.currentSpread;
assert(Math.abs(cs - 10) < 1, 'Test 14: currentSpread should be ~10, got ' + cs);

// -----------------------------------------------------------------------
// Test 15: Manual override via setSpread
// -----------------------------------------------------------------------
bro.crosshair.setSpread(0);
advanceTime(100);

assertPixel(cx, cy, isGreen, 'Test 15: manual spread=0, center green');

// Return to auto
bro.crosshair.autoSpread();
advanceTime(500);

var cs2 = bro.crosshair.currentSpread;
assert(Math.abs(cs2 - 10) < 1, 'Test 15: auto spread should return to ~10');

// Cleanup
bro.crosshair.hide();
