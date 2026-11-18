// A worker's onmessage is read at each message, not once per batch of
// queued messages: a handler that replaces itself on the first message
// hands every later one to the replacement, even when the messages were
// posted together (host_worker_thread.cpp). Monaco's worker threw "WebWorker
// already initialized!" when its first handler ran twice.

function pumpUntil(pred, ms) {
    const deadline = Date.now() + (ms || 15000);
    while (!pred() && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
}

const w = new Worker('../workers/worker_swap_onmessage.js');
const got = [];
w.onmessage = (e) => got.push(e.data.via + ':' + e.data.n + ':' + e.data.firstCalls);
w.postMessage(1);
w.postMessage(2);
w.postMessage(3);
pumpUntil(() => got.length >= 3);
assert(got.join(' ') === 'first:1:1 second:2:1 second:3:1', 'later messages reach the replaced handler, got ' + got.join(' '));
w.terminate();
