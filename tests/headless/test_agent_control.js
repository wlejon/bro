// The agent control surface (docs/agent-control.md), driven in-process
// through controlCommand(...argv) / controlResult(id): the same commands
// bro-ctl sends over the control socket, without the socket.
//
// Input commands play out on a timeline against the engine's clock (virtual
// time here), so run() advances time until the reply arrives.

function run() {
    var id = controlCommand.apply(null, arguments);
    for (var i = 0; i < 400; i++) {
        var r = controlResult(id);
        if (r) return r;
        advanceTime(16);
    }
    throw new Error('no reply to ' + Array.prototype.join.call(arguments, ' '));
}
function ok() {
    var r = run.apply(null, arguments);
    assert(r.ok, Array.prototype.join.call(arguments, ' ') + ' succeeds: ' + r.payload);
    return r.payload;
}

var root = document.getElementById('root') || document.body;
var box = document.createElement('button');
box.id = 'ctl-target';
box.textContent = 'press';
box.style.cssText = 'position:absolute; left:100px; top:50px; width:200px; height:80px;';
root.appendChild(box);
var field = document.createElement('input');
field.id = 'ctl-field';
field.style.cssText = 'position:absolute; left:100px; top:200px; width:200px; height:30px;';
root.appendChild(field);
advanceTime(16);

// ── info / help ─────────────────────────────────────────────────────────────

var info = JSON.parse(ok('info'));
assert(info.mode === 'headless', 'info reports the headless mode: ' + info.mode);
assert(info.viewport && info.viewport.width > 0, 'info carries the viewport');
var help = ok('help');
assert(help.indexOf('click') >= 0 && help.indexOf('trace') >= 0, 'help lists the commands');
var unknown = run('no-such-command');
assert(!unknown.ok, 'an unknown command fails');

// ── rect: client rects by selector ──────────────────────────────────────────

var rects = JSON.parse(ok('rect', '#ctl-target'));
assert(rects.length === 1, 'rect finds one match');
assert(Math.abs(rects[0].x - 100) < 0.5 && Math.abs(rects[0].y - 50) < 0.5, 'rect is the element box');
assert(Math.abs(rects[0].width - 200) < 0.5 && Math.abs(rects[0].height - 80) < 0.5, 'rect has its size');
assert(!run('rect', '#nothing-here').ok, 'rect with no match fails');

// ── click by selector: the element's centre ─────────────────────────────────

var clicks = [];
box.addEventListener('click', function(e) { clicks.push({x: e.clientX, y: e.clientY}); });
var clicked = JSON.parse(ok('click', '#ctl-target'));
assert(clicks.length === 1, 'click delivers one click (got ' + clicks.length + ')');
assert(Math.abs(clicks[0].x - 200) < 1 && Math.abs(clicks[0].y - 90) < 1,
       'the click lands at the element centre: ' + JSON.stringify(clicks[0]));
assert(Math.abs(clicked.pointer.x - 200) < 1, 'the reply reports where the pointer is');

// ── click by coordinates, and a double click ────────────────────────────────

clicks = [];
ok('click', '150,60');
assert(clicks.length === 1 && Math.abs(clicks[0].x - 150) < 1 && Math.abs(clicks[0].y - 60) < 1,
       'click at x,y: ' + JSON.stringify(clicks));
var dbl = 0;
box.addEventListener('dblclick', function() { dbl++; });
ok('click', '#ctl-target', '--count=2');
assert(dbl === 1, 'click --count=2 is a double click');

// ── key / type into a focused input ─────────────────────────────────────────

ok('click', '#ctl-field');
ok('type', 'hello');
assert(field.value === 'hello', 'type enters text: "' + field.value + '"');
var keys = [];
document.addEventListener('keydown', function(e) { keys.push(e.key); });
ok('key', 'ctrl+a', 'escape');
assert(keys.indexOf('Escape') >= 0, 'key presses Escape: ' + keys.join(','));
assert(keys.indexOf('a') >= 0 || keys.indexOf('A') >= 0, 'key presses the chord key: ' + keys.join(','));

// ── wheel ───────────────────────────────────────────────────────────────────

var wheelDy = 0;
box.addEventListener('wheel', function(e) { wheelDy += e.deltaY; });
ok('wheel', '2', '--at=#ctl-target');
assert(wheelDy > 0, 'wheel scrolls down over the target: ' + wheelDy);

// ── eval / style / dom / inspect ────────────────────────────────────────────

assert(ok('eval', '1 + 2') === '3', 'eval returns the completion value');
assert(ok('eval', 'document.getElementById("ctl-target").textContent') === 'press', 'eval returns a string as is');
assert(JSON.parse(ok('eval', '({a: [1, 2]})')).a[1] === 2, 'eval returns an object as JSON');
var thrown = run('eval', 'throw new Error("boom")');
assert(!thrown.ok && thrown.payload.indexOf('boom') >= 0, 'eval reports a thrown error: ' + thrown.payload);
var style = JSON.parse(ok('style', '#ctl-target', 'position', 'width'));
assert(style.position === 'absolute', 'style reads a computed property: ' + JSON.stringify(style));
assert(ok('dom', '#root', '2').indexOf('BUTTON#ctl-target') >= 0, 'dom shows the layout tree');
assert(ok('inspect', '#ctl-target').length > 0, 'inspect describes the element');

// ── animations, wait, wait-idle ─────────────────────────────────────────────

box.animate([{opacity: 1}, {opacity: 0.2}], {duration: 500});
advanceTime(16);
var anims = JSON.parse(ok('animations'));
assert(anims.length >= 1, 'animations lists the running animation: ' + JSON.stringify(anims));
ok('wait-idle', '--timeout=3000');
var later = document.createElement('div');
later.id = 'ctl-later';
later.style.cssText = 'width:10px; height:10px;';
setTimeout(function() { root.appendChild(later); }, 100);
ok('wait', '#ctl-later', '--timeout=2000');
assert(document.getElementById('ctl-later'), 'wait returns once the element is there');

// ── the frame trace ─────────────────────────────────────────────────────────

ok('mark', 'test-mark');
for (var f = 0; f < 10; f++) advanceTime(16);
var trace = JSON.parse(ok('trace', '2', '--json'));
assert(trace.frames > 0, 'trace summarizes recent frames: ' + trace.frames);
assert(trace.phases && trace.phases.layoutWait, 'trace has phase times');
assert(ok('trace', '2').indexOf('phase') >= 0, 'trace prints a text summary');
var frames = JSON.parse(ok('trace', '2', '--frames'));
assert(frames.frames.length > 0 && typeof frames.frames[0].tick === 'number', 'trace --frames lists records');
assert(frames.marks.some(function(m) { return m.label === 'test-mark'; }), 'the mark is in the trace');
