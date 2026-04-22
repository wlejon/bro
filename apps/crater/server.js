// =============================================================================
// Crater — Authoritative Server
// =============================================================================
//
// Owns the match state: lobby roster, turn order, heightmap, tank positions,
// HP, and the projectile sim. Clients only send hello/ready/fire/addBot/start
// and receive state snapshots + shot broadcasts. The server never renders.
//
// Run via launcher (apps.json declares a "server" entry), or standalone:
//   bro-headless apps/crater server.js

'use strict';

// ─── Shared physics (duplicated from shared.js — edit both together) ─────────
// The launcher spawns the server in a separate process without a DOM, so we
// can't <script src="shared.js">. Keep this in sync with apps/crater/shared.js.

const C = {
    WORLD_W:      100,
    COLS:         200,
    MAX_H:        40,
    MIN_H:        0,
    GRAVITY:      40,
    MAX_SPEED:    55,
    TURN_TIMEOUT: 30000,
    BOT_DELAY:    1200,
    BLAST_RADIUS: 7,
    CRATER_RAD:   4.8,
    MAX_DAMAGE:   55,
    TANK_W:       2.4,
    TANK_H:       1.2,
    HP_MAX:       100,
    COLORS: [
        '#e74c3c', '#3498db', '#2ecc71', '#f39c12',
        '#9b59b6', '#1abc9c', '#e67e22', '#e91e63',
    ],
};
C.COL_W = C.WORLD_W / C.COLS;

function rng(seed) {
    let a = seed | 0;
    return function () {
        a |= 0; a = (a + 0x6D2B79F5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

function generateHeightmap(seed) {
    const r = rng(seed);
    const octs = [];
    for (let o = 0; o < 4; o++) {
        octs.push({
            freq:  (1 + o * 2) * (0.25 + r() * 0.4),
            amp:   Math.pow(0.55, o) * (0.6 + r() * 0.5),
            phase: r() * Math.PI * 2,
        });
    }
    const hm = new Float32Array(C.COLS);
    for (let i = 0; i < C.COLS; i++) {
        const u = i / C.COLS;
        let h = 0;
        for (const o of octs) {
            h += Math.sin(u * o.freq * Math.PI * 2 + o.phase) * o.amp;
        }
        const norm = (h + 1.5) / 3.0;
        hm[i] = Math.max(4, Math.min(C.MAX_H - 4, 10 + norm * (C.MAX_H - 16)));
    }
    return hm;
}

function heightAt(hm, x) {
    const t = (x / C.WORLD_W) * (C.COLS - 1);
    if (t <= 0) return hm[0];
    if (t >= C.COLS - 1) return hm[C.COLS - 1];
    const i = Math.floor(t);
    const f = t - i;
    return hm[i] * (1 - f) + hm[i + 1] * f;
}

function simulateShot(hm, x, y, vx, vy) {
    const dt = 0.01, maxT = 20;
    let t = 0;
    while (t < maxT) {
        x  += vx * dt;
        y  += vy * dt;
        vy -= C.GRAVITY * dt;
        t  += dt;
        if (x < -5 || x > C.WORLD_W + 5 || y < -20) {
            return { hit: false, x, y, flightMs: t * 1000 };
        }
        if (x >= 0 && x <= C.WORLD_W && y <= heightAt(hm, x)) {
            return { hit: true, x, y, flightMs: t * 1000 };
        }
    }
    return { hit: false, x, y, flightMs: t * 1000 };
}

function carveCrater(hm, cx, cy, radius) {
    const minCol = Math.max(0,         Math.floor((cx - radius) / C.COL_W));
    const maxCol = Math.min(C.COLS - 1, Math.ceil((cx + radius) / C.COL_W));
    const changes = [];
    for (let i = minCol; i <= maxCol; i++) {
        const wx = i * C.COL_W + C.COL_W * 0.5;
        const dx = wx - cx;
        const d2 = radius * radius - dx * dx;
        if (d2 <= 0) continue;
        const newH = cy - Math.sqrt(d2);
        if (hm[i] > newH) {
            const clamped = Math.max(C.MIN_H, newH);
            hm[i] = clamped;
            changes.push([i, clamped]);
        }
    }
    return changes;
}

function blastDamage(cx, cy, tx, ty) {
    const dx = tx - cx, dy = ty - cy;
    const d  = Math.sqrt(dx * dx + dy * dy);
    if (d >= C.BLAST_RADIUS) return 0;
    const falloff = 1 - d / C.BLAST_RADIUS;
    return Math.round(C.MAX_DAMAGE * falloff * falloff);
}

// ─── Framing — mirrors apps/lib/netroom.js ──────────────────────────────────
const enc = new TextEncoder();
const dec = new TextDecoder();
function frame(tag, msg) {
    const o = Object.assign({ t: tag }, msg || {});
    return enc.encode(JSON.stringify(o)).buffer;
}
function unframe(buf) {
    try {
        const o = JSON.parse(dec.decode(buf));
        if (o && typeof o.t === 'string') return o;
    } catch (e) {}
    return null;
}

// ─── Match state ────────────────────────────────────────────────────────────

const PORT = 27100;
const MAX_PLAYERS = 6;

const state = {
    phase:   'lobby',       // 'lobby' | 'match' | 'ended'
    players: new Map(),     // id → player (humans keyed by connId; bots by negative id)
    hostId:  null,
    hm:      null,          // Float32Array(COLS) — active match heightmap
    seed:    0,
    turn:    null,          // player id whose turn it is
    turnIdx: 0,             // index into aliveOrder
    aliveOrder: [],         // snapshot of player ids in match turn order
    turnStart: 0,           // ms
    winnerId: null,
};

let nextBotId = -1;

function makePlayer({ id, name, bot }) {
    const colorIdx = state.players.size % C.COLORS.length;
    return {
        id,
        name:  name  || (bot ? 'Bot ' + (-id) : 'Player ' + id),
        color: C.COLORS[colorIdx],
        ready: !!bot,
        bot:   !!bot,
        hp:    C.HP_MAX,
        x:     0,
        alive: true,
    };
}

function lobbyState() {
    return {
        phase: 'lobby',
        hostId: state.hostId,
        players: Array.from(state.players.values()).map(p => ({
            id: p.id, name: p.name, color: p.color,
            ready: p.ready, bot: p.bot,
        })),
    };
}

function matchSnapshot() {
    return {
        phase: 'match',
        seed:  state.seed,
        turn:  state.turn,
        hm:    Array.from(state.hm),
        players: Array.from(state.players.values()).map(p => ({
            id: p.id, name: p.name, color: p.color, bot: p.bot,
            x: p.x, hp: p.hp, alive: p.alive,
        })),
    };
}

function broadcast(tag, msg, reliable) {
    const r = reliable !== false;
    try { bro.net.broadcast(frame(tag, msg), r); } catch (e) {}
}

function sendTo(connId, tag, msg, reliable) {
    const r = reliable !== false;
    try { bro.net.send(connId, frame(tag, msg), r); } catch (e) {}
}

// ─── Lobby logic ────────────────────────────────────────────────────────────

function onHello(connId, m) {
    if (state.phase !== 'lobby') {
        sendTo(connId, 'denied', { reason: 'Match in progress — try later' });
        try { bro.net.disconnect(connId); } catch (e) {}
        return;
    }
    if (state.players.size >= MAX_PLAYERS) {
        sendTo(connId, 'denied', { reason: 'Server full' });
        try { bro.net.disconnect(connId); } catch (e) {}
        return;
    }
    const p = makePlayer({ id: connId, name: m.name || ('Player ' + connId) });
    state.players.set(connId, p);
    if (state.hostId == null) state.hostId = connId;
    sendTo(connId, 'welcome', { id: connId });
    broadcast('state', lobbyState());
    log('joined:', p.name);
}

function onLeave(connId) {
    if (!state.players.has(connId)) return;
    state.players.delete(connId);
    if (state.hostId === connId) {
        // Promote next human.
        state.hostId = null;
        for (const p of state.players.values()) {
            if (!p.bot) { state.hostId = p.id; break; }
        }
    }
    if (state.phase === 'match') {
        // If it was their turn, advance.
        if (state.turn === connId) advanceTurn();
        // Remove from alive order and check win.
        state.aliveOrder = state.aliveOrder.filter(id => id !== connId);
        checkWinCondition();
    }
    broadcast('state', state.phase === 'match' ? matchSnapshot() : lobbyState());
    log('left:', connId);
}

function onReady(connId, m) {
    const p = state.players.get(connId);
    if (!p || state.phase !== 'lobby') return;
    p.ready = !!m.ready;
    broadcast('state', lobbyState());
}

function onAddBot(connId) {
    if (connId !== state.hostId || state.phase !== 'lobby') return;
    if (state.players.size >= MAX_PLAYERS) return;
    const id = nextBotId--;
    const p = makePlayer({ id, bot: true });
    state.players.set(id, p);
    broadcast('state', lobbyState());
}

function onStart(connId) {
    if (connId !== state.hostId || state.phase !== 'lobby') return;
    const all = Array.from(state.players.values());
    if (all.length < 2) return;
    // Every human must be ready.
    for (const p of all) {
        if (!p.bot && !p.ready) return;
    }
    startMatch();
}

// ─── Match logic ────────────────────────────────────────────────────────────

function startMatch() {
    state.phase = 'match';
    state.seed  = (Math.random() * 2147483647) | 0;
    state.hm    = generateHeightmap(state.seed);

    // Place tanks evenly with jitter, clamp away from edges.
    const players = Array.from(state.players.values());
    const margin  = 8;
    const span    = C.WORLD_W - margin * 2;
    const r       = rng(state.seed ^ 0xA5A5);
    const positions = players.map((_, i) => {
        const slot = margin + (i + 0.5) * (span / players.length);
        return slot + (r() - 0.5) * (span / players.length) * 0.4;
    });
    // Shuffle tanks into slots so order ≠ join order.
    for (let i = positions.length - 1; i > 0; i--) {
        const j = Math.floor(r() * (i + 1));
        [positions[i], positions[j]] = [positions[j], positions[i]];
    }
    for (let i = 0; i < players.length; i++) {
        players[i].x     = positions[i];
        players[i].hp    = C.HP_MAX;
        players[i].alive = true;
    }
    state.aliveOrder = players.map(p => p.id);
    state.turnIdx    = 0;
    state.turn       = state.aliveOrder[0];
    state.turnStart  = Date.now();
    state.winnerId   = null;

    broadcast('match', matchSnapshot());
    scheduleBotTurn();
}

function advanceTurn(extraDelayMs) {
    if (!state.aliveOrder.length) return;
    for (let step = 0; step < state.aliveOrder.length; step++) {
        state.turnIdx = (state.turnIdx + 1) % state.aliveOrder.length;
        const id = state.aliveOrder[state.turnIdx];
        const p  = state.players.get(id);
        if (p && p.alive) {
            state.turn = id;
            state.turnStart = Date.now();
            scheduleBotTurn(extraDelayMs || 0);
            return;
        }
    }
}

function checkWinCondition(broadcastDelayMs) {
    const alive = state.aliveOrder.filter(id => {
        const p = state.players.get(id);
        return p && p.alive;
    });
    if (alive.length <= 1) {
        state.phase    = 'ended';
        state.winnerId = alive.length === 1 ? alive[0] : null;
        const w = state.winnerId != null ? state.players.get(state.winnerId) : null;
        const payload = {
            winnerId:   state.winnerId,
            winnerName: w ? w.name : null,
        };
        // Defer by broadcastDelayMs so the killing shot's animation plays out
        // on clients before the game-over screen replaces the match.
        const delay = Math.max(0, broadcastDelayMs || 0);
        setTimeout(() => broadcast('over', payload), delay);
        setTimeout(returnToLobby, delay + 5000);
        return true;
    }
    return false;
}

function returnToLobby() {
    state.phase = 'lobby';
    state.hm = null;
    state.turn = null;
    state.aliveOrder = [];
    // Reset hp for nicer UX, drop bots so host can re-configure.
    const toRemove = [];
    for (const p of state.players.values()) {
        if (p.bot) toRemove.push(p.id);
        else { p.hp = C.HP_MAX; p.alive = true; p.ready = false; }
    }
    for (const id of toRemove) state.players.delete(id);
    broadcast('state', lobbyState());
}

function onFire(connId, m) {
    if (state.phase !== 'match') return;
    if (state.turn !== connId) return;
    const p = state.players.get(connId);
    if (!p || !p.alive) return;
    executeFire(p, m.angle, m.power, m.dir);
}

function executeFire(p, angle, power, dir) {
    angle = clamp(+angle || 0, 0, Math.PI / 2);
    power = clamp(+power || 0, 0.05, 1);
    dir   = (+dir >= 0) ? 1 : -1;

    const muzzleLen = C.TANK_W * 0.9;
    const surfaceY  = heightAt(state.hm, p.x);
    const originX   = p.x + dir * muzzleLen * Math.cos(angle);
    const originY   = surfaceY + C.TANK_H + muzzleLen * Math.sin(angle);
    const speed     = power * C.MAX_SPEED;
    const vx        = dir * speed * Math.cos(angle);
    const vy        = speed * Math.sin(angle);

    const res = simulateShot(state.hm, originX, originY, vx, vy);

    const damages = [];
    const dead    = [];
    let craterCols = [];

    if (res.hit) {
        craterCols = carveCrater(state.hm, res.x, res.y, C.CRATER_RAD);
        for (const t of state.players.values()) {
            if (!t.alive) continue;
            const surfY = heightAt(state.hm, t.x) + C.TANK_H * 0.5;
            const dmg   = blastDamage(res.x, res.y, t.x, surfY);
            if (dmg > 0) {
                t.hp = Math.max(0, t.hp - dmg);
                damages.push([t.id, t.hp]);
                if (t.hp === 0) {
                    t.alive = false;
                    dead.push(t.id);
                }
            }
        }
    }

    // Keep every tank sitting on the new surface.
    for (const t of state.players.values()) {
        if (!t.alive) continue;
        // (x unchanged; client derives y from heightmap)
    }

    const prevTurn = state.turn;
    // Defer the 'over' broadcast by flightMs so the client can play out the
    // killing shot's animation before the game-over screen appears.
    const over = checkWinCondition(res.flightMs + 500);
    // Don't schedule the next turn's bot until this projectile has landed
    // on the clients — otherwise a fast-lobbing bot can fire mid-flight and
    // the client clobbers the in-flight projectile.
    if (!over) advanceTurn(res.flightMs);

    broadcast('shot', {
        shooter:   p.id,
        originX, originY, vx, vy,
        flightMs:  res.flightMs,
        hit:       res.hit,
        impactX:   res.x,
        impactY:   res.y,
        craterCols, damages, dead,
        prevTurn,
        nextTurn:  over ? null : state.turn,
    });
}

function scheduleBotTurn(extraDelayMs) {
    const p = state.players.get(state.turn);
    if (!p || !p.bot || !p.alive) return;
    setTimeout(() => botFire(p), C.BOT_DELAY + (extraDelayMs || 0));
}

function botFire(p) {
    if (state.phase !== 'match' || state.turn !== p.id || !p.alive) return;

    // Pick nearest living enemy.
    let best = null, bestD = Infinity;
    for (const t of state.players.values()) {
        if (t.id === p.id || !t.alive) continue;
        const d = Math.abs(t.x - p.x);
        if (d < bestD) { bestD = d; best = t; }
    }
    if (!best) return;

    const dir = best.x > p.x ? 1 : -1;
    const dx  = Math.max(1, Math.abs(best.x - p.x));
    const dy  = heightAt(state.hm, best.x) - heightAt(state.hm, p.x);

    // Solve a ballistic angle for a chosen power, with jitter.
    const r = rng(((Date.now() & 0xffff) << 16) ^ (p.id & 0xffff));
    let power = 0.45 + r() * 0.35;         // 0.45..0.80
    const speed = power * C.MAX_SPEED;
    const g = C.GRAVITY;
    // Classic low-angle ballistic: angle = atan((v² − √(v⁴ − g(gx² + 2yv²))) / gx)
    const disc = speed * speed * speed * speed - g * (g * dx * dx + 2 * dy * speed * speed);
    let angle;
    if (disc >= 0) {
        angle = Math.atan((speed * speed - Math.sqrt(disc)) / (g * dx));
    } else {
        angle = 0.9 + r() * 0.4;           // can't solve → lob it
    }
    // Add jitter so bots miss occasionally.
    angle = Math.max(0.1, Math.min(1.45, angle + (r() - 0.5) * 0.12));
    power = Math.max(0.15, Math.min(1.0,  power + (r() - 0.5) * 0.08));

    executeFire(p, angle, power, dir);
}

// Forfeit turn if the active player takes too long (keeps bots responsive
// and prevents disconnected clients from hanging the match).
setInterval(() => {
    if (state.phase !== 'match' || state.turn == null) return;
    if (Date.now() - state.turnStart < C.TURN_TIMEOUT) return;
    const p = state.players.get(state.turn);
    if (!p || p.bot) return;               // bots drive themselves
    log('timeout — skipping turn for', p.name);
    advanceTurn();
    broadcast('skip', { prevTurn: p.id, nextTurn: state.turn });
}, 1000);

// ─── Glue ───────────────────────────────────────────────────────────────────

function clamp(v, a, b) { return v < a ? a : (v > b ? b : v); }
function log(...args)   { try { console.log('[crater]', ...args); } catch (e) {} }

bro.net.init();
if (!bro.net.host(PORT)) {
    log('failed to host on', PORT);
    throw new Error('host failed');
}
log('listening on', PORT);

bro.net.onconnect    = (_id) => { /* wait for hello */ };
bro.net.ondisconnect = (id)  => onLeave(id);
bro.net.onmessage    = (id, data) => {
    const m = unframe(data);
    if (!m) return;
    switch (m.t) {
        case 'hello':  onHello(id, m); break;
        case 'ready':  onReady(id, m); break;
        case 'addBot': onAddBot(id);   break;
        case 'start':  onStart(id);    break;
        case 'fire':   onFire(id, m);  break;
    }
};
