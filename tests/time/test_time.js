// Test bro.time — global pause + timescale (src/js/time_bindings.cpp,
// scaled clock in src/engine/engine_frame.cpp / headless_api.cpp).
//
// Headless composition: advanceTime(ms) advances the scaled clock by
// ms * bro.time.scale — and not at all while paused — while virtual time
// (system panels, audio pump, brokit) advances by the full ms.

// =========================================================================
// Defaults — must be exactly scale 1 / unpaused so the rest of the suite
// sees unscaled time.
// =========================================================================
assert(typeof bro.time === 'object', 'bro.time exists');
assert(bro.time.scale === 1, 'default scale is 1, got ' + bro.time.scale);
assert(bro.time.paused === false, 'default paused is false');
assert(typeof bro.time.now === 'number' && bro.time.now > 0, 'now is a positive number');

// now is read-only (silent no-op, non-strict code)
const nowBefore = bro.time.now;
try { bro.time.now = 12345; } catch (e) { /* strict-mode TypeError also fine */ }
assert(bro.time.now === nowBefore, 'now is read-only');

// Clamping
bro.time.scale = -5;
assert(bro.time.scale === 0, 'negative scale clamps to 0');
bro.time.scale = 1000;
assert(bro.time.scale === 100, 'huge scale clamps to 100');
bro.time.scale = 1;

// =========================================================================
// scale 0.5 halves timer + performance.now + bro.time.now progression
// =========================================================================
bro.time.scale = 0.5;

let halfFired = false;
setTimeout(() => { halfFired = true; }, 100);

const perf0 = performance.now();
const scaled0 = bro.time.now;

advanceTime(150);           // scaled: 75ms — timer must NOT fire
assert(!halfFired, 'scale 0.5: 100ms timer has not fired after advanceTime(150)');
assert(Math.abs((performance.now() - perf0) - 75) < 1,
       'scale 0.5: performance.now advanced 75ms, got ' + (performance.now() - perf0));
assert(Math.abs((bro.time.now - scaled0) - 75) < 1,
       'scale 0.5: bro.time.now advanced 75ms');

advanceTime(60);            // scaled total: 105ms — now it fires
assert(halfFired, 'scale 0.5: 100ms timer fired once scaled time passed 100ms');

// rAF timestamp progression is scaled too (cadence is per-frame, the
// timestamp follows the scaled clock)
let ts1 = -1, ts2 = -1;
requestAnimationFrame((t) => { ts1 = t; });
advanceTime(16);            // scaled +8
requestAnimationFrame((t) => { ts2 = t; });
advanceTime(16);            // scaled +8
assert(ts1 > 0 && ts2 > 0, 'rAF callbacks fired under scale 0.5');
assert(Math.abs((ts2 - ts1) - 8) < 1,
       'scale 0.5: rAF timestamps advanced 8ms per 16ms step, got ' + (ts2 - ts1));

bro.time.scale = 1;

// =========================================================================
// scale 2 doubles progression
// =========================================================================
bro.time.scale = 2;
let dblFired = false;
setTimeout(() => { dblFired = true; }, 100);
advanceTime(60);            // scaled: 120ms
assert(dblFired, 'scale 2: 100ms timer fired within advanceTime(60)');
bro.time.scale = 1;

// =========================================================================
// Pause freezes setTimeout / rAF / performance.now / bro.time.now
// =========================================================================
let pausedTimerFired = false;
let pausedRafFired = false;
setTimeout(() => { pausedTimerFired = true; }, 10);

bro.time.paused = true;
assert(bro.time.paused === true, 'paused reads back true');
// scale stays readable/settable while paused; effective scale is 0
assert(bro.time.scale === 1, 'scale unchanged by pause');

requestAnimationFrame(() => { pausedRafFired = true; });
const perfPaused = performance.now();
const nowPaused = bro.time.now;

advanceTime(500);
assert(!pausedTimerFired, 'paused: 10ms timer frozen through advanceTime(500)');
assert(!pausedRafFired, 'paused: rAF callback not fired');
assert(performance.now() === perfPaused, 'paused: performance.now frozen');
assert(bro.time.now === nowPaused, 'paused: bro.time.now frozen');

bro.time.paused = false;
advanceTime(20);
assert(pausedTimerFired, 'unpaused: frozen timer fires once time resumes');
assert(pausedRafFired, 'unpaused: queued rAF fires on the next frame');

// =========================================================================
// Pause freezes a falling physics body; unpause resumes the fall
// =========================================================================
Physics.destroyAll();
Physics.setGravity(0, -9.81, 0);
Physics.setTimeStep(1 / 60);

const ball = Physics.createBody({
    shape: 'sphere', radius: 0.5, position: { x: 0, y: 50, z: 0 },
});
assert(ball > 0, 'falling body created');

advanceTime(100);           // let it start falling
const yFalling = Physics.getTransform(ball).position.y;
assert(yFalling < 50, 'body is falling before pause, y=' + yFalling);

bro.time.paused = true;
const yPaused = Physics.getTransform(ball).position.y;
advanceTime(500);
const yAfterPause = Physics.getTransform(ball).position.y;
assert(yAfterPause === yPaused,
       'paused: body did not move (y ' + yPaused + ' -> ' + yAfterPause + ')');

bro.time.paused = false;
advanceTime(500);
const yResumed = Physics.getTransform(ball).position.y;
assert(yResumed < yAfterPause, 'unpaused: body resumes falling, y=' + yResumed);

// Timescale scales sim speed: two identical drops, one at scale 2, must
// fall further in the same advanceTime span.
Physics.destroyAll();
const slow = Physics.createBody({ shape: 'sphere', radius: 0.5, position: { x: 0, y: 100, z: 0 } });
advanceTime(200);
const dropAt1 = 100 - Physics.getTransform(slow).position.y;

Physics.destroyAll();
bro.time.scale = 2;
const fast = Physics.createBody({ shape: 'sphere', radius: 0.5, position: { x: 0, y: 100, z: 0 } });
advanceTime(200);
const dropAt2 = 100 - Physics.getTransform(fast).position.y;
bro.time.scale = 1;
Physics.destroyAll();

assert(dropAt1 > 0, 'scale 1 drop fell (' + dropAt1 + ')');
assert(dropAt2 > dropAt1 * 2, 'scale 2 falls >2x as far in the same span: ' +
       dropAt2 + ' vs ' + dropAt1 + ' (gravity is quadratic in sim time)');

// =========================================================================
// CSS transition obeys the scale
// =========================================================================
const root = document.getElementById('root');
root.innerHTML = '';
const box = document.createElement('div');
box.style.cssText =
    'width:100px;height:100px;opacity:1;transition:opacity 500ms linear;';
root.appendChild(box);
flush();

bro.time.scale = 0.5;
box.style.opacity = '0';
flush();

advanceTime(500);           // scaled: 250ms = halfway through a 500ms linear fade
const opMid = parseFloat(getComputedStyle(box).opacity);
assert(Math.abs(opMid - 0.5) < 0.1,
       'scale 0.5: opacity ~0.5 halfway through, got ' + opMid);

// Pause mid-transition: value holds
bro.time.paused = true;
advanceTime(1000);
const opHeld = parseFloat(getComputedStyle(box).opacity);
assert(Math.abs(opHeld - opMid) < 0.02,
       'paused: transition value held (' + opMid + ' -> ' + opHeld + ')');
bro.time.paused = false;

// Back to scale 1 for the remainder — completes after 250 more scaled ms
bro.time.scale = 1;
advanceTime(400);
const opEnd = parseFloat(getComputedStyle(box).opacity);
assert(opEnd < 0.05, 'transition completed after resume, opacity=' + opEnd);

// =========================================================================
// Mid-run scale changes compose (elapsed scaled time accumulates exactly)
// =========================================================================
let composed = false;
setTimeout(() => { composed = true; }, 140);
bro.time.scale = 0.5;
advanceTime(100);           // +50  (total 50)
bro.time.scale = 2;
advanceTime(50);            // +100 (total 150 >= 140)
assert(composed, 'scale changes mid-run compose: 50 + 100 scaled ms fired a 140ms timer');

// Leave the world as we found it
bro.time.scale = 1;
bro.time.paused = false;
assert(bro.time.scale === 1 && bro.time.paused === false, 'state restored');
