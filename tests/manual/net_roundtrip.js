// Net roundtrip smoke: this context is one subscriber. We host on a port,
// connect to ourselves, and observe the callbacks fire.
//
// Single-context test because NetBindings is per-context and this script runs
// in one context — this exercises the service thread's command→event path
// for one subscriber. Two-subscriber (worker) exercise is a future test.

let gotHosting = false;
let gotConnect = false;
let gotDisconnect = false;
let gotMessage = null;

bro.net.onconnect = (connId) => {
    gotConnect = true;
    console.log('[test] onconnect conn=' + connId);
    bro.net.send(connId, 'hello');
};

bro.net.onmessage = (connId, data) => {
    gotMessage = new Uint8Array(data);
    console.log('[test] onmessage bytes=' + gotMessage.length);
};

bro.net.ondisconnect = (connId, reason) => {
    gotDisconnect = true;
    console.log('[test] ondisconnect conn=' + connId + ' reason=' + reason);
};

// NetService runs on a real OS thread, so it needs wall-clock time to open
// the socket and accept connections. advanceTime() only burns virtual time
// (and drains the event queue) — it gives the real thread no time to work.
// Pump both: wallSleep() lets the service thread make progress, advanceTime()
// drains its event queue and fires the JS callbacks.
function pump(realMs, virtualMs) {
    wallSleep(realMs);
    advanceTime(virtualMs);
}

console.log('[test] hosting on 27066');
bro.net.host(27066);

for (let i = 0; i < 40 && !bro.net.isHosting(); i++) pump(25, 25);
gotHosting = bro.net.isHosting();
console.log('[test] isHosting=' + gotHosting);

console.log('[test] connecting to 127.0.0.1:27066');
bro.net.connect('127.0.0.1:27066');

// Pump until the service thread accepts the connection and echoes the
// message back, or we time out.
for (let i = 0; i < 40 && gotMessage === null; i++) pump(25, 25);

assert(gotHosting, 'should be hosting');
assert(gotConnect, 'should have received onconnect');
assert(gotMessage !== null, 'should have received onmessage');
console.log('[test] PASS');

bro.net.close();
advanceTime(100);
