// bro.net.sync — high-level multiplayer over bro.net.
//
// Host in the main context, client in a Worker over loopback (the proven
// pattern from test_net_worker_clone.js). Covers:
//   - late join: a client connecting after spawns receives the replay
//     (factory runs with the current state)
//   - prop changes on the host arrive on the client within a few ticks
//   - delta correctness: unchanged props are not resent (stats counter)
//   - spawn after join + despawn destroys on the client
//   - RPC host->client (call + callTo) and client->host, with cloned args
//   - authority transfer: the client moves an object, the host sees it
//   - handler chaining: app onmessage/onconnect/ondisconnect set AFTER
//     sync.host() still fire, and non-sync messages (raw + clone) pass
//     through untouched in both directions
//   - interpolation: replica smooths an interpolated prop (intermediate
//     values observed) and converges exactly
//   - authority disconnect: objects revert to host authority

const PORT = 28300 + (Date.now() % 400);

function pump(realMs, virtualMs) {
    wallSleep(realMs);
    advanceTime(virtualMs);
}

const sync = bro.net.sync;
assert(sync && typeof sync.host === 'function', 'bro.net.sync is installed');
assert(sync.active === false, 'sync starts inactive');

// --- Host-side registry. Host defs use no interpolation so host-side reads
// in this test are exact. ---
const destroyed = [];
for (const type of ['ball', 'crate']) {
    sync.register(type, {
        create(state) { return { kind: type }; },
        destroy(obj) { destroyed.push(obj); },
        sync: ['x', 'y', 'hp', 'label'],
    });
}

sync.host({ port: PORT, tickHz: 20, keyframeEvery: 8 });
assert(sync.active === true && sync.isHost === true, 'sync active as host');
for (let i = 0; i < 200 && !bro.net.isHosting(); i++) pump(10, 10);
assert(bro.net.isHosting(), 'hosting on port ' + PORT);

// Spawn BEFORE the client joins — exercises the late-join replay.
const b1 = sync.spawn('ball', { x: 1, y: 2, hp: 100, label: 'b1' });
assert(b1.kind === 'ball', 'spawn ran the local factory');
assert(b1.x === 1 && b1.hp === 100, 'spawn assigned initial state');
assert(typeof sync.idOf(b1) === 'number', 'spawned object has an id');
assert(sync.get(sync.idOf(b1)) === b1, 'get(id) resolves the object');
assert(sync.typeOf(b1) === 'ball', 'typeOf resolves');
assert(sync.isAuthority(b1) === true, 'host is the default authority');

// App-level handlers set AFTER sync.host() — must chain, not clobber.
const appMsgs = [];
bro.net.onmessage = (conn, data, channel) => appMsgs.push({ conn, data, channel });
const connEvents = [];
bro.net.onconnect = (conn) => connEvents.push(conn);
const disconnects = [];
bro.net.ondisconnect = (conn, reason) => disconnects.push(conn);
assert(typeof bro.net.onmessage === 'function', 'wrapped onmessage reads back');

// --- Client worker ---
const w = new Worker('../net/worker_net_sync.js');
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

// Poll the client's view of an object until pred(state) or ~15 attempts.
function waitClientState(id, pred) {
    for (let attempt = 0; attempt < 15; attempt++) {
        const r = ask({ cmd: 'query', id }, 1000);
        if (r && r.state && pred(r.state)) return r.state;
        pump(50, 50);
    }
    return null;
}

// --- 1. Late join: the pre-existing ball replays into the client factory. ---
const sp1 = waitMsg((m) => m.ev === 'spawn' && m.type === 'ball');
assert(sp1 !== null, 'client factory ran for the pre-join spawn');
assert(sp1.state.x === 1 && sp1.state.y === 2 && sp1.state.hp === 100
       && sp1.state.label === 'b1', 'late-join replica carries the full state ('
       + JSON.stringify(sp1.state) + ')');
const b1id = sp1.id;
assert(b1id === sync.idOf(b1), 'client and host agree on the object id');
assert(connEvents.length === 1, 'chained app onconnect fired (got ' + connEvents.length + ')');
const clientConn = connEvents[0];

// --- 2. Host prop change arrives within a few ticks. ---
b1.hp = 75;
assert(waitClientState(b1id, (s) => s.hp === 75) !== null, 'hp change replicated');

// --- 3. Delta correctness: change ONE prop; at most one prop rides deltas
// (zero if a keyframe tick happened to carry it). Unchanged props never
// re-send as deltas. ---
const statsBefore = sync._stats();
b1.x = 5;
assert(waitClientState(b1id, (s) => s.x === 5) !== null, 'x change replicated');
const statsAfter = sync._stats();
const deltaPropsSent = statsAfter.deltaProps - statsBefore.deltaProps;
assert(deltaPropsSent <= 1, 'only the changed prop rode deltas (sent '
       + deltaPropsSent + ' props for 1 change)');
assert(statsAfter.keyframes > 0, 'periodic keyframes are going out');

// --- 4. Spawn after join. ---
const c1 = sync.spawn('crate', { x: 0, y: 0, hp: 10, label: 'c1' });
const sp2 = waitMsg((m) => m.ev === 'spawn' && m.type === 'crate');
assert(sp2 !== null, 'post-join spawn replicated');
assert(sp2.state.label === 'c1' && sp2.state.hp === 10, 'post-join spawn state intact');
const c1id = sp2.id;

// --- 5. RPC host -> all clients, with structured args. ---
sync.call('ping', 42, { msg: 'hey' }, new Float32Array([1.5, 2.5]));
const rpc1 = waitMsg((m) => m.ev === 'rpc' && m.args && m.args[0] === 42);
assert(rpc1 !== null, 'host->client rpc arrived');
assert(rpc1.args[1].msg === 'hey', 'rpc object arg roundtrips');
assert(rpc1.args[2] instanceof Float32Array && rpc1.args[2][1] === 2.5,
       'rpc typed-array arg roundtrips');
assert(typeof rpc1.from === 'number', 'rpc handler received the sender conn');

// --- 6. RPC host -> specific client (callTo). ---
sync.callTo(clientConn, 'ping', 'direct');
assert(waitMsg((m) => m.ev === 'rpc' && m.args && m.args[0] === 'direct') !== null,
       'targeted rpc (callTo) arrived');

// --- 7. RPC client -> host. ---
let reported = null;
sync.rpc('report', (from, a, b) => { reported = { from, a, b }; });
assert(ask({ cmd: 'rpc', name: 'report', args: [7, { deep: { n: 3 } }] }) !== null,
       'client issued the rpc');
for (let i = 0; i < 500 && reported === null; i++) pump(10, 10);
assert(reported !== null, 'client->host rpc arrived');
assert(reported.from === clientConn, 'rpc sender is the client conn');
assert(reported.a === 7 && reported.b.deep.n === 3, 'client->host rpc args roundtrip');

// --- 8. Authority transfer: client drives the crate, host follows. ---
sync.setAuthority(c1, clientConn);
assert(sync.isAuthority(c1) === false, 'host relinquished authority');
{
    let mine = null;
    for (let attempt = 0; attempt < 15 && mine !== true; attempt++) {
        const r = ask({ cmd: 'isAuthority', id: c1id }, 1000);
        mine = r && r.value;
        if (mine !== true) pump(50, 50);
    }
    assert(mine === true, 'client learned it has authority');
}
assert(ask({ cmd: 'set', id: c1id, props: { x: 42.5, label: 'moved' } }).ok === true,
       'client wrote to its object');
for (let i = 0; i < 800 && !(c1.x === 42.5 && c1.label === 'moved'); i++) pump(10, 10);
assert(c1.x === 42.5 && c1.label === 'moved',
       'client-authority writes replicated to the host (x=' + c1.x
       + ', label=' + c1.label + ')');

// --- 9. Non-sync traffic passes through to app handlers, both directions. ---
appMsgs.length = 0;
assert(ask({ cmd: 'appsend', value: { hello: 'app', n: 1 }, raw: 'raw-bytes' }) !== null,
       'client sent app-level messages');
for (let i = 0; i < 500 && appMsgs.length < 2; i++) pump(10, 10);
assert(appMsgs.length >= 2, 'host app onmessage got both messages (got '
       + appMsgs.length + ')');
const cloneMsg = appMsgs.find((m) => !(m.data instanceof ArrayBuffer));
const rawMsg = appMsgs.find((m) => m.data instanceof ArrayBuffer);
assert(cloneMsg && cloneMsg.data.hello === 'app' && cloneMsg.channel === 3,
       'app clone value passed through untouched');
assert(rawMsg && new TextDecoder().decode(rawMsg.data) === 'raw-bytes',
       'raw bytes passed through untouched');
assert(appMsgs.every((m) => m.data instanceof ArrayBuffer || m.data.__sync === undefined),
       'no sync traffic leaked into the app handler');

// Host -> client app message despite continuous sync ticking.
bro.net.sendClone(clientConn, { direct: 'toWorker' }, { channel: 5 });
const appEv = waitMsg((m) => m.ev === 'appmsg');
assert(appEv !== null, 'client app onmessage got the direct message');
assert(appEv.data && appEv.data.direct === 'toWorker' && appEv.channel === 5,
       'client app message intact');
assert(wmsgs.filter((m) => m.ev === 'appmsg').length === 1,
       'no sync traffic leaked into the client app handler');

// --- 10. Interpolation: replica smooths y (registered interpolate on the
// client) — intermediate values observed, exact convergence at the end. ---
{
    const seq = ++seqCounter;
    w.postMessage({ cmd: 'watch', id: b1id, prop: 'y', target: 200, seq });
    pump(100, 100);        // let the watcher start sampling
    b1.y = 200;
    const r = waitMsg((m) => m.ev === 'reply' && m.seq === seq, 15000);
    assert(r !== null, 'interpolation watch completed');
    assert(r.final === 200, 'interpolated prop converged exactly (got ' + r.final + ')');
    assert(r.sawIntermediate === true,
           'replica lerped through intermediate values (start ' + r.start + ')');
}

// --- 11. Despawn destroys on the client. ---
sync.despawn(b1);
assert(destroyed.includes(b1), 'despawn ran the local destroy hook');
assert(sync.get(b1id) === null, 'despawned object dropped from the registry');
assert(waitMsg((m) => m.ev === 'despawn' && m.id === b1id) !== null,
       'client destroy hook ran');

// --- 12. Authority disconnect: the crate reverts to host authority. ---
assert(ask({ cmd: 'disconnect' }) !== null, 'client disconnected');
for (let i = 0; i < 800 && disconnects.length === 0; i++) pump(10, 10);
assert(disconnects.length === 1, 'chained app ondisconnect fired');
assert(sync.isAuthority(c1) === true, 'client-owned object reverted to host authority');

w.terminate();
bro.net.close();
advanceTime(100);
console.log('[test] PASS');
