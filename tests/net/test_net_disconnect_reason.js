// bro.net.disconnect(peer, reason): the application reason code travels to the
// peer, whose ondisconnect receives it. The port's wrapper dropped the second
// argument, so every kick arrived as reason 0 — indistinguishable from a
// network drop.
//
// GNS carries application codes in its App range (1000..1999); anything else
// is rewritten to the generic app code on the wire, so the test uses one
// inside the range.

if (!bro.net || bro.net.available === false) {
    console.log('net unavailable; skipping');
} else {
    assert(bro.net.available === true, 'bro.net.available is true when compiled in');

    const PORT = 27500 + (Date.now() % 400);
    const REASON = 1234;

    const connections = [];
    const hostDisconnects = [];
    bro.net.onconnect = (conn) => connections.push(conn);
    bro.net.ondisconnect = (conn, reason) => hostDisconnects.push({ conn, reason });

    assert(bro.net.host(PORT) !== false, 'host() accepted');
    let waited = 0;
    while (!bro.net.isHosting() && waited < 3000) { advanceTime(16); wallSleep(2); waited += 16; }
    assert(bro.net.isHosting() === true, 'hosting on port ' + PORT);

    const w = new Worker('../net/worker_net_disconnect.js');
    const wmsgs = [];
    w.onmessage = (e) => wmsgs.push(e.data);
    w.postMessage({ port: PORT });

    function waitFor(pred, ms) {
        for (let t = 0; t < ms; t += 10) {
            const hit = wmsgs.find(pred);
            if (hit) return hit;
            advanceTime(10); wallSleep(10);
        }
        return null;
    }

    assert(waitFor(m => m.ev === 'connected', 8000) !== null, 'worker connected');
    waited = 0;
    while (connections.length === 0 && waited < 4000) { advanceTime(10); wallSleep(10); waited += 10; }
    assert(connections.length === 1, 'host saw the connection');

    bro.net.disconnect(connections[0], REASON);
    const kicked = waitFor(m => m.ev === 'disconnected', 8000);
    assert(kicked !== null, 'worker saw the disconnect');
    assert(kicked.reason === REASON, 'peer received reason ' + REASON + ', got ' + kicked.reason);

    w.terminate();
    bro.net.close();
    console.log('disconnect reason OK');
}
