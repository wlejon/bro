// What creating a worker costs the page's thread: the constructor only
// queues a thread (the worker installs its globals and compiles its script on
// that thread), and the page's frames while it boots stay as cheap as frames
// without it. An app once measured "~22 ms to create a Worker"; that was the
// page's own first frame (~20-26 ms headless, worker or not), which its
// worker happened to be created before.

function frames(n) {
    const out = [];
    for (let i = 0; i < n; i++) {
        const t = perf.now();
        advanceTime(16);
        out.push(perf.now() - t);
        wallSleep(2);
    }
    return out;
}
function median(a) { const s = a.slice().sort((x, y) => x - y); return s[s.length >> 1]; }
function max(a) { return a.reduce((m, x) => Math.max(m, x), 0); }

frames(20);  // past the page's first frames
const base = frames(30);

const ctor = [];
const booting = [];
const ready = [];
for (let k = 0; k < 5; k++) {
    const t0 = perf.now();
    const w = new Worker('../workers/worker_basic.js');
    ctor.push(perf.now() - t0);
    let got = false;
    w.onmessage = () => { got = true; };
    w.postMessage({ cmd: 'echo', payload: k });
    const t1 = perf.now();
    while (!got && perf.now() - t1 < 15000) {
        const t = perf.now();
        advanceTime(16);
        booting.push(perf.now() - t);
        wallSleep(1);
    }
    assert(got, 'worker ' + k + ' answered');
    ready.push(perf.now() - t1);
    w.terminate();
}

console.log('new Worker() on the page: median ' + median(ctor).toFixed(3) + ' ms, max ' + max(ctor).toFixed(3) + ' ms');
console.log('page frames: idle median ' + median(base).toFixed(2) + ' max ' + max(base).toFixed(2) +
            ' ms; while a worker boots median ' + median(booting).toFixed(2) + ' max ' + max(booting).toFixed(2) + ' ms');
console.log('worker ready (first reply) median ' + median(ready).toFixed(1) + ' ms (off the page thread)');

assert(median(ctor) < 2, 'new Worker() costs the page ' + median(ctor).toFixed(3) + ' ms (median)');
// A worker booting must not stall the page: no frame near the old 22 ms.
assert(median(booting) < median(base) + 2,
       'page frames while a worker boots: median ' + median(booting).toFixed(2) + ' vs ' + median(base).toFixed(2) + ' idle');
assert(max(booting) < 12, 'worst page frame while a worker boots: ' + max(booting).toFixed(2) + ' ms');
