// Worker for test_basic.js — echoes messages and computes a sum.

self.onmessage = (e) => {
    const data = e.data;
    if (data && data.cmd === 'echo') {
        self.postMessage({ echo: data.payload });
    } else if (data && data.cmd === 'sum') {
        let s = 0;
        for (const n of data.values) s += n;
        self.postMessage({ sum: s });
    } else if (data && data.cmd === 'error') {
        throw new Error('worker error: ' + data.msg);
    } else if (data && data.cmd === 'transferback') {
        // Echo back the transferred buffer
        const buf = data.buf;
        self.postMessage({ size: buf.byteLength });
    } else {
        self.postMessage({ unknown: true });
    }
};
