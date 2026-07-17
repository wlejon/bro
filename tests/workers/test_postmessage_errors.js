// Error paths of the structured-clone serializer (src/js/message_serializer.cpp)
// via the Worker postMessage surface. The bro.net sendClone surface exercises
// the same paths in tests/net/test_net_channels_clone.js; this covers the
// worker channel and the worker-side (worker -> main) writer.
//
// The serializer caps recursion at depth 64 (both writer and reader) and
// rejects functions with a TypeError instead of silently cloning them as {}.
// Circular references are NOT supported: the cycle re-descends until the depth
// cap trips, so they surface as the depth TypeError — never a hang or a stack
// overflow.

const workerPath = '../workers/worker_clone_errors.js';
const w = new Worker(workerPath);

let got = null;
w.onmessage = (e) => { got = e.data; };

function waitReply(what) {
    let waited = 0;
    while (got === null && waited < 5000) { advanceTime(16); waited += 16; }
    assert(got !== null, 'reply received for ' + what);
    const r = got;
    got = null;
    return r;
}

function expectTypeError(fn, what) {
    let threw = null;
    try { fn(); } catch (e) { threw = e; }
    assert(threw instanceof TypeError,
           what + ' rejected with TypeError (got ' + threw + ')');
    return threw;
}

// =========================================================================
// main -> worker: function anywhere in the graph throws TypeError
// =========================================================================
expectTypeError(() => w.postMessage(() => 1), 'bare function');
expectTypeError(() => w.postMessage({ fn: () => 1 }), 'function in object');
expectTypeError(() => w.postMessage({ arr: [1, { deep: () => 1 }] }), 'function nested in array');

// =========================================================================
// main -> worker: nesting beyond the depth limit throws TypeError
// =========================================================================
{
    let deep = { leaf: true };
    for (let i = 0; i < 100; i++) deep = { next: deep };
    const err = expectTypeError(() => w.postMessage(deep), 'over-deep object nesting');
    assert(/deeply nested/.test(err.message),
           'depth error names the nesting cause (' + err.message + ')');
}
{
    let deep = [true];
    for (let i = 0; i < 100; i++) deep = [deep];
    expectTypeError(() => w.postMessage(deep), 'over-deep array nesting');
}

// =========================================================================
// main -> worker: circular references trip the depth cap (not supported)
// =========================================================================
{
    const a = { name: 'a' };
    a.self = a;
    const err = expectTypeError(() => w.postMessage(a), 'self-referencing object');
    assert(/deeply nested/.test(err.message),
           'circular ref surfaces as the depth TypeError (' + err.message + ')');
}
{
    const x = { name: 'x' }, y = { name: 'y', x };
    x.y = y;  // two-object cycle
    expectTypeError(() => w.postMessage({ x }), 'two-object cycle');
    const arr = [1, 2];
    arr.push(arr);
    expectTypeError(() => w.postMessage(arr), 'self-referencing array');
}

// =========================================================================
// Depth just under the limit round-trips main -> worker -> main intact
// =========================================================================
{
    let ok = { leaf: 60 };
    for (let i = 0; i < 60; i++) ok = { next: ok };
    w.postMessage({ cmd: 'echo', payload: ok });
    const r = waitReply('depth-60 echo');
    assert(r.depth === 60, 'worker saw 60 nesting hops (got ' + r.depth + ')');
    let cur = r.echo, hops = 0;
    while (cur && cur.next) { cur = cur.next; hops++; }
    assert(hops === 60 && cur.leaf === 60,
           'depth-60 payload round-trips intact (hops=' + hops + ')');
}

// =========================================================================
// worker -> main: the worker-side serializer rejects the same way
// =========================================================================
{
    w.postMessage({ cmd: 'postFunction' });
    const r = waitReply('worker-side function post');
    assert(r.result === 'threw', 'worker postMessage(function) threw (got ' + r.result + ')');
    assert(r.name === 'TypeError', 'worker-side rejection is a TypeError (got ' + r.name + ')');
}
{
    w.postMessage({ cmd: 'postDeep', depth: 100 });
    const r = waitReply('worker-side deep post');
    assert(r.result === 'threw', 'worker postMessage(100-deep) threw (got ' + r.result + ')');
    assert(r.name === 'TypeError', 'worker-side depth rejection is a TypeError (got ' + r.name + ')');
    assert(/deeply nested/.test(r.message),
           'worker-side depth error names the cause (' + r.message + ')');
}

// The channel is still healthy after all the rejected posts.
{
    w.postMessage({ cmd: 'echo', payload: { fine: 1 } });
    const r = waitReply('post-rejection echo');
    assert(r.echo && r.echo.fine === 1, 'worker channel still works after rejections');
}

w.terminate();
console.log('postmessage error-path tests passed');
