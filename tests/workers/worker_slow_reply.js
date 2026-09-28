// Worker side of test_worker_unreferenced: stays busy for `busyMs` of wall
// time before replying, so the page's collections run while the reply is
// still being produced.
self.onmessage = (e) => {
    const until = Date.now() + e.data.busyMs;
    let spins = 0;
    while (Date.now() < until) spins++;
    self.postMessage({ tag: e.data.tag, spun: spins > 0 });
};
