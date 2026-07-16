// Client half of tests/net/test_net_sync.js.
//
// Runs bro.net.sync as a CLIENT in its own JS context/thread: registers the
// shared types, joins the host over loopback, and reports every sync event
// (factory spawns, destroys, RPCs, app-level messages) back over postMessage.
// The main context drives scenarios by posting commands; replies echo the
// command's `seq` so the host side can await them.

const replicas = new Map();   // id -> obj (via our factories)

function report(msg) { postMessage(msg); }

// By the time destroy() runs the object is already out of sync's registry
// (idOf returns null), so recover the id from our own bookkeeping.
function idOfReplica(obj) {
    for (const [id, o] of replicas) {
        if (o === obj) { replicas.delete(id); return id; }
    }
    return null;
}

function registerTypes() {
    bro.net.sync.register('ball', {
        create(state) {
            const obj = { kind: 'ball' };
            // Report AFTER sync assigns props? It assigns after create — so
            // report from a microtask to capture the assigned state.
            Promise.resolve().then(() => {
                const id = bro.net.sync.idOf(obj);
                replicas.set(id, obj);
                report({ ev: 'spawn', type: 'ball', id,
                         state: { x: obj.x, y: obj.y, hp: obj.hp, label: obj.label } });
            });
            return obj;
        },
        destroy(obj) {
            report({ ev: 'despawn', type: 'ball', id: idOfReplica(obj) });
        },
        sync: { props: ['x', 'y', 'hp', 'label'], interpolate: ['y'] },
    });
    bro.net.sync.register('crate', {
        create(state) {
            const obj = { kind: 'crate' };
            Promise.resolve().then(() => {
                const id = bro.net.sync.idOf(obj);
                replicas.set(id, obj);
                report({ ev: 'spawn', type: 'crate', id,
                         state: { x: obj.x, y: obj.y, hp: obj.hp, label: obj.label } });
            });
            return obj;
        },
        destroy(obj) {
            report({ ev: 'despawn', type: 'crate', id: idOfReplica(obj) });
        },
        sync: ['x', 'y', 'hp', 'label'],
    });

    bro.net.sync.rpc('ping', (from, ...args) => {
        report({ ev: 'rpc', name: 'ping', from, args });
    });
}

onmessage = (e) => {
    const m = e.data;
    switch (m.cmd) {
        case 'join': {
            registerTypes();
            // App-level handler set AFTER join must still see non-sync
            // messages (and none of the sync traffic).
            bro.net.sync.join({ address: '127.0.0.1:' + m.port,
                                tickHz: 20, keyframeEvery: 8 });
            bro.net.onmessage = (conn, data, channel) => {
                report({ ev: 'appmsg', data, channel,
                         isBuffer: data instanceof ArrayBuffer ? true : false,
                         text: data instanceof ArrayBuffer
                               ? new TextDecoder().decode(data) : null });
            };
            report({ ev: 'joined' });
            break;
        }
        case 'query': {
            const obj = bro.net.sync.get(m.id);
            report({ ev: 'reply', seq: m.seq,
                     state: obj ? { x: obj.x, y: obj.y, hp: obj.hp, label: obj.label } : null });
            break;
        }
        case 'isAuthority': {
            const obj = bro.net.sync.get(m.id);
            report({ ev: 'reply', seq: m.seq,
                     value: obj ? bro.net.sync.isAuthority(obj) : null });
            break;
        }
        case 'set': {
            const obj = bro.net.sync.get(m.id);
            if (obj) Object.assign(obj, m.props);
            report({ ev: 'reply', seq: m.seq, ok: !!obj });
            break;
        }
        case 'rpc': {
            bro.net.sync.call(m.name, ...m.args);
            report({ ev: 'reply', seq: m.seq });
            break;
        }
        case 'appsend': {
            // Non-sync traffic from the client: a plain clone value and raw
            // bytes. Both must reach the HOST APP's own onmessage untouched.
            bro.net.sendClone(bro.net.sync.hostConn, m.value, { channel: 3 });
            bro.net.send(bro.net.sync.hostConn, m.raw, { channel: 3 });
            report({ ev: 'reply', seq: m.seq });
            break;
        }
        case 'watch': {
            // Sample obj[prop] on a fast real-time interval until it reaches
            // `target` (or times out). Reports whether any strictly
            // intermediate value was observed — evidence of interpolation.
            const obj = bro.net.sync.get(m.id);
            if (!obj) { report({ ev: 'reply', seq: m.seq, error: 'no obj' }); break; }
            const start = obj[m.prop];
            let sawIntermediate = false;
            let ticks = 0;
            const t = setInterval(() => {
                const v = obj[m.prop];
                const lo = Math.min(start, m.target), hi = Math.max(start, m.target);
                if (v > lo && v < hi) sawIntermediate = true;
                ticks++;
                if (v === m.target || ticks > 1500) {
                    clearInterval(t);
                    report({ ev: 'reply', seq: m.seq,
                             final: v, sawIntermediate, start });
                }
            }, 2);
            break;
        }
        case 'disconnect': {
            bro.net.disconnect(bro.net.sync.hostConn);
            report({ ev: 'reply', seq: m.seq });
            break;
        }
        default:
            report({ ev: 'error', error: 'unknown cmd ' + m.cmd });
    }
};
