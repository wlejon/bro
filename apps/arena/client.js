// =============================================================================
// Arena — Client
// =============================================================================

const canvas = document.querySelector('#game');
const ctx = canvas.getContext('2d');
const statusEl = document.querySelector('#status');
const playersEl = document.querySelector('#players-online');
const connectScreen = document.querySelector('#connect-screen');
const connectBtn = document.querySelector('#connect-btn');
const nameInput = document.querySelector('#name-input');
const addressInput = document.querySelector('#address-input');
const errorMsg = document.querySelector('#error-msg');

// ── State ────────────────────────────────────────────────────────────────────

let myId = null;
let arena = { w: 1200, h: 800 };
let serverTickrate = 30;
let connected = false;
let serverConn = null; // connection id to server
const players = new Map(); // id → { x, y, vx, vy, color, name, score }
const keys = { up: false, down: false, left: false, right: false };
let lastInputSent = '';

// ── Canvas sizing (from layout box) ──────────────────────────────────────────

const CANVAS_W = 1920;
const CANVAS_H = 1080;

// ── Helpers ──────────────────────────────────────────────────────────────────

const encoder = new TextEncoder();
const decoder = new TextDecoder();

function sendJSON(obj) {
    if (serverConn == null) return;
    bro.net.send(serverConn, encoder.encode(JSON.stringify(obj)).buffer);
}

// ── Connect ──────────────────────────────────────────────────────────────────

connectBtn.addEventListener('click', () => {
    const addr = addressInput.value.trim();
    if (!addr) { errorMsg.textContent = 'Enter a server address'; return; }

    errorMsg.textContent = '';
    connectBtn.textContent = 'Connecting...';
    connectBtn.disabled = true;

    bro.net.init();
    bro.net.connect(addr);
});

// Also connect on Enter key in either input
nameInput.addEventListener('keydown', (e) => { if (e.key === 'Enter') connectBtn.click(); });
addressInput.addEventListener('keydown', (e) => { if (e.key === 'Enter') connectBtn.click(); });

// ── Network callbacks ────────────────────────────────────────────────────────

bro.net.onconnect = (connId) => {
    serverConn = connId;
    connected = true;
    connectScreen.classList.add('hidden');
    statusEl.textContent = 'Connected';

    // Send our name
    const name = nameInput.value.trim() || 'Player';
    sendJSON({ type: 'set_name', name });
};

bro.net.ondisconnect = (connId, reason) => {
    connected = false;
    serverConn = null;
    myId = null;
    players.clear();
    statusEl.textContent = 'Disconnected (reason: ' + reason + ')';

    // Show connect screen again
    connectScreen.classList.remove('hidden');
    connectBtn.textContent = 'Connect';
    connectBtn.disabled = false;
    errorMsg.textContent = 'Lost connection to server';
};

bro.net.onmessage = (connId, data) => {
    try {
        const msg = JSON.parse(decoder.decode(data));
        handleMessage(msg);
    } catch (e) {
        // Ignore malformed
    }
};

function handleMessage(msg) {
    switch (msg.type) {
        case 'welcome':
            myId = msg.id;
            arena = msg.arena;
            serverTickrate = msg.tickrate;
            statusEl.textContent = 'Connected (id: ' + myId + ')';
            break;

        case 'state':
            // Full world state from server — authoritative
            for (const p of msg.players) {
                players.set(p.id, p);
            }
            // Remove players not in state
            for (const id of players.keys()) {
                if (!msg.players.find(p => p.id === id)) {
                    players.delete(id);
                }
            }
            playersEl.textContent = players.size + ' player(s)';
            break;

        case 'player_joined':
            // Could show a toast/notification
            break;

        case 'player_left':
            players.delete(msg.id);
            break;
    }
}

// ── Input ────────────────────────────────────────────────────────────────────

const KEY_MAP = {
    'w': 'up', 'arrowup': 'up',
    's': 'down', 'arrowdown': 'down',
    'a': 'left', 'arrowleft': 'left',
    'd': 'right', 'arrowright': 'right',
};

document.addEventListener('keydown', (e) => {
    const dir = KEY_MAP[e.key.toLowerCase()];
    if (dir) { keys[dir] = true; e.preventDefault(); }
});

document.addEventListener('keyup', (e) => {
    const dir = KEY_MAP[e.key.toLowerCase()];
    if (dir) { keys[dir] = false; e.preventDefault(); }
});

// Send input state to server when it changes
setInterval(() => {
    if (!connected) return;
    const sig = `${keys.up}${keys.down}${keys.left}${keys.right}`;
    if (sig !== lastInputSent) {
        sendJSON({ type: 'input', ...keys });
        lastInputSent = sig;
    }
}, 1000 / 60);

// ── Rendering ────────────────────────────────────────────────────────────────

function draw() {
    requestAnimationFrame(draw);

    const W = CANVAS_W;
    const H = CANVAS_H;
    ctx.clearRect(0, 0, W, H);

    // Always draw arena background (centered in viewport)
    const camX = connected && players.has(myId)
        ? players.get(myId).x - W / 2
        : arena.w / 2 - W / 2;
    const camY = connected && players.has(myId)
        ? players.get(myId).y - H / 2
        : arena.h / 2 - H / 2;

    ctx.save();
    ctx.translate(-camX, -camY);

    // ── Arena background ──
    ctx.fillStyle = '#0f3460';
    ctx.fillRect(0, 0, arena.w, arena.h);

    // Grid lines
    ctx.strokeStyle = 'rgba(255,255,255,0.06)';
    ctx.lineWidth = 1;
    for (let x = 0; x <= arena.w; x += 50) {
        ctx.beginPath();
        ctx.moveTo(x, 0);
        ctx.lineTo(x, arena.h);
        ctx.stroke();
    }
    for (let y = 0; y <= arena.h; y += 50) {
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(arena.w, y);
        ctx.stroke();
    }

    // Arena border
    ctx.strokeStyle = '#e94560';
    ctx.lineWidth = 2;
    ctx.strokeRect(0, 0, arena.w, arena.h);

    // ── Draw players ──
    for (const [id, p] of players) {
        const isMe = id === myId;

        // Shadow
        ctx.beginPath();
        ctx.arc(p.x + 2, p.y + 2, 16, 0, Math.PI * 2);
        ctx.fillStyle = 'rgba(0,0,0,0.3)';
        ctx.fill();

        // Body
        ctx.beginPath();
        ctx.arc(p.x, p.y, 16, 0, Math.PI * 2);
        ctx.fillStyle = p.color;
        ctx.fill();

        // Highlight ring for local player
        if (isMe) {
            ctx.strokeStyle = 'white';
            ctx.lineWidth = 2;
            ctx.stroke();
        }

        // Direction indicator
        if (p.vx !== 0 || p.vy !== 0) {
            const len = Math.sqrt(p.vx * p.vx + p.vy * p.vy);
            const nx = p.vx / len;
            const ny = p.vy / len;
            ctx.beginPath();
            ctx.arc(p.x + nx * 12, p.y + ny * 12, 3, 0, Math.PI * 2);
            ctx.fillStyle = 'white';
            ctx.fill();
        }

        // Name label
        ctx.fillStyle = 'white';
        ctx.font = '12px sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText(p.name, p.x, p.y - 24);
    }

    ctx.restore();

    // ── "Waiting" message when not connected ──
    if (!connected || players.size === 0) {
        ctx.fillStyle = 'rgba(255,255,255,0.3)';
        ctx.font = '20px sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText(connected ? 'Waiting for state...' : 'Not connected', W / 2, H / 2);
    }
}

requestAnimationFrame(draw);
