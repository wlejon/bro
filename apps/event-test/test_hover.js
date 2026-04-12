// Test: CSS :hover triggers transition, transition events fire, animation events fire.

flush();

// --- Part 1: Hover + CSS Transition ---

var tb = document.getElementById('trans-box');
assert(tb !== null, 'trans-box exists');

var rect = tb.getBoundingClientRect();
var cx = rect.left + rect.width / 2;
var cy = rect.top + rect.height / 2;

var initialBg = computedStyle('#trans-box', 'background-color');

// Track transition events
var transEvents = [];
tb.addEventListener('transitionstart', function(e) {
    transEvents.push('start:' + e.propertyName);
});
tb.addEventListener('transitionend', function(e) {
    transEvents.push('end:' + e.propertyName + ':' + e.elapsedTime);
});

// Hover the element
mouseMove(cx, cy);

// Advance past the 500ms transition duration
advanceTime(600);

var finalBg = computedStyle('#trans-box', 'background-color');

// Background should have changed from blue to the :hover red
assert(finalBg !== initialBg, 'background should change after hover + transition');
assert(finalBg === 'rgb(239, 68, 68)', 'background should be hover color, got: ' + finalBg);

// Should have transitionstart and transitionend
assert(transEvents.length === 2, 'should have 2 transition events, got: ' + JSON.stringify(transEvents));
assert(transEvents[0] === 'start:background-color', 'first event should be transitionstart');
assert(transEvents[1] === 'end:background-color:0.5', 'second event should be transitionend with 0.5s elapsed');

console.log('PASS: hover transition');

// --- Part 2: CSS Animation events ---
// animationstart already fired at page load before we could listen.
// We can check iteration and end events for the flash animation (3 x 1s).

var ab = document.getElementById('anim-box');
assert(ab !== null, 'anim-box exists');

var animEvents = [];
ab.addEventListener('animationiteration', function(e) {
    animEvents.push('iter:' + e.animationName);
});
ab.addEventListener('animationend', function(e) {
    animEvents.push('end:' + e.animationName);
});

// Advance enough for the 3s animation to complete
advanceTime(4000);

// Should have at least animationend (iteration events may have already passed)
var hasAnimEnd = animEvents.some(function(e) { return e.indexOf('end:') === 0; });
assert(hasAnimEnd, 'animationend should fire, got: ' + JSON.stringify(animEvents));

console.log('PASS: animation events');

// --- Part 3: Mouse leave reverses transition ---

var leaveEvents = [];
tb.addEventListener('transitionstart', function(e) {
    leaveEvents.push('start:' + e.propertyName);
});
tb.addEventListener('transitionend', function(e) {
    leaveEvents.push('end:' + e.propertyName);
});

// Move mouse away
mouseMove(0, 0);
advanceTime(600);

var afterLeaveBg = computedStyle('#trans-box', 'background-color');

// Should have transitioned back toward original
assert(leaveEvents.length >= 1, 'leave should trigger transition events, got: ' + JSON.stringify(leaveEvents));

console.log('PASS: mouse leave transition');
console.log('ALL TESTS PASSED');
