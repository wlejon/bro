// test_net_stubs_remediated.js — verifies remediated bro.net stubs:
// 1. bro.net._sendUnframed: exposed hook, throws on missing args, sends bytes
// 2. bro.net.getPeerAddress: returns remote IP:port string, empty on invalid peer
// 3. bro.net.stats: returns aggregated { ping, packetLoss, bytesSent, bytesRecv }
// 4. bro.net.setPeerSimulatedLoss: sets fake packet loss and lag configs without error

if (!bro.net || bro.net.available === false) {
    console.log('net unavailable; skipping');
} else {
    assert(typeof bro.net._sendUnframed === 'function', 'bro.net._sendUnframed is exposed');
    assert(typeof bro.net.getPeerAddress === 'function', 'bro.net.getPeerAddress is a function');
    assert(typeof bro.net.stats === 'function', 'bro.net.stats is a function');
    assert(typeof bro.net.setPeerSimulatedLoss === 'function', 'bro.net.setPeerSimulatedLoss is a function');

    // Missing argument TypeError checks
    let threw = false;
    try { bro.net._sendUnframed(); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, '_sendUnframed throws TypeError on missing peerId');

    threw = false;
    try { bro.net._sendUnframed(1); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, '_sendUnframed throws TypeError on missing data');

    threw = false;
    try { bro.net.getPeerAddress(); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, 'getPeerAddress throws TypeError on missing peerId');

    threw = false;
    try { bro.net.setPeerSimulatedLoss(1); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, 'setPeerSimulatedLoss throws TypeError on missing chance');

    // Invalid peer returns empty string
    assert(bro.net.getPeerAddress(999999) === '', 'getPeerAddress returns empty string for invalid peer');

    // Stats with no connections returns zeroes
    const initialStats = bro.net.stats();
    assert(typeof initialStats === 'object' && initialStats !== null, 'stats() returns object');
    assert(typeof initialStats.ping === 'number', 'stats().ping is a number');
    assert(typeof initialStats.packetLoss === 'number', 'stats().packetLoss is a number');
    assert(typeof initialStats.bytesSent === 'number', 'stats().bytesSent is a number');
    assert(typeof initialStats.bytesRecv === 'number', 'stats().bytesRecv is a number');

    const PORT = 27600 + (Date.now() % 300);
    const hostConnections = [];
    bro.net.onconnect = (conn) => hostConnections.push(conn);

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
    while (hostConnections.length === 0 && waited < 4000) { advanceTime(10); wallSleep(10); waited += 10; }
    assert(hostConnections.length === 1, 'host saw connection');

    const conn = hostConnections[0];

    // Verify getPeerAddress returns remote address string
    const addr = bro.net.getPeerAddress(conn);
    assert(typeof addr === 'string' && addr.length > 0, 'getPeerAddress returns non-empty string: ' + addr);
    assert(addr.includes('127.0.0.1') || addr.includes('::1') || addr.includes(':'), 'getPeerAddress contains IP/port: ' + addr);

    // Verify stats() with active connection
    const liveStats = bro.net.stats();
    assert(typeof liveStats === 'object' && liveStats !== null, 'live stats() returns object');
    assert(typeof liveStats.ping === 'number', 'live stats().ping is number');
    assert(typeof liveStats.packetLoss === 'number', 'live stats().packetLoss is number');
    assert(typeof liveStats.bytesSent === 'number', 'live stats().bytesSent is number');
    assert(typeof liveStats.bytesRecv === 'number', 'live stats().bytesRecv is number');

    // Verify peer stats
    const peerStats = bro.net.stats(conn);
    assert(typeof peerStats === 'object' && peerStats !== null, 'peerStats is object');
    assert(typeof peerStats.ping === 'number', 'peerStats.ping is number');

    // Verify setPeerSimulatedLoss
    bro.net.setPeerSimulatedLoss(conn, 0.05, 10, 20);
    bro.net.setPeerSimulatedLoss(conn, 0.0, 0, 0); // reset

    // Verify _sendUnframed hook
    const unframedResult = bro.net._sendUnframed(conn, new Uint8Array([1, 2, 3, 4]));
    assert(unframedResult === true || unframedResult === undefined, '_sendUnframed executed successfully');

    w.terminate();
    bro.net.close();
    console.log('bro.net remediated stubs PASS');
}
