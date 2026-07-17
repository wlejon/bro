// Test CompressionStream / DecompressionStream inside a Worker realm.
// brokit's installAll runs per-realm; the compression globals must be
// present and functional in worker contexts, not just the main window.

const w = new Worker('../workers/worker_compression.js');

let got = null;
w.onmessage = (e) => { got = e.data; };

function pumpUntil(pred, ms) {
    const deadline = Date.now() + (ms || 15000);
    while (!pred() && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
}

// gzip round-trip in the worker
const payload = new Uint8Array(4096);
let x = 42;
for (let i = 0; i < payload.length; i++) {
    x ^= x << 13; x ^= x >>> 17; x ^= x << 5; x |= 0;
    payload[i] = x & 0xff;
}
w.postMessage({ cmd: 'roundtrip', format: 'gzip', bytes: payload.buffer });
pumpUntil(() => got !== null);
assert(got !== null, 'worker replied');
assert(got.ok === true, 'worker round-trip ok: ' + (got && got.error));
assert(got.hasClasses === true, 'worker realm has CompressionStream/DecompressionStream');
assert(got.compressedLength > 0, 'worker produced compressed bytes');
assert(got.equal === true, 'worker gzip round-trip byte-equal');

// deflate-raw round-trip in the worker
got = null;
w.postMessage({ cmd: 'roundtrip', format: 'deflate-raw', bytes: payload.buffer.slice(0) });
pumpUntil(() => got !== null);
assert(got !== null, 'worker replied (deflate-raw)');
assert(got.ok === true, 'worker deflate-raw round-trip ok: ' + (got && got.error));
assert(got.equal === true, 'worker deflate-raw round-trip byte-equal');

// unknown format throws TypeError in the worker realm too
got = null;
w.postMessage({ cmd: 'badformat' });
pumpUntil(() => got !== null);
assert(got !== null, 'worker replied (badformat)');
assert(got.isTypeError === true, 'unknown format throws TypeError in worker');

w.terminate();
