// =============================================================================
// Arena — Authoritative Game Server
// =============================================================================
//
// Run: bro-server apps/arena server.js --tickrate 30
//
// Standard patterns demonstrated:
//   - Authoritative state: server owns all player positions
//   - Input-driven: clients send inputs, not positions
//   - Tick-based broadcast: world state sent every tick
//   - JSON message protocol with type field
//   - Player lifecycle: join, spawn, disconnect, respawn
//   - Collision detection (simple circle-circle)
//   - Server-side game logic (scoring, boundaries)

const PORT = 27015;
const ARENA_W = 1200;
const ARENA_H = 800;
const PLAYER_RADIUS = 16;
const PLAYER_SPEED = 200; // pixels per second
const COLORS = [
    '#e74c3c', '#3498db', '#2ecc71', '#f39c12',
    '#9b59b6', '#1abc9c', '#e67e22', '#e91e63',
];

// ── State ────────────────────────────────────────────────────────────────────

const players = new Map();  // connId → player state
let nextColorIdx = 0;

function spawnPlayer(connId) {
    const color = COLORS[nextColorIdx % COLORS.length];
    nextColorIdx++;
    return {
        id: connId,
        name: 'Player ' + connId,
        x: 100 + Math.random() * (ARENA_W - 200),
        y: 100 + Math.random() * (ARENA_H - 200),
        vx: 0,
        vy: 0,
        radius: PLAYER_RADIUS,
        color: color,
        score: 0,
        input: { up: false, down: false, left: false, right: false },
    };
}

// ── Helpers ──────────────────────────────────────────────────────────────────

const encoder = new TextEncoder();
const decoder = new TextDecoder();

function sendJSON(connId, obj) {
    bro.net.send(connId, encoder.encode(JSON.stringify(obj)).buffer);
}

function broadcastJSON(obj) {
    bro.net.broadcast(encoder.encode(JSON.stringify(obj)).buffer);
}

// ── Networking ───────────────────────────────────────────────────────────────

bro.net.init();
bro.net.host(PORT);
console.log(`Arena server hosting on port ${PORT}`);
console.log(`Tick rate: ${bro.server.tickrate} Hz`);
console.log(`Arena: ${ARENA_W}x${ARENA_H}`);

bro.net.onconnect = (connId) => {
    const player = spawnPlayer(connId);
    players.set(connId, player);
    console.log(`${player.name} (${player.color}) joined [${players.size} players]`);

    // Tell the new player their id and the arena config
    sendJSON(connId, {
        type: 'welcome',
        id: connId,
        arena: { w: ARENA_W, h: ARENA_H },
        tickrate: bro.server.tickrate,
    });

    // Tell everyone about the new player
    broadcastJSON({
        type: 'player_joined',
        player: { id: player.id, name: player.name, color: player.color },
    });
};

bro.net.ondisconnect = (connId, reason) => {
    const player = players.get(connId);
    if (player) {
        console.log(`${player.name} left (reason: ${reason}) [${players.size - 1} players]`);
        players.delete(connId);
        broadcastJSON({ type: 'player_left', id: connId });
    }
};

bro.net.onmessage = (connId, data) => {
    const player = players.get(connId);
    if (!player) return;

    try {
        const msg = JSON.parse(decoder.decode(data));

        switch (msg.type) {
            case 'input':
                // Client sends which keys are held
                player.input.up = !!msg.up;
                player.input.down = !!msg.down;
                player.input.left = !!msg.left;
                player.input.right = !!msg.right;
                break;

            case 'set_name':
                if (typeof msg.name === 'string' && msg.name.length > 0 && msg.name.length <= 16) {
                    player.name = msg.name;
                    console.log(`Player ${connId} renamed to "${player.name}"`);
                }
                break;
        }
    } catch (e) {
        // Ignore malformed messages
    }
};

// ── Game Loop (runs every server tick) ───────────────────────────────────────

let lastTickTime = Date.now();

setInterval(() => {
    const now = Date.now();
    const dt = (now - lastTickTime) / 1000; // seconds
    lastTickTime = now;

    // ── Update player positions from inputs ──
    for (const [id, p] of players) {
        p.vx = 0;
        p.vy = 0;
        if (p.input.left)  p.vx -= PLAYER_SPEED;
        if (p.input.right) p.vx += PLAYER_SPEED;
        if (p.input.up)    p.vy -= PLAYER_SPEED;
        if (p.input.down)  p.vy += PLAYER_SPEED;

        // Normalize diagonal movement
        if (p.vx !== 0 && p.vy !== 0) {
            const inv = 1 / Math.sqrt(2);
            p.vx *= inv;
            p.vy *= inv;
        }

        p.x += p.vx * dt;
        p.y += p.vy * dt;

        // Clamp to arena bounds
        p.x = Math.max(p.radius, Math.min(ARENA_W - p.radius, p.x));
        p.y = Math.max(p.radius, Math.min(ARENA_H - p.radius, p.y));
    }

    // ── Collision: push overlapping players apart ──
    const arr = Array.from(players.values());
    for (let i = 0; i < arr.length; i++) {
        for (let j = i + 1; j < arr.length; j++) {
            const a = arr[i], b = arr[j];
            const dx = b.x - a.x;
            const dy = b.y - a.y;
            const dist = Math.sqrt(dx * dx + dy * dy);
            const minDist = a.radius + b.radius;
            if (dist < minDist && dist > 0) {
                const overlap = (minDist - dist) / 2;
                const nx = dx / dist;
                const ny = dy / dist;
                a.x -= nx * overlap;
                a.y -= ny * overlap;
                b.x += nx * overlap;
                b.y += ny * overlap;
            }
        }
    }

    // ── Broadcast world state (unreliable — it's sent every tick) ──
    if (players.size > 0) {
        const state = [];
        for (const [id, p] of players) {
            state.push({
                id: p.id,
                x: Math.round(p.x * 10) / 10,
                y: Math.round(p.y * 10) / 10,
                vx: Math.round(p.vx),
                vy: Math.round(p.vy),
                color: p.color,
                name: p.name,
                score: p.score,
            });
        }
        broadcastJSON({ type: 'state', players: state }, false);
    }

}, 1000 / bro.server.tickrate);

// ── Periodic stats ───────────────────────────────────────────────────────────

setInterval(() => {
    if (players.size === 0) return;
    console.log(`[${bro.server.uptime.toFixed(0)}s] ${players.size} player(s) online`);
}, 10000);
