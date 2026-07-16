// Touch input: a single-finger tap drives the full web event sequence in
// spec order — pointerdown → touchstart → pointerup → touchend → compat
// mousedown → mouseup → click — with pointerType "touch", a stable unique
// pointerId (≥ 2, never the mouse's 1), isPrimary on the first contact, and
// TouchEvent touches/targetTouches/changedTouches lists. Driven through the
// headless touchDown/touchUp injectors, which enter the engine at the same
// point as SDL finger events.

var root = document.getElementById('root');
root.innerHTML =
    '<div id="pad" style="position:absolute;left:0;top:0;width:200px;height:120px"></div>' +
    '<button id="btn" style="position:absolute;left:300px;top:0;width:120px;height:40px">Tap me</button>';
flush();

var pad = document.getElementById('pad');
var r = pad.getBoundingClientRect();
var cx = r.left + 50, cy = r.top + 50;

var log = [];
['pointerdown', 'pointermove', 'pointerup', 'pointercancel',
 'touchstart', 'touchmove', 'touchend', 'touchcancel',
 'mousedown', 'mouseup', 'click'].forEach(function (t) {
    pad.addEventListener(t, function (e) { log.push({ type: t, e: e }); });
});

// --- one tap, exact sequence ------------------------------------------------
touchDown(1, cx, cy);
touchUp(1, cx, cy);

var types = log.map(function (l) { return l.type; });
assert(types.join(',') ===
       'pointerdown,touchstart,pointerup,touchend,mousedown,mouseup,click',
       'tap event order is pointerdown,touchstart,pointerup,touchend,' +
       'mousedown,mouseup,click (got ' + types.join(',') + ')');

// --- pointer event payload ---------------------------------------------------
var pd = log[0].e;
assert(pd.pointerType === 'touch', 'pointerdown pointerType is "touch"');
assert(pd.pointerId >= 2, 'touch pointerId is >= 2 (got ' + pd.pointerId + ')');
assert(pd.pointerId !== 1, 'touch pointerId never collides with the mouse id 1');
assert(pd.isPrimary === true, 'first contact is the primary pointer');
assert(pd.buttons === 1, 'pointerdown buttons has the contact bit set');
assert(pd.button === 0, 'pointerdown button is 0 (contact = primary button)');
assert(pd.pressure > 0, 'pointerdown pressure is > 0');
assert(Math.abs(pd.clientX - cx) < 1 && Math.abs(pd.clientY - cy) < 1,
       'pointerdown clientX/Y at the contact point');

var pu = log[2].e;
assert(pu.type === 'pointerup' && pu.pointerId === pd.pointerId,
       'pointerup carries the same pointerId as its pointerdown');
assert(pu.buttons === 0, 'pointerup buttons is 0');
assert(pu.pressure === 0, 'pointerup pressure is 0');

// --- touch event payload -----------------------------------------------------
var ts = log[1].e;
assert(ts instanceof TouchEvent, 'touchstart is a TouchEvent instance');
assert(ts.touches.length === 1, 'touchstart touches has the one live contact');
assert(ts.targetTouches.length === 1, 'touchstart targetTouches has the contact');
assert(ts.changedTouches.length === 1, 'touchstart changedTouches has the contact');
var t0 = ts.changedTouches.item(0);
assert(t0.identifier === pd.pointerId,
       'Touch.identifier equals the contact pointerId (1:1 correlation)');
assert(t0.target === pad, 'Touch.target is the touchstart hit element');
assert(Math.abs(t0.clientX - cx) < 1 && Math.abs(t0.clientY - cy) < 1,
       'Touch clientX/Y at the contact point');

var te = log[3].e;
assert(te.touches.length === 0, 'touchend touches excludes the lifted contact');
assert(te.changedTouches.length === 1, 'touchend changedTouches has the lifted contact');
assert(te.changedTouches.item(0).identifier === pd.pointerId,
       'touchend changedTouches identifier matches');

// --- compat mouse payload ----------------------------------------------------
var md = log[4].e;
assert(md.button === 0 && md.buttons === 1, 'compat mousedown is a left press');
assert(Math.abs(md.clientX - cx) < 1, 'compat mousedown at the tap point');
var ck = log[6].e;
assert(ck.type === 'click', 'compat click fired last');

// --- pointerIds are unique per contact ---------------------------------------
log = [];
touchDown(1, cx, cy);
touchUp(1, cx, cy);
var pd2 = log[0].e;
assert(pd2.pointerId !== pd.pointerId,
       'a new contact gets a new pointerId (' + pd.pointerId + ' -> ' + pd2.pointerId + ')');

// --- a plain button gets click from a tap (compat end-to-end) -----------------
var btn = document.getElementById('btn');
var br = btn.getBoundingClientRect();
var btnClicks = 0;
btn.addEventListener('click', function () { btnClicks++; });
touchDown(7, br.left + 20, br.top + 20);
touchUp(7, br.left + 20, br.top + 20);
assert(btnClicks === 1, 'a tap on a plain <button> produces a click');

root.innerHTML = '';
