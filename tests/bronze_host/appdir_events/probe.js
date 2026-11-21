// The events probe: a bronze-compiled app that does nothing but register
// listeners and say what reached them.
//
// It is the subject of run_events_test.sh, and it exists because "the engine
// dispatches to compiled listeners" is not a claim any unit test can make. The
// only honest proof is a real click, through the real input pipeline — hit
// test, event path, capture/at-target/bubble — landing in a handler that was
// compiled to machine code before the engine existed.
//
// WHAT IT COVERS, one listener per thing that could be broken separately:
//
//   canvas   click, mousemove, wheel   — a hit-tested pointer event reaching
//                                        the element the app itself appended
//   document keydown                   — the documentElement delegation, i.e.
//                                        an event that arrives by BUBBLING
//   document app:toPage / page:toApp   — CustomEvent both directions between
//                                        the compiled app and the appdir's
//                                        interpreted page script
//   window   app:toWindow              — the window listener path, which is
//                                        the one that already worked, kept as
//                                        a control
//
// EVERY LINE IS `APP <name>=<data>` and every value is an integer, a boolean or
// a string the driver chose. Nothing here prints a float, a clock reading or a
// pointer: the expectation beside this file is written by hand from what must
// be true, not recorded from a run (tests/bronze_host/README.md).
//
// NO `new CustomEvent(...)`: a dispatch from compiled code passes a plain
// descriptor object — `{type, detail, bubbles}` — and the host builds the real
// dom::CustomEvent behind it. That was once forced (nothing could be built on
// a chosen prototype); it is now just unwritten — class_probe.js pins the
// shape that would build it. See src/bronze_host/README.md, "The boundary
// rule".

function say(label, value) { console.log('APP ' + label + '=' + value); }

// --- The canvas the probe owns -------------------------------------------
// Positioned by the page's stylesheet at 0,0 with a known size, so the driver
// can name a coordinate that is inside it without measuring anything.
const canvas = document.createElement('canvas');
canvas.width = 200;
canvas.height = 100;
document.body.appendChild(canvas);
say('canvas.width', canvas.width);

// --- Canvas listeners -----------------------------------------------------

let clicks = 0;
canvas.addEventListener('click', function (e) {
    clicks = clicks + 1;
    // The identity check that matters: the target is the very canvas object
    // this program created, not a rebuilt stand-in for it.
    say('click.targetIsCanvas', e.target === canvas);
    // clientX/clientY and NOT offsetX/offsetY: bro's synthesized `click` event
    // is the one mouse event the engine builds without applyMouseOffset, so
    // its offsets are 0 for every listener, compiled or interpreted. Pinning
    // them here would pin that gap as if it were this layer's answer. The
    // mousemove line below reads offsets, and they are right there.
    say('click.client', e.clientX + ',' + e.clientY);
    say('click.button', e.button);
    say('click.trusted', e.isTrusted);
    say('click.count', clicks);
});

canvas.addEventListener('mousemove', function (e) {
    say('mousemove.offset', e.offsetX + ',' + e.offsetY);
});

canvas.addEventListener('wheel', function (e) {
    // A sign and a zero, not the numbers: the engine scales a wheel notch by
    // the user's scroll-speed setting on the way to the DOM event, so the
    // magnitude that arrives here is a setting's value and not this layer's
    // answer. What must be true is that the deltas arrived, on the right axis,
    // with the sign the driver asked for.
    say('wheel.deltaYPositive', e.deltaY > 0);
    say('wheel.deltaXZero', e.deltaX === 0);
});

// --- A document listener, reached by bubbling ------------------------------

document.addEventListener('keydown', function (e) {
    say('keydown.key', e.key);
    say('keydown.shift', e.shiftKey);
});

// --- CustomEvent, interpreted -> compiled ----------------------------------
// The page script dispatches `page:toApp` on the document; this is the compiled
// half of that conversation. It answers on `app:toPage`, which the page script
// is listening for — so one line of output on each side proves a round trip
// that crossed the boundary twice.

document.addEventListener('page:toApp', function (e) {
    say('fromPage', e.detail);
    document.dispatchEvent({ type: 'app:toPage', detail: 'pong:' + e.detail });
});

// --- CustomEvent to the window, and a cancelled one ------------------------

window.addEventListener('app:toWindow', function (e) {
    say('window.detail', e.detail);
});

// preventDefault from a compiled listener has to reach the dispatching
// dom::Event, or the dispatcher's `dispatchEvent(...) === false` would be a
// lie. The driver dispatches `app:cancelme` and checks exactly that.
document.addEventListener('app:cancelme', function (e) {
    say('cancelme.seen', e.detail);
    e.preventDefault();
});

// stopPropagation from a canvas listener must stop the document listener that
// would otherwise see the same event bubble past it.
canvas.addEventListener('app:stophere', function (e) {
    say('stophere.atCanvas', e.detail);
    e.stopPropagation();
});
document.addEventListener('app:stophere', function (e) {
    say('stophere.atDocument', e.detail);
});

// --- Top level done -------------------------------------------------------
// The driver advances a frame before doing anything, so a `ready` line here is
// what says the compiled top level ran at all — an app whose globals failed to
// resolve would have thrown before reaching it.
say('ready', 1);
