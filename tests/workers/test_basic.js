// Test Worker basic functionality — postMessage, onmessage, terminate,
// structured clone, error handling. Exercises src/js/worker.cpp and
// the message-serialization in src/js/message_serializer.cpp.

const workerPath = '../workers/worker_basic.js';

// =========================================================================
// Echo round-trip
// =========================================================================
const w = new Worker(workerPath);

let got = null;
w.onmessage = (e) => { got = e.data; };

w.postMessage({ cmd: 'echo', payload: { name: 'bro', n: 7 } });

let waited = 0;
while (got === null && waited < 5000) { advanceTime(16); waited += 16; }
assert(got !== null, 'echo reply received');
assert(typeof got.echo === 'object', 'echo is object');
assert(got.echo.name === 'bro', 'echo.name');
assert(got.echo.n === 7, 'echo.n');

// =========================================================================
// Sum: send array, receive scalar
// =========================================================================
got = null;
w.postMessage({ cmd: 'sum', values: [1, 2, 3, 4, 5] });
waited = 0;
while (got === null && waited < 5000) { advanceTime(16); waited += 16; }
assert(got !== null, 'sum reply received');
assert(got.sum === 15, 'sum = 15');

// =========================================================================
// ArrayBuffer transfer
// =========================================================================
got = null;
const buf = new Float32Array([1, 2, 3, 4]).buffer;
const sizeBefore = buf.byteLength;
assert(sizeBefore === 16, 'buf before transfer');

w.postMessage({ cmd: 'transferback', buf: buf }, [buf]);
// After transfer, buf should be detached
const sizeAfter = buf.byteLength;
assert(sizeAfter === 0, 'buf detached after transfer, got ' + sizeAfter);

waited = 0;
while (got === null && waited < 5000) { advanceTime(16); waited += 16; }
assert(got !== null, 'transfer reply received');
assert(got.size === 16, 'worker saw 16-byte buffer');

// =========================================================================
// Multiple messages in sequence
// =========================================================================
let recv = 0;
w.onmessage = (e) => { recv++; };
for (let i = 0; i < 5; ++i) {
    w.postMessage({ cmd: 'echo', payload: i });
}
waited = 0;
while (recv < 5 && waited < 5000) { advanceTime(16); waited += 16; }
assert(recv === 5, '5 messages received, got ' + recv);

// =========================================================================
// terminate
// =========================================================================
w.terminate();
// After terminate, postMessage should be a no-op (or throw harmlessly)
try { w.postMessage({ cmd: 'echo', payload: 'late' }); } catch(e) {}

// =========================================================================
// New worker after terminating the first
// =========================================================================
const w2 = new Worker(workerPath);
let got2 = null;
w2.onmessage = (e) => { got2 = e.data; };
w2.postMessage({ cmd: 'echo', payload: 'second' });
waited = 0;
while (got2 === null && waited < 5000) { advanceTime(16); waited += 16; }
assert(got2 !== null, 'second worker replied');
w2.terminate();
