// Worker half of tests/net/test_net_loopback.js.
//
// Connects back to the host on loopback and floods it with `count` numbered
// messages as fast as it can. Each payload is just its own index, so the host
// can prove that every single one arrived, exactly once, in order.

onmessage = (e) => {
    const { port, count } = e.data;

    bro.net.onconnect = (conn) => {
        for (let i = 0; i < count; i++) {
            const buf = new Uint8Array(4);
            new DataView(buf.buffer).setUint32(0, i, true);
            bro.net.send(conn, buf.buffer, true); // reliable
        }
        postMessage({ done: true, sent: count });
    };

    if (!bro.net.connect('127.0.0.1:' + port)) {
        postMessage({ done: false, error: 'connect() refused' });
    }
};
