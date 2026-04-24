// =============================================================================
// FPS Arena — Client
// =============================================================================
//
// Scene-graph 3D rendering, first-person camera with pointer lock,
// local movement with server correction, entity interpolation, binary protocol.

// ─── DOM ─────────────────────────────────────────────────────────────────────

const canvas      = document.querySelector('#game');
const scene       = canvas.getContext('scene');
const connectScreen = document.querySelector('#connect-screen');
const connectBtn  = document.querySelector('#connect-btn');
const nameInput   = document.querySelector('#name-input');
const addressInput = document.querySelector('#address-input');
const errorMsg    = document.querySelector('#error-msg');
const hud         = document.querySelector('#hud');
const healthFill  = document.querySelector('#health-fill');
const healthText  = document.querySelector('#health-text');
const killsCount  = document.querySelector('#kills-count');
const killFeed    = document.querySelector('#kill-feed');
const netInfo     = document.querySelector('#net-info');
const deathScreen = document.querySelector('#death-screen');
const clickPrompt = document.querySelector('#click-prompt');

// ─── Constants (must match server) ───────────────────────────────────────────

const ARENA_HALF   = 20;
const WALL_H       = 3;
const WALL_THICK   = 0.5;
const PLAYER_RADIUS = 0.4;
const EYE_HEIGHT   = 1.6;
const MOVE_SPEED   = 6.0;

const MSG_INPUT   = 0x01;
const MSG_STATE   = 0x02;
const MSG_WELCOME = 0x03;
const MSG_EVENT   = 0x04;

const EVT_KILL  = 0;
const EVT_HIT   = 1;
const EVT_SPAWN = 2;

const IN_FWD   = 1;
const IN_BACK  = 2;
const IN_LEFT  = 4;
const IN_RIGHT = 8;
const IN_SHOOT = 16;

// Obstacles — same as server
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
const WALLS = [
    { x: 0,           z: -ARENA_HALF, hw: ARENA_HALF, hd: WALL_THICK, hh: WALL_H },
    { x: 0,           z:  ARENA_HALF, hw: ARENA_HALF, hd: WALL_THICK, hh: WALL_H },
    { x: -ARENA_HALF, z: 0,           hw: WALL_THICK, hd: ARENA_HALF, hh: WALL_H },
    { x:  ARENA_HALF, z: 0,           hw: WALL_THICK, hd: ARENA_HALF, hh: WALL_H },
];
const ALL_SOLIDS = OBSTACLES.concat(WALLS);

const PLAYER_COLORS = [
    '#e74c3c', '#3498db', '#2ecc71', '#f39c12',
    '#9b59b6', '#1abc9c', '#e67e22', '#e91e63',
];

// ─── State ───────────────────────────────────────────────────────────────────

let myId = null;
let connected = false;
let serverConn = null;
let clientTick = 0;
let lastServerTick = 0;
let pointerLocked = false;

// Local player — moved locally, server nudges gently
let localX = 0, localZ = 0;
let localYaw = 0, localPitch = 0;
let localHealth = 100;
let localAlive = true;
let localKills = 0;

// Server correction: smoothly blend toward authoritative position
let serverX = 0, serverZ = 0;
let hasServerPos = false;

// Input
// Input actions backed by bro.settings (System → Input to rebind).
Input.init([
    { name: "forward", label: "Forward", defaults: ["w", "ArrowUp"] },
    { name: "back",    label: "Back",    defaults: ["s", "ArrowDown"] },
    { name: "left",    label: "Strafe Left",  defaults: ["a", "ArrowLeft"] },
    { name: "right",   label: "Strafe Right", defaults: ["d", "ArrowRight"] },
    { name: "primary", label: "Shoot",   defaults: ["Mouse0"] },
]);
Input.attach(window);

// Remote players
const remotePlayers = new Map();
const INTERP_DELAY = 100; // ms behind latest server state

// ─── Collision (matches server) ──────────────────────────────────────────────

function pushCircleOutOfAABB(px, pz, r, box) {
    const bx0 = box.x - box.hw, bx1 = box.x + box.hw;
    const bz0 = box.z - box.hd, bz1 = box.z + box.hd;
    const cx = Math.max(bx0, Math.min(px, bx1));
    const cz = Math.max(bz0, Math.min(pz, bz1));
    const dx = px - cx, dz = pz - cz;
    const dist2 = dx * dx + dz * dz;
    if (dist2 < r * r && dist2 > 0.0001) {
        const dist = Math.sqrt(dist2);
        const pen = r - dist;
        return { x: px + (dx / dist) * pen, z: pz + (dz / dist) * pen };
    }
    if (dist2 < 0.0001) {
        const dl = px - bx0, dr = bx1 - px, dt = pz - bz0, db = bz1 - pz;
        const min = Math.min(dl, dr, dt, db);
        if (min === dl) return { x: bx0 - r, z: pz };
        if (min === dr) return { x: bx1 + r, z: pz };
        if (min === dt) return { x: px, z: bz0 - r };
        return { x: px, z: bz1 + r };
    }
    return null;
}

// ─── Scene Setup ─────────────────────────────────────────────────────────────

function buildScene() {
    // Floor
    scene.createMesh({
        mesh: 'plane', halfW: ARENA_HALF, halfD: ARENA_HALF,
        x: 0, y: 0, z: 0, color: '#2a2a3e',
    });

    // Walls
    const wallDefs = [
        { x: 0,           y: WALL_H / 2, z: -ARENA_HALF, sx: ARENA_HALF * 2, sy: WALL_H, sz: WALL_THICK * 2 },
        { x: 0,           y: WALL_H / 2, z:  ARENA_HALF, sx: ARENA_HALF * 2, sy: WALL_H, sz: WALL_THICK * 2 },
        { x: -ARENA_HALF, y: WALL_H / 2, z: 0,           sx: WALL_THICK * 2, sy: WALL_H, sz: ARENA_HALF * 2 },
        { x:  ARENA_HALF, y: WALL_H / 2, z: 0,           sx: WALL_THICK * 2, sy: WALL_H, sz: ARENA_HALF * 2 },
    ];
    for (const w of wallDefs) {
        scene.createMesh({
            mesh: 'box', halfW: w.sx / 2, halfH: w.sy / 2, halfD: w.sz / 2,
            x: w.x, y: w.y, z: w.z, color: '#3a4a6e',
        });
    }

    // Obstacles
    for (const ob of OBSTACLES) {
        scene.createMesh({
            mesh: 'box', halfW: ob.hw, halfH: ob.hh, halfD: ob.hd,
            x: ob.x, y: ob.hh, z: ob.z, color: '#4a5a80',
        });
    }

    // Ground grid lines
    for (let i = -ARENA_HALF; i <= ARENA_HALF; i += 5) {
        scene.createMesh({
            mesh: 'box', halfW: ARENA_HALF, halfH: 0.005, halfD: 0.02,
            x: 0, y: 0.01, z: i, color: '#3a3a5e',
        });
        scene.createMesh({
            mesh: 'box', halfW: 0.02, halfH: 0.005, halfD: ARENA_HALF,
            x: i, y: 0.01, z: 0, color: '#3a3a5e',
        });
    }
}

// ─── Remote Player Meshes ────────────────────────────────────────────────────

let nextRemoteColor = 0;

function getOrCreateRemote(id) {
    let r = remotePlayers.get(id);
    if (r) return r;

    const colorIdx = nextRemoteColor++ % PLAYER_COLORS.length;
    const bodyNode = scene.createMesh({
        mesh: 'capsule', radius: PLAYER_RADIUS, halfHeight: 0.5,
        x: 0, y: 0.9, z: 0,
        color: PLAYER_COLORS[colorIdx],
    });

    r = {
        bodyNode, colorIdx,
        states: [], // [{t, x, y, z, yaw, health, alive, kills}]
    };
    remotePlayers.set(id, r);
    return r;
}

function removeRemote(id) {
    const r = remotePlayers.get(id);
    if (!r) return;
    scene.destroyNode(r.bodyNode);
    remotePlayers.delete(id);
}

// ─── Binary Protocol ─────────────────────────────────────────────────────────

function getInputBits() {
    let bits = 0;
    // Only sample gameplay input while the pointer is captured so stray
    // clicks/keys on the connect screen don't leak into the movement bits.
    if (!pointerLocked) return bits;
    if (Input.down("forward")) bits |= IN_FWD;
    if (Input.down("back"))    bits |= IN_BACK;
    if (Input.down("left"))    bits |= IN_LEFT;
    if (Input.down("right"))   bits |= IN_RIGHT;
    if (Input.down("primary")) bits |= IN_SHOOT;
    return bits;
}

function sendInput() {
    if (serverConn == null) return;
    const buf = new ArrayBuffer(14);
    const v = new DataView(buf);
    v.setUint8(0, MSG_INPUT);
    v.setUint32(1, clientTick, true);
    v.setUint8(5, getInputBits());
    v.setFloat32(6, localYaw, true);
    v.setFloat32(10, localPitch, true);
    bro.net.send(serverConn, buf, false); // unreliable
}

function sendName(name) {
    if (serverConn == null) return;
    const data = new TextEncoder().encode(JSON.stringify({ type: 'set_name', name }));
    bro.net.send(serverConn, data.buffer);
}

// ─── Local Movement ──────────────────────────────────────────────────────────

function moveLocal(dt) {
    const bits = getInputBits();

    // Scene uses -Z as forward (OpenGL convention)
    const fwdX = Math.sin(localYaw), fwdZ = -Math.cos(localYaw);
    const rightX = Math.cos(localYaw), rightZ = Math.sin(localYaw);

    let mx = 0, mz = 0;
    if (bits & IN_FWD)   { mx += fwdX;   mz += fwdZ; }
    if (bits & IN_BACK)  { mx -= fwdX;   mz -= fwdZ; }
    if (bits & IN_LEFT)  { mx -= rightX; mz -= rightZ; }
    if (bits & IN_RIGHT) { mx += rightX; mz += rightZ; }

    const len = Math.sqrt(mx * mx + mz * mz);
    if (len > 0.001) {
        mx = (mx / len) * MOVE_SPEED * dt;
        mz = (mz / len) * MOVE_SPEED * dt;
    }

    localX += mx;
    localZ += mz;

    // Collide with walls/obstacles
    for (const box of ALL_SOLIDS) {
        const result = pushCircleOutOfAABB(localX, localZ, PLAYER_RADIUS, box);
        if (result) { localX = result.x; localZ = result.z; }
    }

    const lim = ARENA_HALF - PLAYER_RADIUS - WALL_THICK;
    localX = Math.max(-lim, Math.min(lim, localX));
    localZ = Math.max(-lim, Math.min(lim, localZ));
}

// ─── Entity Interpolation ────────────────────────────────────────────────────

function interpolateRemotes() {
    const renderTime = Date.now() - INTERP_DELAY;

    for (const [id, r] of remotePlayers) {
        if (id === myId) { r.bodyNode.visible = false; continue; }

        const states = r.states;
        if (states.length === 0) { r.bodyNode.visible = false; continue; }

        r.bodyNode.visible = true;

        // Find the two states to interpolate between
        let s0 = null, s1 = null;
        for (let i = 0; i < states.length - 1; i++) {
            if (states[i].t <= renderTime && states[i + 1].t >= renderTime) {
                s0 = states[i]; s1 = states[i + 1]; break;
            }
        }

        if (s0 && s1) {
            const alpha = (renderTime - s0.t) / (s1.t - s0.t);
            r.bodyNode.x = s0.x + (s1.x - s0.x) * alpha;
            r.bodyNode.y = 0.9;
            r.bodyNode.z = s0.z + (s1.z - s0.z) * alpha;
            r.bodyNode.rotationY = -lerpAngle(s0.yaw, s1.yaw, alpha);
        } else if (states.length > 0) {
            const latest = states[states.length - 1];
            r.bodyNode.x = latest.x;
            r.bodyNode.y = latest.alive ? 0.9 : 0.2;
            r.bodyNode.z = latest.z;
            r.bodyNode.rotationY = -latest.yaw;
        }

        // Trim old states
        while (states.length > 2 && states[0].t < renderTime - 500) {
            states.shift();
        }
    }
}

function lerpAngle(a, b, t) {
    let diff = b - a;
    while (diff > Math.PI) diff -= 2 * Math.PI;
    while (diff < -Math.PI) diff += 2 * Math.PI;
    return a + diff * t;
}

// ─── Network Message Handling ────────────────────────────────────────────────

function handleBinaryMessage(data) {
    const v = new DataView(data);
    const type = v.getUint8(0);

    switch (type) {
        case MSG_WELCOME: {
            myId = v.getUint16(1, true);
            lastServerTick = v.getUint32(3, true);
            console.log('Welcome! id=' + myId + ' serverTick=' + lastServerTick);
            break;
        }

        case MSG_STATE: {
            const sTick = v.getUint32(1, true);
            const count = v.getUint8(9);
            lastServerTick = sTick;

            const now = Date.now();
            const idsInState = new Set();

            let off = 10;
            for (let i = 0; i < count; i++) {
                const id = v.getUint16(off, true);      off += 2;
                const x = v.getFloat32(off, true);       off += 4;
                const y = v.getFloat32(off, true);       off += 4;
                const z = v.getFloat32(off, true);       off += 4;
                const yaw = v.getFloat32(off, true);     off += 4;
                const health = v.getUint8(off);           off += 1;
                const flags = v.getUint8(off);            off += 1;
                const kills = v.getUint16(off, true);     off += 2;

                const alive = !!(flags & 1);
                idsInState.add(id);

                if (id === myId) {
                    // Store authoritative position for gentle correction
                    serverX = x;
                    serverZ = z;
                    hasServerPos = true;
                    localHealth = health;
                    localAlive = alive;
                    localKills = kills;
                } else {
                    const r = getOrCreateRemote(id);
                    r.states.push({ t: now, x, y, z, yaw, health, alive, kills });
                }
            }

            for (const id of remotePlayers.keys()) {
                if (!idsInState.has(id) && id !== myId) removeRemote(id);
            }
            break;
        }

        case MSG_EVENT: {
            const evtType = v.getUint8(1);
            if (evtType === EVT_KILL) {
                const killerId = v.getUint16(2, true);
                const victimId = v.getUint16(4, true);
                addKillFeed(killerId, victimId);
            } else if (evtType === EVT_HIT) {
                const victimId = v.getUint16(4, true);
                if (victimId === myId) flashDamage();
            } else if (evtType === EVT_SPAWN) {
                const sx = v.getFloat32(2, true);
                const sz = v.getFloat32(6, true);
                localX = sx; localZ = sz;
                serverX = sx; serverZ = sz;
                hasServerPos = true;
                localAlive = true;
                localHealth = 100;
            }
            break;
        }
    }
}

// ─── HUD ─────────────────────────────────────────────────────────────────────

function updateHUD() {
    healthFill.style.width = localHealth + '%';
    healthFill.style.background = localHealth > 60 ? '#2ecc71' :
                                  localHealth > 30 ? '#f39c12' : '#e74c3c';
    healthText.textContent = localHealth;
    killsCount.textContent = localKills;

    deathScreen.classList.toggle('hidden', localAlive);
    clickPrompt.classList.toggle('hidden', pointerLocked);
}

function addKillFeed(killerId, victimId) {
    const entry = document.createElement('div');
    entry.className = 'kill-entry';
    const kName = killerId === myId ? 'You' : 'Player ' + killerId;
    const vName = victimId === myId ? 'You' : 'Player ' + victimId;
    entry.textContent = kName + ' killed ' + vName;
    killFeed.appendChild(entry);
    setTimeout(() => { if (entry.parentNode) entry.parentNode.removeChild(entry); }, 3500);
}

let damageFlashTimer = 0;
function flashDamage() { damageFlashTimer = 0.2; }

// ─── Input ───────────────────────────────────────────────────────────────────

// Escape releases pointer lock. Movement/shoot state is sampled each frame
// from lib/input; we only need the pointer lock gesture here.
document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && pointerLocked) {
        document.exitPointerLock();
        pointerLocked = false;
    }
});

canvas.addEventListener('mousedown', (e) => {
    if (!connected) return;
    if (!pointerLocked) {
        canvas.requestPointerLock();
        pointerLocked = true;
    }
});

document.addEventListener('mousemove', (e) => {
    if (!pointerLocked) return;
    localYaw += e.movementX * 0.002;
    localPitch -= e.movementY * 0.002;
    localPitch = Math.max(-Math.PI / 2 * 0.95, Math.min(Math.PI / 2 * 0.95, localPitch));
});

// ─── Connect ─────────────────────────────────────────────────────────────────

connectBtn.addEventListener('click', () => {
    const addr = addressInput.value.trim();
    if (!addr) { errorMsg.textContent = 'Enter a server address'; return; }
    errorMsg.textContent = '';
    connectBtn.textContent = 'Connecting...';
    connectBtn.disabled = true;
    bro.net.init();
    bro.net.connect(addr);
});

nameInput.addEventListener('keydown', (e) => { if (e.key === 'Enter') connectBtn.click(); });
addressInput.addEventListener('keydown', (e) => { if (e.key === 'Enter') connectBtn.click(); });

bro.net.onconnect = (connId) => {
    serverConn = connId;
    connected = true;
    connectScreen.classList.add('hidden');
    canvas.classList.add('active');
    hud.classList.remove('hidden');
    bro.crosshair.configure({
        style: 'crossdot', size: 12, thickness: 2, spread: 3, dotSize: 1,
        color: '#ffffff', opacity: 0.8, outline: true,
        outlineThickness: 1, outlineColor: '#000000',
        moveSpread: 8, fireBloom: 6, adsSpread: 1,
        bloomDecay: 30, lerpSpeed: 10
    });
    bro.crosshair.show();
    buildScene();
    const name = nameInput.value.trim() || 'Player';
    sendName(name);
};

bro.net.ondisconnect = () => {
    connected = false;
    serverConn = null;
    myId = null;
    pointerLocked = false;
    bro.crosshair.hide();
    canvas.classList.remove('active');
    hud.classList.add('hidden');
    connectScreen.classList.remove('hidden');
    connectBtn.textContent = 'Connect';
    connectBtn.disabled = false;
    errorMsg.textContent = 'Lost connection to server';
    for (const [id] of remotePlayers) removeRemote(id);
};

bro.net.onmessage = (connId, data) => {
    handleBinaryMessage(data);
};

// ─── Render Loop ─────────────────────────────────────────────────────────────

let lastTime = Date.now();
let frameCount = 0, fpsAccum = 0, fps = 0;
let inputSendAccum = 0;
const INPUT_SEND_INTERVAL = 1.0 / 60; // send inputs at 60hz max

function render() {
    requestAnimationFrame(render);

    const now = Date.now();
    const dt = Math.min((now - lastTime) / 1000, 0.05);
    lastTime = now;

    frameCount++;
    fpsAccum += dt;
    if (fpsAccum >= 0.5) { fps = Math.round(frameCount / fpsAccum); frameCount = 0; fpsAccum = 0; }

    if (!connected || myId === null) return;

    // Send input at a steady rate (not tied to framerate)
    inputSendAccum += dt;
    if (inputSendAccum >= INPUT_SEND_INTERVAL) {
        inputSendAccum -= INPUT_SEND_INTERVAL;
        clientTick++;
        sendInput();
    }

    // Move locally with real frame dt — feels instant, no prediction buffer needed
    if (localAlive) {
        moveLocal(dt);
    }

    // Update crosshair spread state
    const bits = getInputBits();
    const isMoving = (bits & (IN_FWD | IN_BACK | IN_LEFT | IN_RIGHT)) !== 0;
    bro.crosshair.setMoving(isMoving);
    if ((bits & IN_SHOOT) && localAlive) bro.crosshair.addBloom(dt * 40);

    // Gentle server correction: nudge toward authoritative position
    // On localhost this is nearly zero; on real network it keeps us honest
    if (hasServerPos) {
        const errX = serverX - localX;
        const errZ = serverZ - localZ;
        const err2 = errX * errX + errZ * errZ;

        if (err2 > 9.0) {
            // Large desync (>3 units) — teleport (respawn, lag spike)
            localX = serverX;
            localZ = serverZ;
        } else if (err2 > 0.0001) {
            // Blend gently: 10% per frame toward server truth
            const blend = Math.min(1.0, 5.0 * dt);
            localX += errX * blend;
            localZ += errZ * blend;
        }
    }

    // Interpolate remote players
    interpolateRemotes();

    // Damage flash
    if (damageFlashTimer > 0) damageFlashTimer -= dt;

    // Camera — first person (scene uses -Z forward)
    const fwdX = Math.sin(localYaw) * Math.cos(localPitch);
    const fwdY = Math.sin(localPitch);
    const fwdZ = -Math.cos(localYaw) * Math.cos(localPitch);

    const w = canvas.clientWidth || 1;
    const h = canvas.clientHeight || 1;

    scene.setCamera({
        fov: 90,
        aspect: w / h,
        near: 0.1,
        far: 200,
        position: [localX, EYE_HEIGHT, localZ],
        target: [localX + fwdX, EYE_HEIGHT + fwdY, localZ + fwdZ],
    });

    // HUD
    updateHUD();
    netInfo.textContent = 'fps ' + fps + ' | tick ' + clientTick + ' | server ' + lastServerTick;
}

requestAnimationFrame(render);
