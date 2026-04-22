// netroom.js — small lobby + message helpers over bro.net.
//
// Wraps bro.net with:
//   - JSON framing (tag + payload) so message types are easy to extend
//   - Host-side roster of named players (id, name, meta)
//   - Ready-state / turn-state broadcast helpers
//
// Design goal: the majority of multiplayer arcade games only need a
// tiny subset of networking — join/leave, sync a small JSON state,
// send typed actions. That's what this covers. Hot-path binary
// messages (FPS player input at 60Hz) should still use bro.net.send
// directly with a binary encoding.
//
// Wire format:
//   ArrayBuffer = TextEncoder(JSON.stringify({ t, ...payload }))
//
// Server (host) usage:
//   const room = NetRoom.host({
//       port: 27100,
//       onJoin(player)   { ... },   // { id, name, meta }
//       onLeave(player)  { ... },
//       onMessage(pid, tag, msg) {},
//   });
//   room.broadcast("state", { turn: 0, map: [...] });
//   room.send(playerId, "youAre", { id });
//
// Client usage:
//   const client = NetRoom.join({
//       address: "127.0.0.1:27100",
//       name: "Jonny",
//       onMessage(tag, msg) {},
//       onDisconnect() {},
//   });
//   client.send("fire", { angle: 45, power: 0.7 });

(function (global) {
    'use strict';

    const enc = new TextEncoder();
    const dec = new TextDecoder();

    function frame(tag, msg) {
        const obj = Object.assign({ t: tag }, msg || {});
        return enc.encode(JSON.stringify(obj)).buffer;
    }

    function unframe(buf) {
        try {
            const obj = JSON.parse(dec.decode(buf));
            if (obj && typeof obj.t === 'string') return obj;
        } catch (e) {}
        return null;
    }

    function ensureNet() {
        if (typeof bro === 'undefined' || !bro.net) {
            throw new Error('bro.net unavailable — NetRoom requires engine networking');
        }
    }

    // ─── host ─────────────────────────────────────────────────────────────
    function host(opts) {
        ensureNet();
        opts = opts || {};
        const port       = opts.port || 27100;
        const maxPlayers = opts.maxPlayers || 8;
        const onJoin     = opts.onJoin    || function () {};
        const onLeave    = opts.onLeave   || function () {};
        const onMessage  = opts.onMessage || function () {};

        // connId → { id, name, meta }. id === connId for simplicity.
        const players = new Map();

        bro.net.init();
        if (!bro.net.host(port)) {
            throw new Error('Failed to host on port ' + port);
        }

        bro.net.onconnect = (connId) => {
            if (players.size >= maxPlayers) {
                bro.net.send(connId, frame('full', { reason: 'Server full' }));
                bro.net.disconnect(connId);
                return;
            }
            // Name arrives via "hello" message; placeholder until then.
            players.set(connId, { id: connId, name: 'Player ' + connId, meta: {} });
        };

        bro.net.ondisconnect = (connId) => {
            const p = players.get(connId);
            if (p) {
                players.delete(connId);
                onLeave(p);
            }
        };

        bro.net.onmessage = (connId, data) => {
            const m = unframe(data);
            if (!m) return;
            const p = players.get(connId);
            if (!p) return;
            if (m.t === 'hello') {
                p.name = (typeof m.name === 'string' && m.name) ? m.name : p.name;
                p.meta = m.meta || {};
                bro.net.send(connId, frame('welcome', { id: connId }));
                onJoin(p);
                return;
            }
            onMessage(connId, m.t, m);
        };

        return {
            port,
            players: () => Array.from(players.values()),
            get: (id) => players.get(id) || null,
            count: () => players.size,
            send(connId, tag, msg, reliable) {
                const r = reliable !== false;
                bro.net.send(connId, frame(tag, msg), r);
            },
            broadcast(tag, msg, reliable) {
                const r = reliable !== false;
                bro.net.broadcast(frame(tag, msg), r);
            },
            kick(connId, reason) {
                if (reason) bro.net.send(connId, frame('kicked', { reason }));
                bro.net.disconnect(connId);
            },
            close() {
                try { bro.net.close(); } catch (e) {}
            },
        };
    }

    // ─── client ───────────────────────────────────────────────────────────
    function join(opts) {
        ensureNet();
        opts = opts || {};
        const address     = opts.address;
        const name        = opts.name || 'Player';
        const meta        = opts.meta || {};
        const onConnect   = opts.onConnect    || function () {};
        const onMessage   = opts.onMessage    || function () {};
        const onDisconnect= opts.onDisconnect || function () {};

        let connId = null;
        let myId   = null;

        bro.net.init();

        bro.net.onconnect = (id) => {
            connId = id;
            // Introduce ourselves. Server replies with "welcome" carrying
            // our assigned id.
            bro.net.send(id, frame('hello', { name, meta }));
            onConnect();
        };

        bro.net.ondisconnect = (_id, reason) => {
            connId = null;
            onDisconnect(reason);
        };

        bro.net.onmessage = (_id, data) => {
            const m = unframe(data);
            if (!m) return;
            if (m.t === 'welcome') myId = m.id;
            onMessage(m.t, m);
        };

        if (!bro.net.connect(address)) {
            throw new Error('Failed to connect to ' + address);
        }

        return {
            myId: () => myId,
            send(tag, msg, reliable) {
                if (connId == null) return false;
                const r = reliable !== false;
                return bro.net.send(connId, frame(tag, msg), r);
            },
            close() {
                if (connId != null) {
                    try { bro.net.disconnect(connId); } catch (e) {}
                }
            },
        };
    }

    global.NetRoom = { host, join, frame, unframe };
})(typeof window !== 'undefined' ? window : globalThis);
