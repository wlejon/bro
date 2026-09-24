// Worker postMessage keeps ArrayBuffer identity within one message: views
// over one buffer arrive over one buffer, and a buffer posted beside a view
// of it arrives as that view's buffer — as structuredClone does. Each view
// used to carry its own copy of the bytes, so a write through one was not
// seen by the other.

const w = new Worker('../workers/worker_buffer_identity.js');
let got = null;
w.onmessage = (e) => { got = e.data; };

function waitReply() {
    const deadline = Date.now() + 15000;
    while (got === null && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
    assert(got !== null, 'worker replied');
}

// Two views of one buffer, plus the buffer itself, a DataView of it, and a
// view of an unrelated buffer.
{
    const b = new ArrayBuffer(8);
    w.postMessage({ a: new Uint8Array(b), c: new Uint8Array(b, 2), raw: b,
                    dv: new DataView(b, 1, 3), other: new Uint8Array(4) });
    waitReply();
    assert(got.same === true, 'two views share one buffer (' + JSON.stringify(got) + ')');
    assert(got.seen === 0, 'c starts at byte 2, so it reads 0 there');
    assert(got.rawSame === true, 'the buffer posted beside its view is that view\'s buffer');
    assert(got.dvSame === true, 'a DataView shares it too');
    assert(got.offsets.join() === '0,2', 'offsets kept (' + got.offsets + ')');
    assert(got.other === true, 'an unrelated buffer stays separate');
}

// A write through one view is seen through the other, and the reply (worker
// to page) keeps identity the same way.
{
    got = null;
    const b = new ArrayBuffer(4);
    w.postMessage({ a: new Uint8Array(b), c: new Uint8Array(b) });
    waitReply();
    assert(got.same === true && got.seen === 42, 'write through a seen through c (' + got.seen + ')');
    assert(got.x.buffer === got.y.buffer, 'the reply\'s views share one buffer');
    assert(got.x.buffer.byteLength === 8 && got.y.byteOffset === 4, 'with their offsets');
}

// Transferred: two views over one transferred buffer arrive over one buffer.
{
    got = null;
    const b = new ArrayBuffer(4);
    w.postMessage({ a: new Uint8Array(b), c: new Uint8Array(b) }, [b]);
    assert(b.byteLength === 0, 'the transferred buffer is detached');
    waitReply();
    assert(got.same === true && got.seen === 42, 'transferred views share one buffer');
}

w.terminate();
console.log('test_postmessage_buffer_identity: OK');
