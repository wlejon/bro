// Touch cancellation and compat-mouse suppression:
//   * touchCancel drives pointercancel → touchcancel (in that order),
//     releases any capture, and never synthesizes compat mouse events.
//   * preventDefault() on touchstart or pointerdown suppresses the compat
//     mousedown/mouseup/click of a tap (the standard "cancel the compat
//     mouse events" rule); pointerup/touchend still fire.
//   * a contact that drags past the tap slop gets no compat click either.
//   * touch never drives hover: no mouseover/mouseenter, and :hover styling
//     stays untouched (hover is mouse-only in bro — documented policy).

var root = document.getElementById('root');
root.innerHTML =
    '<style>#pad:hover { background-color: rgb(255, 0, 0); }</style>' +
    '<div id="pad" style="position:absolute;left:0;top:0;width:200px;height:120px;background-color:rgb(0,0,255)"></div>';
flush();

var pad = document.getElementById('pad');
var r = pad.getBoundingClientRect();
var cx = r.left + 50, cy = r.top + 50;

var log = [];
['pointerdown', 'pointermove', 'pointerup', 'pointercancel',
 'touchstart', 'touchmove', 'touchend', 'touchcancel',
 'mousedown', 'mouseup', 'click', 'mouseover', 'mouseenter',
 'gotpointercapture', 'lostpointercapture'].forEach(function (t) {
    pad.addEventListener(t, function (e) { log.push({ type: t, e: e }); });
});
function count(type) {
    return log.filter(function (l) { return l.type === type; }).length;
}
function types() { return log.map(function (l) { return l.type; }).join(','); }

// --- cancel path ---------------------------------------------------------------
touchDown(1, cx, cy);
log = [];
touchCancel(1, cx, cy);
assert(types() === 'pointercancel,touchcancel',
       'cancel order is pointercancel,touchcancel (got ' + types() + ')');
assert(count('mousedown') === 0 && count('click') === 0,
       'a cancelled contact synthesizes no compat mouse events');
var pc = log[0].e;
assert(pc.pointerType === 'touch' && pc.buttons === 0 && pc.pressure === 0,
       'pointercancel carries touch type with buttons/pressure cleared');
var tc = log[1].e;
assert(tc.touches.length === 0, 'touchcancel touches excludes the cancelled contact');

// --- cancel releases capture -----------------------------------------------------
log = [];
pad.addEventListener('pointerdown', function (e) {
    pad.setPointerCapture(e.pointerId);
}, { once: true });
touchDown(2, cx, cy);
var capturedId = log[0].e.pointerId;
assert(pad.hasPointerCapture(capturedId) === true, 'capture engaged on touch pointerdown');
touchCancel(2, cx, cy);
assert(pad.hasPointerCapture(capturedId) === false, 'pointercancel released the capture');
assert(count('lostpointercapture') === 1, 'lostpointercapture fired on cancel');

// --- preventDefault on touchstart suppresses compat mouse -------------------------
log = [];
pad.addEventListener('touchstart', function (e) { e.preventDefault(); }, { once: true });
touchDown(3, cx, cy);
touchUp(3, cx, cy);
assert(count('pointerup') === 1 && count('touchend') === 1,
       'pointerup/touchend still fire after a prevented touchstart');
assert(count('mousedown') === 0 && count('mouseup') === 0 && count('click') === 0,
       'preventDefault on touchstart suppressed the compat mouse sequence');

// --- preventDefault on pointerdown suppresses compat mouse ------------------------
log = [];
pad.addEventListener('pointerdown', function (e) { e.preventDefault(); }, { once: true });
touchDown(4, cx, cy);
touchUp(4, cx, cy);
assert(count('mousedown') === 0 && count('click') === 0,
       'preventDefault on pointerdown suppressed the compat mouse sequence');

// --- a drag past the tap slop is not a tap ----------------------------------------
log = [];
touchDown(5, cx, cy);
touchMove(5, cx + 60, cy);       // well past the ~10px slop
touchUp(5, cx + 60, cy);
assert(count('pointermove') === 1 && count('touchmove') === 1,
       'drag dispatched pointermove and touchmove');
assert(count('click') === 0 && count('mousedown') === 0,
       'a dragged contact synthesizes no compat mouse events');

// --- an unmoved tap still produces the compat sequence (control case) -------------
log = [];
touchDown(6, cx, cy);
touchUp(6, cx, cy);
assert(count('click') === 1, 'control tap still clicks');

// --- touch never drives hover ------------------------------------------------------
assert(count('mouseover') === 0 && count('mouseenter') === 0,
       'no touch interaction ever produced mouseover/mouseenter');
touchDown(7, cx, cy);
flush();
assert(computedStyle('#pad', 'background-color') !== 'rgb(255, 0, 0)',
       'a touch contact resting on the element does not activate :hover');
touchUp(7, cx, cy);

root.innerHTML = '';
