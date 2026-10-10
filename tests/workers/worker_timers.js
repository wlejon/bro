// Companion worker for test_worker_timers.js: each command times a run of
// event-loop turns in the worker and reports the elapsed real time.

function zeroTimer() { return new Promise((r) => setTimeout(r, 0)); }

async function timeoutChain(n) {
    const t0 = Date.now();
    for (let i = 0; i < n; i++) {
        await zeroTimer();
    }
    return Date.now() - t0;
}

function intervalRun(n, ms) {
    return new Promise((resolve) => {
        const t0 = Date.now();
        let ticks = 0;
        const id = setInterval(() => {
            if (++ticks === n) { clearInterval(id); resolve(Date.now() - t0); }
        }, ms);
    });
}

async function promiseTurns(n) {
    const t0 = Date.now();
    for (let i = 0; i < n; i++) {
        await Promise.resolve(i);
    }
    return Date.now() - t0;
}

// Task/microtask order: a promise reaction queued by one timer runs before
// the next due timer, as in HTML's event loop.
function order() {
    return new Promise((resolve) => {
        const seen = [];
        setTimeout(() => { seen.push('t1'); Promise.resolve().then(() => seen.push('m1')); }, 0);
        setTimeout(() => { seen.push('t2'); }, 0);
        setTimeout(() => resolve(seen.join(',')), 5);
    });
}

onmessage = async (e) => {
    const m = e.data;
    if (m.cmd === 'timeouts') {
        postMessage({ cmd: m.cmd, ms: await timeoutChain(m.n) });
    } else if (m.cmd === 'interval') {
        postMessage({ cmd: m.cmd, ms: await intervalRun(m.n, m.ms) });
    } else if (m.cmd === 'promises') {
        postMessage({ cmd: m.cmd, ms: await promiseTurns(m.n) });
    } else if (m.cmd === 'order') {
        postMessage({ cmd: m.cmd, order: await order() });
    } else if (m.cmd === 'idleThenTimeout') {
        // Scheduled from a promise reaction, after the loop's timer pass:
        // the loop must still see it rather than sleep its idle wait.
        await null;
        const t0 = Date.now();
        setTimeout(() => postMessage({ cmd: m.cmd, ms: Date.now() - t0 }), 0);
    }
};
