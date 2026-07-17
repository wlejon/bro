// Worker for test_postmessage_errors.js — echoes payloads back, and on
// request tries to post non-cloneable values from the worker side, reporting
// whether the serializer threw (and what) via a normal message.

function depthOf(obj) {
    let d = 0;
    let cur = obj;
    while (cur && typeof cur === 'object' && 'next' in cur) { d++; cur = cur.next; }
    return d;
}

self.onmessage = (e) => {
    const data = e.data;
    if (data && data.cmd === 'echo') {
        self.postMessage({ echo: data.payload, depth: depthOf(data.payload) });
    } else if (data && data.cmd === 'postFunction') {
        // Worker-side serializer must reject functions the same way.
        try {
            self.postMessage({ fn: () => 1 });
            self.postMessage({ result: 'no-throw' });
        } catch (err) {
            self.postMessage({ result: 'threw', name: err.name, message: String(err.message) });
        }
    } else if (data && data.cmd === 'postDeep') {
        // Worker-side depth limit.
        let deep = { leaf: true };
        for (let i = 0; i < data.depth; i++) deep = { next: deep };
        try {
            self.postMessage({ deep });
            self.postMessage({ result: 'no-throw' });
        } catch (err) {
            self.postMessage({ result: 'threw', name: err.name, message: String(err.message) });
        }
    } else {
        self.postMessage({ unknown: true });
    }
};
