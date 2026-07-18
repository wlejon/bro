// Raw TCP (brokit net module): loopback echo server + client in one headless
// instance. Binary + string round trip, graceful end, close events, and the
// safe loopback bind default.

const net = require('net');

assert(typeof net.createServer === 'function', 'net.createServer is fn');
assert(typeof net.connect === 'function', 'net.connect is fn');

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

let serverPeer = null;
let serverSawEnd = false;
let serverSockClosed = false;
let serverClosed = false;
let clientConnected = false;
let clientClosed = false;
let clientChunks = [];

const server = net.createServer((sock) => {
    serverPeer = sock.remoteAddress;
    sock.on('data', (chunk) => {
        sock.write(chunk);  // echo bytes
        sock.write('tail'); // plus a string write
    });
    sock.on('end', () => { serverSawEnd = true; });
    sock.on('close', () => { serverSockClosed = true; });
});
server.on('close', () => { serverClosed = true; });
server.listen(0); // ephemeral port, default host = 127.0.0.1

const addr = server.address();
assert(addr && addr.port > 0, 'ephemeral port assigned: ' + (addr && addr.port));
assert(addr.address === '127.0.0.1', 'default bind is loopback: ' + addr.address);

const client = net.connect(addr.port, '127.0.0.1', () => { clientConnected = true; });
client.on('data', (chunk) => { clientChunks.push(chunk); });
client.on('close', (hadError) => {
    clientClosed = true;
    assert(hadError === false, 'client closed cleanly');
});

assert(pump(() => clientConnected, 3000), 'client connected');

client.write(new Uint8Array([1, 2, 250, 255]));

assert(pump(() => {
    let total = 0;
    for (const c of clientChunks) total += c.length;
    return total >= 8;
}, 3000), 'echo round trip completed');

let all = [];
for (const c of clientChunks) all = all.concat(Array.from(c));
assert(all.length === 8, 'client received 8 bytes: ' + all.length);
assert(all[0] === 1 && all[1] === 2 && all[2] === 250 && all[3] === 255,
       'binary bytes intact through echo');
assert(String.fromCharCode(all[4], all[5], all[6], all[7]) === 'tail',
       'string write delivered');
assert(serverPeer === '127.0.0.1', 'server saw loopback peer: ' + serverPeer);

client.end(); // graceful: FIN, server auto-ends its side
assert(pump(() => clientClosed && serverSockClosed, 3000),
       'both sides closed after end()');
assert(serverSawEnd, 'server observed FIN as end event');

server.close();
assert(pump(() => serverClosed, 3000), 'server closed');

// Connection-refused surfaces as error + close(hadError), not a throw.
let refusedError = false;
let refusedHadError = false;
const bad = net.connect(1, '127.0.0.1');
bad.on('error', () => { refusedError = true; });
bad.on('close', (hadError) => { refusedHadError = hadError; });
assert(pump(() => refusedError && refusedHadError, 10000),
       'refused connect emitted error + close(hadError)');
