// Test engine-side two-finger gesture recognition (pinch / pan / rotate):
// gesturestart / gesturechange / gestureend from active touch contacts, with
// WebKit-style scale / rotation / clientX / clientY event properties.
// Exercises the gesture half of src/engine/touch_input.cpp via the headless
// touch injection seam. Regular pointer/touch events must keep firing
// untouched alongside.

const gestures = [];
const touchTypes = [];
for (const t of ['gesturestart', 'gesturechange', 'gestureend']) {
    document.addEventListener(t, (e) => gestures.push({
        type: e.type,
        scale: e.scale,
        rotation: e.rotation,
        clientX: e.clientX,
        clientY: e.clientY,
    }));
}
for (const t of ['touchstart', 'touchmove', 'touchend']) {
    document.addEventListener(t, () => touchTypes.push(t));
}

// =========================================================================
// One finger: no gesture
// =========================================================================

touchDown(1, 150, 150);
touchMove(1, 160, 150);
assert(gestures.length === 0, 'single finger produces no gesture events');

// =========================================================================
// Second finger lands: gesturestart at the centroid, scale 1, rotation 0
// =========================================================================

touchDown(2, 250, 150);   // fingers at (160,150) and (250,150)
assert(gestures.length === 1, 'gesturestart fired, got ' + gestures.length);
let g = gestures[0];
assert(g.type === 'gesturestart', 'first event is gesturestart');
assert(g.scale === 1, 'start scale is 1');
assert(g.rotation === 0, 'start rotation is 0');
assert(Math.abs(g.clientX - 205) < 1e-3, 'centroid x = 205, got ' + g.clientX);
assert(Math.abs(g.clientY - 150) < 1e-3, 'centroid y = 150, got ' + g.clientY);

// =========================================================================
// Pinch out: scale grows monotonically, distance-ratio exact
// =========================================================================

// Start distance is 90 px (160 -> 250 at same y).
const startDist = 90;
let prevScale = 1;
for (const x of [280, 310, 340]) {
    touchMove(2, x, 150);
    g = gestures[gestures.length - 1];
    assert(g.type === 'gesturechange', 'move fired gesturechange');
    const expected = (x - 160) / startDist;
    assert(Math.abs(g.scale - expected) < 1e-3,
           'scale = dist/startDist (' + expected + '), got ' + g.scale);
    assert(g.scale > prevScale, 'pinch-out scale increases monotonically');
    assert(Math.abs(g.rotation) < 1e-3, 'pure pinch keeps rotation 0');
    prevScale = g.scale;
}

// Pinch back in: scale falls below 1 when closer than at start.
touchMove(2, 205, 150);   // dist 45 = half the start distance
g = gestures[gestures.length - 1];
assert(Math.abs(g.scale - 0.5) < 1e-3, 'pinch-in scale 0.5, got ' + g.scale);

// =========================================================================
// Rotate: moving finger 2 below finger 1 is clockwise = positive degrees
// =========================================================================

// Reset geometry: finger 1 at (160,150), finger 2 at (260,150) — angle 0.
touchMove(2, 260, 150);
const base = gestures.length;

// Finger 2 swings down to (160, 250): vector (0, +100) -> +90 deg clockwise.
touchMove(2, 160, 250);
g = gestures[gestures.length - 1];
assert(gestures.length === base + 1, 'rotation move fired one gesturechange');
assert(Math.abs(g.rotation - 90) < 1e-3, 'clockwise quarter turn is +90, got ' + g.rotation);
assert(Math.abs(g.scale - 100 / startDist) < 1e-3, 'scale tracks distance during rotation');

// Counterclockwise past the start: up to (160, 50) -> vector (0, -100) = -90.
touchMove(2, 160, 50);
g = gestures[gestures.length - 1];
assert(Math.abs(g.rotation + 90) < 1e-3, 'counterclockwise is negative, got ' + g.rotation);

// Centroid tracks the fingers.
assert(Math.abs(g.clientX - 160) < 1e-3 && Math.abs(g.clientY - 100) < 1e-3,
       'centroid follows: (160,100), got (' + g.clientX + ',' + g.clientY + ')');

// =========================================================================
// A third finger is ignored; lifting a founder ends + re-bases the gesture
// =========================================================================

const before3 = gestures.length;
touchDown(3, 400, 300);
assert(gestures.length === before3, 'third finger fires no gesture event');
touchMove(3, 420, 300);
assert(gestures.length === before3, 'third-finger moves are ignored');

// Lift founding finger 2: gestureend (final values), then a NEW gesturestart
// over fingers 1+3, re-based to scale 1 / rotation 0.
touchUp(2, 160, 50);
assert(gestures.length === before3 + 2, 'gestureend + fresh gesturestart');
const endEvt = gestures[before3];
assert(endEvt.type === 'gestureend', 'end fired on founder lift');
assert(Math.abs(endEvt.rotation + 90) < 1e-3, 'gestureend carries final rotation');
const restart = gestures[before3 + 1];
assert(restart.type === 'gesturestart', 'remaining 2 fingers restart a gesture');
assert(restart.scale === 1 && restart.rotation === 0, 'restart re-based to 1/0');

// Lift everything: one more gestureend, then silence.
touchUp(3, 420, 300);
g = gestures[gestures.length - 1];
assert(g.type === 'gestureend', 'last founder lift ends the gesture');
const total = gestures.length;
touchUp(1, 160, 150);
assert(gestures.length === total, 'last single finger lift fires nothing');

// =========================================================================
// Touch stream untouched: every contact still fired its touch events
// =========================================================================

assert(touchTypes.filter((t) => t === 'touchstart').length === 3,
       'all three contacts fired touchstart');
assert(touchTypes.filter((t) => t === 'touchend').length === 3,
       'all three contacts fired touchend');
assert(touchTypes.indexOf('touchmove') !== -1, 'touchmove stream intact');

// =========================================================================
// touchcancel of a founder also ends the gesture
// =========================================================================

const beforeCancel = gestures.length;
touchDown(10, 100, 100);
touchDown(11, 200, 100);
assert(gestures.length === beforeCancel + 1 &&
       gestures[gestures.length - 1].type === 'gesturestart',
       'fresh pair starts a gesture');
touchCancel(10, 100, 100);
assert(gestures[gestures.length - 1].type === 'gestureend',
       'cancelling a founder fires gestureend');
touchUp(11, 200, 100);
