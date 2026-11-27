// Worker helper for test_worker_preempt_and_wake.js.
onmessage = (e) => {
    const msg = e.data;
    const sig = new Int32Array(msg.sab);

    if (msg.cmd === 'spin') {
        Atomics.store(sig, 0, 1);
        while (true) {}
    }

    if (msg.cmd === 'wait-forever') {
        Atomics.store(sig, 0, 1);
        Atomics.wait(sig, 1, 0);
        return;
    }

    if (msg.cmd === 'arm-wait-async') {
        const res = Atomics.waitAsync(sig, msg.slot, 0, msg.timeoutMs);
        res.value.then((status) => {
            postMessage({ cmd: 'async-done', slot: msg.slot, status, at: Date.now() });
        });
        postMessage({ cmd: 'armed', slot: msg.slot });
        return;
    }
};
