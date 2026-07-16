// Worker half of tests/net/test_net_worker_clone.js.
//
// Connects to the host, sendClones a structured value on channel 2, then
// waits for the host's clone reply on channel 4 and reports it back over
// postMessage. Proves that clone serialization runs in the worker's context
// and deserialization runs in the receiving context, both directions.

onmessage = (e) => {
    const { port } = e.data;

    bro.net.onconnect = (conn) => {
        bro.net.sendClone(conn, {
            greeting: 'from-worker',
            pos: new Float32Array([1.5, -2.5, 3.5]),
            meta: { seq: 1, tags: ['a', 'b'] },
        }, { channel: 2 });
    };

    bro.net.onmessage = (conn, data, channel) => {
        // Reply from the host — a decoded structured value, not bytes.
        postMessage({
            done: true,
            isValue: !(data instanceof ArrayBuffer),
            ack: data && data.ack,
            echoSeq: data && data.echo && data.echo.seq,
            sum: data && data.sum,
            channel,
        });
    };

    if (!bro.net.connect('127.0.0.1:' + port)) {
        postMessage({ done: false, error: 'connect() refused' });
    }
};
