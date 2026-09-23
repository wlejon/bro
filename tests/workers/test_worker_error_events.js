// Worker error events, on both sides of the thread boundary, and transfer
// lists — the paths that build event objects and walk transfer lists while
// the heap is under churn. Every value those paths hold across an allocation
// has to be rooted; a raw one would name a dead address after a collection
// (run this under BRONZE_GC_STRESS=1 to make that happen on every one).

const workerPath = '../workers/worker_error_events.js';

function pumpUntil(pred, ms) {
    const deadline = Date.now() + (ms || 15000);
    while (!pred() && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
}

function churn(n) {
    let keep = [];
    for (let i = 0; i < n; i++) keep.push({ i, s: 'main-garbage-' + i });
    return keep.length;
}

const w = new Worker(workerPath);
const fromWorker = [];
const mainErrors = [];
const mainListenerErrors = [];
w.onmessage = (e) => { fromWorker.push(e.data); };
w.onerror = (e) => { churn(1000); mainErrors.push({ type: e.type, message: String(e.message), filename: e.filename }); };
w.addEventListener('error', (e) => { mainListenerErrors.push({ type: e.type, message: String(e.message) }); });

// ---------------------------------------------------------------------------
// Errors thrown inside the worker reach the worker's own onerror and error
// listeners, and the main thread's onerror and error listeners.
// ---------------------------------------------------------------------------
const ROUNDS = 12;
for (let n = 0; n < ROUNDS; n++) {
    churn(2000);
    w.postMessage({ cmd: 'throw', n });
}
pumpUntil(() => mainErrors.length >= ROUNDS && mainListenerErrors.length >= ROUNDS &&
                fromWorker.filter((m) => m.from === 'onerror').length >= ROUNDS &&
                fromWorker.filter((m) => m.from === 'listener').length >= ROUNDS);

assert(mainErrors.length === ROUNDS, `main onerror fired ${ROUNDS} times, got ${mainErrors.length}`);
assert(mainListenerErrors.length === ROUNDS, `main error listener fired ${ROUNDS} times, got ${mainListenerErrors.length}`);
for (let n = 0; n < mainErrors.length; n++) {
    const e = mainErrors[n];
    assert(e.type === 'error', `main error event ${n} type, got ${e.type}`);
    assert(e.message.indexOf('boom ' + n) >= 0, `main error event ${n} message names round ${n}, got ${e.message}`);
    assert(typeof e.filename === 'string', `main error event ${n} has a filename`);
    assert(mainListenerErrors[n].message === e.message, `listener saw the same message for round ${n}`);
}

const inWorker = fromWorker.filter((m) => m.from === 'onerror');
const inWorkerListener = fromWorker.filter((m) => m.from === 'listener');
assert(inWorker.length === ROUNDS, `worker onerror fired ${ROUNDS} times, got ${inWorker.length}`);
assert(inWorkerListener.length === ROUNDS, `worker error listener fired ${ROUNDS} times, got ${inWorkerListener.length}`);
for (let n = 0; n < inWorker.length; n++) {
    assert(inWorker[n].type === 'error', `worker onerror event ${n} type`);
    assert(inWorker[n].message.indexOf('boom ' + n) >= 0, `worker onerror event ${n} message, got ${inWorker[n].message}`);
    assert(inWorker[n].hasFilename, `worker onerror event ${n} has a filename`);
    assert(inWorkerListener[n].message.indexOf('boom ' + n) >= 0, `worker error listener event ${n} message`);
}

// ---------------------------------------------------------------------------
// Transfer lists: every listed buffer is transferred (the sender's is
// detached), however many there are and whatever the heap does meanwhile.
// ---------------------------------------------------------------------------
fromWorker.length = 0;
const COUNT = 16;
const bufs = [];
for (let i = 0; i < COUNT; i++) {
    const b = new ArrayBuffer(8 + i);
    new Uint8Array(b)[0] = i + 1;
    bufs.push(b);
    churn(200);
}
w.postMessage({ cmd: 'transfer', bufs }, bufs);
for (let i = 0; i < COUNT; i++) {
    assert(bufs[i].byteLength === 0, `sender's buffer ${i} is detached, got byteLength ${bufs[i].byteLength}`);
}

pumpUntil(() => fromWorker.some((m) => m.from === 'afterTransfer'));
const echo = fromWorker.find((m) => m.from === 'transfer');
const after = fromWorker.find((m) => m.from === 'afterTransfer');
assert(echo, 'worker echoed the transfer');
if (echo) {
    for (let i = 0; i < COUNT; i++) {
        assert(echo.lens[i] === 8 + i, `worker received buffer ${i} whole, got ${echo.lens[i]}`);
        assert(echo.firsts[i] === i + 1, `worker received buffer ${i}'s bytes, got ${echo.firsts[i]}`);
        assert(echo.bufs[i].byteLength === 8 + i, `main received buffer ${i} back, got ${echo.bufs[i].byteLength}`);
        assert(new Uint8Array(echo.bufs[i])[0] === i + 1, `buffer ${i} came back with its bytes`);
    }
}
assert(after && after.lens.every((l) => l === 0), 'the worker\'s buffers were detached by its own transfer');

w.terminate();
