// Pointer lock relative motion & click safety:
// - mouseMoveBy(dx, dy) and controlCommand('move', '--by', ...) deliver exact
//   relative deltas without drifting locked pointer coordinates.
// - Calling click x,y or mouseMove(farX, farY) while pointer-locked never
//   mutates lastMouseX_/Y_ or whips movementX/Y on mousedown/mouseup/click.

function runCtl() {
    var id = controlCommand.apply(null, arguments);
    for (var i = 0; i < 400; i++) {
        var r = controlResult(id);
        if (r) return r;
        advanceTime(16);
    }
    throw new Error('no reply to ' + Array.prototype.join.call(arguments, ' '));
}
function okCtl() {
    var r = runCtl.apply(null, arguments);
    assert(r.ok, Array.prototype.join.call(arguments, ' ') + ' succeeds: ' + r.payload);
    return JSON.parse(r.payload);
}

var root = document.getElementById('root') || document.body;
root.innerHTML = '<div id="arena" style="position:fixed;inset:0"></div>';
flush();

var arena = document.getElementById('arena');
var lockX = 200, lockY = 150;

var moves = [];
var buttons = [];
arena.addEventListener('pointermove', function(e) {
    moves.push({ kind: 'pointermove', x: e.clientX, y: e.clientY, mx: e.movementX, my: e.movementY });
});
arena.addEventListener('mousemove', function(e) {
    moves.push({ kind: 'mousemove', x: e.clientX, y: e.clientY, mx: e.movementX, my: e.movementY });
});
['mousedown', 'mouseup', 'click'].forEach(function(t) {
    arena.addEventListener(t, function(e) {
        buttons.push({ type: t, x: e.clientX, y: e.clientY, mx: e.movementX, my: e.movementY });
    });
});

mouseMove(lockX, lockY);
moves = [];

arena.requestPointerLock();
assert(document.pointerLockElement === arena, 'arena acquired pointer lock');

// ── 1. mouseMoveBy in headless JS ───────────────────────────────────────────
mouseMoveBy(120, -45);
mouseMoveBy(-30, 15);
assert(moves.length === 4, 'two mouseMoveBy calls deliver 2 pointermove + 2 mousemove, got ' + moves.length);
assert(moves[0].kind === 'pointermove' && moves[0].mx === 120 && moves[0].my === -45 &&
       moves[0].x === lockX && moves[0].y === lockY,
       'first relative pointermove: ' + JSON.stringify(moves[0]));
assert(moves[1].kind === 'mousemove' && moves[1].mx === 120 && moves[1].my === -45,
       'first relative mousemove: ' + JSON.stringify(moves[1]));
assert(moves[2].mx === -30 && moves[2].my === 15 && moves[3].x === lockX && moves[3].y === lockY,
       'second relative move keeps clientX/Y stationary: ' + JSON.stringify(moves.slice(2)));

// ── 2. bro-ctl move --by (instant and --ms=24 glide) ────────────────────────
moves = [];
var rep1 = okCtl('move', '--by', '200', '-50');
assert(Math.abs(rep1.pointer.x - lockX) < 0.5 && Math.abs(rep1.pointer.y - lockY) < 0.5,
       'move --by keeps pointer anchored at lock coords: ' + JSON.stringify(rep1.pointer));
var sumMouse = moves.filter(function(m) { return m.kind === 'mousemove'; })
                    .reduce(function(a, m) { return { x: a.x + m.mx, y: a.y + m.my }; }, { x: 0, y: 0 });
assert(sumMouse.x === 200 && sumMouse.y === -50, 'move --by 200 -50 delta exact: ' + JSON.stringify(sumMouse));

moves = [];
var rep2 = okCtl('move', '--by=-100,25', '--ms=24');
assert(rep2.events >= 2, 'glided --by emits multiple steps: ' + JSON.stringify(rep2));
var sumGlide = moves.filter(function(m) { return m.kind === 'mousemove'; })
                    .reduce(function(a, m) { return { x: a.x + m.mx, y: a.y + m.my }; }, { x: 0, y: 0 });
assert(Math.abs(sumGlide.x - (-100)) < 0.5 && Math.abs(sumGlide.y - 25) < 0.5,
       'move --by=-100,25 --ms=24 sums cleanly: ' + JSON.stringify(sumGlide));

// ── 3. click x,y & mouseDown/Up while locked never whip aim ─────────────────
// Even if something sends an unlocked mouseMove(1800, 900) before click,
// click 100,100 and direct mouseDown/Up keep lastMouseX_/Y_ locked and report
// movementX=0, movementY=0 with zero spurious move step.
mouseMove(1800, 900);
moves = [];
buttons = [];
okCtl('click', '100,100');
assert(moves.length === 0, 'click x,y while locked must NOT emit any pointermove/mousemove: ' + JSON.stringify(moves));
assert(buttons.length === 3, 'click emits mousedown, mouseup, click: ' + JSON.stringify(buttons));
buttons.forEach(function(b) {
    assert(b.x === lockX && b.y === lockY && b.mx === 0 && b.my === 0,
           'locked button event has locked coords and zero movement: ' + JSON.stringify(b));
});

// Direct JS mouseDown/mouseUp at arbitrary coords while locked
buttons = [];
mouseDown(50, 60);
mouseUp(70, 80);
assert(buttons.length === 3, 'direct locked mouseDown+mouseUp emits mousedown, mouseup, click');
buttons.forEach(function(b) {
    assert(b.x === lockX && b.y === lockY && b.mx === 0 && b.my === 0,
           'direct locked mouseDown/Up reports locked coords and zero movement: ' + JSON.stringify(b));
});

// ── 4. Exit lock restores unlocked pointer at lockX, lockY ──────────────────
document.exitPointerLock();
assert(document.pointerLockElement === null, 'exited pointer lock');
moves = [];
mouseMoveBy(15, 10);
var mUnlocked = moves.filter(function(m) { return m.kind === 'mousemove'; })[0];
assert(mUnlocked && mUnlocked.x === lockX + 15 && mUnlocked.y === lockY + 10,
       'unlocked mouseMoveBy advances from restored lock position: ' + JSON.stringify(mUnlocked));

root.innerHTML = '';
console.log('test_pointer_lock_relative.js PASSED');
