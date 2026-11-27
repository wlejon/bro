// Worker for test_shared_array_buffer.js: exercises shared memory mutations,
// Atomics.wait/notify wakeups, concurrent Atomics.add, and waitAsync wakeup.

onmessage = (e) => {
    const msg = e.data;
    if (msg.cmd === 'identity') {
        const view = msg.view;
        const ok = msg.sab instanceof SharedArrayBuffer &&
                   view instanceof Int32Array &&
                   view.buffer === msg.sab &&
                   msg.sab === msg.sameSab &&
                   msg.sab.byteLength === 64 &&
                   Atomics.load(view, 0) === 42;
        Atomics.store(view, 1, 99);
        postMessage({ cmd: 'identity-reply', ok: ok });
        return;
    }

    if (msg.cmd === 'wait-for-main') {
        const view = new Int32Array(msg.sab);
        // Signal readiness on slot 2, then block until main writes non-zero and notifies.
        Atomics.store(view, 2, 1);
        const res = Atomics.wait(view, 3, 0, 5000);
        const seen = Atomics.load(view, 4);
        postMessage({ cmd: 'wait-done', result: res, seen: seen });
        return;
    }

    if (msg.cmd === 'concurrent-add') {
        const view = new Int32Array(msg.sab);
        const iters = msg.iters;
        Atomics.store(view, 5, 1);
        for (let i = 0; i < iters; ++i) {
            Atomics.add(view, 6, 1);
        }
        postMessage({ cmd: 'add-done' });
        return;
    }

    if (msg.cmd === 'notify-main-waiter') {
        const view = new Int32Array(msg.sab);
        Atomics.store(view, 7, 123);
        const woken = Atomics.notify(view, 7, 1);
        postMessage({ cmd: 'notified', woken: woken });
        return;
    }

    if (msg.cmd === 'notify-only') {
        const view = new Int32Array(msg.sab);
        Atomics.notify(view, msg.index, 1);
        return;
    }

    if (msg.cmd === 'fill-floats') {
        const f32 = new Float32Array(msg.sab, 32, 4);
        f32[0] = 1.5;
        f32[1] = 2.5;
        f32[2] = 3.5;
        f32[3] = 4.5;
        postMessage({ cmd: 'floats-ready' });
        return;
    }
};
