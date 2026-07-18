// bro.net.sync — per-RPC configuration (Godot rpc_config analog).
//
// Host in the main context, client in a Worker over loopback (the proven
// pattern from test_net_sync.js). Covers:
//   - sync.rpc(name, fn, opts) validation of mode/authority values
//   - mode:'unreliable' RPCs arrive both directions (loopback won't drop),
//     including a config-only registration (fn = null) on the sender
//   - callLocal: the caller's own handler fires exactly once, synchronously
//     after send, with fromConn 0 — on the host AND on a client
//   - authority:'host': a client call is rejected host-side (warn + stats
//     counter, handler never runs); host -> client still works
//   - unknown RPC names warn and drop, never throw across the wire

const PORT = 28700 + (Date.now() % 400);

function pump(realMs, virtualMs) {
    wallSleep(realMs);
    advanceTime(virtualMs);
}

const sync = bro.net.sync;
assert(sync && typeof sync.rpc === 'function', 'bro.net.sync is installed');

// --- Registration validation. ---
for (const bad of [{ mode: 'nope' }, { authority: 'client' }]) {
    let threw = false;
    try { sync.rpc('bad', () => {}, bad); } catch (e) { threw = true; }
    assert(threw, 'sync.rpc rejected ' + JSON.stringify(bad));
}
{
    let threw = false;
    try { sync.rpc('bad', 42); } catch (e) { threw = true; }
    assert(threw, 'sync.rpc rejected a non-function handler');
}

// --- Capture host-side warns (the rejection/unknown paths log, not throw). ---
const warns = [];
const origWarn = console.warn;
console.warn = (...a) => { warns.push(a.join(' ')); origWarn(...a); };

// --- Host-side RPCs, mirroring the worker's registrations. ---
const got = { uping: [], announce: [], admin: [], upstream: [] };
sync.rpc('uping', (from, ...args) => got.uping.push({ from, args }),
         { mode: 'unreliable' });
sync.rpc('announce', (from, ...args) => got.announce.push({ from, args }),
         { callLocal: true });
sync.rpc('admin', (from, ...args) => got.admin.push({ from, args }),
         { authority: 'host' });
sync.rpc('upstream', (from, ...args) => got.upstream.push({ from, args }),
         { mode: 'unreliable' });

sync.host({ port: PORT, tickHz: 20 });
for (let i = 0; i < 200 && !bro.net.isHosting(); i++) pump(10, 10);
assert(bro.net.isHosting(), 'hosting on port ' + PORT);

const connEvents = [];
bro.net.onconnect = (conn) => connEvents.push(conn);

// --- Client worker. ---
const w = new Worker('../net/worker_net_sync_rpc.js');
const wmsgs = [];
w.onmessage = (e) => wmsgs.push(e.data);
w.postMessage({ cmd: 'join', port: PORT });

function waitMsg(pred, ms = 8000) {
    for (let t = 0; t < ms; t += 10) {
        const hit = wmsgs.find(pred);
        if (hit) return hit;
        pump(10, 10);
    }
    return null;
}

let seqCounter = 0;
function ask(cmd, ms = 8000) {
    const seq = ++seqCounter;
    w.postMessage({ ...cmd, seq });
    return waitMsg((m) => m.ev === 'reply' && m.seq === seq, ms);
}

assert(waitMsg((m) => m.ev === 'joined') !== null, 'client joined');
for (let i = 0; i < 500 && connEvents.length === 0; i++) pump(10, 10);
assert(connEvents.length === 1, 'client connected');
const clientConn = connEvents[0];

// --- 1. Unreliable RPC host -> client arrives (loopback won't drop). ---
sync.call('uping', 1, 'u');
const u1 = waitMsg((m) => m.ev === 'rpc' && m.name === 'uping');
assert(u1 !== null, 'unreliable host->client rpc arrived');
assert(u1.args[0] === 1 && u1.args[1] === 'u', 'unreliable rpc args intact');
assert(typeof u1.from === 'number' && u1.from !== 0,
       'client handler saw a real (non-local) sender conn');

// --- 2. Unreliable RPC client -> host, config-only registration (fn null)
// on the sending side. ---
assert(ask({ cmd: 'call', name: 'upstream', args: [9] }) !== null,
       'client issued the unreliable rpc');
for (let i = 0; i < 500 && got.upstream.length === 0; i++) pump(10, 10);
assert(got.upstream.length === 1, 'unreliable client->host rpc arrived');
assert(got.upstream[0].from === clientConn && got.upstream[0].args[0] === 9,
       'unreliable client->host rpc sender + args intact');

// --- 3. callLocal on the host: local handler fires exactly once,
// synchronously after send, with fromConn 0. ---
sync.call('announce', 'from-host');
assert(got.announce.length === 1, 'callLocal fired exactly once, synchronously');
assert(got.announce[0].from === 0, 'local invocation has fromConn 0');
assert(got.announce[0].args[0] === 'from-host', 'local invocation got the args');
const a1 = waitMsg((m) => m.ev === 'rpc' && m.name === 'announce');
assert(a1 !== null, 'callLocal rpc still reached the client');
assert(got.announce.length === 1,
       'no duplicate local invocation after the wire round-trip (got '
       + got.announce.length + ')');

// --- 4. callLocal on a client: its own handler fires locally (fromConn 0)
// and the host handler fires once over the wire. ---
assert(ask({ cmd: 'call', name: 'announce', args: ['from-client'] }) !== null,
       'client issued the callLocal rpc');
const a2 = waitMsg((m) => m.ev === 'rpc' && m.name === 'announce' && m.from === 0);
assert(a2 !== null, 'client callLocal fired its own handler with fromConn 0');
assert(a2.args[0] === 'from-client', 'client local invocation got the args');
for (let i = 0; i < 500 && got.announce.length < 2; i++) pump(10, 10);
assert(got.announce.length === 2, 'host handler also ran once');
assert(got.announce[1].from === clientConn, 'wire invocation kept the true sender');

// --- 5. authority:'host': the client's call is rejected host-side. ---
const rejectedBefore = sync._stats().rpcsRejected;
assert(rejectedBefore === 0, 'no rejections yet');
assert(ask({ cmd: 'call', name: 'admin', args: ['sneaky'] }) !== null,
       'client issued the host-only rpc');
for (let i = 0; i < 500 && sync._stats().rpcsRejected === rejectedBefore; i++)
    pump(10, 10);
assert(sync._stats().rpcsRejected === rejectedBefore + 1,
       'host counted the rejection');
assert(got.admin.length === 0, 'host-only handler never ran for the client call');
assert(warns.some((s) => s.includes('admin') && s.includes('host-only')),
       'rejection logged with the rpc name');

// Host -> client still works for the same RPC.
sync.call('admin', 'legit');
const adm = waitMsg((m) => m.ev === 'rpc' && m.name === 'admin');
assert(adm !== null && adm.args[0] === 'legit',
       'host may still invoke the host-only rpc on clients');

// --- 6. Unknown RPC warns and drops; the connection stays healthy. ---
warns.length = 0;
assert(ask({ cmd: 'call', name: 'nonexistent', args: [] }) !== null,
       'client issued an unknown rpc');
for (let i = 0; i < 500 && warns.length === 0; i++) pump(10, 10);
assert(warns.some((s) => s.includes('nonexistent')),
       'unknown rpc logged, not thrown');
assert(ask({ cmd: 'call', name: 'upstream', args: [10] }) !== null,
       'client issued a follow-up rpc');
for (let i = 0; i < 500 && got.upstream.length < 2; i++) pump(10, 10);
assert(got.upstream.length === 2 && got.upstream[1].args[0] === 10,
       'session still healthy after the unknown rpc');

console.warn = origWarn;
w.terminate();
bro.net.close();
advanceTime(100);
console.log('[test] PASS');
