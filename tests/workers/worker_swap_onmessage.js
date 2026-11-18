// Replaces its own onmessage on the first message (as Monaco's editor worker
// does); every later message must reach the replacement.
let firstCalls = 0;
self.onmessage = (e) => {
    firstCalls++;
    globalThis.onmessage = (e2) => postMessage({ via: 'second', n: e2.data, firstCalls });
    postMessage({ via: 'first', n: e.data, firstCalls });
};
