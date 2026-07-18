// Client half of tests/net/test_net_sync_rpc.js.
//
// Joins the host as a bro.net.sync CLIENT and reports every RPC invocation
// back over postMessage. The same file serves every client instance the test
// spawns; the main context drives scenarios by posting commands, and replies
// echo the command's `seq` so the host side can await them.

function report(msg) { postMessage(msg); }

function registerRpcs() {
    const sync = bro.net.sync;
    // Same per-RPC configs as the host registers (like Godot, both ends
    // declare the same rpc_config).
    sync.rpc('uping', (from, ...args) =>
        report({ ev: 'rpc', name: 'uping', from, args }), { mode: 'unreliable' });
    sync.rpc('announce', (from, ...args) =>
        report({ ev: 'rpc', name: 'announce', from, args }), { callLocal: true });
    sync.rpc('admin', (from, ...args) =>
        report({ ev: 'rpc', name: 'admin', from, args }), { authority: 'host' });
    // Config-only registration: this context only SENDS 'upstream' (the
    // handler lives on the host); the config makes the send unreliable.
    sync.rpc('upstream', null, { mode: 'unreliable' });
}

onmessage = (e) => {
    const m = e.data;
    switch (m.cmd) {
        case 'join': {
            registerRpcs();
            bro.net.sync.join({ address: '127.0.0.1:' + m.port, tickHz: 20 });
            report({ ev: 'joined' });
            break;
        }
        case 'call': {
            bro.net.sync.call(m.name, ...(m.args || []));
            report({ ev: 'reply', seq: m.seq });
            break;
        }
        default:
            report({ ev: 'error', error: 'unknown cmd ' + m.cmd });
    }
};
