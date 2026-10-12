// A transfer costs the same however big the page's heap is. Detaching a
// buffer closes every view over it, and bronze found those views by walking
// the whole heap on every detach: a page relaying decoded images to a worker
// (the Image Viewer's Find Animals, a transfer a photo) paid a walk of its
// growing heap per message, 0.4 ms at the start of a card of 1500 photos and
// 0.7 ms by its end, and 2-8 ms a transfer here with 100k-500k objects live.
// Now a thread that has detached a buffer keeps the views of the buffers it
// makes in bronze's view table, and a detach refreshes just those.

function medianMs(f, reps) {
    const times = [];
    for (let i = 0; i < reps; i++) {
        const t = perf.now();
        f(i);
        times.push(perf.now() - t);
    }
    times.sort((a, b) => a - b);
    return times[Math.floor(reps / 2)];
}

// One message the way an image goes to a worker: pixels in a view, the buffer transferred.
function relay(i) {
    const pixels = new Uint8Array(64 * 64 * 4);
    pixels[0] = i & 255;
    const window = new Uint8Array(pixels.buffer, 16, 32);
    const out = structuredClone({ image: { width: 64, height: 64, pixels } }, { transfer: [pixels.buffer] });
    if (pixels.length !== 0 || window.length !== 0 || pixels.buffer.byteLength !== 0) throw new Error('the sender\'s views are not closed');
    if (out.image.pixels.length !== 64 * 64 * 4 || out.image.pixels[0] !== (i & 255)) throw new Error('the pixels did not arrive');
}

// The first detach on a thread may walk once.
relay(0);
const small = medianMs(relay, 60);

const live = [];
for (let i = 0; i < 400000; i++) live.push({ i, pair: [i, i + 1], name: 'n' + i });
const big = medianMs(relay, 60);
console.log(`transfer: ${(small * 1000).toFixed(0)} us with a small heap, ${(big * 1000).toFixed(0)} us with ${live.length} more objects live`);
assert(big < small * 3 + 0.1, `a transfer does not walk the heap: ${big.toFixed(3)} ms with 400k objects live, ${small.toFixed(3)} ms without`);

// A view made before the heap grew, over a buffer made after the first detach, still closes.
{
    const a = new Float32Array(8);
    const b = new Int16Array(a.buffer, 8, 4);
    structuredClone(a.buffer, { transfer: [a.buffer] });
    assert(a.length === 0 && b.length === 0, 'both views over a transferred buffer are closed');
}

// And through a Worker, as the app does it.
{
    const w = new Worker('../workers/worker_transfer_view.js');
    let got = null;
    w.onmessage = (e) => { got = e.data; };
    const v = new Uint8Array(4);
    v.set([1, 2, 3, 4]);
    const t = perf.now();
    w.postMessage({ v }, [v.buffer]);
    const ms = perf.now() - t;
    assert(v.length === 0, 'worker: the sender\'s view is closed');
    assert(ms < small * 3 + 2, `worker: a transfer with a big heap took ${ms.toFixed(3)} ms`);
    const deadline = Date.now() + 15000;
    while (got === null && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
    assert(got && got.vals.join() === '1,2,3,4', 'worker: the view arrived ' + JSON.stringify(got));
    w.terminate();
}
live.length = 0;

console.log('test_transfer_heap_size: OK');
