// Preemptible Worker.terminate() (infinite JS loop + Atomics.wait) and
// immediate Atomics.waitAsync idle wakeups (notify + timeout).
function pumpUntil(pred, ms) {
    const deadline = Date.now() + (ms || 10000);
    while (!pred() && Date.now() < deadline) {
        advanceTime(16);
        wallSleep(2);
    }
}

// 1. Preempt Worker stuck in an infinite JS loop.
{
    const sab = new SharedArrayBuffer(32);
    const sig = new Int32Array(sab);
    const w = new Worker('../workers/worker_preempt_and_wake.js');
    w.postMessage({ cmd: 'spin', sab });
    pumpUntil(() => Atomics.load(sig, 0) === 1, 5000);
    assert(Atomics.load(sig, 0) === 1, 'spin worker entered loop');
    const t0 = Date.now();
    w.terminate();
    const elapsed = Date.now() - t0;
    assert(elapsed < 1000, 'terminate() preempted infinite JS loop quickly (' + elapsed + ' ms)');
}

// 2. Preempt Worker blocked indefinitely in Atomics.wait().
{
    const sab = new SharedArrayBuffer(32);
    const sig = new Int32Array(sab);
    const w = new Worker('../workers/worker_preempt_and_wake.js');
    w.postMessage({ cmd: 'wait-forever', sab });
    pumpUntil(() => Atomics.load(sig, 0) === 1, 5000);
    wallSleep(10);
    const t0 = Date.now();
    w.terminate();
    const elapsed = Date.now() - t0;
    assert(elapsed < 1000, 'terminate() woke and joined Atomics.wait worker promptly (' + elapsed + ' ms)');
}

// 3. Immediate wakeup of idle Worker awaiting Atomics.waitAsync on Atomics.notify().
{
    const sab = new SharedArrayBuffer(32);
    const sig = new Int32Array(sab);
    const w = new Worker('../workers/worker_preempt_and_wake.js');
    const msgs = [];
    w.onmessage = (e) => msgs.push(e.data);

    w.postMessage({ cmd: 'arm-wait-async', sab, slot: 2 });
    pumpUntil(() => msgs.some((m) => m.cmd === 'armed' && m.slot === 2), 5000);
    wallSleep(40); // ensure worker has parked in its idle condvar wait

    const t0 = Date.now();
    const woken = Atomics.notify(sig, 2, 1);
    assert(woken === 1, 'notify woke idle async waiter, got ' + woken);
    pumpUntil(() => msgs.some((m) => m.cmd === 'async-done' && m.slot === 2), 2000);
    const reply = msgs.find((m) => m.cmd === 'async-done' && m.slot === 2);
    assert(reply && reply.status === 'ok', 'idle waitAsync resolved ok: ' + JSON.stringify(reply));
    const latency = Date.now() - t0;
    assert(latency < 150, 'idle Worker woke immediately without 500ms sleep stall (' + latency + ' ms)');

    // 4. Prompt finite timeout expiry on idle Worker awaiting Atomics.waitAsync().
    w.postMessage({ cmd: 'arm-wait-async', sab, slot: 3, timeoutMs: 40 });
    pumpUntil(() => msgs.some((m) => m.cmd === 'armed' && m.slot === 3), 5000);
    const t1 = Date.now();
    pumpUntil(() => msgs.some((m) => m.cmd === 'async-done' && m.slot === 3), 2000);
    const timeoutReply = msgs.find((m) => m.cmd === 'async-done' && m.slot === 3);
    assert(timeoutReply && timeoutReply.status === 'timed-out',
           'idle waitAsync timed out: ' + JSON.stringify(timeoutReply));
    const timeoutMs = Date.now() - t1;
    assert(timeoutMs < 250, 'idle Worker waitAsync timeout folded into idle wait (' + timeoutMs + ' ms)');

    w.terminate();
}

console.log('test_worker_preempt_and_wake OK');
