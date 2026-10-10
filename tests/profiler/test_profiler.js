// bro.profiler — bronze's sampling profiler started and stopped from script
// (docs/profiler-api.js, src/bronze_host/native_profiler.cpp, bronze's
// runtime/profiler.cpp).
//
// What is asserted is the shape and the attribution a profile must have, not
// sample counts: a busy function the script runs for a few hundred wall
// milliseconds shows up with self samples and a tier from the documented set,
// the main thread is the one sampled, a Worker's busy loop is seen when the
// profile selects workers, and the misuse cases throw.

const TIERS = ['interpreter', 'tier 1', 'tier 2', 'tier 2 osr', 'aot', 'stub', 'native'];

assert(typeof bro.profiler === 'object', 'bro.profiler exists');
assert(bro.profiler.running === false, 'not running at start');

let threw = false;
try { bro.profiler.stop(); } catch (e) { threw = /no profile is running/.test(String(e.message)); }
assert(threw, 'stop() without a profile throws');

threw = false;
try { bro.profiler.start({ threads: 'gpu' }); } catch (e) { threw = e instanceof TypeError; }
assert(threw, 'an unknown threads value is a TypeError');
assert(bro.profiler.running === false, 'a refused start leaves nothing running');

// A function hot enough to own most of the window's self time.
function hotLoopForProfiler(n) {
    let acc = 0;
    for (let i = 0; i < n; i++) acc = (acc + Math.sqrt(i * 3 + 1)) % 1000003;
    return acc;
}

function burn(ms) {
    const until = Date.now() + ms;
    let sink = 0;
    while (Date.now() < until) sink += hotLoopForProfiler(20000);
    return sink;
}

// ---- main thread -----------------------------------------------------------
if (process.platform !== 'win32') {
    let unsupported = false;
    try { bro.profiler.start({ hz: 2000 }); } catch (e) {
        unsupported = /supported on Windows x64 only/.test(String(e.message));
    }
    assert(unsupported, 'start() on non-Windows reports unsupported');
    console.log('bro.profiler: supported on Windows x64 only; non-Windows behaviour verified');
} else {
bro.profiler.start({ hz: 2000 });
assert(bro.profiler.running === true, 'running after start');

threw = false;
try { bro.profiler.start(); } catch (e) { threw = /already running/.test(String(e.message)); }
assert(threw, 'a second start throws while one runs');

burn(400);
const p = bro.profiler.stop({ callers: true, report: true, top: 15 });
assert(bro.profiler.running === false, 'not running after stop');

assert(p.hz === 2000, 'hz echoed, got ' + p.hz);
// burn() counts whole Date.now() milliseconds, so its 400 can be 399.x.
assert(p.durationMs >= 399, 'duration covers the window, got ' + p.durationMs);
assert(p.samples > 50, 'a 400 ms window at 2 kHz has samples, got ' + p.samples);
assert(p.truncated === false, 'not truncated');
assert(p.threads.length === 1 && p.threads[0].kind === 'main', 'only the main thread sampled: ' + JSON.stringify(p.threads));
assert(Array.isArray(p.functions) && p.functions.length > 0, 'functions listed');
for (const f of p.functions) {
    assert(TIERS.indexOf(f.tier) >= 0, 'tier from the documented set: ' + f.tier + ' (' + f.name + ')');
    assert(f.total >= f.self, 'total >= self for ' + f.name);
}
for (let i = 1; i < p.functions.length; i++) {
    assert(p.functions[i - 1].self >= p.functions[i].self, 'sorted by self');
}

const hot = p.functions.filter((f) => f.name.indexOf('hotLoopForProfiler') >= 0);
assert(hot.length > 0, 'the hot function is in the profile; top: ' +
       p.functions.slice(0, 8).map((f) => f.name + ' [' + f.tier + '] ' + f.self).join(', '));
const hotSelf = hot.reduce((s, f) => s + f.self, 0);
const hotTotal = Math.max.apply(null, hot.map((f) => f.total));
assert(hotTotal > p.samples / 2, 'the hot function is on most stacks: ' + hotTotal + '/' + p.samples);
assert(hotSelf > 0, 'the hot function has self samples');
assert(hot.every((f) => f.tier !== 'native'), 'a JS function is never billed as native');

const burnRow = p.functions.findIndex((f) => f.name.indexOf('burn') >= 0 && f.tier !== 'native');
const hotRow = p.functions.indexOf(hot[0]);
assert(Array.isArray(p.callers), 'callers present with callers: true');
if (burnRow >= 0) {
    assert(p.callers.some((e) => e.caller === burnRow && p.functions[e.callee].name.indexOf('hotLoopForProfiler') >= 0),
           'an edge burn -> hotLoopForProfiler');
}
assert(p.callers.every((e) => e.caller < p.functions.length && e.callee < p.functions.length && e.count > 0),
       'edges index functions');
assert(typeof p.report === 'string' && p.report.indexOf('Function') >= 0 && p.report.indexOf('hotLoopForProfiler') >= 0,
       'the text report names the hot function');
console.log(p.report);

// Without the options the extras are absent.
const q = bro.profiler.profile(() => burn(100));
assert(q.callers === undefined && q.report === undefined, 'callers/report only on request');
assert(q.samples > 0, 'profile() samples its body');

// ---- a worker --------------------------------------------------------------
bro.profiler.start({ threads: 'workers', hz: 2000 });
const w = new Worker('../profiler/worker_busy.js');
let done = false;
w.onmessage = () => { done = true; };
w.postMessage(400);
const deadline = Date.now() + 20000;
while (!done && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
const pw = bro.profiler.stop();
w.terminate();
assert(done, 'the worker finished its busy loop');
const workerThreads = pw.threads.filter((t) => t.kind === 'worker');
assert(workerThreads.length >= 1 && workerThreads[0].samples > 20,
       'the worker thread was sampled: ' + JSON.stringify(pw.threads));
assert(pw.threads.every((t) => t.kind === 'worker'), 'the main thread was not sampled');
assert(pw.functions.some((f) => f.name.indexOf('workerSpin') >= 0),
       'the worker\'s busy function is in the profile; top: ' + pw.functions.slice(0, 6).map((f) => f.name).join(', '));
}
