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
function hasGreen(p) { return p.g > 150 && p.r < 150; }

// -----------------------------------------------------------------------
// Test 1: Crosshair hidden by default
// -----------------------------------------------------------------------
flush();
assertPixel(cx, cy, isBg, 'Test 1: center should be background with no crosshair');

// -----------------------------------------------------------------------
// Test 2: Show a basic green cross, no outline, no gap
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'cross',
    size: 20,
    thickness: 2,
    gap: 0,
    color: '#00ff00',
    opacity: 1.0,
    outline: false
});
bro.crosshair.show();
assert(bro.crosshair.visible === true, 'visible getter should be true');
flush();

// Center pixel should be green
assertPixel(cx, cy, isGreen, 'Test 2: center pixel should be green');

// Along the horizontal arm (right side)
assertPixel(cx + 10, cy, isGreen, 'Test 2: right arm at +10');
assertPixel(cx + 19, cy, isGreen, 'Test 2: right arm at +19 (near tip)');

// Beyond the arm
assertPixel(cx + 21, cy, isBg, 'Test 2: beyond right arm at +21');

// Along the vertical arm (top side, y decreases)
assertPixel(cx, cy - 10, isGreen, 'Test 2: top arm at -10');
assertPixel(cx, cy - 19, isGreen, 'Test 2: top arm at -19');

// Beyond top arm
assertPixel(cx, cy - 21, isBg, 'Test 2: beyond top arm at -21');

// Diagonal — should be background (no crosshair at 45 degrees from center)
assertPixel(cx + 10, cy + 10, isBg, 'Test 2: diagonal should be background');

// -----------------------------------------------------------------------
// Test 3: Cross with gap
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'cross',
    size: 20,
    thickness: 2,
    gap: 6,
    color: '#00ff00',
    opacity: 1.0,
    outline: false
});
flush();

// Center should be background (gap)
assertPixel(cx, cy, isBg, 'Test 3: center should be background (gap)');

// Just inside the gap — still background
assertPixel(cx + 5, cy, isBg, 'Test 3: inside gap at +5');

// Just past the gap — should be green
assertPixel(cx + 7, cy, isGreen, 'Test 3: past gap at +7 should be green');

// Arms still extend to size
assertPixel(cx + 19, cy, isGreen, 'Test 3: right arm at +19');
assertPixel(cx + 21, cy, isBg, 'Test 3: beyond arm at +21');

// -----------------------------------------------------------------------
// Test 4: Dot style
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'dot',
    dotSize: 4,
    color: '#ff0000',
    opacity: 1.0,
    outline: false
});
flush();

// Center should be red
var pc = getPixel(cx, cy);
assert(pc.r > 200 && pc.g < 50, 'Test 4: center should be red — got rgba(' + pc.r + ',' + pc.g + ',' + pc.b + ',' + pc.a + ')');

// Outside the dot
assertPixel(cx + 6, cy, isBg, 'Test 4: outside dot at +6');

// -----------------------------------------------------------------------
// Test 5: CrossDot style
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'crossdot',
    size: 15,
    thickness: 2,
    gap: 4,
    dotSize: 2,
    color: '#00ff00',
    opacity: 1.0,
    outline: false
});
flush();

// Center should be green (dot)
assertPixel(cx, cy, isGreen, 'Test 5: center should be green (dot)');

// Arm past gap
assertPixel(cx + 8, cy, isGreen, 'Test 5: arm past gap at +8');

// -----------------------------------------------------------------------
// Test 6: Circle style
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'circle',
    size: 10,
    thickness: 2,
    color: '#ffffff',
    opacity: 1.0,
    outline: false
});
flush();

// Center should be background (circle is hollow)
assertPixel(cx, cy, isBg, 'Test 6: circle center should be background');

// On the circle edge (right side, at radius=10) — white on white won't be distinguishable
// Use a colored circle instead for this test
bro.crosshair.configure({
    style: 'circle',
    size: 10,
    thickness: 2,
    color: '#ff0000',
    opacity: 1.0,
    outline: false
});
flush();

// On the circle edge
var pEdge = getPixel(cx + 10, cy);
assert(pEdge.r > 200 && pEdge.g < 100,
       'Test 6: circle edge should be red — got rgba(' + pEdge.r + ',' + pEdge.g + ',' + pEdge.b + ',' + pEdge.a + ')');

// Well outside the circle
assertPixel(cx + 15, cy, isBg, 'Test 6: outside circle at +15');

// -----------------------------------------------------------------------
// Test 7: Outline
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'cross',
    size: 20,
    thickness: 2,
    gap: 0,
    color: '#00ff00',
    opacity: 1.0,
    outline: true,
    outlineThickness: 2,
    outlineColor: '#ff0000'
});
flush();

// Center should still be green (fill on top of outline)
assertPixel(cx, cy, isGreen, 'Test 7: center with outline should be green');

// Arm body should be green
assertPixel(cx + 10, cy, isGreen, 'Test 7: arm body at +10');

// Check outline pixel: 2px above the arm center (arm is 1px above center, outline adds 2px)
// arm top edge is at cy - 1, outline extends to cy - 3
var pOut = getPixel(cx + 10, cy - 2);
assert(pOut.r > 150, 'Test 7: outline pixel should have red — got rgba(' + pOut.r + ',' + pOut.g + ',' + pOut.b + ',' + pOut.a + ')');

// -----------------------------------------------------------------------
// Test 8: Hide
// -----------------------------------------------------------------------
bro.crosshair.hide();
assert(bro.crosshair.visible === false, 'visible should be false after hide');
flush();

assertPixel(cx, cy, isBg, 'Test 8: center should be background after hide');

// -----------------------------------------------------------------------
// Test 9: Configure color via #RRGGBB
// -----------------------------------------------------------------------
bro.crosshair.configure({
    style: 'dot',
    dotSize: 4,
    color: '#0000ff',
    opacity: 1.0,
    outline: false
});
bro.crosshair.show();
flush();

var pBlue = getPixel(cx, cy);
assert(pBlue.b > 200 && pBlue.r < 50 && pBlue.g < 50,
       'Test 9: dot should be blue — got rgba(' + pBlue.r + ',' + pBlue.g + ',' + pBlue.b + ',' + pBlue.a + ')');

bro.crosshair.hide();
