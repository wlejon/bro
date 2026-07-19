// Every TypedArray element type survives worker.postMessage as itself.
//
// Regression: the serializer identified views by reading constructor.name
// against a table that stopped at Float64Array, and anything unmatched fell
// through to a `subtype = 7` Float32 default. So a BigInt64Array (and
// BigUint64Array, and Float16Array) arrived as a Float32Array over the very
// same bytes — no error, no warning, just silently reinterpreted numbers.
// A 64-bit id sent to a worker came back as two garbage floats.
//
// Reading constructor.name was also wrong for subclasses, which report their
// own name and so hit the same default; the serializer now asks QuickJS for
// the element type instead, and refuses anything it cannot name.

const workerPath = '../workers/worker_postmessage_typedarray.js';

// One view per element type, with values chosen to be unmistakable if the
// bytes were reinterpreted: the 64-bit cases exceed 2^53 so a float
// round-trip cannot preserve them even in principle.
const sent = {
    i8:   new Int8Array([-128, 0, 127]),
    u8:   new Uint8Array([0, 128, 255]),
    u8c:  new Uint8ClampedArray([0, 128, 255]),
    i16:  new Int16Array([-32768, 0, 32767]),
    u16:  new Uint16Array([0, 32768, 65535]),
    i32:  new Int32Array([-2147483648, 0, 2147483647]),
    u32:  new Uint32Array([0, 2147483648, 4294967295]),
    f32:  new Float32Array([-1.5, 0, 1.5]),
    f64:  new Float64Array([-1.5, 0, 1.5]),
    f16:  new Float16Array([-1.5, 0, 1.5]),
    i64:  new BigInt64Array([-9223372036854775808n, 0n, 9223372036854775807n]),
    u64:  new BigUint64Array([0n, 1n, 18446744073709551615n]),
};

const expect = {
    i8:  { ctor: 'Int8Array',         bpe: 1 },
    u8:  { ctor: 'Uint8Array',        bpe: 1 },
    u8c: { ctor: 'Uint8ClampedArray', bpe: 1 },
    i16: { ctor: 'Int16Array',        bpe: 2 },
    u16: { ctor: 'Uint16Array',       bpe: 2 },
    i32: { ctor: 'Int32Array',        bpe: 4 },
    u32: { ctor: 'Uint32Array',       bpe: 4 },
    f32: { ctor: 'Float32Array',      bpe: 4 },
    f64: { ctor: 'Float64Array',      bpe: 8 },
    f16: { ctor: 'Float16Array',      bpe: 2 },
    i64: { ctor: 'BigInt64Array',     bpe: 8 },
    u64: { ctor: 'BigUint64Array',    bpe: 8 },
};

const w = new Worker(workerPath);

let got = null;
w.onmessage = (e) => { got = e.data; };
w.postMessage(sent);

const deadline = Date.now() + 15000;
while (got === null && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
assert(got !== null, 'worker replied within budget');

for (const key of Object.keys(expect)) {
    const want = expect[key];
    const seen = got.seen[key];
    assert(seen, key + ' arrived at the worker');
    assert(seen.ctor === want.ctor,
           key + ' deserialized as ' + want.ctor + ', got ' + seen.ctor);
    assert(seen.bpe === want.bpe,
           key + ' has BYTES_PER_ELEMENT ' + want.bpe + ', got ' + seen.bpe);

    // Values, not just the tag: a correct tag over shifted bytes is still wrong.
    const wantVals = Array.from(sent[key], (v) => String(v));
    assert(seen.vals.length === wantVals.length,
           key + ' element count ' + wantVals.length + ', got ' + seen.vals.length);
    for (let i = 0; i < wantVals.length; i++) {
        assert(seen.vals[i] === wantVals[i],
               key + '[' + i + '] = ' + wantVals[i] + ', got ' + seen.vals[i]);
    }

    // Return leg.
    const back = got.echoed[key];
    assert(back.constructor.name === want.ctor,
           key + ' is still ' + want.ctor + ' coming back, got ' + back.constructor.name);
    for (let i = 0; i < wantVals.length; i++) {
        assert(String(back[i]) === wantVals[i],
               key + '[' + i + '] survived the return leg as ' + wantVals[i] +
               ', got ' + String(back[i]));
    }
}

// A view over a non-zero byteOffset: the offset/length header travels
// separately from the element tag, so exercise them together.
const backing = new ArrayBuffer(32);
new BigInt64Array(backing).set([1n, 2n, 3n, 4n]);
const windowed = new BigInt64Array(backing, 8, 2);
assert(windowed.length === 2, 'windowed view has 2 elements before send');

let got2 = null;
w.onmessage = (e) => { got2 = e.data; };
w.postMessage({ win: windowed });

const deadline2 = Date.now() + 15000;
while (got2 === null && Date.now() < deadline2) { advanceTime(16); wallSleep(2); }
assert(got2 !== null, 'worker replied to the offset-view probe');
assert(got2.seen.win.ctor === 'BigInt64Array', 'offset view kept its type');
assert(got2.seen.win.vals.join(',') === '2,3',
       'offset view kept its window, got ' + got2.seen.win.vals.join(','));

w.terminate();

console.log('PASS test_postmessage_typedarray');
