// unhandledrejection / rejectionhandled inside a worker: the worker's own
// microtask drains report to its `self` (onunhandledrejection, then the
// listeners), a promise handled before the report is never reported, and one
// handled after it fires rejectionhandled. See worker_unhandled_rejection.js.

function pumpUntil(pred, ms) {
    const deadline = Date.now() + (ms || 15000);
    while (!pred() && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
}

const w = new Worker('../workers/worker_unhandled_rejection.js');
let reply = null;
w.onmessage = (e) => { reply = e.data; };

function fetchLog() {
    reply = null;
    w.postMessage({ cmd: 'log' });
    pumpUntil(() => reply !== null);
    assert(reply !== null, 'worker replied');
    return reply.log;
}

w.postMessage({ cmd: 'reject' });
let log = [];
pumpUntil(() => { log = fetchLog(); return log.length >= 2; });
assert(log.length === 2, 'two entries after the rejection, got ' + JSON.stringify(log));
assert(log[0] === 'attr:unhandledrejection:in-worker',
       'self.onunhandledrejection first, got ' + log[0]);
assert(log[1] === 'listener:unhandledrejection:late:true',
       'then the listener, with the promise and cancelable, got ' + log[1]);

w.postMessage({ cmd: 'handle' });
pumpUntil(() => { log = fetchLog(); return log.length >= 3; });
assert(log.length === 3 && log[2] === 'handled:late',
       'rejectionhandled for the late handler, got ' + JSON.stringify(log));

w.terminate();
console.log('worker unhandledrejection OK');
