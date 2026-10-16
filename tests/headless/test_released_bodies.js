// An idle window releases the MIR bodies of compiled script functions
// (brass Module::release_bodies, bronze builds one again on demand). Every
// path that reads a body must still work for a function whose body is gone:
// tier-up, deopt, stack traces, the profiler. `mem --trim` releases them
// here, in the middle of this script, with its own frames live.

function run() {
    var id = controlCommand.apply(null, arguments);
    for (var i = 0; i < 100; i++) {
        var r = controlResult(id);
        if (r) return r;
        advanceTime(16);
    }
    throw new Error('no reply to ' + Array.prototype.join.call(arguments, ' '));
}
function mirLine() {
    var text = run('mem').payload;
    return text.split('\n').filter(function(l) { return l.indexOf('mir ') === 0; })[0] || '';
}
function released() {
    var m = /released (\d+)/.exec(mirLine());
    return m ? Number(m[1]) : -1;
}
function trim() {
    var r = run('mem', '--trim');
    assert(r.ok, 'mem --trim succeeds: ' + r.payload);
    return r.payload;
}

function hot(a, b) { return a.x * b + (a.y | 0); }
function warm(n) {
    var s = 0;
    for (var i = 0; i < n; i++) s += hot({ x: i, y: 1 }, 2);
    return s;
}
function thrower() { return new Error('from a released body').stack; }

var first = warm(50000);
var stack0 = thrower();
var before = released();
var t = trim();
assert(t.indexOf('script function bodies released') >= 0, 'trim releases bodies:\n' + t);
var after = released();
assert(after > before, 'the trim released bodies: ' + before + ' -> ' + after + ' (' + mirLine() + ')');

// The same answers from the same functions, now built again as they tier up.
assert(warm(50000) === first, 'a released function computes what it did');
for (var k = 0; k < 5; k++) assert(warm(20000) === warm(20000), 'and keeps doing so as it tiers up');

// Deopt: a speculated Number that stops being one, in a body released again.
trim();
assert(hot({ x: '3', y: 1 }, 2) === 7, 'a string operand deopts into a body built again');
assert(hot({ x: 3, y: 'z' }, 2) === 6, 'and another guard');
assert(warm(10000) === warm(10000), 'and tiers up again after');

// Stack traces name the function whose body was released.
trim();
var stack1 = thrower();
assert(stack1.indexOf('thrower') >= 0, 'a stack trace names a released function:\n' + stack1);
assert(stack0.split('\n').length === stack1.split('\n').length,
       'the same depth as before the release:\n' + stack0 + '\n---\n' + stack1);

// The profiler names a released function's frames.
trim();
if (bro.profiler && typeof bro.profiler.start === 'function') {
    bro.profiler.start({ hz: 2000 });
    var t0 = Date.now(), s = 0;
    while (Date.now() - t0 < 300) s += warm(20000);
    var p = bro.profiler.stop();
    var names = p.functions.map(function(f) { return f.name; });
    assert(names.some(function(n) { return n.indexOf('warm') >= 0 || n.indexOf('hot') >= 0; }),
           'the profile names the functions whose bodies were released: ' + names.slice(0, 15).join(', '));
}
