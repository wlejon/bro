// Channels (GNS lanes) + structured-value messaging + wire-format robustness.
//
// Single-context self-connect (same pattern as tests/net_roundtrip.js): this
// context hosts and connects to itself, so one subscriber owns both ends of
// the link and every send arrives back at our own onmessage. Covers:
//   - raw send/receive regression (string + binary, legacy boolean arg)
//   - channel roundtrip on onmessage (send on channel N, receive channel N)
//   - reliable ordering preserved within a channel
//   - sendClone deep roundtrip of a nested structured value
//   - clone rejection of Mesh / ImageBitmap / functions / transfer lists
//   - malformed-frame robustness: crafted hostile payloads (bad magic,
//     unknown frame type, truncated clone, deep-nesting bomb, pointer-slot
//     tags) are dropped with a diagnostic, never crash, never reach onmessage
//   - broadcastClone reaches every connection
//   - unreliable + nodelay delivery

const PORT = 27460 + (Date.now() % 400);

function pump(realMs, virtualMs) {
    wallSleep(realMs);
    advanceTime(virtualMs);
}

// Collect everything that reaches onmessage.
let received = [];
bro.net.onmessage = (conn, data, channel) => {
    received.push({ conn, data, channel });
};

const conns = [];
bro.net.onconnect = (conn) => conns.push(conn);

assert(bro.net.host(PORT) !== false, 'host() accepted');
for (let i = 0; i < 200 && !bro.net.isHosting(); i++) pump(10, 10);
assert(bro.net.isHosting(), 'hosting on port ' + PORT);

bro.net.connect('127.0.0.1:' + PORT);
// Self-connect: onconnect fires for BOTH ends (accepted + outgoing).
for (let i = 0; i < 400 && conns.length < 2; i++) pump(10, 10);
assert(conns.length === 2, 'both ends connected (got ' + conns.length + ')');
const connA = conns[0];

// Drain until `received` has at least n messages (reliable sends only).
function waitFor(n, ms = 5000) {
    for (let t = 0; t < ms && received.length < n; t += 10) pump(10, 10);
    return received.length >= n;
}

// --- 1. Raw send regression: legacy boolean third arg, channel defaults to 0.
received = [];
bro.net.send(connA, 'legacy-raw', true);
assert(waitFor(1), 'legacy raw send arrived');
assert(received[0].data instanceof ArrayBuffer, 'raw payload is an ArrayBuffer');
assert(new TextDecoder().decode(received[0].data) === 'legacy-raw', 'raw bytes intact');
assert(received[0].channel === 0, 'default channel is 0');

// --- 2. Channel roundtrip: send binary on channel 5, receive channel 5.
received = [];
const bin = new Uint8Array([0xde, 0xad, 0xbe, 0xef]);
bro.net.send(connA, bin, { reliable: true, channel: 5 });
assert(waitFor(1), 'channel-5 send arrived');
assert(received[0].channel === 5, 'message carries channel 5 (got ' + received[0].channel + ')');
{
    const got = new Uint8Array(received[0].data);
    assert(got.length === 4 && got[0] === 0xde && got[3] === 0xef, 'binary payload intact');
}

// --- 3. Out-of-range channel clamps to 7 (top lane), never fails the send.
received = [];
bro.net.send(connA, 'clamped', { channel: 42 });
assert(waitFor(1), 'clamped-channel send arrived');
assert(received[0].channel === 7, 'channel 42 clamped to 7 (got ' + received[0].channel + ')');

// --- 4. Reliable ordering within a channel: N numbered messages, in order.
received = [];
const N = 300;
for (let i = 0; i < N; i++) {
    const b = new Uint8Array(4);
    new DataView(b.buffer).setUint32(0, i, true);
    bro.net.send(connA, b, { reliable: true, channel: 3 });
}
assert(waitFor(N, 15000), 'all ' + N + ' ordered messages arrived (got ' + received.length + ')');
for (let i = 0; i < N; i++) {
    const v = new DataView(received[i].data).getUint32(0, true);
    assert(v === i, 'message ' + i + ' in order (got ' + v + ')');
    assert(received[i].channel === 3, 'ordered message on channel 3');
}

// --- 5. sendClone: deep structured roundtrip.
const bigStr = 'x'.repeat(10 * 1024);
const value = {
    a: 1,
    s: 'str',
    f: 3.25,
    yes: true,
    no: false,
    nil: null,
    big: bigStr,
    bignum: 123456789012345678901234567890n,
    arr: [1, 2, 3, 'four', [5, { six: 6 }]],
    f32: new Float32Array([0.5, -1.25, 3e7]),
    bytes: new Uint8Array([9, 8, 7]).buffer,
    nested: { deep: { deeper: { n: 42 } } },
};
received = [];
assert(bro.net.sendClone(connA, value, { channel: 2 }) === true, 'sendClone queued');
assert(waitFor(1), 'clone arrived');
{
    const v = received[0].data;
    assert(received[0].channel === 2, 'clone carries channel 2');
    assert(!(v instanceof ArrayBuffer), 'clone payload is a decoded value');
    assert(v.a === 1 && v.s === 'str' && v.f === 3.25, 'scalars roundtrip');
    assert(v.yes === true && v.no === false && v.nil === null, 'bool/null roundtrip');
    assert(v.big === bigStr, '10KB string roundtrips');
    assert(v.bignum === 123456789012345678901234567890n, 'BigInt roundtrips');
    assert(Array.isArray(v.arr) && v.arr.length === 5 && v.arr[3] === 'four'
           && v.arr[4][1].six === 6, 'nested array roundtrips');
    assert(v.f32 instanceof Float32Array && v.f32.length === 3
           && v.f32[0] === 0.5 && v.f32[1] === -1.25 && v.f32[2] === 3e7,
           'Float32Array roundtrips');
    assert(v.bytes instanceof ArrayBuffer && new Uint8Array(v.bytes)[0] === 9,
           'nested ArrayBuffer roundtrips');
    assert(v.nested.deep.deeper.n === 42, 'deep nesting roundtrips');
}

// --- 6. Clone rejection: pointer-transfer types and non-clonables throw.
function expectTypeError(fn, what) {
    let threw = null;
    try { fn(); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, what + ' rejected with TypeError (got ' + threw + ')');
}
expectTypeError(() => bro.net.sendClone(connA, { fn: () => 1 }), 'function');
expectTypeError(() => bro.net.sendClone(connA, { m: Mesh.box() }), 'Mesh');
const bmp = await createImageBitmap({
    width: 2, height: 2, data: new Uint8ClampedArray(16),
});
expectTypeError(() => bro.net.sendClone(connA, { img: bmp }), 'ImageBitmap');
expectTypeError(() => bro.net.sendClone(connA, { deep: [bmp] }), 'nested ImageBitmap');
expectTypeError(() => bro.net.sendClone(connA, { a: 1 }, []), 'transfer-list options');

// Writer-side depth limit (the reader-side mirror is covered by the nesting
// bomb in section 7): 100 nested objects exceed the serializer's depth cap of
// 64 and must throw a TypeError, not recurse the C stack.
{
    let deep = { leaf: true };
    for (let i = 0; i < 100; i++) deep = { next: deep };
    expectTypeError(() => bro.net.sendClone(connA, deep), 'over-deep nesting');
}
// Circular references are not supported: the cycle re-descends until the same
// depth cap trips, surfacing as the depth TypeError rather than a hang/crash.
{
    const a = { name: 'a' };
    a.self = a;
    expectTypeError(() => bro.net.sendClone(connA, a), 'circular reference');
}
// Depth just under the limit still round-trips (regression guard for the cap
// being lowered accidentally).
{
    received = [];
    let ok = { leaf: 60 };
    for (let i = 0; i < 60; i++) ok = { next: ok };
    assert(bro.net.sendClone(connA, ok) === true, 'depth-60 clone accepted');
    assert(waitFor(1, 5000), 'depth-60 clone received');
    let cur = received[0].data, hops = 0;
    while (cur && cur.next) { cur = cur.next; hops++; }
    assert(hops === 60 && cur.leaf === 60, 'depth-60 clone round-trips intact (hops=' + hops + ')');
}

// --- 7. Malformed-frame robustness. _sendUnframed writes bytes verbatim with
// no wire header — exactly what a buggy or hostile peer would put on the wire.
// None of these may reach onmessage; none may crash; the link must stay usable.
received = [];
const hostile = [
    new Uint8Array([0x42]),                                  // too short + bad magic
    new Uint8Array([0x00, 0x00, 1, 2, 3]),                   // bad magic
    new Uint8Array([0xb7, 0x7f, 1, 2, 3]),                   // unknown frame type
    new Uint8Array([0xb7, 0x01]),                            // clone frame, no payload
    new Uint8Array([0xb7, 0x01, 0x06, 0xff, 0xff, 0xff, 0x7f]), // string, huge length
    new Uint8Array([0xb7, 0x01, 0x0b, 0x07, 0x00, 0x00]),    // truncated typed-array header
    new Uint8Array([0xb7, 0x01, 0x0c, 0x00, 0x00, 0x00, 0x00]), // Mesh pointer-slot tag
    new Uint8Array([0xb7, 0x01, 0x0a, 0x05, 0x00, 0x00, 0x00]), // transfer index OOB
];
// Deep-nesting bomb: 100 nested kArray(len=1) frames — must hit the depth
// limit, not the C stack.
{
    const bomb = new Uint8Array(2 + 100 * 5 + 1);
    bomb[0] = 0xb7; bomb[1] = 0x01;
    for (let i = 0; i < 100; i++) {
        bomb[2 + i * 5] = 0x07;                 // kArray
        bomb[2 + i * 5 + 1] = 1;                // len = 1 (little endian u32)
    }
    bomb[2 + 100 * 5] = 0x00;                   // innermost: kUndefined
    hostile.push(bomb);
}
for (const h of hostile) bro.net._sendUnframed(connA, h, { reliable: true });

// Chase with a good clone; reliable + same channel ⇒ it arrives after all of
// the hostile frames were processed (and dropped).
bro.net.sendClone(connA, { alive: true });
assert(waitFor(1, 8000), 'receiver survived hostile frames');
assert(received.length === 1, 'hostile frames were all dropped (got ' + received.length + ' messages)');
assert(received[0].data.alive === true, 'good clone still decodes after hostile frames');

// --- 8. broadcastClone: reaches every connection (both ends here).
received = [];
bro.net.broadcastClone({ hello: 'all' }, { channel: 1 });
assert(waitFor(2), 'broadcastClone reached both ends');
assert(received[0].data.hello === 'all' && received[1].data.hello === 'all',
       'broadcast clone values decode');
assert(received[0].channel === 1 && received[1].channel === 1, 'broadcast clone channel');

// --- 9. Unreliable + nodelay: fire-and-forget delivery on loopback. Unreliable
// sends may legitimately drop, so retry until one lands.
received = [];
let landed = false;
for (let attempt = 0; attempt < 50 && !landed; attempt++) {
    bro.net.send(connA, 'fast', { reliable: false, nodelay: true, channel: 6 });
    for (let t = 0; t < 200 && received.length === 0; t += 10) pump(10, 10);
    landed = received.length > 0;
}
assert(landed, 'unreliable+nodelay message delivered');
assert(received[0].channel === 6, 'unreliable message carries its channel');
assert(new TextDecoder().decode(received[0].data) === 'fast', 'unreliable payload intact');

bro.net.close();
advanceTime(100);
console.log('[test] PASS');
