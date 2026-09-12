// bro.net.sync — Godot-style high-level multiplayer over the bro.net
// primitives: spawn/despawn replication, per-object authority, snapshot+delta
// state sync with periodic keyframes, and named RPCs. Pure JS, layered on
// bro.net.sendClone/broadcastClone; evaluated by NetBindings::install in every
// context that gets a real bro.net (main document and workers), so a worker
// can host or join exactly like the main thread. See docs/net-sync-api.js.
//
// Topology is a star: one host, N clients. The host owns object identity
// (spawn/despawn/authority), relays client-authority state to the other
// clients, and relays client->client targeted RPCs (callTo) after validating
// them against its own per-RPC config. Objects are id-keyed plain JS objects produced by registered type
// factories — nothing here touches the DOM or the scene graph.
//
// Wire discipline (all sync traffic is tagged clone values {__sync: op}):
//   channel 0 (control) reliable ......... spawn/despawn/authority/keyframes,
//                                          reliable RPCs (the default)
//   channel 1 (state)   unreliable+nodelay delta state updates, RPCs
//                                          registered mode:'unreliable'
// Reliable ordering holds per channel, so everything on the control channel is
// seen in a consistent order; deltas are freshest-data-only and carry a
// per-object sequence number so late/reordered ones are dropped. Loss is
// repaired by the periodic keyframes. Raw sends and untagged clone values pass
// through untouched to the app's own bro.net.onmessage.
(function () {
    'use strict';

    const broNs = globalThis.bro;
    if (!broNs || !broNs.net || broNs.net.available === false) return;
    const net = broNs.net;

    const PROTOCOL = 1;          // bumped on incompatible sync-protocol changes
    const TAG = '__sync';        // reserved key marking sync clone values
    const CH_CONTROL = 0;
    const CH_STATE = 1;
    const CONTROL_OPTS = { reliable: true, channel: CH_CONTROL };
    const STATE_OPTS = { reliable: false, channel: CH_STATE, nodelay: true };
    const INTERP_STEP_MS = 16;   // replica-side interpolation timer cadence

    // ── Module state ─────────────────────────────────────────────────────────
    const types = new Map();     // type name -> normalized def
    const records = new Map();   // id -> record
    const byObj = new WeakMap(); // obj -> record
    const rpcs = new Map();      // rpc name -> {fn, mode, callLocal, authority, relay}
    const appHandlers = { onmessage: null, onconnect: null, ondisconnect: null };

    let active = false;
    let isHost = false;
    let hostConn = null;         // client side: the connection to the host
    let nextId = 1;              // host-assigned object ids
    let tickMs = 50;             // 1000 / tickHz
    let keyframeEvery = 30;      // full keyframe every N ticks
    let tickTimer = null;
    let tickCount = 0;
    let interpTimer = null;

    const stats = {
        ticks: 0,
        deltaMsgs: 0, deltaEntities: 0, deltaProps: 0,
        keyframes: 0,
        rpcsSent: 0, rpcsRecv: 0, rpcsRejected: 0, rpcsRelayed: 0,
        applied: 0,          // individual prop values applied from the wire
        stale: 0,            // state entries dropped by the sequence guard
        unknown: 0,          // state entries for ids we don't know (spawn in flight)
    };

    // A record tracks one replicated object.
    //   id        stable numeric id (host-assigned)
    //   type      registered type name
    //   def       normalized type def
    //   obj       the app's object (returned by def.create)
    //   authority host view: 0 = host, else the owning client's conn id.
    //             Clients don't know each other's conn ids; they only track
    //             whether THEY are the authority (local).
    //   local     true when this context is the object's authority
    //   lastSent  authority side: snapshot at last send (null forces full send)
    //   sendSeq   authority side: per-object sequence stamped on sends
    //   recvSeq   replica side: highest sequence applied (older ones dropped)
    //   interp    replica side: in-flight interpolations {prop: {from,to,p}}
    function makeRecord(id, type, def, obj, local) {
        return { id, type, def, obj, authority: 0, local,
                 lastSent: null, sendSeq: 0, recvSeq: -1, interp: null };
    }

    // ── Type registry ────────────────────────────────────────────────────────
    function normalizeDef(def) {
        if (!def || typeof def.create !== 'function')
            throw new TypeError('sync.register: def.create(state) is required');
        if (def.destroy != null && typeof def.destroy !== 'function')
            throw new TypeError('sync.register: def.destroy must be a function');
        let props, interpolate;
        const s = def.sync;
        if (Array.isArray(s)) {
            props = s.slice();
            interpolate = [];
        } else if (s && typeof s === 'object') {
            props = Array.isArray(s.props) ? s.props.slice() : [];
            interpolate = Array.isArray(s.interpolate) ? s.interpolate.slice() : [];
        } else {
            throw new TypeError("sync.register: def.sync must be ['prop', ...] " +
                                'or {props: [...], interpolate: [...]}');
        }
        for (const p of interpolate) if (!props.includes(p)) props.push(p);
        return {
            create: def.create,
            destroy: def.destroy || null,
            props,
            propSet: new Set(props),
            interp: new Set(interpolate),
            acceptAll: false,
        };
    }

    // Fallback def for spawns of a type this context never registered: track
    // state on a bare object so a late register at least has the data, and
    // warn — normally both sides register the same types before connecting.
    function fallbackDef(state) {
        const props = Object.keys(state);
        return { create: () => ({}), destroy: null,
                 props, propSet: new Set(props), interp: new Set(),
                 acceptAll: true };
    }

    // ── Snapshots, diffs, application ────────────────────────────────────────
    function snapshot(rec) {
        const st = {};
        for (const p of rec.def.props) {
            const v = rec.obj[p];
            if (typeof v === 'function') continue; // not clonable; skip loudly-typed mistakes
            st[p] = v;
        }
        return st;
    }

    function applyState(rec, st) {
        for (const p in st) {
            if (!rec.def.propSet.has(p)) {
                if (!rec.def.acceptAll) continue; // authority may only write declared props
                rec.def.props.push(p);
                rec.def.propSet.add(p);
            }
            const v = st[p];
            if (rec.def.interp.has(p) && typeof v === 'number' &&
                typeof rec.obj[p] === 'number' && rec.obj[p] !== v) {
                setInterpTarget(rec, p, v);
            } else {
                if (rec.interp) delete rec.interp[p]; // direct set overrides in-flight lerp
                rec.obj[p] = v;
            }
            stats.applied++;
        }
    }

    // ── Replica-side interpolation ───────────────────────────────────────────
    // Numeric props listed in the type's `interpolate` are not applied
    // instantly on the replica: each incoming value becomes a lerp target
    // reached over one tick interval (so displayed state trails the authority
    // by ~1/tickHz — that added latency is the price of smoothness). Progress
    // is accumulated per timer step rather than read off a clock so it behaves
    // identically under headless virtual time.
    function setInterpTarget(rec, prop, to) {
        if (!rec.interp) rec.interp = {};
        rec.interp[prop] = { from: rec.obj[prop], to, p: 0 };
        if (interpTimer === null) interpTimer = setInterval(stepInterp, INTERP_STEP_MS);
    }

    function stepInterp() {
        let any = false;
        for (const rec of records.values()) {
            if (!rec.interp) continue;
            for (const prop in rec.interp) {
                const it = rec.interp[prop];
                it.p += INTERP_STEP_MS / tickMs;
                if (it.p >= 1) {
                    rec.obj[prop] = it.to;
                    delete rec.interp[prop];
                } else {
                    rec.obj[prop] = it.from + (it.to - it.from) * it.p;
                    any = true;
                }
            }
            if (rec.interp && Object.keys(rec.interp).length === 0) rec.interp = null;
        }
        if (!any && interpTimer !== null) {
            clearInterval(interpTimer);
            interpTimer = null;
        }
    }

    // ── Sending ──────────────────────────────────────────────────────────────
    // Host: broadcast (optionally excluding the client the data came from).
    // Client: everything goes to the host, which relays.
    function sendSync(msg, opts, exceptConn) {
        if (isHost) {
            if (exceptConn == null) {
                net.broadcastClone(msg, opts);
            } else {
                for (const c of net.connections())
                    if (c !== exceptConn) net.sendClone(c, msg, opts);
            }
        } else if (hostConn !== null) {
            net.sendClone(hostConn, msg, opts);
        }
    }

    // ── The authority tick ───────────────────────────────────────────────────
    // Every tick, snapshot each locally-authoritative object and send what
    // changed (unreliable+nodelay deltas). Every keyframeEvery ticks — and for
    // objects that never sent (fresh spawn/authority) — send the full state
    // reliably on the control channel instead, which repairs any lost deltas.
    function tick() {
        if (!active) return;
        tickCount++;
        stats.ticks++;
        const isKeyframeTick = (tickCount % keyframeEvery) === 0;

        const kfEntries = [];
        const dEntries = [];
        for (const rec of records.values()) {
            if (!rec.local) continue;
            const st = snapshot(rec);
            if (isKeyframeTick || rec.lastSent === null) {
                rec.sendSeq++;
                kfEntries.push([rec.id, rec.sendSeq, st]);
                rec.lastSent = st;
                continue;
            }
            let delta = null;
            for (const p of rec.def.props) {
                if (!Object.is(st[p], rec.lastSent[p])) {
                    if (!delta) delta = {};
                    delta[p] = st[p];
                }
            }
            if (delta) {
                rec.sendSeq++;
                dEntries.push([rec.id, rec.sendSeq, delta]);
                rec.lastSent = st;
                stats.deltaEntities++;
                stats.deltaProps += Object.keys(delta).length;
            }
        }
        if (kfEntries.length > 0) {
            stats.keyframes++;
            sendSync({ [TAG]: 'kf', e: kfEntries }, CONTROL_OPTS);
        }
        if (dEntries.length > 0) {
            stats.deltaMsgs++;
            sendSync({ [TAG]: 'd', e: dEntries }, STATE_OPTS);
        }
    }

    // ── Receiving ────────────────────────────────────────────────────────────
    function isSyncMsg(data) {
        return data !== null && typeof data === 'object' &&
               !(data instanceof ArrayBuffer) && typeof data[TAG] === 'string';
    }

    function handleSync(conn, msg) {
        const op = msg[TAG];
        if (isHost) {
            // Clients may only send state for objects they own, and RPCs.
            switch (op) {
                case 'd':
                case 'kf': hostApplyState(conn, msg, op); break;
                case 'rpc': hostRpc(conn, msg); break;
                default:
                    console.warn('[net.sync] host ignoring op "' + op +
                                 '" from client ' + conn);
            }
            return;
        }
        // Client: only the host is trusted.
        if (hostConn === null || conn !== hostConn) return;
        switch (op) {
            case 'hi':
                if (msg.v !== PROTOCOL) {
                    console.warn('[net.sync] protocol version mismatch: host v' +
                                 msg.v + ', local v' + PROTOCOL);
                }
                break;
            case 'sp': applySpawn(msg); break;
            case 'ds': applyDespawn(msg.id); break;
            case 'au': applyAuthority(msg); break;
            case 'd':
            case 'kf': clientApplyState(msg); break;
            // A relayed frame carries the true origin in `f` (stamped by the
            // host); direct host RPCs don't, so the sender is the host conn.
            case 'rpc': invokeRpc(typeof msg.f === 'number' ? msg.f : conn, msg); break;
            default:
                console.warn('[net.sync] client ignoring unknown op "' + op + '"');
        }
    }

    // Host receives state from a client: verify per-object authority, apply,
    // and relay to the other clients (same op, same channel semantics, same
    // per-object sequence numbers — there is exactly one authority per object,
    // so sequences stay coherent through the relay).
    function hostApplyState(conn, msg, op) {
        if (!Array.isArray(msg.e)) return;
        const relay = [];
        for (const entry of msg.e) {
            if (!Array.isArray(entry) || entry.length < 3) continue;
            const [id, seq, st] = entry;
            const rec = records.get(id);
            if (!rec) { stats.unknown++; continue; }
            if (rec.authority !== conn) {
                // Not this client's object — a bug, a race right after an
                // authority transfer, or a hostile peer. Never apply.
                stats.stale++;
                continue;
            }
            if (typeof seq !== 'number' || seq <= rec.recvSeq) { stats.stale++; continue; }
            if (!st || typeof st !== 'object') continue;
            rec.recvSeq = seq;
            applyState(rec, st);
            relay.push(entry);
        }
        if (relay.length > 0) {
            sendSync({ [TAG]: op, e: relay },
                     op === 'kf' ? CONTROL_OPTS : STATE_OPTS, conn);
        }
    }

    function clientApplyState(msg) {
        if (!Array.isArray(msg.e)) return;
        for (const entry of msg.e) {
            if (!Array.isArray(entry) || entry.length < 3) continue;
            const [id, seq, st] = entry;
            const rec = records.get(id);
            // Unknown id: the spawn (reliable, control channel) may still be
            // in flight behind this unreliable delta. Drop it — the spawn
            // carries full state, and keyframes repair anything after that.
            if (!rec) { stats.unknown++; continue; }
            if (rec.local) continue;      // we are the authority; ignore echoes
            if (typeof seq !== 'number' || seq <= rec.recvSeq) { stats.stale++; continue; }
            if (!st || typeof st !== 'object') continue;
            rec.recvSeq = seq;
            applyState(rec, st);
        }
    }

    function applySpawn(msg) {
        const { id, ty, st } = msg;
        if (typeof id !== 'number' || records.has(id)) return;
        const state = (st && typeof st === 'object') ? st : {};
        let def = types.get(ty);
        if (!def) {
            console.warn('[net.sync] spawn of unregistered type "' + ty +
                         '" (id ' + id + ') — tracking state on a bare object');
            def = fallbackDef(state);
        }
        let obj = def.create(state);
        if (obj === null || typeof obj !== 'object') {
            console.warn('[net.sync] create("' + ty + '") did not return an ' +
                         'object — substituting {}');
            obj = {};
        }
        for (const p of def.props) if (p in state) obj[p] = state[p];
        const rec = makeRecord(id, ty, def, obj, !!msg.mine);
        records.set(id, rec);
        byObj.set(obj, rec);
    }

    function applyDespawn(id) {
        const rec = records.get(id);
        if (!rec) return;
        records.delete(id);
        byObj.delete(rec.obj);
        if (rec.def.destroy) rec.def.destroy(rec.obj);
    }

    function applyAuthority(msg) {
        const rec = records.get(msg.id);
        if (!rec) return;
        rec.local = !!msg.mine;
        rec.sendSeq = 0;
        rec.recvSeq = -1;
        rec.interp = null;
        // Fresh authority does a full send on its next tick (reliable, via the
        // lastSent === null keyframe path), giving replicas a clean baseline.
        rec.lastSent = null;
    }

    // Per-RPC configuration (Godot's rpc_config analog). Reads fall back to
    // the defaults for names never registered in this context — mode and
    // callLocal are the CALLER's registration, authority (and relay, see the
    // relay path) are enforced from the HOST's registration; register the same
    // config on every peer, exactly like Godot.
    const RPC_DEFAULTS = Object.freeze({
        fn: null, mode: 'reliable', callLocal: false, authority: 'any', relay: true,
    });

    function rpcConfig(name) {
        return rpcs.get(name) || RPC_DEFAULTS;
    }

    // Unreliable RPCs ride the state lane (channel 1, unreliable+nodelay) —
    // the same lane as delta state updates, keeping the two-lane wire
    // discipline: channel 0 = everything reliable, channel 1 = everything
    // loss-tolerant. Lost or reordered unreliable RPCs are simply gone.
    function rpcOpts(cfg) {
        return cfg.mode === 'unreliable' ? STATE_OPTS : CONTROL_OPTS;
    }

    function invokeRpc(from, msg) {
        const fn = rpcConfig(msg.n).fn;
        if (typeof fn !== 'function') {
            console.warn('[net.sync] no rpc handler registered for "' + msg.n + '"');
            return;
        }
        stats.rpcsRecv++;
        fn(from, ...(Array.isArray(msg.a) ? msg.a : []));
    }

    // Host receives an rpc frame from a client: enforce per-RPC authority,
    // and route frames carrying `to` (a client->client callTo) to the relay.
    // Rejections warn and drop — never a throw across the wire. Any `f` a
    // client stamped is ignored: the host only trusts the connection it
    // actually received the frame on.
    function hostRpc(conn, msg) {
        if (msg.to != null) { relayRpc(conn, msg); return; }
        if (rpcConfig(msg.n).authority === 'host') {
            stats.rpcsRejected++;
            console.warn('[net.sync] rejecting rpc "' + msg.n + '" from client ' +
                         conn + ': registered authority is host-only');
            return;
        }
        invokeRpc(conn, msg);
    }

    // Client->client targeted RPC. The origin client sent {rpc, n, a, to};
    // the host validates against ITS registration — the RPC must allow relay
    // and must not be host-authority — then forwards to the target with the
    // true origin stamped in `f`, so the target's handler sees the origin's
    // conn id. The forwarded frame carries no `to` and clients never relay,
    // so a relayed frame is never re-relayed.
    function relayRpc(conn, msg) {
        const cfg = rpcConfig(msg.n);
        if (cfg.relay === false) {
            stats.rpcsRejected++;
            console.warn('[net.sync] refusing to relay rpc "' + msg.n +
                         '" from client ' + conn + ': registered relay:false');
            return;
        }
        if (cfg.authority === 'host') {
            stats.rpcsRejected++;
            console.warn('[net.sync] refusing to relay rpc "' + msg.n +
                         '" from client ' + conn + ': registered authority is host-only');
            return;
        }
        let live = false;
        for (const c of net.connections()) if (c === msg.to) { live = true; break; }
        if (!live) {
            console.warn('[net.sync] dropping relayed rpc "' + msg.n +
                         '" from client ' + conn + ': target ' + msg.to +
                         ' is not connected');
            return;
        }
        stats.rpcsRelayed++;
        net.sendClone(msg.to, { [TAG]: 'rpc', n: msg.n, a: msg.a, f: conn },
                      rpcOpts(cfg));
    }

    // ── bro.net handler chaining ─────────────────────────────────────────────
    // bro.net's onmessage/onconnect/ondisconnect are single-slot callbacks. On
    // activation, sync claims the underlying slots for its dispatchers and
    // redefines the properties so app reads/writes go to a chained app slot —
    // `bro.net.onmessage = fn` keeps working before OR after sync starts, and
    // every non-sync message (raw or clone) is forwarded to the app untouched.
    function wrapNetHandlers() {
        appHandlers.onmessage = net.onmessage || null;
        appHandlers.onconnect = net.onconnect || null;
        appHandlers.ondisconnect = net.ondisconnect || null;
        net.onmessage = dispatchMessage;       // via the original C setter
        net.onconnect = dispatchConnect;
        net.ondisconnect = dispatchDisconnect;
        for (const name of ['onmessage', 'onconnect', 'ondisconnect']) {
            Object.defineProperty(net, name, {
                configurable: true,
                get: () => appHandlers[name],
                set: (fn) => { appHandlers[name] = fn; },
            });
        }
    }

    function dispatchMessage(conn, data, channel) {
        if (isSyncMsg(data)) {
            handleSync(conn, data);
            return;
        }
        if (typeof appHandlers.onmessage === 'function')
            appHandlers.onmessage(conn, data, channel);
    }

    function dispatchConnect(conn) {
        if (isHost) {
            greet(conn);
        } else if (hostConn === null) {
            hostConn = conn;
        }
        if (typeof appHandlers.onconnect === 'function')
            appHandlers.onconnect(conn);
    }

    function dispatchDisconnect(conn, reason) {
        if (isHost) {
            // Objects owned by the departing client revert to host authority
            // (they are not despawned — the world outlives any one client).
            for (const rec of records.values()) {
                if (rec.authority !== conn) continue;
                rec.authority = 0;
                rec.local = true;
                rec.sendSeq = 0;
                rec.recvSeq = -1;
                rec.interp = null;
                rec.lastSent = null; // full reliable send next tick
                sendSync({ [TAG]: 'au', id: rec.id, mine: false }, CONTROL_OPTS);
            }
        } else if (conn === hostConn) {
            // Lost the host. Replicated objects are kept as-is (frozen at
            // their last state) — the app decides what a dropped session means.
            hostConn = null;
        }
        if (typeof appHandlers.ondisconnect === 'function')
            appHandlers.ondisconnect(conn, reason);
    }

    // Late join: greet a fresh connection with the protocol version and a
    // replay of every live object (spawn + current full state, reliable on the
    // control channel — a joiner needs no other catch-up).
    function greet(conn) {
        net.sendClone(conn, { [TAG]: 'hi', v: PROTOCOL }, CONTROL_OPTS);
        for (const rec of records.values()) {
            net.sendClone(conn, {
                [TAG]: 'sp', id: rec.id, ty: rec.type,
                st: snapshot(rec), mine: rec.authority === conn,
            }, CONTROL_OPTS);
        }
    }

    // ── Activation ───────────────────────────────────────────────────────────
    function configure(opts) {
        const hz = opts.tickHz == null ? 20 : Number(opts.tickHz);
        if (!(hz > 0 && hz <= 1000))
            throw new RangeError('sync: tickHz must be in (0, 1000]');
        tickMs = 1000 / hz;
        const kf = opts.keyframeEvery == null ? 30 : Number(opts.keyframeEvery);
        if (!(kf >= 1)) throw new RangeError('sync: keyframeEvery must be >= 1');
        keyframeEvery = Math.floor(kf);
    }

    function activate() {
        wrapNetHandlers();
        tickTimer = setInterval(tick, tickMs);
        active = true;
    }

    function requireActive() {
        if (!active) throw new Error('bro.net.sync: call sync.host() or sync.join() first');
    }

    function requireHost(what) {
        requireActive();
        if (!isHost) throw new Error('bro.net.sync: ' + what + ' is host-only');
    }

    function recordOf(obj, what) {
        const rec = byObj.get(obj);
        if (!rec) throw new Error('bro.net.sync: ' + what + ': not a replicated object');
        return rec;
    }

    // ── Public API ───────────────────────────────────────────────────────────
    const sync = {};

    sync.register = function (type, def) {
        if (typeof type !== 'string' || type.length === 0)
            throw new TypeError('sync.register: type must be a non-empty string');
        types.set(type, normalizeDef(def));
    };

    // Become the session host. With opts.port, calls bro.net.init() +
    // bro.net.host(port); without it, layers over a listen socket the app
    // already opened. Call BEFORE clients connect — connections that already
    // exist are greeted immediately, but anything they were sent earlier
    // predates sync.
    sync.host = function (opts = {}) {
        if (active) throw new Error('bro.net.sync: already started');
        configure(opts);
        net.init();
        if (opts.port != null) {
            if (net.host(opts.port) === false)
                throw new Error('bro.net.sync: bro.net.host(' + opts.port + ') failed');
        }
        isHost = true;
        activate();
        for (const c of net.connections()) greet(c);
    };

    // Join a session as a client. With opts.address, calls bro.net.init() +
    // bro.net.connect(address); without it, layers over a connection the app
    // is establishing itself. Call BEFORE the connection lands so the spawn
    // replay is received.
    sync.join = function (opts = {}) {
        if (active) throw new Error('bro.net.sync: already started');
        configure(opts);
        net.init();
        isHost = false;
        activate();
        if (opts.address != null) {
            net.connect(String(opts.address));
        } else {
            const conns = net.connections();
            if (conns.length > 0) hostConn = conns[0];
        }
    };

    // Host-only: create a replicated object. Runs the local factory, then
    // replicates the spawn (with full initial state) to every client.
    sync.spawn = function (type, state = {}) {
        requireHost('spawn()');
        const def = types.get(type);
        if (!def) throw new Error('bro.net.sync: spawn of unregistered type "' + type + '"');
        if (state === null || typeof state !== 'object')
            throw new TypeError('bro.net.sync: spawn state must be an object');
        let obj = def.create(state);
        if (obj === null || typeof obj !== 'object') {
            console.warn('[net.sync] create("' + type + '") did not return an ' +
                         'object — substituting {}');
            obj = {};
        }
        for (const p of def.props) if (p in state) obj[p] = state[p];
        const id = nextId++;
        const rec = makeRecord(id, type, def, obj, true);
        records.set(id, rec);
        byObj.set(obj, rec);
        sendSync({ [TAG]: 'sp', id, ty: type, st: snapshot(rec), mine: false },
                 CONTROL_OPTS);
        return obj;
    };

    // Host-only: destroy a replicated object everywhere.
    sync.despawn = function (obj) {
        requireHost('despawn()');
        const rec = recordOf(obj, 'despawn()');
        records.delete(rec.id);
        byObj.delete(obj);
        if (rec.def.destroy) rec.def.destroy(obj);
        sendSync({ [TAG]: 'ds', id: rec.id }, CONTROL_OPTS);
    };

    // Host-only: hand authority over an object to a client connection (or back
    // to the host with conn = 0/null). Only the authority's writes replicate.
    sync.setAuthority = function (obj, conn) {
        requireHost('setAuthority()');
        const rec = recordOf(obj, 'setAuthority()');
        const owner = (conn == null) ? 0 : conn;
        if (owner === rec.authority) return;
        rec.authority = owner;
        rec.local = (owner === 0);
        rec.sendSeq = 0;
        rec.recvSeq = -1;
        rec.interp = null;
        rec.lastSent = null;
        for (const c of net.connections()) {
            net.sendClone(c, { [TAG]: 'au', id: rec.id, mine: c === owner },
                          CONTROL_OPTS);
        }
    };

    // Register a named RPC handler and/or its per-RPC configuration.
    // Handlers receive (fromConn, ...args); fromConn is 0 for callLocal
    // self-invocations. fn may be null to declare config for an RPC this
    // context only sends. Defaults preserve the unconfigured behavior:
    // reliable, remote-only, callable by anyone.
    sync.rpc = function (name, fn, opts) {
        if (typeof name !== 'string' || name.length === 0)
            throw new TypeError('sync.rpc: name must be a non-empty string');
        if (fn != null && typeof fn !== 'function')
            throw new TypeError('sync.rpc: fn must be a function (or null for config-only)');
        const o = (opts == null) ? {} : opts;
        const mode = o.mode == null ? 'reliable' : o.mode;
        if (mode !== 'reliable' && mode !== 'unreliable')
            throw new TypeError("sync.rpc: mode must be 'reliable' or 'unreliable'");
        const authority = o.authority == null ? 'any' : o.authority;
        if (authority !== 'any' && authority !== 'host')
            throw new TypeError("sync.rpc: authority must be 'any' or 'host'");
        rpcs.set(name, { fn: fn || null, mode, callLocal: !!o.callLocal,
                         authority, relay: o.relay !== false });
    };

    // Invoke a named RPC remotely: from a client it runs on the host; from
    // the host it runs on every client. With callLocal configured, the local
    // handler also runs, synchronously after the send, with fromConn 0. Args
    // ride sendClone — anything structured-clonable, functions rejected with
    // a TypeError.
    sync.call = function (name, ...args) {
        requireActive();
        if (!isHost && hostConn === null)
            throw new Error('bro.net.sync: call(): not connected to a host');
        const cfg = rpcConfig(name);
        stats.rpcsSent++;
        sendSync({ [TAG]: 'rpc', n: name, a: args }, rpcOpts(cfg));
        if (cfg.callLocal) invokeRpc(0, { n: name, a: args });
    };

    // Invoke a named RPC on one specific peer.
    //   Host:   sends directly to client `conn`.
    //   Client: targets ANOTHER CLIENT by its host-side connection id — the
    //           frame goes to the host, which validates (per-RPC relay +
    //           authority) and forwards with the true origin stamped, so the
    //           target's handler sees this client's conn id as fromConn.
    sync.callTo = function (conn, name, ...args) {
        requireActive();
        const cfg = rpcConfig(name);
        stats.rpcsSent++;
        if (isHost) {
            net.sendClone(conn, { [TAG]: 'rpc', n: name, a: args }, rpcOpts(cfg));
        } else {
            if (hostConn === null)
                throw new Error('bro.net.sync: callTo(): not connected to a host');
            net.sendClone(hostConn, { [TAG]: 'rpc', n: name, a: args, to: conn },
                          rpcOpts(cfg));
        }
    };

    // ── Introspection ────────────────────────────────────────────────────────
    sync.get = (id) => { const r = records.get(id); return r ? r.obj : null; };
    sync.idOf = (obj) => { const r = byObj.get(obj); return r ? r.id : null; };
    sync.typeOf = (obj) => { const r = byObj.get(obj); return r ? r.type : null; };
    sync.isAuthority = (obj) => { const r = byObj.get(obj); return r ? r.local : false; };
    sync.objects = () => { const out = []; for (const r of records.values()) out.push(r.obj); return out; };

    Object.defineProperty(sync, 'active', { get: () => active });
    Object.defineProperty(sync, 'isHost', { get: () => active && isHost });
    Object.defineProperty(sync, 'hostConn', { get: () => hostConn });

    // Debug/test counters (also handy for a net HUD): message + prop counts.
    sync._stats = () => ({ ...stats, objects: records.size });

    net.sync = sync;
})();
