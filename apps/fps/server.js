// =============================================================================
// FPS Arena — Authoritative Server
// =============================================================================
//
// Run: bro-server apps/fps server.js --tickrate 60
//
// Binary protocol, server-authoritative movement, hitscan shooting,
// health/respawn. Clients send inputs; server resolves all game state.
// AI bots use bro.ai.game for pathfinding and steering, and the shared
// BotAim system (apps/lib/bot_aim.js) for human-like aim tracking.

// ─── BotAim — copy of apps/lib/bot_aim.js (keep in sync) ────────────────────
// Server scripts can't <script src=...> a shared lib, so the canonical file
// lives in apps/lib/bot_aim.js and is duplicated here. Edit both together.
var BotAim = {};
(function () {
    "use strict";
    function angleDelta(from, to) {
        var d = to - from;
        while (d >  Math.PI) d -= 2 * Math.PI;
        while (d < -Math.PI) d += 2 * Math.PI;
        return d;
    }
    BotAim.create = function (opts) {
        opts = opts || {};
        return {
            yaw: 0, pitch: 0, desiredYaw: 0, desiredPitch: 0, sampleT: -1e9,
            turnSpeed:      opts.turnSpeed   != null ? opts.turnSpeed   : 5.0,
            sampleInterval: 1.0 / (opts.sampleHz != null ? opts.sampleHz : 15),
            fireConeRad:    opts.fireConeRad != null ? opts.fireConeRad : 0.15,
        };
    };
    BotAim.set = function (aim, yaw, pitch) {
        aim.yaw = yaw; aim.pitch = pitch || 0;
        aim.desiredYaw = aim.yaw; aim.desiredPitch = aim.pitch;
        aim.sampleT = -1e9;
    };
    BotAim.requestAim = function (aim, simT, yaw, pitch) {
        if (simT - aim.sampleT < aim.sampleInterval) return;
        aim.sampleT = simT;
        aim.desiredYaw = yaw; aim.desiredPitch = pitch || 0;
    };
    BotAim.requestAimAt = function (aim, simT, fromX, fromY, fromZ, toX, toY, toZ) {
        var dx = toX - fromX, dy = toY - fromY, dz = toZ - fromZ;
        var horizDist = Math.sqrt(dx * dx + dz * dz);
        if (horizDist < 1e-4 && Math.abs(dy) < 1e-4) return;
        var yaw = Math.atan2(dx, -dz);
        var pitch = Math.atan2(dy, horizDist);
        BotAim.requestAim(aim, simT, yaw, pitch);
    };
    BotAim.tick = function (aim, dt) {
        var maxStep = aim.turnSpeed * dt;
        var dy = angleDelta(aim.yaw, aim.desiredYaw);
        if (dy >  maxStep) dy =  maxStep; else if (dy < -maxStep) dy = -maxStep;
        aim.yaw += dy;
        if (aim.yaw >  Math.PI) aim.yaw -= 2 * Math.PI;
        else if (aim.yaw < -Math.PI) aim.yaw += 2 * Math.PI;
        var dp = aim.desiredPitch - aim.pitch;
        if (dp >  maxStep) dp =  maxStep; else if (dp < -maxStep) dp = -maxStep;
        aim.pitch += dp;
        var P = Math.PI / 2 - 0.01;
        if (aim.pitch >  P) aim.pitch =  P; else if (aim.pitch < -P) aim.pitch = -P;
    };
    BotAim.forward = function (aim) {
        var cp = Math.cos(aim.pitch);
        return { x: Math.sin(aim.yaw)*cp, y: Math.sin(aim.pitch), z: -Math.cos(aim.yaw)*cp };
    };
    BotAim.canFireAt = function (aim, fromX, fromY, fromZ, toX, toY, toZ) {
        var dx = toX - fromX, dy = toY - fromY, dz = toZ - fromZ;
        var len = Math.sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 1e-4) return false;
        var f = BotAim.forward(aim);
        var dot = (f.x * dx + f.y * dy + f.z * dz) / len;
        return dot >= Math.cos(aim.fireConeRad);
    };
})();

const PORT = 27015;
const TICK_RATE = 60;
const NUM_BOTS = 3;

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

        // Send hit event to victim and shooter (only real clients)
        if (!bestVictim.isBot) sendEvent(bestVictim.id, EVT_HIT, shooter.id, bestVictim.id, HIT_DAMAGE);
        if (!shooter.isBot) sendEvent(shooter.id, EVT_HIT, shooter.id, bestVictim.id, HIT_DAMAGE);

        if (bestVictim.health <= 0) {
            bestVictim.health = 0;
            bestVictim.alive = false;
            bestVictim.respawnTimer = RESPAWN_SECS;
            bestVictim.deaths++;
            shooter.kills++;

            // Reward/penalty for bots
            if (shooter.isBot && bots.has(shooter.id)) bots.get(shooter.id).score += 2;
            if (bestVictim.isBot && bots.has(bestVictim.id)) bots.get(bestVictim.id).score -= 1;

            // Broadcast kill event to real clients only
            for (const [id, p] of players) {
                if (!p.isBot) sendEvent(id, EVT_KILL, shooter.id, bestVictim.id, 0);
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
        // Respawn timer (bots + humans)
        if (!p.alive) {
            p.respawnTimer -= dt;
            if (p.respawnTimer <= 0) {
                const sp = pickSpawn();
                p.x = sp.x; p.z = sp.z; p.y = 0;
                p.health = MAX_HEALTH;
                p.alive = true;
                if (!p.isBot) sendSpawnEvent(id, p.x, p.z);
                // Reset bot agent position on respawn
                if (p.isBot && bots.has(id)) {
                    const bot = bots.get(id);
                    bot.agent.setPosition(sp.x, sp.z);
                    bot.agent.clearTarget();
                    bot.state = ST_ROAM;
                    bot.stateTimer = 0;
                    bot.coverPoint = null;
                    bot.peekPoint = null;
                    bot.targetId = null;
                    bot.score -= 1; // death penalty
                    BotAim.set(bot.aim, 0, 0);
                }
                console.log(p.name + ' respawned');
            }
            continue;
        }

        // Bots are driven by AI, skip human input processing
        if (p.isBot) continue;

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

        // Shooting
        p.shootCooldown = Math.max(0, p.shootCooldown - dt);
        const wantsShoot = !!(p.input & IN_SHOOT);
        if (wantsShoot && !p.lastShoot && p.shootCooldown <= 0) {
            processShot(p);
            p.shootCooldown = SHOOT_COOLDOWN;
        }
        p.lastShoot = wantsShoot;
    }

    // ── Bot AI tick ──
    for (const [botId, bot] of bots) {
        if (bot.player.alive) botTick(bot, dt);
    }

    // ── Collision (all players, including bots) ──
    for (const [id, p] of players) {
        if (!p.alive) continue;
        for (const box of ALL_SOLIDS) {
            const result = pushCircleOutOfAABB(p.x, p.z, PLAYER_RADIUS, box);
            if (result) { p.x = result.x; p.z = result.z; }
        }
        const lim = ARENA_HALF - PLAYER_RADIUS - WALL_THICK;
        p.x = Math.max(-lim, Math.min(lim, p.x));
        p.z = Math.max(-lim, Math.min(lim, p.z));
    }

    // Broadcast state only to real clients (positive IDs)
    for (const [id, p] of players) {
        if (!p.isBot) sendState(id, p.lastInputTick);
    }

}, 1000 / TICK_RATE);

// ─── AI Bots ────────────────────────────────────────────────────────────────

// Build navmesh from obstacles (padded by player radius)
const nav = bro.ai.game.createNavGrid({
    minX: -ARENA_HALF, minZ: -ARENA_HALF,
    maxX: ARENA_HALF, maxZ: ARENA_HALF,
    cellSize: 0.5,
    obstacles: OBSTACLES.concat(WALLS),
    padding: PLAYER_RADIUS + 0.2,
});

const BOT_NAMES = ['Alpha', 'Bravo', 'Charlie', 'Delta', 'Echo', 'Foxtrot'];
const bots = new Map(); // botId → { player, agent, ... }
let nextBotId = 10000;

// ─── Bot tuning ─────────────────────────────────────────────────────────────
const BOT_SPEED          = MOVE_SPEED * 0.90;
const HEAL_RATE          = 8;         // HP/sec while healing in cover
const HEAL_THRESHOLD     = 50;        // seek cover below this HP
const HEAL_RESUME        = 85;        // stop healing above this HP
const PEEK_DURATION      = 0.6;       // seconds exposed while peeking/shooting
const COVER_RETREAT_TIME = 1.5;       // seconds hiding in cover before peeking
const ENGAGE_RANGE       = 25;        // max distance to push toward a target
const CLOSE_RANGE        = 10;        // switch to strafe/peek play
const COVER_SEARCH_DIST  = 15;        // max distance to look for cover

// ─── Bot states ─────────────────────────────────────────────────────────────
const ST_ROAM  = 0;  // patrol between cover points, no urgent target
const ST_PUSH  = 1;  // advance toward target (far away)
const ST_COVER = 2;  // moving to a cover position
const ST_PEEK  = 3;  // leaning out of cover to shoot
const ST_HEAL  = 4;  // hunkered in cover, healing

// ─── Cover point system ─────────────────────────────────────────────────────
// Pre-compute candidate cover positions around every obstacle edge, offset by
// player radius so bots don't clip into geometry.

const COVER_POINTS = []; // { x, z, obstacleIdx }

(function buildCoverPoints() {
    // pad must clear the nav grid's *snapped* padded-obstacle cells, not just
    // the obstacle itself. Nav padding (0.6) + cellSize (0.5) inflates the
    // unwalkable region; a smaller pad lands cover points inside that snap
    // zone and findCover returns nothing.
    const pad = PLAYER_RADIUS + 0.2 + 0.5 + 0.3;
    for (let i = 0; i < OBSTACLES.length; i++) {
        const o = OBSTACLES[i];
        // 4 face centers (N/S/E/W offset from obstacle edge)
        const pts = [
            { x: o.x,            z: o.z - o.hd - pad },  // north
            { x: o.x,            z: o.z + o.hd + pad },  // south
            { x: o.x - o.hw - pad, z: o.z },              // west
            { x: o.x + o.hw + pad, z: o.z },              // east
            // 4 corners (diagonal offset)
            { x: o.x - o.hw - pad, z: o.z - o.hd - pad },
            { x: o.x + o.hw + pad, z: o.z - o.hd - pad },
            { x: o.x - o.hw - pad, z: o.z + o.hd + pad },
            { x: o.x + o.hw + pad, z: o.z + o.hd + pad },
        ];
        for (const pt of pts) {
            // Must be inside arena and on walkable nav cell
            const lim = ARENA_HALF - PLAYER_RADIUS - WALL_THICK - 0.5;
            if (Math.abs(pt.x) < lim && Math.abs(pt.z) < lim && nav.isWalkable(pt.x, pt.z)) {
                COVER_POINTS.push({ x: pt.x, z: pt.z, obstIdx: i });
            }
        }
    }
    console.log(COVER_POINTS.length + ' cover points generated');
})();

// Find best cover position: blocks LOS from threat, close to bot, not too close to threat
function findCover(botX, botZ, threatX, threatZ) {
    let best = null, bestScore = -Infinity;
    for (const cp of COVER_POINTS) {
        const dx = cp.x - botX, dz = cp.z - botZ;
        const distToBot = Math.sqrt(dx * dx + dz * dz);
        if (distToBot > COVER_SEARCH_DIST) continue;

        // Must block LOS from threat
        const inCover = !bro.ai.game.hasLineOfSight(cp.x, cp.z, threatX, threatZ,
            OBSTACLES.concat(WALLS));
        if (!inCover) continue;

        // Score: prefer close to bot, not too far from threat (stay in fight)
        const tx = cp.x - threatX, tz = cp.z - threatZ;
        const distToThreat = Math.sqrt(tx * tx + tz * tz);

        // Reward proximity to bot (easy to reach), penalize being too far from fight
        const score = -distToBot * 2.0 - Math.max(0, distToThreat - ENGAGE_RANGE) * 3.0;
        if (score > bestScore) {
            bestScore = score;
            best = cp;
        }
    }
    return best;
}

// Find a cover point that HAS LOS to threat (good peek spot — cover nearby but can see enemy)
function findPeekSpot(botX, botZ, threatX, threatZ) {
    let best = null, bestScore = -Infinity;
    for (const cp of COVER_POINTS) {
        const dx = cp.x - botX, dz = cp.z - botZ;
        const distToBot = Math.sqrt(dx * dx + dz * dz);
        if (distToBot > 6) continue; // peek spots should be very near current position

        // Must HAVE LOS to threat (we want to shoot from here)
        const hasLOS = bro.ai.game.hasLineOfSight(cp.x, cp.z, threatX, threatZ,
            OBSTACLES.concat(WALLS));
        if (!hasLOS) continue;

        const score = -distToBot; // closest peek point wins
        if (score > bestScore) {
            bestScore = score;
            best = cp;
        }
    }
    return best;
}

// ─── Reward-based target selection ──────────────────────────────────────────
function botFindTarget(bot) {
    const p = bot.player;
    let bestId = null, bestScore = -Infinity;

    for (const [id, other] of players) {
        if (id === p.id || !other.alive) continue;
        const dx = other.x - p.x, dz = other.z - p.z;
        const dist = Math.sqrt(dx * dx + dz * dz);

        // Base score: prefer closer targets
        let score = -dist * 1.0;

        // Prefer low-health targets (easy kills = reward)
        score += (MAX_HEALTH - other.health) * 0.5;

        // Prefer targets we have LOS to (can engage immediately)
        const hasLOS = bro.ai.game.hasLineOfSight(p.x, p.z, other.x, other.z,
            OBSTACLES.concat(WALLS));
        if (hasLOS) score += 15;

        // Avoid targets when we're low HP (negative reward for dying)
        if (p.health < HEAL_THRESHOLD && dist < CLOSE_RANGE) score -= 20;

        if (score > bestScore) {
            bestScore = score;
            bestId = id;
        }
    }

    bot.targetId = bestId;
}

// ─── Bot spawn ──────────────────────────────────────────────────────────────
function spawnBot() {
    const botId = nextBotId++;
    const sp = pickSpawn();
    const color = COLORS[nextColorIdx++ % COLORS.length];
    const name = BOT_NAMES[bots.size % BOT_NAMES.length] + ' (bot)';

    const player = {
        id: botId, name, color,
        x: sp.x, y: 0, z: sp.z,
        yaw: 0, pitch: 0,
        health: MAX_HEALTH, alive: true,
        kills: 0, deaths: 0,
        lastInputTick: 0,
        lastShoot: false,
        shootCooldown: 0,
        respawnTimer: 0,
        input: 0, inputYaw: 0, inputPitch: 0,
        isBot: true,
    };

    const agent = bro.ai.game.createAgent({
        navGrid: nav,
        x: sp.x, z: sp.z,
        speed: BOT_SPEED,
        radius: PLAYER_RADIUS,
    });

    players.set(botId, player);
    bots.set(botId, {
        player, agent,
        targetId: null,
        thinkTimer: 0,
        shootDelay: 0,
        state: ST_ROAM,
        stateTimer: 0,       // time spent in current state
        coverPoint: null,     // current cover destination
        peekPoint: null,      // where to peek from
        roamIdx: Math.floor(Math.random() * COVER_POINTS.length), // patrol index
        score: 0,             // cumulative reward score
        // Smoothed aim — 15 Hz target sampling, 5 rad/s turn rate.
        // The 8.6° fire cone gates shooting so bots only fire when on target.
        aim: BotAim.create({ turnSpeed: 5.0, sampleHz: 15, fireConeRad: 0.15 }),
    });

    console.log(name + ' spawned [' + players.size + ' total]');
}

// Spawn initial bots
for (let i = 0; i < NUM_BOTS; i++) spawnBot();

// ─── Bot AI tick ────────────────────────────────────────────────────────────

function botTick(bot, dt) {
    const p = bot.player;
    if (!p.alive) return;

    bot.stateTimer += dt;

    // Rethink target periodically
    bot.thinkTimer -= dt;
    if (bot.thinkTimer <= 0 || !bot.targetId || !players.has(bot.targetId) || !players.get(bot.targetId).alive) {
        botFindTarget(bot);
        bot.thinkTimer = 0.5 + Math.random() * 0.5;
    }

    const target = bot.targetId ? players.get(bot.targetId) : null;
    const hasTarget = target && target.alive;

    let targetDist = Infinity;
    let hasLOS = false;
    if (hasTarget) {
        const dx = target.x - p.x, dz = target.z - p.z;
        targetDist = Math.sqrt(dx * dx + dz * dz);
        hasLOS = bro.ai.game.hasLineOfSight(p.x, p.z, target.x, target.z,
            OBSTACLES.concat(WALLS));
    }

    // ── State transitions ──
    const prevState = bot.state;

    if (!hasTarget) {
        // No target: roam
        bot.state = ST_ROAM;
    } else if (p.health < HEAL_THRESHOLD && bot.state !== ST_HEAL) {
        // Low HP: find cover and heal
        const cover = findCover(p.x, p.z, target.x, target.z);
        if (cover) {
            bot.coverPoint = cover;
            bot.state = ST_HEAL;
        }
        // If no cover available, keep fighting
    } else if (bot.state === ST_HEAL) {
        // Stop healing when HP recovered
        if (p.health >= HEAL_RESUME) {
            bot.state = hasLOS ? ST_PEEK : ST_PUSH;
        }
    } else if (bot.state === ST_ROAM) {
        // Spotted a target: engage
        if (targetDist > CLOSE_RANGE) {
            bot.state = ST_PUSH;
        } else {
            // Close range: find cover first
            const cover = findCover(p.x, p.z, target.x, target.z);
            bot.state = cover ? ST_COVER : ST_PUSH;
            bot.coverPoint = cover;
        }
    } else if (bot.state === ST_PUSH && targetDist < CLOSE_RANGE) {
        // Got close: switch to cover/peek play
        const cover = findCover(p.x, p.z, target.x, target.z);
        if (cover) {
            bot.coverPoint = cover;
            bot.state = ST_COVER;
        }
        // Otherwise keep pushing
    } else if (bot.state === ST_COVER) {
        // Arrived at cover: peek after a delay
        const dx = p.x - (bot.coverPoint ? bot.coverPoint.x : p.x);
        const dz = p.z - (bot.coverPoint ? bot.coverPoint.z : p.z);
        const atCover = (dx * dx + dz * dz) < 1.5;
        if (atCover && bot.stateTimer > COVER_RETREAT_TIME) {
            const peek = findPeekSpot(p.x, p.z, target.x, target.z);
            if (peek) {
                bot.peekPoint = peek;
                bot.state = ST_PEEK;
            } else {
                // No peek spot: push out
                bot.state = ST_PUSH;
            }
        }
    } else if (bot.state === ST_PEEK) {
        // Done peeking: back to cover
        if (bot.stateTimer > PEEK_DURATION) {
            const cover = findCover(p.x, p.z, target.x, target.z);
            if (cover) {
                bot.coverPoint = cover;
                bot.state = ST_COVER;
            } else {
                bot.state = ST_PUSH;
            }
        }
    }

    // Reset timer on state change
    if (bot.state !== prevState) bot.stateTimer = 0;

    // ── Aim system: every state samples the target's eye position into the
    // BotAim tracker; per-tick rotation produces realistic lag instead of
    // instant snap-aim. Player yaw/pitch is read from the tracker after.
    const simT = serverTick * dt;
    if (hasTarget) {
        BotAim.requestAimAt(bot.aim, simT,
            p.x, EYE_HEIGHT, p.z,
            target.x, EYE_HEIGHT, target.z);
    } else if (bot.agent.hasTarget) {
        // No combat target — face the direction we're moving so the body
        // doesn't strafe sideways while patrolling.
        const fy = bot.agent.yaw;
        BotAim.requestAim(bot.aim, simT, fy, 0);
    }
    BotAim.tick(bot.aim, dt);

    // ── Execute current state ──
    switch (bot.state) {
        case ST_ROAM: {
            // Patrol between cover points (or random walkable spots if cover
            // generation produced nothing — defensive against nav misconfig).
            if (!bot.agent.hasTarget || bot.agent.atTarget) {
                if (COVER_POINTS.length > 0) {
                    bot.roamIdx = (bot.roamIdx + 1 + Math.floor(Math.random() * 3)) % COVER_POINTS.length;
                    const dest = COVER_POINTS[bot.roamIdx];
                    bot.agent.setTarget(dest.x, dest.z);
                } else {
                    // Random walkable point inside the arena.
                    const lim = ARENA_HALF - PLAYER_RADIUS - WALL_THICK - 1.0;
                    for (let tries = 0; tries < 8; tries++) {
                        const rx = (Math.random() * 2 - 1) * lim;
                        const rz = (Math.random() * 2 - 1) * lim;
                        if (nav.isWalkable(rx, rz)) { bot.agent.setTarget(rx, rz); break; }
                    }
                }
            }
            bot.agent.update(dt);
            p.x = bot.agent.x;
            p.z = bot.agent.z;
            break;
        }

        case ST_PUSH: {
            if (!hasTarget) break;
            // Advance toward target — strafe when close
            if (targetDist > CLOSE_RANGE) {
                bot.agent.setTarget(target.x, target.z);
            } else {
                // Strafe perpendicular
                const dx = target.x - p.x, dz = target.z - p.z;
                const perpX = -dz / targetDist, perpZ = dx / targetDist;
                const strafeDir = (Math.sin(Date.now() * 0.003 + p.id * 7) > 0) ? 1 : -1;
                bot.agent.setTarget(p.x + perpX * strafeDir * 3, p.z + perpZ * strafeDir * 3);
            }
            bot.agent.update(dt);
            p.x = bot.agent.x;
            p.z = bot.agent.z;
            botShoot(bot, target, hasLOS, targetDist, dt);
            break;
        }

        case ST_COVER: {
            if (bot.coverPoint) {
                bot.agent.setTarget(bot.coverPoint.x, bot.coverPoint.z);
            }
            bot.agent.update(dt);
            p.x = bot.agent.x;
            p.z = bot.agent.z;
            // Opportunistic shot while retreating to cover.
            if (hasLOS && targetDist < ENGAGE_RANGE) {
                botShoot(bot, target, hasLOS, targetDist, dt);
            }
            break;
        }

        case ST_PEEK: {
            if (bot.peekPoint) {
                bot.agent.setTarget(bot.peekPoint.x, bot.peekPoint.z);
            }
            bot.agent.update(dt);
            p.x = bot.agent.x;
            p.z = bot.agent.z;
            if (hasTarget) botShoot(bot, target, hasLOS, targetDist, dt);
            break;
        }

        case ST_HEAL: {
            if (bot.coverPoint) {
                bot.agent.setTarget(bot.coverPoint.x, bot.coverPoint.z);
            }
            bot.agent.update(dt);
            p.x = bot.agent.x;
            p.z = bot.agent.z;
            // Heal when no LOS to threat (safely behind cover).
            if (!hasLOS || !hasTarget) {
                p.health = Math.min(MAX_HEALTH, p.health + HEAL_RATE * dt);
            }
            break;
        }
    }

    // Single source of truth: whatever state we're in, the rendered look
    // direction comes from the smoothed aim tracker.
    p.yaw = bot.aim.yaw;
    p.pitch = bot.aim.pitch;
}

// ─── Bot shoot ──────────────────────────────────────────────────────────────
// Aim is handled by the BotAim tracker (set in botTick). We only fire when:
//   - cooldown elapsed
//   - target in LOS and within engage range
//   - the gun has rotated onto target (within fireConeRad)
// processShot uses p.yaw/p.pitch (already written from bot.aim), so a slow
// turn means early shots miss naturally — exactly the human-like behavior
// the new aim system is meant to produce.
function botShoot(bot, target, hasLOS, dist, dt) {
    const p = bot.player;
    p.shootCooldown = Math.max(0, p.shootCooldown - dt);
    bot.shootDelay = Math.max(0, bot.shootDelay - dt);

    if (!hasLOS || dist >= ENGAGE_RANGE) return;
    if (p.shootCooldown > 0 || bot.shootDelay > 0) return;
    if (!BotAim.canFireAt(bot.aim, p.x, EYE_HEIGHT, p.z,
                          target.x, EYE_HEIGHT, target.z)) return;

    processShot(p);
    p.shootCooldown = SHOOT_COOLDOWN;
    bot.shootDelay = 0.1 + Math.random() * 0.15;
}

// Periodic status
setInterval(() => {
    if (players.size > 0)
        console.log('[' + bro.server.uptime.toFixed(0) + 's] ' + players.size + ' player(s)');
}, 10000);
