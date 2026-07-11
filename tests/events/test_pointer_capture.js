// Pointer capture (Element.setPointerCapture): while captured, pointermove /
// pointerup route to the captured element wherever the cursor is (offsets
// recomputed against it), the capture auto-releases after pointerup, and
// got/lostpointercapture fire. This is the web's drag idiom — a canvas that
// captures on pointerdown keeps seeing the stroke after the cursor leaves it,
// and a release off-element still ends the drag instead of latching it.

var root = document.getElementById('root');
root.innerHTML =
    '<div id="a" style="position:absolute;left:0;top:0;width:100px;height:100px"></div>' +
    '<div id="b" style="position:absolute;left:200px;top:0;width:100px;height:100px"></div>';
flush();

var a = document.getElementById('a');
var b = document.getElementById('b');
var ar = a.getBoundingClientRect();
var br = b.getBoundingClientRect();

var log = [];
function tally(el, name) {
    ['pointerdown', 'pointermove', 'pointerup',
     'gotpointercapture', 'lostpointercapture'].forEach(function (t) {
        el.addEventListener(t, function (e) {
            log.push({ el: name, type: t, offsetX: e.offsetX, clientX: e.clientX });
        });
    });
}
tally(a, 'a');
tally(b, 'b');
function count(el, type) {
    return log.filter(function (e) { return e.el === el && e.type === type; }).length;
}

// --- API presence -----------------------------------------------------------
assert(typeof a.setPointerCapture === 'function', 'setPointerCapture exists');
assert(typeof a.releasePointerCapture === 'function', 'releasePointerCapture exists');
assert(typeof a.hasPointerCapture === 'function', 'hasPointerCapture exists');
assert(a.hasPointerCapture(1) === false, 'no capture before any is set');

// --- the drag idiom ---------------------------------------------------------
// pointerdown on A, capture, drag across B, release over B.
var downX = ar.left + 50, downY = ar.top + 50;
a.addEventListener('pointerdown', function (e) {
    a.setPointerCapture(e.pointerId);
}, { once: true });

mouseDown(downX, downY);
assert(count('a', 'pointerdown') === 1, 'pointerdown reached A');
assert(count('a', 'gotpointercapture') === 1, 'gotpointercapture fired on A');
assert(a.hasPointerCapture(1) === true, 'A holds the capture after setPointerCapture');

log = [];
mouseMove(br.left + 50, br.top + 50);   // cursor is over B now
assert(count('a', 'pointermove') === 1, 'captured pointermove routed to A, not the hit target');
assert(count('b', 'pointermove') === 0, 'B saw no pointermove while A held the capture');
// offsets are recomputed against the element actually receiving the event
var mv = log.filter(function (e) { return e.el === 'a' && e.type === 'pointermove'; })[0];
assert(Math.abs(mv.offsetX - (br.left + 50 - ar.left)) < 1,
       'captured pointermove offsetX is relative to A (got ' + mv.offsetX + ')');

log = [];
mouseUp(br.left + 50, br.top + 50);     // release over B
assert(count('a', 'pointerup') === 1, 'captured pointerup routed to A');
assert(count('b', 'pointerup') === 0, 'B saw no pointerup while A held the capture');
assert(count('a', 'lostpointercapture') === 1, 'implicit release fired lostpointercapture');
assert(a.hasPointerCapture(1) === false, 'capture auto-released after pointerup');

// --- after release, events target the hit element again ---------------------
log = [];
mouseMove(br.left + 40, br.top + 40);
assert(count('b', 'pointermove') === 1, 'post-release pointermove hits B normally');
assert(count('a', 'pointermove') === 0, 'A no longer receives events');

// --- explicit release mid-drag ----------------------------------------------
log = [];
a.addEventListener('pointerdown', function (e) {
    a.setPointerCapture(e.pointerId);
}, { once: true });
mouseDown(downX, downY);
assert(a.hasPointerCapture(1) === true, 'second capture engaged');
a.releasePointerCapture(1);
assert(a.hasPointerCapture(1) === false, 'explicit releasePointerCapture ends it');
assert(count('a', 'lostpointercapture') === 1, 'explicit release fired lostpointercapture');
log = [];
mouseMove(br.left + 50, br.top + 50);
assert(count('b', 'pointermove') === 1, 'after explicit release, moves hit-test normally');
mouseUp(br.left + 50, br.top + 50);

// --- only the holder may release --------------------------------------------
a.addEventListener('pointerdown', function (e) {
    a.setPointerCapture(e.pointerId);
}, { once: true });
mouseDown(downX, downY);
b.releasePointerCapture(1);   // not the holder — must be a no-op
assert(a.hasPointerCapture(1) === true, 'a non-holder cannot release the capture');
mouseUp(downX, downY);
assert(a.hasPointerCapture(1) === false, 'capture released at drag end');

// --- mouse events keep normal hit-test targeting while captured -------------
var bMouseMoves = 0;
b.addEventListener('mousemove', function () { bMouseMoves++; });
a.addEventListener('pointerdown', function (e) {
    a.setPointerCapture(e.pointerId);
}, { once: true });
mouseDown(downX, downY);
mouseMove(br.left + 50, br.top + 50);
assert(bMouseMoves === 1, 'mousemove still targets the hit element while A holds pointer capture');
mouseUp(br.left + 50, br.top + 50);

root.innerHTML = '';
