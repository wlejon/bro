// Worker for test_worker_error_events.js. Throws on request (with heap churn
// first, so a collection is likely while the error event is being built), and
// echoes transferred buffers back, transferring them again.

function churn(n) {
    let keep = [];
    for (let i = 0; i < n; i++) keep.push({ i, s: 'garbage-' + i, a: [i, i + 1] });
    return keep.length;
}

let listenerHits = 0;
self.addEventListener('error', (e) => {
    listenerHits++;
    self.postMessage({ from: 'listener', type: e.type, message: String(e.message) });
});

self.onerror = (e) => {
    churn(2000);
    self.postMessage({ from: 'onerror', type: e.type, message: String(e.message),
                       hasFilename: typeof e.filename === 'string' });
};

self.onmessage = (e) => {
    const d = e.data;
    if (d.cmd === 'throw') {
        churn(5000);
        throw new Error('boom ' + d.n);
    } else if (d.cmd === 'transfer') {
        churn(3000);
        const bufs = d.bufs;
        const lens = bufs.map((b) => b.byteLength);
        const firsts = bufs.map((b) => new Uint8Array(b)[0]);
        self.postMessage({ from: 'transfer', lens, firsts, bufs }, bufs);
        self.postMessage({ from: 'afterTransfer', lens: bufs.map((b) => b.byteLength) });
    } else if (d.cmd === 'hits') {
        self.postMessage({ from: 'hits', listenerHits });
    }
};
