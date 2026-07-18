// bro.net.sync — per-RPC configuration (Godot rpc_config analog) and
// client->client targeted RPC via host relay.
//
// Host in the main context, clients in Workers over loopback (the proven
// pattern from test_net_sync.js). Covers:
//   - sync.rpc(name, fn, opts) validation of mode/authority values
//   - mode:'unreliable' RPCs arrive both directions (loopback won't drop),
//     including a config-only registration (fn = null) on the sender
//   - callLocal: the caller's own handler fires exactly once, synchronously
//     after send, with fromConn 0 — on the host AND on a client
//   - authority:'host': a client call is rejected host-side (warn + stats
//     counter, handler never runs); host -> client still works
//   - unknown RPC names warn and drop, never throw across the wire
//   - client->client callTo relays through the host with the TRUE origin
//     stamped (target handler's fromConn is the origin client's conn id),
//     both directions, host handler not involved
//   - relay:false and authority:'host' block the relay host-side (warn +
//     stats counter, target never receives)

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

// --- Host-side RPCs, mirroring the workers' registrations. 'greet' is
// deliberately NOT registered on the host: relaying is the default for
// unregistered names. 'secret' is pinned relay:false — enforcement reads the
// HOST's registration. ---
const got = { uping: [], announce: [], admin: [], upstream: [] };
sync.rpc('uping', (from, ...args) => got.uping.push({ from, args }),
         { mode: 'unreliable' });
sync.rpc('announce', (from, ...args) => got.announce.push({ from, args }),
         { callLocal: true });
sync.rpc('admin', (from, ...args) => got.admin.push({ from, args }),
         { authority: 'host' });
sync.rpc('upstream', (from, ...args) => got.upstream.push({ from, args }),
         { mode: 'unreliable' });
sync.rpc('secret', null, { relay: false });

sync.host({ port: PORT, tickHz: 20 });
for (let i = 0; i < 200 && !bro.net.isHosting(); i++) pump(10, 10);
assert(bro.net.isHosting(), 'hosting on port ' + PORT);

const connEvents = [];
bro.net.onconnect = (conn) => connEvents.push(conn);

// --- Client workers (shared worker file; each is one sync client). ---
let seqCounter = 0;
function makeClient() {
    const wk = new Worker('../net/worker_net_sync_rpc.js');
    const msgs = [];
    wk.onmessage = (e) => msgs.push(e.data);
    const wait = (pred, ms = 8000) => {
        for (let t = 0; t < ms; t += 10) {
            const hit = msgs.find(pred);
            if (hit) return hit;
            pump(10, 10);
        }
        return null;
    };
    const ask = (cmd, ms = 8000) => {
        const seq = ++seqCounter;
        wk.postMessage({ ...cmd, seq });
        return wait((m) => m.ev === 'reply' && m.seq === seq, ms);
    };
    wk.postMessage({ cmd: 'join', port: PORT });
    return { wk, msgs, wait, ask };
}

const c1 = makeClient();
assert(c1.wait((m) => m.ev === 'joined') !== null, 'client 1 joined');
for (let i = 0; i < 500 && connEvents.length === 0; i++) pump(10, 10);
assert(connEvents.length === 1, 'client 1 connected');
const conn1 = connEvents[0];

// --- 1. Unreliable RPC host -> client arrives (loopback won't drop). ---
sync.call('uping', 1, 'u');
const u1 = c1.wait((m) => m.ev === 'rpc' && m.name === 'uping');
assert(u1 !== null, 'unreliable host->client rpc arrived');
assert(u1.args[0] === 1 && u1.args[1] === 'u', 'unreliable rpc args intact');
assert(typeof u1.from === 'number' && u1.from !== 0,
       'client handler saw a real (non-local) sender conn');

// --- 2. Unreliable RPC client -> host, config-only registration (fn null)
// on the sending side. ---
assert(c1.ask({ cmd: 'call', name: 'upstream', args: [9] }) !== null,
       'client issued the unreliable rpc');
for (let i = 0; i < 500 && got.upstream.length === 0; i++) pump(10, 10);
assert(got.upstream.length === 1, 'unreliable client->host rpc arrived');
assert(got.upstream[0].from === conn1 && got.upstream[0].args[0] === 9,
       'unreliable client->host rpc sender + args intact');

// --- 3. callLocal on the host: local handler fires exactly once,
// synchronously after send, with fromConn 0. ---
sync.call('announce', 'from-host');
assert(got.announce.length === 1, 'callLocal fired exactly once, synchronously');
assert(got.announce[0].from === 0, 'local invocation has fromConn 0');
assert(got.announce[0].args[0] === 'from-host', 'local invocation got the args');
const a1 = c1.wait((m) => m.ev === 'rpc' && m.name === 'announce');
assert(a1 !== null, 'callLocal rpc still reached the client');
assert(got.announce.length === 1,
       'no duplicate local invocation after the wire round-trip (got '
       + got.announce.length + ')');

// --- 4. callLocal on a client: its own handler fires locally (fromConn 0)
// and the host handler fires once over the wire. ---
assert(c1.ask({ cmd: 'call', name: 'announce', args: ['from-client'] }) !== null,
       'client issued the callLocal rpc');
const a2 = c1.wait((m) => m.ev === 'rpc' && m.name === 'announce' && m.from === 0);
assert(a2 !== null, 'client callLocal fired its own handler with fromConn 0');
assert(a2.args[0] === 'from-client', 'client local invocation got the args');
for (let i = 0; i < 500 && got.announce.length < 2; i++) pump(10, 10);
assert(got.announce.length === 2, 'host handler also ran once');
assert(got.announce[1].from === conn1, 'wire invocation kept the true sender');

// --- 5. authority:'host': the client's call is rejected host-side. ---
assert(sync._stats().rpcsRejected === 0, 'no rejections yet');
assert(c1.ask({ cmd: 'call', name: 'admin', args: ['sneaky'] }) !== null,
       'client issued the host-only rpc');
for (let i = 0; i < 500 && sync._stats().rpcsRejected === 0; i++) pump(10, 10);
assert(sync._stats().rpcsRejected === 1, 'host counted the rejection');
assert(got.admin.length === 0, 'host-only handler never ran for the client call');
assert(warns.some((s) => s.includes('admin') && s.includes('host-only')),
       'rejection logged with the rpc name');

// Host -> client still works for the same RPC.
sync.call('admin', 'legit');
const adm = c1.wait((m) => m.ev === 'rpc' && m.name === 'admin');
assert(adm !== null && adm.args[0] === 'legit',
       'host may still invoke the host-only rpc on clients');

// --- 6. Unknown RPC warns and drops; the connection stays healthy. ---
warns.length = 0;
assert(c1.ask({ cmd: 'call', name: 'nonexistent', args: [] }) !== null,
       'client issued an unknown rpc');
for (let i = 0; i < 500 && warns.length === 0; i++) pump(10, 10);
assert(warns.some((s) => s.includes('nonexistent')),
       'unknown rpc logged, not thrown');
assert(c1.ask({ cmd: 'call', name: 'upstream', args: [10] }) !== null,
       'client issued a follow-up rpc');
for (let i = 0; i < 500 && got.upstream.length < 2; i++) pump(10, 10);
assert(got.upstream.length === 2 && got.upstream[1].args[0] === 10,
       'session still healthy after the unknown rpc');

// --- 7. Client->client targeted RPC via host relay, true origin stamped. ---
const c2 = makeClient();
assert(c2.wait((m) => m.ev === 'joined') !== null, 'client 2 joined');
for (let i = 0; i < 500 && connEvents.length < 2; i++) pump(10, 10);
assert(connEvents.length === 2, 'client 2 connected');
const conn2 = connEvents[1];

assert(c1.ask({ cmd: 'callTo', to: conn2, name: 'greet', args: ['hi-from-1'] }) !== null,
       'client 1 issued the targeted rpc');
const g1 = c2.wait((m) => m.ev === 'rpc' && m.name === 'greet');
assert(g1 !== null, 'client->client rpc delivered through the host');
assert(g1.from === conn1,
       'relayed rpc carries the TRUE origin (fromConn ' + g1.from
       + ', expected ' + conn1 + ')');
assert(g1.args[0] === 'hi-from-1', 'relayed rpc args intact');
assert(sync._stats().rpcsRelayed === 1, 'host counted the relay');

// Reply the other way, targeting the fromConn the handler received.
assert(c2.ask({ cmd: 'callTo', to: conn1, name: 'greet', args: ['hi-back'] }) !== null,
       'client 2 replied by conn id');
const g2 = c1.wait((m) => m.ev === 'rpc' && m.name === 'greet');
assert(g2 !== null && g2.from === conn2 && g2.args[0] === 'hi-back',
       'reverse relay delivered with the true origin');

// --- 8. relay:false blocks the relay host-side. ---
warns.length = 0;
const rejBefore = sync._stats().rpcsRejected;
assert(c1.ask({ cmd: 'callTo', to: conn2, name: 'secret', args: ['leak?'] }) !== null,
       'client 1 attempted the pinned rpc');
for (let i = 0; i < 500 && sync._stats().rpcsRejected === rejBefore; i++)
    pump(10, 10);
assert(sync._stats().rpcsRejected === rejBefore + 1,
       'host counted the refused relay');
assert(warns.some((s) => s.includes('secret') && s.includes('relay')),
       'refused relay logged with the rpc name');

// authority:'host' also blocks relaying, even with relay left at default.
assert(c1.ask({ cmd: 'callTo', to: conn2, name: 'admin', args: ['esc'] }) !== null,
       'client 1 attempted to relay a host-only rpc');
for (let i = 0; i < 500 && sync._stats().rpcsRejected === rejBefore + 1; i++)
    pump(10, 10);
assert(sync._stats().rpcsRejected === rejBefore + 2,
       'host refused to relay the host-only rpc');

// A reliable sentinel after the blocked frames: control-channel ordering
// means that when it arrives, the blocked rpcs would already have arrived —
// so their absence proves the block.
assert(c1.ask({ cmd: 'callTo', to: conn2, name: 'greet', args: ['sentinel'] }) !== null,
       'client 1 sent the sentinel');
assert(c2.wait((m) => m.ev === 'rpc' && m.name === 'greet'
                      && m.args[0] === 'sentinel') !== null,
       'sentinel arrived at client 2');
assert(!c2.msgs.some((m) => m.ev === 'rpc'
                            && (m.name === 'secret' || m.args[0] === 'esc')),
       'blocked rpcs never reached client 2');
assert(sync._stats().rpcsRelayed === 3, 'only the permitted rpcs were relayed');

console.warn = origWarn;
c1.wk.terminate();
c2.wk.terminate();
bro.net.close();
advanceTime(100);
console.log('[test] PASS');
