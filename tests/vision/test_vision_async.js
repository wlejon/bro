// bro.vision heavy ops run off the JS thread
// (src/bronze_host/host_vision_jobs.cpp).
//
// The old QuickJS binding took an `onDone` callback, ran the model on a
// worker thread and delivered the result from the engine's frame pump; the
// bronze port made every op synchronous, so a depth pass blocked the frame.
// This checks the restored contract:
//
//   * no onDone  -> the result, synchronously;
//   * onDone     -> a handle with cancel() and `done`, and onDone(result,
//                   info) later, on the JS thread, from the frame pump, with
//                   the compute itself on another thread;
//   * the two paths produce the SAME result — the reason the sync path runs
//     the same compute and the same builder rather than a second copy;
//   * one op at a time per model, and the model is free again inside onDone;
//   * cancel() delivers (null, { cancelled: true });
//   * a compute that throws is a thrown Error synchronously and
//     info.error asynchronously.
//
// WEIGHTS-FREE. Every real vision op needs a checkpoint on disk (brovisionml's
// loaders refuse a directory without one, and the ops refuse an unloaded
// model), so the machine is driven through `__host.visionProbe` — the headless
// test hook that runs a synthetic compute through the very same runVisionOp
// the model ops use. What the models add on top is their compute, which is
// brovisionml's to test.

// Wall-clock budgets. A wait returns as soon as its condition holds, so a
// long budget costs nothing when things work; it only has to outlast a slow
// run (BRONZE_GC_STRESS collects on every allocation, on both threads).
const WAIT_MS = 120000;
// A hold long enough that only cancel() can end it within the wait above.
const CANCEL_HOLD_MS = 600000;

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}

// Run frames until `pred()` or the wall clock runs out. advanceTime() moves
// VIRTUAL time, so it returns instantly and would spin past a worker that has
// not finished; the real sleep between steps is what gives the worker thread
// wall time to run. Date.now() is the real clock (performance.now() is
// virtual under advanceTime).
function pump(pred, timeoutMs, what) {
    const t0 = Date.now();
    while (Date.now() - t0 < timeoutMs) {
        advanceTime(16);
        if (pred()) return true;
        wallSleep(2);
    }
    assert(false, what + ' did not complete within ' + timeoutMs + 'ms');
    return false;
}

function assertSameResult(a, b, what) {
    const ka = Object.keys(a).sort().filter(k => k !== 'onWorker');
    const kb = Object.keys(b).sort().filter(k => k !== 'onWorker');
    assert(ka.join(',') === kb.join(','),
           what + ': same keys (' + ka.join(',') + ' vs ' + kb.join(',') + ')');
    for (const k of ka) {
        const va = a[k], vb = b[k];
        if (va instanceof ImageBitmap) {
            assert(vb instanceof ImageBitmap, what + '.' + k + ' is a bitmap on both sides');
            assert(va.width === vb.width && va.height === vb.height,
                   what + '.' + k + ' bitmaps agree on size');
        } else if (typeof va === 'number') {
            assert(va === vb, what + '.' + k + ' is ' + va + ' on both sides, got ' + vb);
        } else if (va && typeof va.length === 'number') {
            assert(vb && vb.length === va.length, what + '.' + k + ' planes agree on length');
            for (let i = 0; i < va.length; i++) {
                if (va[i] !== vb[i]) {
                    assert(false, what + '.' + k + '[' + i + ']: ' + va[i] + ' vs ' + vb[i]);
                    break;
                }
            }
        }
    }
}

assert(typeof bro === 'object', 'bro global exists');
assert(bro.vision !== undefined && bro.vision !== null, 'bro.vision namespace exists');

if (bro.vision.available === false || typeof __host.visionProbe !== 'function') {
    console.log('bro.vision is compiled out; no job machine to drive');
} else {
    const probe = __host.visionProbe;
    const W = 10, H = 8;

    // ── sync form ───────────────────────────────────────────────────────────
    const sync = probe(W, H);
    assert(sync.width === W && sync.height === H, 'sync result has the size');
    assert(sync.onWorker === false, 'the sync form computes on the calling thread');
    assert(sync.plane instanceof Float32Array && sync.plane.length === W * H,
           'sync plane is a Float32Array of w*h');

    // ── the handle, the callback, and the thread ────────────────────────────
    {
        let got = null, info = null, calls = 0;
        const handle = probe(W, H, { holdMs: 30, onDone(r, i) { got = r; info = i; calls++; } });
        assert(handle !== null && typeof handle === 'object', 'the onDone form returns a handle');
        assert(typeof handle.cancel === 'function', 'the handle has cancel()');
        assert(handle.done === false, 'handle.done is false while the worker holds');
        assert(got === null && calls === 0, 'onDone does not fire inside the call');

        pump(() => calls > 0, WAIT_MS, 'probe onDone');
        assert(calls === 1, 'onDone fires exactly once, got ' + calls);
        assert(handle.done === true, 'handle.done is true once settled');
        assert(info !== null && info.cancelled === false, 'info.cancelled is false');
        assert(info.error === undefined, 'info carries no error: ' + info.error);
        assert(got !== null, 'onDone received a result');
        assert(got.onWorker === true, 'the async compute ran on another thread');

        // The point of running one compute and one builder for both paths.
        assertSameResult(sync, got, 'sync vs async');

        // Nothing fires twice on later frames.
        for (let i = 0; i < 5; i++) advanceTime(16);
        assert(calls === 1, 'onDone stays at one call after more frames');
    }

    // ── one op at a time per model ──────────────────────────────────────────
    {
        let calls = 0;
        probe(W, H, { holdMs: 50, onDone() { calls++; } });
        const err = expectThrows(() => probe(W, H), 'a second op while one is in flight');
        assert(String(err.message).includes('already in flight'),
               'the busy error says so: ' + err.message);
        pump(() => calls > 0, WAIT_MS, 'held probe onDone');
        // ...and the model is free again as soon as the callback has run.
        assert(probe(W, H).width === W, 'the model is usable again after the job settles');
    }

    // ── the model is released BEFORE onDone: step i can start step i+1 ─────
    {
        let chained = null, calls = 0;
        probe(W, H, {
            onDone() {
                calls++;
                chained = probe(W, H, { onDone() { calls++; } });
            }
        });
        pump(() => calls >= 2, WAIT_MS, 'chained probe');
        assert(chained !== null && typeof chained.cancel === 'function',
               'onDone could launch the next op on the same model');
    }

    // ── cancel ──────────────────────────────────────────────────────────────
    {
        let got = 'unset', info = null, calls = 0;
        const handle = probe(W, H, { holdMs: CANCEL_HOLD_MS, onDone(r, i) { got = r; info = i; calls++; } });
        const t0 = Date.now();
        handle.cancel();
        pump(() => calls > 0, WAIT_MS, 'cancelled probe onDone');
        assert(Date.now() - t0 < CANCEL_HOLD_MS / 2, 'cancel() cut the worker short');
        assert(info.cancelled === true, 'a cancelled job reports cancelled');
        assert(got === null, 'a cancelled job delivers a null result, got ' + got);
        assert(probe(W, H).width === W, 'the model is free after a cancel');
    }

    // ── a throwing compute ──────────────────────────────────────────────────
    {
        const e = expectThrows(() => probe(W, H, { fail: 'probe exploded' }), 'sync failure');
        assert(String(e.message).includes('probe exploded'), 'sync error carries the message: ' + e.message);
        assert(probe(W, H).width === W, 'a sync failure releases the model');

        let got = 'unset', info = null, calls = 0;
        probe(W, H, { fail: 'async exploded', onDone(r, i) { got = r; info = i; calls++; } });
        pump(() => calls > 0, WAIT_MS, 'failed probe onDone');
        assert(got === null, 'a failed job delivers a null result');
        assert(info.cancelled === false, 'a failed job is not cancelled');
        assert(String(info.error).includes('async exploded'), 'info.error carries the message: ' + info.error);
        assert(probe(W, H).width === W, 'an async failure releases the model');
    }

    // ── a throwing onDone is reported, not fatal, and the model is free ─────
    {
        let calls = 0;
        probe(W, H, { onDone() { calls++; throw new Error('from onDone'); } });
        pump(() => calls > 0, WAIT_MS, 'throwing onDone');
        assert(probe(W, H).width === W, 'the model is free after a throwing onDone');
    }

    // ── the model ops refuse a model with no weights up front ───────────────
    // (the loaders already refuse a directory without a checkpoint, which is
    // why the machine above is driven through the probe)
    {
        const e = expectThrows(() => bro.vision.loadDepth('.'), 'loadDepth of a weightless dir');
        assert(/loadDepth failed/.test(String(e.message)), 'loadDepth says what failed: ' + e.message);
    }

    console.log('bro.vision async jobs OK (weights-free)');
}
