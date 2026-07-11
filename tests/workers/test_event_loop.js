// Worker event-loop wait regimes — guards the deadline-aware wait in
// src/js/worker.cpp. The loop blocks when fully idle, sleeps to the next
// timer deadline when timers are pending, and keeps polling while pollables
// (fetch etc.) are in flight. Each section would hang or stall if one of
// those wake sources were missed.

const w = new Worker('../workers/worker_eventloop.js');

let last = null;
const ticks = [];
let fetched = null;
let closing = false;
w.onmessage = (e) => {
    const m = e.data;
    if (m.pong !== undefined) last = m;
    if (m.fired !== undefined) last = m;
    if (m.tick !== undefined) ticks.push(m.tick);
    if (m.fetched !== undefined) fetched = m.fetched;
    if (m.fetchError !== undefined) fetched = 'ERROR: ' + m.fetchError;
    if (m.closing) closing = true;
};

// Worker timers/fetch run on real time, so wait in real time while pumping
// the main loop (advanceTime drains worker->main messages each frame).
function waitForReal(pred, ms) {
    let waited = 0;
    while (!pred() && waited < ms) { wallSleep(20); advanceTime(1); waited += 20; }
    return pred();
}

// ── Idle block wakes on postMessage ──────────────────────────────────────
w.postMessage({ cmd: 'ping', n: 1 });
assert(waitForReal(() => last && last.pong === 1, 5000), 'idle worker answered ping');

// Let it sit blocked with nothing scheduled, then ping again — the wake
// must work after a long idle block, not just right after startup.
wallSleep(300);
last = null;
w.postMessage({ cmd: 'ping', n: 2 });
assert(waitForReal(() => last && last.pong === 2, 5000), 'worker woke from idle block');

// ── Timer deadline fires with no messages arriving ───────────────────────
last = null;
w.postMessage({ cmd: 'oneshot', delay: 100 });
assert(waitForReal(() => last && last.fired === 100, 5000),
       'setTimeout fired from the deadline wait');

// ── Repeating timer keeps firing ─────────────────────────────────────────
w.postMessage({ cmd: 'startInterval' });
assert(waitForReal(() => ticks.length >= 5, 5000),
       'setInterval ticked 5x unmessaged, got ' + ticks.length);

// ── Fetch pollable completes (loop must not block while it is in flight) ─
w.postMessage({ cmd: 'fetch', path: 'index.html' });
assert(waitForReal(() => fetched !== null, 5000), 'fetch completed in worker');
assert(!String(fetched).startsWith('ERROR'), 'fetch succeeded: ' + fetched);

// ── self.close() exits the loop ──────────────────────────────────────────
w.postMessage({ cmd: 'close' });
assert(waitForReal(() => closing, 5000), 'worker acked close');
w.terminate();
