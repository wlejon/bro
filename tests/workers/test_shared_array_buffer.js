// Multi-threaded Worker SharedArrayBuffer + Atomics.wait/notify/waitAsync test.
const w = new Worker('../workers/worker_shared_array_buffer.js');

function pumpUntil(pred, ms) {
    const deadline = Date.now() + (ms || 15000);
    while (!pred() && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
}

const replies = [];
w.onmessage = (e) => { replies.push(e.data); };

const sab = new SharedArrayBuffer(64);
const view = new Int32Array(sab);
Atomics.store(view, 0, 42);

// 1. Identity preservation across structured clone + mutual visibility.
w.postMessage({ cmd: 'identity', sab: sab, sameSab: sab, view: view });
pumpUntil(() => replies.some((r) => r.cmd === 'identity-reply'));
const idReply = replies.find((r) => r.cmd === 'identity-reply');
assert(idReply && idReply.ok === true, 'worker preserved SharedArrayBuffer identity and initial write');
assert(Atomics.load(view, 1) === 99, 'main observed worker store via SharedArrayBuffer');

// 2. Reject transferring a SharedArrayBuffer (must throw TypeError).
let transferThrew = false;
try {
    w.postMessage({ sab: sab }, [sab]);
} catch (e) {
    transferThrew = e instanceof TypeError;
}
assert(transferThrew, 'transferring SharedArrayBuffer throws TypeError');
assert(sab.byteLength === 64, 'SharedArrayBuffer remains intact after rejected transfer');

// 3. Cross-thread Atomics.wait in worker woken by main Atomics.notify.
w.postMessage({ cmd: 'wait-for-main', sab: sab });
pumpUntil(() => Atomics.load(view, 2) === 1);
Atomics.store(view, 4, 777);
let wokenByMain = 0;
const notifyDeadline = Date.now() + 5000;
while (wokenByMain === 0 && Date.now() < notifyDeadline) {
    wokenByMain = Atomics.notify(view, 3, 1);
    if (wokenByMain === 0) wallSleep(1);
}
assert(wokenByMain === 1, 'main woke waiting worker, got ' + wokenByMain);
pumpUntil(() => replies.some((r) => r.cmd === 'wait-done'));
const waitReply = replies.find((r) => r.cmd === 'wait-done');
assert(waitReply && waitReply.result === 'ok', 'worker Atomics.wait returned ok');
assert(waitReply.seen === 777, 'worker observed payload before notify');

// 4. Concurrent lock-free Atomics.add from main and worker threads.
const iters = 20000;
w.postMessage({ cmd: 'concurrent-add', sab: sab, iters: iters });
pumpUntil(() => Atomics.load(view, 5) === 1);
for (let i = 0; i < iters; ++i) {
    Atomics.add(view, 6, 1);
}
pumpUntil(() => replies.some((r) => r.cmd === 'add-done'));
const total = Atomics.load(view, 6);
assert(total === iters * 2, 'concurrent Atomics.add lost updates: ' + total);

// 5. Cross-thread Atomics.waitAsync on main resolved by worker Atomics.notify.
const waitAsyncRes = Atomics.waitAsync(view, 7, 0);
assert(waitAsyncRes.async === true, 'waitAsync returned async=true');
assert(waitAsyncRes.value instanceof Promise, 'waitAsync returned Promise');
let resolvedValue = null;
waitAsyncRes.value.then((v) => { resolvedValue = v; });

w.postMessage({ cmd: 'notify-main-waiter', sab: sab });
pumpUntil(() => resolvedValue !== null && replies.some((r) => r.cmd === 'notified'));
const notifyReply = replies.find((r) => r.cmd === 'notified');
assert(notifyReply && notifyReply.woken === 1, 'worker notify woke 1 main waiter');
assert(resolvedValue === 'ok', 'waitAsync resolved with ok, got ' + resolvedValue);

// 6. Atomics.waitAsync woken solely by Worker notify (no postMessage reply).
Atomics.store(view, 8, 0);
const asyncOnly = Atomics.waitAsync(view, 8, 0);
assert(asyncOnly.async === true, 'async-only waitAsync returned async=true');
let asyncOnlyVal = null;
asyncOnly.value.then((v) => { asyncOnlyVal = v; });
w.postMessage({ cmd: 'notify-only', sab: sab, index: 8 });
pumpUntil(() => asyncOnlyVal !== null);
assert(asyncOnlyVal === 'ok', 'waitAsync woke via microtasksPending without postMessage, got ' + asyncOnlyVal);

// 7. Atomics.waitAsync finite timeout expires via microtasksPending.
const timeoutWait = Atomics.waitAsync(view, 7, 123, 25);
assert(timeoutWait.async === true, 'timed waitAsync returned async=true');
let timeoutVal = null;
timeoutWait.value.then((v) => { timeoutVal = v; });
pumpUntil(() => timeoutVal !== null);
assert(timeoutVal === 'timed-out', 'timed waitAsync resolved to timed-out, got ' + timeoutVal);

w.terminate();
