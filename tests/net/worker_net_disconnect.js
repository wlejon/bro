// Client half of tests/net/test_net_disconnect_reason.js.
//
// Connects back to the host on loopback and reports the reason code its
// ondisconnect receives when the host kicks it.

onmessage = (e) => {
    const { port } = e.data;

    bro.net.onconnect = (conn) => {
        postMessage({ ev: 'connected', conn });
    };
    bro.net.ondisconnect = (conn, reason) => {
        postMessage({ ev: 'disconnected', conn, reason });
    };

    if (!bro.net.connect('127.0.0.1:' + port)) {
        postMessage({ ev: 'error', error: 'connect() refused' });
    }
};
