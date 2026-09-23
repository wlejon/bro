// Worker for test_worker_global_proto.js — reports the worker global's prototype chain.

self.onmessage = () => {
    let str = null, err = null;
    try { str = String(self); } catch (e) { err = String(e && e.message); }
    self.postMessage({
        str,
        err,
        hasCtor: typeof DedicatedWorkerGlobalScope === 'function',
        isInstance: typeof DedicatedWorkerGlobalScope === 'function' && self instanceof DedicatedWorkerGlobalScope,
        isEventTarget: typeof EventTarget === 'function' && self instanceof EventTarget,
        hasOwn: typeof self.hasOwnProperty === 'function' && self.hasOwnProperty('postMessage'),
    });
};
