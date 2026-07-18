// WebSocketServer (brokit): RFC 6455 server accepting the existing WebSocket
// client in one headless instance. Text + binary echo, close-code round trip,
// clients list, clean shutdown.

const net = require('net');

assert(typeof WebSocketServer === 'function', 'WebSocketServer global exists');
assert(typeof require('websocket-server').WebSocketServer === 'function',
       'websocket-server module resolves');

// maxMs is real wall time. headless sleep() is an alias for advanceTime
// (virtual), so this loops on Date.now() — Windows needs >1s of real time
// to report a refused loopback connect, and socket IO runs at wall speed.
function pump(cond, maxMs) {
    const t0 = Date.now();
    do {
        advanceTime(10);
        flush();
        if (cond()) return true;
    } while (Date.now() - t0 < maxMs);
    return cond();
}

let srvGotText = null;
let srvGotBinary = null;
let srvClose = null;
let connectionCount = 0;

const wss = new WebSocketServer({ port: 0 }); // default host 127.0.0.1
const port = wss.address().port;
assert(port > 0, 'wss ephemeral port: ' + port);
assert(wss.address().address === '127.0.0.1', 'wss default bind is loopback');

wss.on('connection', (ws, request) => {
    connectionCount++;
    assert(ws.readyState === 1, 'server socket arrives OPEN');
    assert(typeof ws.send === 'function' && typeof ws.close === 'function' &&
           'onmessage' in ws && 'binaryType' in ws,
           'server socket mirrors the client surface');
    assert(request.url === '/hello', 'request url: ' + request.url);
    ws.onmessage = (ev) => {
        if (typeof ev.data === 'string') {
            srvGotText = ev.data;
            ws.send(ev.data.toUpperCase());
        } else {
            srvGotBinary = ev.data;
            ws.send(ev.data); // echo binary
        }
    };
    ws.onclose = (ev) => { srvClose = ev; };
});

// The EXISTING brokit WebSocket client (curl) connects to our server.
const ws = new WebSocket('ws://127.0.0.1:' + port + '/hello');
let opened = false;
let gotText = null;
let gotBinary = null;
let clientClose = null;
ws.onopen = () => { opened = true; };
ws.onmessage = (ev) => {
    if (typeof ev.data === 'string') gotText = ev.data;
    else gotBinary = ev.data;
};
ws.onclose = (ev) => { clientClose = ev; };

assert(pump(() => opened, 5000), 'client completed handshake against server');
assert(connectionCount === 1, 'server emitted one connection');
assert(wss.clients.length === 1, 'clients list tracks the connection');

ws.send('echo me');
assert(pump(() => gotText !== null, 3000), 'text round trip');
assert(gotText === 'ECHO ME', 'server transformed text: ' + gotText);
assert(srvGotText === 'echo me', 'server received text intact');

ws.send(new Uint8Array([5, 0, 200]));
assert(pump(() => gotBinary !== null, 3000), 'binary round trip');
assert(gotBinary instanceof Uint8Array && gotBinary.length === 3 &&
       gotBinary[0] === 5 && gotBinary[1] === 0 && gotBinary[2] === 200,
       'binary echo bytes intact');
assert(srvGotBinary && srvGotBinary.length === 3, 'server received binary');

// Close handshake with a custom code, both directions observe it.
ws.close(4001, 'bye now');
assert(pump(() => clientClose !== null && srvClose !== null, 5000),
       'close handshake completed both sides');
assert(clientClose.code === 4001, 'client close code: ' + clientClose.code);
assert(srvClose.code === 4001, 'server close code: ' + srvClose.code);
assert(srvClose.reason === 'bye now', 'server close reason: ' + srvClose.reason);
assert(srvClose.wasClean === true, 'server close was clean');
assert(pump(() => wss.clients.length === 0, 3000), 'clients list drained');

let wssClosed = false;
wss.close(() => { wssClosed = true; });
assert(pump(() => wssClosed, 3000), 'server closed');

// Post-shutdown: new TCP connects to the port must fail (listener really gone).
let refused = false;
const probe = net.connect(port, '127.0.0.1');
probe.on('error', () => { refused = true; });
probe.on('close', () => {});
assert(pump(() => refused, 10000), 'port no longer accepts after close');
