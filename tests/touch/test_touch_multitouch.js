// Multi-touch: concurrent contacts get distinct pointerIds with only the
// first being primary; TouchEvent lists see all live fingers; and pointer
// capture is per pointerId — a captured contact's moves route to the holder
// while a second, uncaptured contact keeps hit-testing normally.

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
     'gotpointercapture', 'lostpointercapture',
     'touchstart', 'touchmove', 'touchend'].forEach(function (t) {
        el.addEventListener(t, function (e) {
            log.push({ el: name, type: t, e: e });
        });
    });
}
tally(a, 'a');
tally(b, 'b');
function count(el, type) {
    return log.filter(function (l) { return l.el === el && l.type === type; }).length;
}
function last(el, type) {
    var m = log.filter(function (l) { return l.el === el && l.type === type; });
    return m.length ? m[m.length - 1].e : null;
}

// --- two concurrent contacts -------------------------------------------------
touchDown(1, ar.left + 50, ar.top + 50);   // finger 1 on A (primary)
touchDown(2, br.left + 50, br.top + 50);   // finger 2 on B (secondary)

var pdA = last('a', 'pointerdown');
var pdB = last('b', 'pointerdown');
assert(pdA && pdB, 'each contact fired pointerdown on its own hit target');
assert(pdA.pointerId !== pdB.pointerId, 'concurrent contacts have distinct pointerIds');
assert(pdA.pointerId >= 2 && pdB.pointerId >= 2, 'both ids are touch ids (>= 2)');
assert(pdA.isPrimary === true, 'first contact of the set is primary');
assert(pdB.isPrimary === false, 'second concurrent contact is NOT primary');

var tsB = last('b', 'touchstart');
assert(tsB.touches.length === 2, 'second touchstart sees both live contacts in touches');
assert(tsB.targetTouches.length === 1, 'targetTouches only has the contact that started on B');
assert(tsB.changedTouches.length === 1 &&
       tsB.changedTouches.item(0).identifier === pdB.pointerId,
       'changedTouches is just the new contact');

// --- per-pointer capture -----------------------------------------------------
// A captures finger 1's pointer. Finger 1 then moves over B: its pointermove
// must route to A. Finger 2 (uncaptured) moving over B must still hit B.
a.setPointerCapture(pdA.pointerId);
assert(count('a', 'gotpointercapture') === 1, 'gotpointercapture fired on A');
assert(a.hasPointerCapture(pdA.pointerId) === true, 'A holds capture for finger 1');
assert(a.hasPointerCapture(pdB.pointerId) === false, 'A does not hold finger 2');

log = [];
touchMove(1, br.left + 60, br.top + 60);   // captured finger crosses onto B
assert(count('a', 'pointermove') === 1, 'captured pointermove routed to A');
assert(count('b', 'pointermove') === 0, 'B saw no pointermove for the captured finger');
var mvA = last('a', 'pointermove');
assert(mvA.pointerId === pdA.pointerId, 'captured move carries finger 1\'s pointerId');
assert(Math.abs(mvA.offsetX - (br.left + 60 - ar.left)) < 1,
       'captured pointermove offsetX recomputed against A');
// Touch events keep firing at the touchstart target regardless of capture.
assert(count('a', 'touchmove') === 1, 'touchmove fires at the contact\'s start target');

log = [];
touchMove(2, br.left + 40, br.top + 40);   // uncaptured finger moves on B
assert(count('b', 'pointermove') === 1, 'uncaptured contact still hit-tests to B');
assert(count('a', 'pointermove') === 0, 'A saw nothing for the uncaptured contact');
assert(last('b', 'pointermove').pointerId === pdB.pointerId,
       'B\'s move carries finger 2\'s pointerId');

// --- capture auto-releases on pointerup --------------------------------------
log = [];
touchUp(1, br.left + 60, br.top + 60);
assert(count('a', 'pointerup') === 1, 'captured pointerup routed to A');
assert(count('a', 'lostpointercapture') === 1, 'implicit release fired lostpointercapture');
assert(a.hasPointerCapture(pdA.pointerId) === false, 'capture gone after pointerup');
// The lifted primary finger travelled past the tap slop — no compat click.
var teA = last('a', 'touchend');
assert(teA.touches.length === 1, 'touchend touches still lists the other live finger');

// --- secondary contact tap does not synthesize compat mouse -------------------
var bClicks = 0;
b.addEventListener('click', function () { bClicks++; });
touchUp(2, br.left + 40, br.top + 40);
assert(bClicks === 0, 'a non-primary contact tap produces no compat click');

// --- after the set ends, the next contact is primary again ---------------------
log = [];
touchDown(9, ar.left + 10, ar.top + 10);
assert(last('a', 'pointerdown').isPrimary === true,
       'first contact of a fresh set is primary again');
touchUp(9, ar.left + 10, ar.top + 10);

// --- setPointerCapture with an inactive touch id is a silent no-op -------------
a.setPointerCapture(9999);
assert(a.hasPointerCapture(9999) === false, 'capturing an inactive pointerId does nothing');

root.innerHTML = '';
