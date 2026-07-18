// Raw UDP (brokit dgram module): loopback round trip in one headless instance.
// String + binary payloads, rinfo, opt-in broadcast flag, close events.

const dgram = require('dgram');

assert(typeof dgram.createSocket === 'function', 'dgram.createSocket is fn');

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

let bGot = null;
let bRinfo = null;
let aGot = null;
let aClosed = false;
let bClosed = false;

const a = dgram.createSocket('udp4');
const b = dgram.createSocket('udp4');
a.on('close', () => { aClosed = true; });
b.on('close', () => { bClosed = true; });

b.on('message', (msg, rinfo) => {
    bGot = msg;
    bRinfo = rinfo;
    b.send(new Uint8Array([9, 8, 7]), rinfo.port, rinfo.address); // binary reply
});
a.on('message', (msg) => { aGot = msg; });

a.bind(0); // ephemeral, default host 127.0.0.1
b.bind(0);
const aPort = a.address().port;
const bPort = b.address().port;
assert(aPort > 0 && bPort > 0, 'ephemeral ports assigned');
assert(a.address().address === '127.0.0.1', 'udp default bind is loopback');

// Broadcast is opt-in; toggling must not throw.
a.setBroadcast(true);
a.setBroadcast(false);

a.send('ping!', bPort); // host omitted → 127.0.0.1

assert(pump(() => aGot !== null, 3000), 'udp round trip completed');

assert(bGot && bGot.length === 5, 'b received 5 bytes');
let text = '';
for (let i = 0; i < bGot.length; i++) text += String.fromCharCode(bGot[i]);
assert(text === 'ping!', 'string payload intact: ' + text);
assert(bRinfo && bRinfo.address === '127.0.0.1', 'rinfo.address is loopback');
assert(bRinfo && bRinfo.port === aPort, 'rinfo.port is sender port');
assert(bRinfo && bRinfo.family === 'IPv4', 'rinfo.family IPv4');
assert(bRinfo && bRinfo.size === 5, 'rinfo.size 5');
assert(aGot.length === 3 && aGot[0] === 9 && aGot[1] === 8 && aGot[2] === 7,
       'binary reply intact');

a.close();
b.close();
assert(pump(() => aClosed && bClosed, 3000), 'both udp sockets closed');

// udp6 shares the code path over ::1.
let sixGot = null;
const s6a = dgram.createSocket('udp6');
const s6b = dgram.createSocket('udp6');
s6b.on('message', (msg, rinfo) => {
    sixGot = msg;
    assert(rinfo.family === 'IPv6', 'udp6 rinfo family: ' + rinfo.family);
});
s6a.bind(0); // default ::1
s6b.bind(0);
assert(s6b.address().address === '::1', 'udp6 default bind is ::1');
s6a.send('six', s6b.address().port);
assert(pump(() => sixGot !== null, 3000), 'udp6 datagram delivered over ::1');
assert(sixGot.length === 3, 'udp6 payload length');

let s6Closed = 0;
s6a.close(() => { s6Closed++; });
s6b.close(() => { s6Closed++; });
assert(pump(() => s6Closed === 2, 3000), 'udp6 sockets closed with callbacks');
