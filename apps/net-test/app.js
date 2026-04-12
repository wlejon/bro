const logEl = document.querySelector('#log');
const statusEl = document.querySelector('#status');
const connsEl = document.querySelector('#conns');

function log(msg) {
    const line = document.createElement('div');
    line.textContent = `[${new Date().toLocaleTimeString()}] ${msg}`;
    logEl.appendChild(line);
    console.log(msg);
}

function updateConns() {
    const c = bro.net.connections();
    connsEl.textContent = c.length + ' — ' + c.join(', ');
}

// Init
document.querySelector('#btn-init').addEventListener('click', () => {
    const ok = bro.net.init();
    log(ok ? 'Network initialized!' : 'Init failed');
    statusEl.textContent = ok ? 'Initialized' : 'Init failed';
    if (ok) statusEl.className = 'connected';
});

// Host
document.querySelector('#btn-host').addEventListener('click', () => {
    const ok = bro.net.host(27015);
    log(ok ? 'Hosting on port 27015' : 'Host failed');
    if (ok) statusEl.textContent = 'Hosting on :27015';
});

// Connect
document.querySelector('#btn-connect').addEventListener('click', () => {
    const addr = document.querySelector('#addr').value;
    const ok = bro.net.connect(addr);
    log(ok ? `Connecting to ${addr}...` : `Connect failed`);
});

// Send
document.querySelector('#btn-send').addEventListener('click', () => {
    const conns = bro.net.connections();
    if (conns.length === 0) { log('No connections'); return; }
    const msg = 'Hello from bro.net! Time: ' + Date.now();
    const encoder = new TextEncoder();
    const data = encoder.encode(msg);
    for (const c of conns) {
        bro.net.send(c, data.buffer, true);
        log(`Sent to ${c}: ${msg}`);
    }
});

// Broadcast
document.querySelector('#btn-broadcast').addEventListener('click', () => {
    const msg = 'Broadcast: ' + Date.now();
    const encoder = new TextEncoder();
    bro.net.broadcast(encoder.encode(msg).buffer, true);
    log('Broadcast: ' + msg);
});

// Close
document.querySelector('#btn-close').addEventListener('click', () => {
    bro.net.close();
    log('Closed');
    statusEl.textContent = 'Closed';
    updateConns();
});

// Callbacks
bro.net.onconnect = (connId) => {
    log(`Connected: ${connId}`);
    updateConns();

    // Query stats after a short delay
    setTimeout(() => {
        const s = bro.net.stats(connId);
        if (s) log(`Stats for ${connId}: ping=${s.ping}ms loss=${(s.packetLoss*100).toFixed(1)}%`);
    }, 1000);
};

bro.net.ondisconnect = (connId, reason) => {
    log(`Disconnected: ${connId} (reason: ${reason})`);
    updateConns();
};

bro.net.onmessage = (connId, data) => {
    const decoder = new TextDecoder();
    const text = decoder.decode(data);
    log(`Message from ${connId}: ${text}`);
};

log('Network test app loaded. Click "Init Network" to start.');
