// A worker's global has a prototype chain, as the window's does:
// self -> DedicatedWorkerGlobalScope.prototype -> EventTarget.prototype -> Object.prototype.

const w = new Worker('../workers/worker_global_proto.js');
let got = null;
w.onmessage = (e) => { got = e.data; };
w.postMessage('go');

function pumpUntil(pred, ms) {
    const deadline = Date.now() + (ms || 15000);
    while (!pred() && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
}
pumpUntil(() => got !== null);
w.terminate();

assert(got !== null, 'worker replied');
assert(got.err === null, 'String(self) does not throw, got ' + got.err);
assert(got.str === '[object DedicatedWorkerGlobalScope]', 'String(self), got ' + got.str);
assert(got.hasCtor, 'DedicatedWorkerGlobalScope is a global interface object');
assert(got.isInstance, 'self instanceof DedicatedWorkerGlobalScope');
assert(got.isEventTarget, 'self instanceof EventTarget');
assert(got.hasOwn, 'Object.prototype methods reach the worker global');

console.log('test_worker_global_proto: OK');
