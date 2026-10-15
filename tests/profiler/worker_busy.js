// Spins for the number of wall milliseconds it is sent, then answers
// (test_profiler.js samples it with threads: 'workers').
function workerSpin(n) {
    let acc = 0;
    for (let i = 0; i < n; i++) acc = (acc * 31 + (i ^ (acc >>> 3))) | 0;
    return acc;
}

onmessage = (e) => {
    const until = Date.now() + e.data;
    let sink = 0;
    while (Date.now() < until) sink += workerSpin(20000);
    postMessage(sink);
};
