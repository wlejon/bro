// bro.vision heavy ops run off the JS thread again
// (src/bronze_host/host_vision_jobs.cpp).
//
// The old QuickJS binding took an `onDone` callback, ran the model on a
// worker thread and delivered the result from the engine's frame pump; the
// bronze port made every op synchronous, so a depth pass blocked the frame.
// This checks the restored contract:
//
//   * no onDone  -> the result, synchronously, exactly as before;
//   * onDone     -> an AsyncHandle with cancel(), and onDone(result, info)
//                   later, on the JS thread, from the frame pump;
//   * the two paths produce the SAME result — the whole reason the sync path
//     runs the same compute and the same builder rather than a second copy;
//   * one op at a time per model;
//   * cancel() delivers (null, { cancelled: true }).
//
// WEIGHTS-FREE: the loaders are handed '.', which exists and holds no
// checkpoint, so each op runs its probe answer. The job machine does not care
// what the worker computed — only that it computed it on another thread and
// that the callback arrived here.

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}

function makeImage(w, h) {
    const data = new Uint8Array(w * h * 4);
    for (let i = 0; i < w * h; i++) {
        data[4 * i + 0] = (i * 31) & 255;
        data[4 * i + 1] = (i * 17) & 255;
        data[4 * i + 2] = (i * 3) & 255;
        data[4 * i + 3] = 255;
    }
    return { width: w, height: h, data };
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
        sleep(2);
    }
    assert(false, what + ' did not complete within ' + timeoutMs + 'ms');
    return false;
}

// Key sets and every typed-array / number leaf, compared. ImageBitmaps
// compare by size: two runs of the same compute rasterize the same pixels,
// but the bitmap objects are necessarily distinct.
function assertSameResult(a, b, what) {
    const ka = Object.keys(a).sort();
    const kb = Object.keys(b).sort();
    assert(ka.join(',') === kb.join(','),
           what + ': same keys (' + ka.join(',') + ' vs ' + kb.join(',') + ')');
    for (const k of ka) {
        const va = a[k], vb = b[k];
        if (va instanceof ImageBitmap) {
            assert(vb instanceof ImageBitmap, what + '.' + k + ' is a bitmap on both sides');
            assert(va.width === vb.width && va.height === vb.height,
                   what + '.' + k + ' bitmaps agree on size');
        } else if (va === null) {
            assert(vb === null, what + '.' + k + ' is null on both sides');
        } else if (typeof va === 'number') {
            assert(va === vb, what + '.' + k + ' is ' + va + ' on both sides, got ' + vb);
        } else if (Array.isArray(va)) {
            assert(Array.isArray(vb) && va.length === vb.length,
                   what + '.' + k + ' arrays agree on length');
        } else if (va && typeof va.length === 'number') {
            assert(vb && vb.length === va.length,
                   what + '.' + k + ' planes agree on length');
            for (let i = 0; i < va.length; i++) {
                if (va[i] !== vb[i]) {
                    assert(false, what + '.' + k + '[' + i + ']: ' + va[i] + ' vs ' + vb[i]);
                }
            }
        }
    }
}

assert(typeof bro === 'object', 'bro global exists');
assert(bro.vision !== undefined && bro.vision !== null, 'bro.vision namespace exists');

if (bro.vision.available === false) {
    console.log('bro.vision is the unavailable stub; no jobs to run');
} else {
    const W = 10, H = 8;
    const img = makeImage(W, H);
    const dir = '.';

    // ── the handle, the callback, and the thread ────────────────────────────
    {
        const depth = bro.vision.loadDepth(dir);

        const sync = depth.estimate(img);
        assert(sync.width === W, 'the no-callback form still returns the result');

        let got = null, info = null, calls = 0;
        const handle = depth.estimate(img, {
            onDone(r, i) { got = r; info = i; calls++; }
        });
        assert(handle !== null && typeof handle === 'object',
               'the onDone form returns a handle');
        assert(typeof handle.cancel === 'function', 'the handle has cancel()');
        assert(got === null, 'onDone does not fire inside the call');

        pump(() => calls > 0, 5000, 'depth.estimate onDone');
        assert(calls === 1, 'onDone fires exactly once, got ' + calls);
        assert(info !== null && info.cancelled === false, 'info.cancelled is false');
        assert(info.error === undefined, 'info carries no error: ' + info.error);
        assert(got !== null, 'onDone received a result');

        // The point of running one compute and one builder for both paths.
        assertSameResult(sync, got, 'depth sync vs async');
    }

    // ── one op at a time per model ──────────────────────────────────────────
    {
        const hed = bro.vision.loadHed(dir);
        let calls = 0;
        hed.detect(img, { onDone() { calls++; } });
        const err = expectThrows(() => hed.detect(img, { onDone() {} }),
                                 'a second op while one is in flight');
        assert(String(err.message).includes('already in flight'),
               'the busy error says so: ' + err.message);
        pump(() => calls > 0, 5000, 'hed.detect onDone');
        // ...and the model is free again as soon as the callback has run.
        const after = hed.detect(img);
        assert(after.width === W, 'the model is usable again after the job settles');
    }

    // ── cancel ──────────────────────────────────────────────────────────────
    {
        const lineart = bro.vision.loadLineart(dir);
        let got = 'unset', info = null, calls = 0;
        const handle = lineart.detect(img, {
            onDone(r, i) { got = r; info = i; calls++; }
        });
        handle.cancel();
        pump(() => calls > 0, 5000, 'cancelled lineart.detect onDone');
        assert(info.cancelled === true, 'a cancelled job reports cancelled');
        assert(got === null, 'a cancelled job delivers a null result, got ' + got);
        // Cancelling still releases the model.
        assert(lineart.detect(img).width === W, 'the model is free after a cancel');
    }

    // ── every wrapped family takes the callback ─────────────────────────────
    {
        const cases = [
            ['normal',     bro.vision.loadNormal(dir),    m => (o) => m.estimate(img, o)],
            ['mlsd',       bro.vision.loadMlsd(dir),      m => (o) => m.detect(img, o)],
            ['openpose',   bro.vision.loadOpenpose(dir),  m => (o) => m.detect(img, o)],
            ['segformer',  bro.vision.loadSegformer(dir), m => (o) => m.detect(img, o)],
            ['birefnet',   bro.vision.loadBirefnet(dir),  m => (o) => m.removeBackground(img, o)],
        ];
        for (const [name, model, mk] of cases) {
            const call = mk(model);
            const sync = call(undefined);
            let got = null, calls = 0;
            call({ onDone(r) { got = r; calls++; } });
            pump(() => calls > 0, 5000, name + ' onDone');
            assert(got !== null, name + ' async delivered a result');
            assertSameResult(sync, got, name + ' sync vs async');
        }
    }

    // ── SAM: setImage and segmentEverything are the expensive halves ────────
    {
        const sam = bro.vision.loadSam(dir);
        let calls = 0;
        sam.setImage(img, { onDone() { calls++; } });
        pump(() => calls > 0, 5000, 'sam.setImage onDone');
        assert(sam.hasImage === true, 'setImage recorded the frame');

        const sync = sam.segmentEverything(img);
        let got = null;
        calls = 0;
        sam.segmentEverything(img, { onDone(r) { got = r; calls++; } });
        pump(() => calls > 0, 5000, 'sam.segmentEverything onDone');
        assertSameResult(sync, got, 'segmentEverything sync vs async');
    }

    // ── StyleGAN3 ───────────────────────────────────────────────────────────
    {
        const gan = bro.vision.loadStyleGAN3(dir, { resolution: 256 });
        const sync = gan.generate({ seed: 3 });
        let got = null, calls = 0;
        gan.generate({ seed: 3, onDone(r) { got = r; calls++; } });
        pump(() => calls > 0, 10000, 'stylegan3.generate onDone');
        assertSameResult(sync, got, 'stylegan3 generate sync vs async');
    }

    console.log('bro.vision async jobs OK (weights-free)');
}
