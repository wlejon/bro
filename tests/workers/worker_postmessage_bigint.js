// Worker side of test_postmessage_bigint.
// Echo the received value back so the main thread can compare types/values.

self.onmessage = (e) => {
    const msg = e.data;
    // Build a fresh structured payload exercising BigInt in nested positions:
    // top-level, array element, object property, alongside other primitives.
    self.postMessage({
        echoed: msg,
        derived: {
            plus1: msg.tiny + 1n,
            negated: -msg.tiny,
            huge2x: msg.huge * 2n,
            mixed: { a: 1, b: msg.tiny, c: 'x' },
            arr: [0n, msg.tiny, msg.huge],
        },
    });
};
