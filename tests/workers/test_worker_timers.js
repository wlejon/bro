// A worker's timers run on its next turn: setTimeout(0) chained from promise
// reactions, a short setInterval, and plain promise turns all complete in a
// few milliseconds rather than waiting out the loop's idle wait (they once
// took ~500 ms per zero timer, because a timer set from a microtask after the
// loop's timer pass was not seen until the idle wait expired).

const w = new Worker('../workers/worker_timers.js');
const replies = {};
w.onmessage = (e) => { replies[e.data.cmd] = e.data; };

function run(msg, ms) {
    delete replies[msg.cmd];
    w.postMessage(msg);
    const deadline = Date.now() + (ms || 20000);
    while (!replies[msg.cmd] && Date.now() < deadline) { advanceTime(1); wallSleep(1); }
    assert(replies[msg.cmd], msg.cmd + ': worker replied');
    return replies[msg.cmd];
}

// Warm-up: the first command compiles the handler's code paths.
run({ cmd: 'timeouts', n: 2 });

const N = 50;
const t = run({ cmd: 'timeouts', n: N });
console.log('worker setTimeout(0) x' + N + ': ' + t.ms + ' ms');
// 50 zero timers in well under a second: ~10 ms each would still pass, the
// old 500 ms idle wait per timer (25 s) cannot.
assert(t.ms < 500, N + ' chained setTimeout(0) took ' + t.ms + ' ms');

const iv = run({ cmd: 'interval', n: 20, ms: 1 });
console.log('worker setInterval(1) x20: ' + iv.ms + ' ms');
assert(iv.ms < 500, '20 ticks of a 1 ms interval took ' + iv.ms + ' ms');

const p = run({ cmd: 'promises', n: 10000 });
console.log('worker 10000 promise turns: ' + p.ms + ' ms');
assert(p.ms < 1000, '10000 promise turns took ' + p.ms + ' ms');

const o = run({ cmd: 'order' });
assert(o.order === 't1,m1,t2', 'a timer\'s microtasks run before the next timer: ' + o.order);

const idle = run({ cmd: 'idleThenTimeout' });
console.log('worker setTimeout(0) from a microtask: ' + idle.ms + ' ms');
assert(idle.ms < 100, 'zero timer set from a microtask fired after ' + idle.ms + ' ms');

w.terminate();
