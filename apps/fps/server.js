// =============================================================================
// FPS Arena — Authoritative Server
// =============================================================================
//
// Run: bro-server apps/fps server.js --tickrate 60
//
// Binary protocol, server-authoritative movement, hitscan shooting,
// health/respawn. Clients send inputs; server resolves all game state.

const PORT = 27015;
const TICK_RATE = 60;

// --- Arena ---
const ARENA_HALF = 20;
const WALL_H = 3;
const WALL_THICK = 0.5;

// --- Player ---
const PLAYER_RADIUS = 0.4;
const PLAYER_HEIGHT = 1.8;
const EYE_HEIGHT = 1.6;
const MOVE_SPEED = 6.0;
const MAX_HEALTH = 100;
const HIT_DAMAGE = 25;
const RESPAWN_SECS = 3.0;
const SHOOT_COOLDOWN = 0.15; // seconds between shots

// --- Protocol message types ---
const MSG_INPUT   = 0x01;
const MSG_STATE   = 0x02;
const MSG_WELCOME = 0x03;
const MSG_EVENT   = 0x04;

// --- Event subtypes ---
const EVT_KILL  = 0;
const EVT_HIT   = 1;
const EVT_SPAWN = 2;

// --- Input bits ---
const IN_FWD   = 1;
const IN_BACK  = 2;
const IN_LEFT  = 4;
const IN_RIGHT = 8;
const IN_SHOOT = 16;

// --- Obstacles: { x, z, hw, hd, hh } (center, half-widths, half-height) ---
const OBSTACLES = [
    { x: -8, z: -8, hw: 1.5, hd: 1.5, hh: 1.5 },
    { x:  8, z:  8, hw: 1.5, hd: 1.5, hh: 1.5 },
    { x: -8, z:  8, hw: 1.0, hd: 3.0, hh: 1.0 },
    { x:  8, z: -8, hw: 3.0, hd: 1.0, hh: 1.0 },
    { x:  0, z:  0, hw: 1.0, hd: 1.0, hh: 2.5 },
    { x:-15, z:  0, hw: 0.5, hd: 4.0, hh: 1.5 },
    { x: 15, z:  0, hw: 0.5, hd: 4.0, hh: 1.5 },
    { x:  0, z: 15, hw: 4.0, hd: 0.5, hh: 1.5 },
    { x:  0, z:-15, hw: 4.0, hd: 0.5, hh: 1.5 },
];

// Build full AABB list (obstacles + 4 walls)
const WALLS = [
    { x: 0,           z: -ARENA_HALF, hw: ARENA_HALF, hd: WALL_THICK, hh: WALL_H },
    { x: 0,           z:  ARENA_HALF, hw: ARENA_HALF, hd: WALL_THICK, hh: WALL_H },
    { x: -ARENA_HALF, z: 0,           hw: WALL_THICK, hd: ARENA_HALF, hh: WALL_H },
    { x:  ARENA_HALF, z: 0,           hw: WALL_THICK, hd: ARENA_HALF, hh: WALL_H },
];
const ALL_SOLIDS = OBSTACLES.concat(WALLS);

// --- Spawn points ---
const SPAWNS = [
    { x:-16, z:-16 }, { x: 16, z:-16 }, { x:-16, z: 16 }, { x: 16, z: 16 },
    { x:  0, z:-16 }, { x:  0, z: 16 }, { x:-16, z:  0 }, { x: 16, z:  0 },
];

// --- Player colors ---
const COLORS = [
    '#e74c3c', '#3498db', '#2ecc71', '#f39c12',
    '#9b59b6', '#1abc9c', '#e67e22', '#e91e63',
];

// ─── State ───────────────────────────────────────────────────────────────────

const players = new Map();
let serverTick = 0;
let nextColorIdx = 0;

function pickSpawn() {
    // Pick spawn farthest from all players
    if (players.size === 0) return SPAWNS[Math.floor(Math.random() * SPAWNS.length)];
    let best = SPAWNS[0], bestDist = -1;
    for (const sp of SPAWNS) {
        let minDist = Infinity;
        for (const [, p] of players) {
            if (!p.alive) continue;
            const dx = p.x - sp.x, dz = p.z - sp.z;
            minDist = Math.min(minDist, dx * dx + dz * dz);
        }
        if (minDist > bestDist) { bestDist = minDist; best = sp; }
    }
    return best;
}

function createPlayer(connId) {
    const sp = pickSpawn();
    const color = COLORS[nextColorIdx++ % COLORS.length];
    return {
        id: connId, name: 'Player', color,
        x: sp.x, y: 0, z: sp.z,
        yaw: 0, pitch: 0,
        health: MAX_HEALTH, alive: true,
        kills: 0, deaths: 0,
        lastInputTick: 0,
        lastShoot: false, // for edge detection
        shootCooldown: 0,
        respawnTimer: 0,
        input: 0, inputYaw: 0, inputPitch: 0,
    };
}

// ─── Collision ───────────────────────────────────────────────────────────────

// Push a circle (xz plane, radius r) out of an AABB
function pushCircleOutOfAABB(px, pz, r, box) {
    const bx0 = box.x - box.hw, bx1 = box.x + box.hw;
    const bz0 = box.z - box.hd, bz1 = box.z + box.hd;

    // Find closest point on AABB to circle center
    const cx = Math.max(bx0, Math.min(px, bx1));
    const cz = Math.max(bz0, Math.min(pz, bz1));

    const dx = px - cx, dz = pz - cz;
    const dist2 = dx * dx + dz * dz;

    if (dist2 < r * r && dist2 > 0.0001) {
        const dist = Math.sqrt(dist2);
        const pen = r - dist;
        return { x: px + (dx / dist) * pen, z: pz + (dz / dist) * pen };
    }

    // Circle center is inside AABB — push out via shortest axis
    if (dist2 < 0.0001) {
        const dl = px - bx0, dr = bx1 - px;
        const dt = pz - bz0, db = bz1 - pz;
        const min = Math.min(dl, dr, dt, db);
        if (min === dl) return { x: bx0 - r, z: pz };
        if (min === dr) return { x: bx1 + r, z: pz };
        if (min === dt) return { x: px, z: bz0 - r };
        return { x: px, z: bz1 + r };
    }

    return null; // no collision
}

// Ray vs AABB (slab method), returns fraction or -1
function rayAABB(ox, oy, oz, dx, dy, dz, box) {
    const bx0 = box.x - box.hw, bx1 = box.x + box.hw;
    const by0 = 0,               by1 = box.hh * 2;
    const bz0 = box.z - box.hd, bz1 = box.z + box.hd;

    let tmin = -Infinity, tmax = Infinity;
    const axes = [[ox, dx, bx0, bx1], [oy, dy, by0, by1], [oz, dz, bz0, bz1]];
    for (const [o, d, mn, mx] of axes) {
        if (Math.abs(d) < 1e-9) {
            if (o < mn || o > mx) return -1;
        } else {
            let t1 = (mn - o) / d, t2 = (mx - o) / d;
            if (t1 > t2) { const tmp = t1; t1 = t2; t2 = tmp; }
            tmin = Math.max(tmin, t1);
            tmax = Math.min(tmax, t2);
            if (tmin > tmax) return -1;
        }
    }
    return tmin >= 0 ? tmin : (tmax >= 0 ? tmax : -1);
}

// Ray vs vertical cylinder (player hitbox): center (cx, cz), radius r, y range [0, h]
function rayCylinder(ox, oy, oz, dx, dy, dz, cx, cz, r, h) {
    // 2D ray-circle in XZ
    const ex = ox - cx, ez = oz - cz;
    const a = dx * dx + dz * dz;
    const b = 2 * (ex * dx + ez * dz);
    const c = ex * ex + ez * ez - r * r;
    const disc = b * b - 4 * a * c;
    if (disc < 0) return -1;
    const sqrtD = Math.sqrt(disc);
    let t = (-b - sqrtD) / (2 * a);
    if (t < 0) t = (-b + sqrtD) / (2 * a);
    if (t < 0) return -1;
    const hitY = oy + dy * t;
    if (hitY < 0 || hitY > h) return -1;
    return t;
}

// ─── Hitscan ─────────────────────────────────────────────────────────────────

function processShot(shooter) {
    const yaw = shooter.yaw, pitch = shooter.pitch;
    // Forward vector from yaw/pitch (-Z forward convention)
    const dx = Math.sin(yaw) * Math.cos(pitch);
    const dy = Math.sin(pitch);
    const dz = -Math.cos(yaw) * Math.cos(pitch);

    const ox = shooter.x, oy = EYE_HEIGHT, oz = shooter.z;
    const maxDist = 100;

    let bestT = maxDist, bestVictim = null;

    // Check against all other alive players
    for (const [id, p] of players) {
        if (id === shooter.id || !p.alive) continue;
        const t = rayCylinder(ox, oy, oz, dx, dy, dz, p.x, p.z, PLAYER_RADIUS * 1.5, PLAYER_HEIGHT);
        if (t >= 0 && t < bestT) {
            // Check that no obstacle blocks the shot
            let blocked = false;
            for (const box of ALL_SOLIDS) {
                const bt = rayAABB(ox, oy, oz, dx, dy, dz, box);
                if (bt >= 0 && bt < t) { blocked = true; break; }
            }
            if (!blocked) { bestT = t; bestVictim = p; }
        }
    }

    if (bestVictim) {
        bestVictim.health -= HIT_DAMAGE;

        // Send hit event to victim
        sendEvent(bestVictim.id, EVT_HIT, shooter.id, bestVictim.id, HIT_DAMAGE);
        // Send hit event to shooter (confirmation)
        sendEvent(shooter.id, EVT_HIT, shooter.id, bestVictim.id, HIT_DAMAGE);

        if (bestVictim.health <= 0) {
            bestVictim.health = 0;
            bestVictim.alive = false;
            bestVictim.respawnTimer = RESPAWN_SECS;
            bestVictim.deaths++;
            shooter.kills++;

            // Broadcast kill event to all
            for (const [id] of players) {
                sendEvent(id, EVT_KILL, shooter.id, bestVictim.id, 0);
            }

            console.log(`${shooter.name} killed ${bestVictim.name} [${shooter.kills} kills]`);
        }
    }
}

// ─── Binary Protocol ─────────────────────────────────────────────────────────

function sendWelcome(connId, player) {
    const buf = new ArrayBuffer(7);
    const v = new DataView(buf);
    v.setUint8(0, MSG_WELCOME);
    v.setUint16(1, connId, true);
    v.setUint32(3, serverTick, true);
    bro.net.send(connId, buf);
}

function sendEvent(connId, evtType, id1, id2, value) {
    const buf = new ArrayBuffer(8);
    const v = new DataView(buf);
    v.setUint8(0, MSG_EVENT);
    v.setUint8(1, evtType);
    v.setUint16(2, id1, true);
    v.setUint16(4, id2, true);
    v.setUint16(6, value, true);
    bro.net.send(connId, buf);
}

function sendSpawnEvent(connId, x, z) {
    const buf = new ArrayBuffer(10);
    const v = new DataView(buf);
    v.setUint8(0, MSG_EVENT);
    v.setUint8(1, EVT_SPAWN);
    v.setFloat32(2, x, true);
    v.setFloat32(6, z, true);
    bro.net.send(connId, buf);
}

// State packet per client (includes their lastInputTick)
// Header: type(1) + serverTick(4) + lastInputTick(4) + playerCount(1) = 10
// Per player: id(2) + x(4) + y(4) + z(4) + yaw(4) + health(1) + flags(1) + kills(2) = 22
function sendState(connId, lastInputTick) {
    const count = players.size;
    const buf = new ArrayBuffer(10 + count * 22);
    const v = new DataView(buf);

    v.setUint8(0, MSG_STATE);
    v.setUint32(1, serverTick, true);
    v.setUint32(5, lastInputTick, true);
    v.setUint8(9, count);

    let off = 10;
    for (const [id, p] of players) {
        v.setUint16(off, id, true);         off += 2;
        v.setFloat32(off, p.x, true);       off += 4;
        v.setFloat32(off, p.y, true);       off += 4;
        v.setFloat32(off, p.z, true);       off += 4;
        v.setFloat32(off, p.yaw, true);     off += 4;
        v.setUint8(off, p.health);           off += 1;
        const flags = (p.alive ? 1 : 0) | (p.shootCooldown > 0 ? 2 : 0);
        v.setUint8(off, flags);              off += 1;
        v.setUint16(off, p.kills, true);     off += 2;
    }

    bro.net.send(connId, buf, false); // unreliable
}

function parseInput(data) {
    if (data.byteLength < 14) return null;
    const v = new DataView(data);
    if (v.getUint8(0) !== MSG_INPUT) return null;
    return {
        tick: v.getUint32(1, true),
        keys: v.getUint8(5),
        yaw: v.getFloat32(6, true),
        pitch: v.getFloat32(10, true),
    };
}

// ─── Networking ──────────────────────────────────────────────────────────────

bro.net.init();
if (!bro.net.host(PORT)) {
    console.error('Failed to bind port ' + PORT);
    bro.server.stop();
}
console.log('FPS server on port ' + PORT);
bro.server.tickrate = TICK_RATE;

bro.net.onconnect = (connId) => {
    const player = createPlayer(connId);
    players.set(connId, player);
    console.log('Player ' + connId + ' joined [' + players.size + ' players]');
    sendWelcome(connId, player);
    sendSpawnEvent(connId, player.x, player.z);
};

bro.net.ondisconnect = (connId) => {
    const p = players.get(connId);
    if (p) console.log(p.name + ' left [' + (players.size - 1) + ' players]');
    players.delete(connId);
};

bro.net.onmessage = (connId, data) => {
    const p = players.get(connId);
    if (!p) return;

    // Check for JSON name-set (first message might be JSON)
    if (data.byteLength > 2) {
        const firstByte = new Uint8Array(data)[0];
        if (firstByte === 0x7B) { // '{' - JSON
            try {
                const msg = JSON.parse(new TextDecoder().decode(data));
                if (msg.type === 'set_name' && typeof msg.name === 'string') {
                    p.name = msg.name.substring(0, 16);
                    console.log('Player ' + connId + ' is "' + p.name + '"');
                }
            } catch (e) {}
            return;
        }
    }

    const input = parseInput(data);
    if (!input) return;

    p.input = input.keys;
    p.inputYaw = input.yaw;
    p.inputPitch = input.pitch;
    p.lastInputTick = input.tick;
};

// ─── Game Tick ───────────────────────────────────────────────────────────────

const dt = 1.0 / TICK_RATE;

setInterval(() => {
    serverTick++;

    for (const [id, p] of players) {
        // Respawn timer
        if (!p.alive) {
            p.respawnTimer -= dt;
            if (p.respawnTimer <= 0) {
                const sp = pickSpawn();
                p.x = sp.x; p.z = sp.z; p.y = 0;
                p.health = MAX_HEALTH;
                p.alive = true;
                sendSpawnEvent(id, p.x, p.z);
                console.log(p.name + ' respawned');
            }
            continue;
        }

        // Apply input
        p.yaw = p.inputYaw;
        p.pitch = p.inputPitch;

        // Movement direction (XZ plane only, -Z is forward)
        const fwdX = Math.sin(p.yaw);
        const fwdZ = -Math.cos(p.yaw);
        const rightX = Math.cos(p.yaw);
        const rightZ = Math.sin(p.yaw);

        let mx = 0, mz = 0;
        if (p.input & IN_FWD)   { mx += fwdX;   mz += fwdZ; }
        if (p.input & IN_BACK)  { mx -= fwdX;   mz -= fwdZ; }
        if (p.input & IN_LEFT)  { mx -= rightX; mz -= rightZ; }
        if (p.input & IN_RIGHT) { mx += rightX; mz += rightZ; }

        // Normalize diagonal
        const len = Math.sqrt(mx * mx + mz * mz);
        if (len > 0.001) {
            mx = (mx / len) * MOVE_SPEED * dt;
            mz = (mz / len) * MOVE_SPEED * dt;
        }

        p.x += mx;
        p.z += mz;

        // Collide with all solids
        for (const box of ALL_SOLIDS) {
            const result = pushCircleOutOfAABB(p.x, p.z, PLAYER_RADIUS, box);
            if (result) { p.x = result.x; p.z = result.z; }
        }

        // Arena boundary
        const lim = ARENA_HALF - PLAYER_RADIUS - WALL_THICK;
        p.x = Math.max(-lim, Math.min(lim, p.x));
        p.z = Math.max(-lim, Math.min(lim, p.z));

        // Shooting
        p.shootCooldown = Math.max(0, p.shootCooldown - dt);
        const wantsShoot = !!(p.input & IN_SHOOT);
        if (wantsShoot && !p.lastShoot && p.shootCooldown <= 0) {
            processShot(p);
            p.shootCooldown = SHOOT_COOLDOWN;
        }
        p.lastShoot = wantsShoot;
    }

    // Broadcast state to each player (with their specific lastInputTick)
    for (const [id, p] of players) {
        sendState(id, p.lastInputTick);
    }

}, 1000 / TICK_RATE);

// Periodic status
setInterval(() => {
    if (players.size > 0)
        console.log('[' + bro.server.uptime.toFixed(0) + 's] ' + players.size + ' player(s)');
}, 10000);
