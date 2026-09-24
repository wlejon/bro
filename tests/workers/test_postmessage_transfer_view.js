// postMessage / structuredClone with a TypedArray view in the payload whose
// buffer is in the transfer list: valid on the web. The value is serialized
// first and the buffer detached after, so the view arrives intact and the
// sender's buffer (and view) end up detached. It used to throw DataCloneError
// ("Cannot clone TypedArray with detached buffer") because the buffer was
// detached before the payload was walked.

// ---- structuredClone ---------------------------------------------------------
{
    const ab = new ArrayBuffer(8);
    const v = new Uint8Array(ab, 2, 4);
    v.set([1, 2, 3, 4]);
    const out = structuredClone({ v, ab }, { transfer: [ab] });
    assert(ab.byteLength === 0 && v.length === 0, 'sender buffer and view detached');
    assert(out.v.length === 4 && out.v.byteOffset === 2, 'view keeps offset and length');
    assert(Array.from(out.v).join() === '1,2,3,4', 'view bytes: ' + Array.from(out.v).join());
    assert(out.v.buffer === out.ab, 'the view shares the transferred buffer in the clone');

    // View only: the buffer is reached through the view alone.
    const ab2 = new ArrayBuffer(4);
    const v2 = new Int16Array(ab2);
    v2.set([-7, 9]);
    const out2 = structuredClone({ v: v2 }, { transfer: [ab2] });
    assert(ab2.byteLength === 0, 'view-only payload still detaches the listed buffer');
    assert(out2.v instanceof Int16Array && out2.v[0] === -7 && out2.v[1] === 9, 'view-only payload arrives');

    // A clone error detaches nothing.
    const ab3 = new ArrayBuffer(4);
    let threw = false;
    try { structuredClone({ v: new Uint8Array(ab3), f() {} }, { transfer: [ab3] }); }
    catch (e) { threw = true; }
    assert(threw, 'a function is still not cloneable');
    assert(ab3.byteLength === 4, 'a failed clone leaves the transfer list attached');
}

// ---- a secondary window ------------------------------------------------------
{
    const win = bro.window.open('multiwin_msg', { width: 80, height: 40 });
    flush();
    let reply = null;
    win.addEventListener('message', (ev) => { reply = ev.data; });
    const ab = new ArrayBuffer(16);
    const v = new Uint8Array(ab, 4, 4);
    v.set([10, 20, 30, 7]);
    win.postMessage({ buf: v }, [v.buffer]);
    assert(ab.byteLength === 0, 'window: sender buffer detached');
    flush();
    assert(reply && reply.kind === 'sum', 'window: child replied ' + JSON.stringify(reply));
    assert(reply.len === 4 && reply.sum === 67, 'window: view arrived intact ' + JSON.stringify(reply));
    win.close();
    flush();
}

// ---- a Worker ----------------------------------------------------------------
{
    const w = new Worker('../workers/worker_transfer_view.js');
    let got = null;
    w.onmessage = (e) => { got = e.data; };
    const ab = new ArrayBuffer(12);
    const v = new Uint16Array(ab, 4, 3);
    v.set([500, 600, 700]);
    // The buffer appears first as itself, then through the view.
    w.postMessage({ raw: ab, v }, [ab]);
    assert(ab.byteLength === 0, 'worker: sender buffer detached');
    const deadline = Date.now() + 15000;
    while (got === null && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
    assert(got !== null, 'worker replied');
    assert(got.vals.join() === '500,600,700' && got.offset === 4,
           'worker: view arrived intact ' + JSON.stringify(got));
    assert(got.rawLen === 12, 'worker: the buffer itself arrived ' + JSON.stringify(got));

    got = null;
    const ab2 = new ArrayBuffer(4);
    const v2 = new Uint8Array(ab2);
    v2.set([1, 2, 3, 4]);
    w.postMessage({ v: v2 }, [ab2]);
    assert(ab2.byteLength === 0, 'worker: a buffer reached only through a view is detached');
    const deadline2 = Date.now() + 15000;
    while (got === null && Date.now() < deadline2) { advanceTime(16); wallSleep(2); }
    assert(got && got.vals.join() === '1,2,3,4', 'worker: view-only payload arrived ' + JSON.stringify(got));
    w.terminate();
}

console.log('test_postmessage_transfer_view: OK');
