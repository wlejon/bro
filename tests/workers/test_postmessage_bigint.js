// BigInt round-trips cleanly through worker.postMessage.
//
// Regression: the C++ structured-clone serializer used to throw
// "value is not cloneable" on any BigInt, blocking the obvious pattern
// of versioning weights / IDs across threads with a 64-bit counter.

// Worker path is resolved relative to the test app directory (tests/test_app/).
const workerPath = '../workers/worker_postmessage_bigint.js';

const tiny = 12345n;
const huge = 1234567890123456789012345678901234567890n;  // > 2^128
const neg  = -42n;

const w = new Worker(workerPath);

let got = null;
w.onmessage = (e) => { got = e.data; };

w.postMessage({ tiny, huge, neg });

// Pump until the worker replies (or fail loud after a generous budget).
// advanceTime() delivers queued replies, wallSleep() gives the worker's real
// thread CPU time; the budget is wall-clock so a loaded machine (e.g. the
// parallel test runner) can't starve the worker out of a virtual-time budget.
const deadline = Date.now() + 15000;
while (got === null && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
assert(got !== null, 'worker replied within budget');

// Echo: BigInts survived the trip both ways.
assert(typeof got.echoed.tiny === 'bigint', 'tiny is bigint after roundtrip');
assert(typeof got.echoed.huge === 'bigint', 'huge is bigint after roundtrip');
assert(typeof got.echoed.neg  === 'bigint', 'neg is bigint after roundtrip');
assert(got.echoed.tiny === tiny, 'tiny value preserved');
assert(got.echoed.huge === huge, 'huge (>64-bit) value preserved');
assert(got.echoed.neg  === neg,  'negative value preserved');

// Derived: BigInt arithmetic in the worker came back intact.
assert(got.derived.plus1   === tiny + 1n, 'arith plus1');
assert(got.derived.negated === -tiny,     'arith negated');
assert(got.derived.huge2x  === huge * 2n, 'arith huge2x (still arbitrary precision)');

// Nested in an object alongside other primitives.
assert(got.derived.mixed.a === 1,    'mixed.a int preserved');
assert(got.derived.mixed.b === tiny, 'mixed.b bigint preserved');
assert(got.derived.mixed.c === 'x',  'mixed.c string preserved');

// Nested in an array.
assert(Array.isArray(got.derived.arr), 'arr is array');
assert(got.derived.arr.length === 3, 'arr length');
assert(got.derived.arr[0] === 0n,   'arr[0] zero bigint');
assert(got.derived.arr[1] === tiny, 'arr[1] tiny');
assert(got.derived.arr[2] === huge, 'arr[2] huge');

w.terminate();
