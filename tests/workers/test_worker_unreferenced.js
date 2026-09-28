// A running Worker the page no longer references still delivers its reply.
//
// The common shape is `const w = new Worker(...); w.onmessage = ...;
// w.postMessage(job);` with nothing reading `w` afterwards: the reply is what
// the page waits on. The Worker object used to be an ordinary collectable
// wrapper, so a collection while the worker was busy finalized it, and the
// finalizer terminated the worker, joining its thread on the main thread (a
// stall as long as the job) and deleting the reply with the instance. The page
// saw one frame hang for seconds and then waited forever. A worker the page
// terminates, or one that closes itself, is collectable again.
//
// The page allocates enough to collect, and runs idle seconds (which collect
// too, host_gc.cpp), while the workers are still busy.

function waitFor(pred, ms) {
    const deadline = Date.now() + (ms || 15000);
    while (!pred() && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
    return pred();
}

const replies = [];

// No binding outlives this call: only the worker's own reply can reach it.
function startUnreferenced(tag, busyMs) {
    const w = new Worker('../workers/worker_slow_reply.js');
    w.onmessage = (e) => { replies.push(e.data); };
    w.postMessage({ tag, busyMs });
}

startUnreferenced('a', 1500);
startUnreferenced('b', 1500);

// Garbage enough to collect while the workers are busy, plus idle frames.
let worstFrame = 0;
let sink = null;
for (let i = 0; i < 3; i++) {
    const s = Date.now();
    for (let j = 0; j < 200000; j++) sink = { j, pad: [j, j, j, j] };
    advanceTime(1100);
    worstFrame = Math.max(worstFrame, Date.now() - s);
}

assert(waitFor(() => replies.length === 2), 'both unreferenced workers replied, got ' + JSON.stringify(replies));
const tags = replies.map((r) => r.tag).sort().join();
assert(tags === 'a,b', 'replies from both workers: ' + tags);
assert(replies.every((r) => r.spun), 'each worker ran its job');
// A collection that terminated a busy worker joined its thread in the frame:
// about the job's length. The frames themselves do nothing slow.
assert(worstFrame < 1000, 'no frame waited on a worker thread, worst ' + worstFrame + ' ms');

// A terminated worker is released: this one's reply never comes, and its
// wrapper is collectable (the idle frames below collect it without incident).
{
    let late = null;
    const w = new Worker('../workers/worker_slow_reply.js');
    w.onmessage = (e) => { late = e.data; };
    w.postMessage({ tag: 'late', busyMs: 10 });
    w.terminate();
    advanceTime(1100);
    advanceTime(1100);
    assert(late === null, 'a terminated worker delivers nothing');
}

console.log('test_worker_unreferenced: OK');
