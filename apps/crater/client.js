// =============================================================================
// Crater — Client
// =============================================================================
//
// 2D canvas turn-based artillery. The server is authoritative for physics
// and turn order — we render the world it describes and submit aim/fire
// intents. Everything else (lobby UI, HUD, menu nav, SFX, input binding)
// comes from apps/lib/*.
//
// Coordinate spaces
//   world units:    CraterShared.C.WORLD_W wide, y increases UP.
//   canvas pixels:  worldToCanvas()/canvasToWorld() map between.

'use strict';

const { C,
        generateHeightmap, heightAt,
        carveCrater, applyCraterDiff, blastDamage } = CraterShared;

// ─── DOM + scene boilerplate ────────────────────────────────────────────────

const canvas = document.querySelector('#view');
const ctx    = canvas.getContext('2d');

function resizeCanvas() {
    const dpr = window.devicePixelRatio || 1;
    const w = window.innerWidth  * dpr;
    const h = window.innerHeight * dpr;
    if (canvas.width !== w || canvas.height !== h) {
        canvas.width  = w;
        canvas.height = h;
    }
}
window.addEventListener('resize', resizeCanvas);
resizeCanvas();

// ─── Library singletons ─────────────────────────────────────────────────────

const store = Storage.create('crater');
store.load({
    name:      'Player',
    address:   '127.0.0.1:27100',
    sfxVol:    0.6,
});
document.getElementById('in-name').value    = store.get('name');
document.getElementById('in-address').value = store.get('address');

Input.init([
    { name: 'aim_left',   label: 'Aim Left',   defaults: ['ArrowLeft'] },
    { name: 'aim_right',  label: 'Aim Right',  defaults: ['ArrowRight'] },
    { name: 'angle_up',   label: 'Angle Up',   defaults: ['ArrowUp'] },
    { name: 'angle_down', label: 'Angle Down', defaults: ['ArrowDown'] },
    { name: 'power_down', label: 'Power Down', defaults: ['q'] },
    { name: 'power_up',   label: 'Power Up',   defaults: ['e'] },
    { name: 'fire',       label: 'Fire',       defaults: [' '] },
    { name: 'pause',      label: 'Pause',      defaults: ['Escape'] },
], { storageKey: 'crater:controls' });
Input.attach();

SFX.init({ sfxVol: store.get('sfxVol') });
const sfx = {
    menu:   () => SFX.tone(420, 0.04, 'sine', 0.3),
    select: () => SFX.tone(660, 0.08, 'square', 0.35),
    aim:    () => SFX.tone(280, 0.02, 'sine', 0.15),
    fire:   () => SFX.sequence([[200,0.07,'sawtooth',0.5],[140,0.15,'triangle',0.5]]),
    hit:    () => SFX.sequence([[90,0.12,'square',0.8],[60,0.35,'sawtooth',0.7]]),
    miss:   () => SFX.tone(180, 0.2, 'triangle', 0.35),
    die:    () => SFX.sequence([[220,0.1,'sawtooth',0.5],[180,0.1,'sawtooth',0.5],[120,0.4,'sawtooth',0.6]]),
    win:    () => SFX.sequence([[523,0.1,'square',0.6],[659,0.1,'square',0.7],[784,0.15,'square',0.8],[1047,0.25,'square',0.8]]),
};

const screens = Screens.create({
    overlay: '#overlay',
    onMenuMove:   sfx.menu,
    onMenuSelect: sfx.select,
});

// ─── Game state ─────────────────────────────────────────────────────────────

const net = {
    client: null,      // NetRoom client
    myId:   null,
};

const match = {
    active:      false,
    phase:       'lobby',
    hm:          null,          // Float32Array
    players:     [],            // [{id,name,color,x,hp,alive,bot}]
    turn:        null,
    aim:         { angle: Math.PI / 4, power: 0.5, dir: 1 },
    lobby:       { hostId: null, entries: [] },
    projectile:  null,          // active ballistic animation
    explosion:   null,          // active explosion effect
    particles:   [],            // dust/debris
    cameraShake: 0,
    winner:      null,
};

// ─── World/canvas mapping ───────────────────────────────────────────────────

function viewPadding() { return 40; }
function viewScale() {
    // Fit world width with some vertical room for sky.
    const pad = viewPadding();
    return Math.min(
        (canvas.width  - pad * 2) / C.WORLD_W,
        (canvas.height - pad * 2) / (C.MAX_H + 15)
    );
}
function viewOrigin() {
    const s = viewScale();
    return {
        x: (canvas.width  - C.WORLD_W * s) / 2,
        y: canvas.height - viewPadding(),
    };
}
function worldToCanvas(wx, wy) {
    const s = viewScale();
    const o = viewOrigin();
    return { x: o.x + wx * s, y: o.y - wy * s };
}

// ─── Rendering ──────────────────────────────────────────────────────────────

function drawSky() {
    const g = ctx.createLinearGradient(0, 0, 0, canvas.height);
    g.addColorStop(0, '#1a2a55');
    g.addColorStop(0.6, '#3a3b5c');
    g.addColorStop(1.0, '#4f3a2c');
    ctx.fillStyle = g;
    ctx.fillRect(0, 0, canvas.width, canvas.height);
}

function drawTerrain() {
    if (!match.hm) return;
    const s = viewScale();
    const o = viewOrigin();
    ctx.fillStyle = '#3a2e1d';
    ctx.strokeStyle = '#6d5a38';
    ctx.lineWidth = Math.max(1, s * 0.15);
    ctx.beginPath();
    ctx.moveTo(o.x, o.y);
    for (let i = 0; i < C.COLS; i++) {
        const wx = (i + 0.5) * C.COL_W;
        const wy = match.hm[i];
        ctx.lineTo(o.x + wx * s, o.y - wy * s);
    }
    ctx.lineTo(o.x + C.WORLD_W * s, o.y);
    ctx.closePath();
    ctx.fill();
    // Grass strip along the surface.
    ctx.fillStyle = '#5a8a3e';
    for (let i = 0; i < C.COLS; i++) {
        const wx = i * C.COL_W;
        const wy = match.hm[i];
        const p1 = worldToCanvas(wx, wy);
        ctx.fillRect(p1.x, p1.y - Math.max(2, s * 0.4), C.COL_W * s + 1, Math.max(2, s * 0.4));
    }
    ctx.stroke();
}

function drawTanks() {
    if (!match.hm) return;
    const s = viewScale();
    for (const p of match.players) {
        if (!p.alive) continue;
        const gy = heightAt(match.hm, p.x);
        const c = worldToCanvas(p.x, gy);
        const w = C.TANK_W * s;
        const h = C.TANK_H * s;
        // Chassis
        ctx.fillStyle = p.color;
        ctx.fillRect(c.x - w / 2, c.y - h, w, h);
        // Turret
        ctx.beginPath();
        ctx.arc(c.x, c.y - h, w * 0.25, 0, Math.PI * 2);
        ctx.fill();
        // HP strip
        ctx.fillStyle = 'rgba(0,0,0,0.5)';
        ctx.fillRect(c.x - w / 2, c.y - h - 10, w, 4);
        ctx.fillStyle = p.hp > 50 ? '#3ecf4a' : (p.hp > 20 ? '#f5b940' : '#e55');
        ctx.fillRect(c.x - w / 2, c.y - h - 10, w * (p.hp / C.HP_MAX), 4);
        // Name
        ctx.fillStyle = '#fff';
        ctx.font = (Math.max(10, s * 0.45)) + 'px "Segoe UI", sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText(p.name + (p.id === match.turn ? ' ◄' : ''), c.x, c.y - h - 14);
    }
    ctx.textAlign = 'start';
}

function drawAimIndicator() {
    if (!isMyTurn() || match.projectile) return;
    const me = myPlayer();
    if (!me || !me.alive) return;
    const surfY = heightAt(match.hm, me.x);
    const muzzleLen = C.TANK_W * 0.9;
    const bx = me.x + match.aim.dir * muzzleLen * Math.cos(match.aim.angle);
    const by = surfY + C.TANK_H + muzzleLen * Math.sin(match.aim.angle);
    const b = worldToCanvas(bx, by);
    // Predictor arc (dashed).
    const vx = match.aim.dir * match.aim.power * C.MAX_SPEED * Math.cos(match.aim.angle);
    const vy = match.aim.power * C.MAX_SPEED * Math.sin(match.aim.angle);
    ctx.save();
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.55)';
    ctx.setLineDash([4, 6]);
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.moveTo(b.x, b.y);
    let px = bx, py = by, pvx = vx, pvy = vy;
    const dt = 0.04;
    for (let i = 0; i < 50; i++) {
        px += pvx * dt;
        py += pvy * dt;
        pvy -= C.GRAVITY * dt;
        const cp = worldToCanvas(px, py);
        ctx.lineTo(cp.x, cp.y);
        if (px < 0 || px > C.WORLD_W || py < heightAt(match.hm, Math.max(0, Math.min(C.WORLD_W, px)))) break;
    }
    ctx.stroke();
    ctx.restore();
}

function drawProjectile() {
    if (!match.projectile) return;
    const p = match.projectile;
    const c = worldToCanvas(p.x, p.y);
    ctx.fillStyle = '#fff5b0';
    ctx.beginPath();
    ctx.arc(c.x, c.y, Math.max(2, viewScale() * 0.35), 0, Math.PI * 2);
    ctx.fill();
    // Trail
    ctx.strokeStyle = 'rgba(255, 210, 90, 0.55)';
    ctx.lineWidth = 2;
    ctx.beginPath();
    for (let i = 0; i < p.trail.length; i += 2) {
        const t = worldToCanvas(p.trail[i], p.trail[i + 1]);
        if (i === 0) ctx.moveTo(t.x, t.y);
        else ctx.lineTo(t.x, t.y);
    }
    ctx.stroke();
}

function drawExplosion() {
    if (!match.explosion) return;
    const e = match.explosion;
    const c = worldToCanvas(e.x, e.y);
    const s = viewScale();
    const r = e.radius * s * (1 - e.t * 0.5);
    ctx.save();
    ctx.globalAlpha = 1 - e.t;
    const g = ctx.createRadialGradient(c.x, c.y, 0, c.x, c.y, r);
    g.addColorStop(0.0, 'rgba(255, 240, 140, 1)');
    g.addColorStop(0.4, 'rgba(255, 120, 30, 0.9)');
    g.addColorStop(1.0, 'rgba(90, 30, 10, 0)');
    ctx.fillStyle = g;
    ctx.beginPath();
    ctx.arc(c.x, c.y, Math.max(3, r), 0, Math.PI * 2);
    ctx.fill();
    ctx.restore();
}

function drawParticles() {
    const s = viewScale();
    for (const p of match.particles) {
        const c = worldToCanvas(p.x, p.y);
        ctx.globalAlpha = Math.max(0, p.life / p.maxLife);
        ctx.fillStyle = p.color;
        ctx.fillRect(c.x - 1, c.y - 1, 2 + s * 0.15, 2 + s * 0.15);
    }
    ctx.globalAlpha = 1;
}

function draw() {
    const shake = match.cameraShake;
    ctx.save();
    if (shake > 0) {
        ctx.translate((Math.random() - 0.5) * shake, (Math.random() - 0.5) * shake);
    }
    drawSky();
    drawTerrain();
    drawTanks();
    drawAimIndicator();
    drawProjectile();
    drawExplosion();
    drawParticles();
    ctx.restore();
}

// ─── Simulation (visual only — server is authoritative) ─────────────────────

function tick(dt) {
    const dts = dt / 1000;
    // Camera shake decay.
    if (match.cameraShake > 0) match.cameraShake = Math.max(0, match.cameraShake - dt * 0.15);

    // Projectile animation.
    if (match.projectile) {
        const p = match.projectile;
        const steps = 4;                       // sub-step for smoother trail
        const h = dts / steps;
        for (let i = 0; i < steps; i++) {
            p.x  += p.vx * h;
            p.y  += p.vy * h;
            p.vy -= C.GRAVITY * h;
            p.flown += h;
        }
        // Sample trail ~every 30ms.
        p.sinceTrail += dt;
        if (p.sinceTrail > 30) {
            p.trail.push(p.x, p.y);
            if (p.trail.length > 80) p.trail.splice(0, 2);
            p.sinceTrail = 0;
        }
        if (p.flown * 1000 >= p.flightMs) {
            detonate(p);
            match.projectile = null;
        }
    }

    // Explosion fade.
    if (match.explosion) {
        match.explosion.t += dt / match.explosion.duration;
        if (match.explosion.t >= 1) match.explosion = null;
    }

    // Particles.
    const parts = match.particles;
    for (let i = parts.length - 1; i >= 0; i--) {
        const p = parts[i];
        p.life -= dt;
        if (p.life <= 0) { parts.splice(i, 1); continue; }
        p.x  += p.vx * dts;
        p.y  += p.vy * dts;
        p.vy -= 15 * dts;
    }

    // Input (only the active shooter, and only while the overlay is hidden).
    if (overlayOpen()) return;
    if (isMyTurn() && !match.projectile) {
        let dirty = false;
        if (Input.down('aim_left'))  { match.aim.dir = -1; dirty = true; }
        if (Input.down('aim_right')) { match.aim.dir =  1; dirty = true; }
        if (Input.down('angle_up'))   { match.aim.angle = Math.min(Math.PI / 2 - 0.02, match.aim.angle + 0.8 * dts); dirty = true; }
        if (Input.down('angle_down')) { match.aim.angle = Math.max(0.02,              match.aim.angle - 0.8 * dts); dirty = true; }
        if (Input.down('power_up'))   { match.aim.power = Math.min(1,   match.aim.power + 0.5 * dts); dirty = true; }
        if (Input.down('power_down')) { match.aim.power = Math.max(0.05, match.aim.power - 0.5 * dts); dirty = true; }
        if (dirty) updateAimHud();
        if (Input.pressed('fire')) fire();
    }
}

function overlayOpen() {
    const el = document.getElementById('overlay');
    return el && el.style.display !== 'none';
}

function detonate(p) {
    // Explosion effect (impact info is authoritative from the server).
    const cx = p.impactX, cy = p.impactY;
    if (p.hit) {
        // Terrain carve.
        if (match.hm && p.craterCols) applyCraterDiff(match.hm, p.craterCols);
        match.explosion = { x: cx, y: cy, radius: C.BLAST_RADIUS, t: 0, duration: 700 };
        match.cameraShake = 14;
        spawnDust(cx, cy, 40);
        sfx.hit();
    } else {
        // Off-map miss — small puff where it left.
        match.explosion = { x: p.x, y: Math.max(0, p.y), radius: 2, t: 0, duration: 450 };
        sfx.miss();
    }
    // Apply damages.
    if (p.damages) {
        for (const [id, hp] of p.damages) {
            const t = match.players.find(q => q.id === id);
            if (t) t.hp = hp;
        }
    }
    if (p.dead) {
        for (const id of p.dead) {
            const t = match.players.find(q => q.id === id);
            if (t) { t.alive = false; sfx.die(); spawnDust(t.x, heightAt(match.hm, t.x) + 1, 30); }
        }
    }
    if (p.nextTurn != null) match.turn = p.nextTurn;
    else if (p.over) match.turn = null;
    updateHudPlayers();
}

function spawnDust(x, y, count) {
    const palette = ['#b08050', '#7d5a35', '#d0a070', '#5a3e22'];
    for (let i = 0; i < count; i++) {
        const a = Math.random() * Math.PI;
        const speed = 6 + Math.random() * 10;
        match.particles.push({
            x, y,
            vx: Math.cos(a) * speed * (Math.random() < 0.5 ? -1 : 1),
            vy: Math.sin(a) * speed + 4,
            life: 500 + Math.random() * 600,
            maxLife: 1100,
            color: palette[Math.floor(Math.random() * palette.length)],
        });
    }
}

// ─── Helpers ────────────────────────────────────────────────────────────────

function myPlayer()  { return match.players.find(p => p.id === net.myId) || null; }
function isMyTurn()  { return match.active && match.turn === net.myId; }
function isHost()    { return match.lobby.hostId === net.myId; }

function updateAimHud() {
    Hud.text('#hud-angle', (match.aim.angle * 180 / Math.PI).toFixed(0));
    Hud.text('#hud-power', Math.round(match.aim.power * 100));
    Hud.text('#hud-dir',   match.aim.dir > 0 ? '→' : '←');
}

function updateHudPlayers() {
    const el = document.getElementById('hud-players');
    if (!el) return;
    el.innerHTML = match.players.map(p => {
        const hpPct = Math.max(0, p.hp / C.HP_MAX * 100);
        const cls = [
            'hud-player',
            p.id === match.turn ? 'active' : '',
            !p.alive ? 'dead' : '',
        ].filter(Boolean).join(' ');
        return `
            <div class="${cls}">
                <span style="color:${p.color}">${p.id === net.myId ? '★ ' : ''}${escapeHtml(p.name)}</span>
                <span class="hp-bar"><span class="hp-fill" style="width:${hpPct}%;background:${hpColor(p.hp)}"></span></span>
            </div>`;
    }).join('');
    const turnPlayer = match.players.find(p => p.id === match.turn);
    Hud.text('#hud-turn', turnPlayer ? ('Turn: ' + turnPlayer.name + (turnPlayer.id === net.myId ? ' (YOU)' : ''))
                                     : 'Waiting…');
}

function hpColor(hp) {
    return hp > 50 ? '#3ecf4a' : (hp > 20 ? '#f5b940' : '#e55');
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

// ─── Networking glue ────────────────────────────────────────────────────────

function connect() {
    const name    = document.getElementById('in-name').value.trim() || 'Player';
    const address = document.getElementById('in-address').value.trim() || '127.0.0.1:27100';
    store.set('name', name);
    store.set('address', address);
    store.save();

    try {
        net.client = NetRoom.join({
            address, name,
            onConnect()       { showError(''); },
            onDisconnect()    { onDisconnect(); },
            onMessage(t, msg) { handleServerMessage(t, msg); },
        });
    } catch (e) {
        showError(e.message);
    }
}

function handleServerMessage(t, msg) {
    switch (t) {
        case 'welcome': net.myId = msg.id; break;
        case 'denied':
            showError(msg.reason || 'Connection denied');
            if (net.client) net.client.close();
            break;
        case 'state':   onLobbyState(msg); break;
        case 'match':   onMatchStart(msg); break;
        case 'shot':    onShot(msg);       break;
        case 'skip':    onSkip(msg);       break;
        case 'over':    onGameOver(msg);   break;
    }
}

function onDisconnect() {
    match.active = false;
    net.client = null;
    net.myId   = null;
    Hud.hide('#hud');
    screens.switchTo('title');
    showError('Disconnected from server');
}

function onLobbyState(msg) {
    match.phase        = 'lobby';
    match.lobby.hostId = msg.hostId;
    match.lobby.entries = msg.players;
    if (screens.name() !== 'lobby') screens.switchTo('lobby');
    renderLobby();
}

function onMatchStart(msg) {
    match.phase = 'match';
    match.active = true;
    match.hm = new Float32Array(msg.hm);
    match.players = msg.players.map(p => Object.assign({}, p));
    match.turn = msg.turn;
    match.projectile = null;
    match.explosion = null;
    match.particles.length = 0;
    match.winner = null;
    screens.hideOverlay();
    Hud.show('#hud');
    updateAimHud();
    updateHudPlayers();
}

function onShot(msg) {
    if (!match.active) return;
    // If a previous projectile is still animating, resolve it instantly before
    // starting the next one — otherwise the new shot overwrites the old and
    // the earlier explosion/damage never gets applied.
    if (match.projectile) {
        detonate(match.projectile);
        match.projectile = null;
    }
    // Spawn animating projectile.
    match.projectile = {
        x: msg.originX, y: msg.originY,
        vx: msg.vx,     vy: msg.vy,
        flightMs:    msg.flightMs,
        flown:       0,
        hit:         msg.hit,
        impactX:     msg.impactX,
        impactY:     msg.impactY,
        craterCols:  msg.craterCols,
        damages:     msg.damages,
        dead:        msg.dead,
        nextTurn:    msg.nextTurn,
        over:        msg.nextTurn == null,
        trail:       [msg.originX, msg.originY],
        sinceTrail:  0,
    };
    sfx.fire();
}

function onSkip(msg) {
    match.turn = msg.nextTurn;
    updateHudPlayers();
    Hud.toast('Turn skipped (timeout)', 1800, { id: 'crater-toast' });
}

function onGameOver(msg) {
    match.active = false;
    match.winner = msg;
    const winPlayer = msg.winnerId != null ? match.players.find(p => p.id === msg.winnerId) : null;
    if (winPlayer && winPlayer.id === net.myId) sfx.win();
    document.getElementById('go-title').textContent =
        msg.winnerId == null ? 'Draw' :
        (msg.winnerId === net.myId ? 'Victory!' : 'Defeated');
    document.getElementById('go-subtitle').textContent =
        winPlayer ? (winPlayer.name + ' wins the match.') : 'All tanks destroyed.';
    screens.switchTo('gameover');
    Hud.hide('#hud');
}

function showError(msg) {
    const el = document.getElementById('title-error');
    if (el) el.textContent = msg || '';
}

// ─── Actions ────────────────────────────────────────────────────────────────

function fire() {
    if (!net.client || !isMyTurn() || match.projectile) return;
    net.client.send('fire', {
        angle: match.aim.angle,
        power: match.aim.power,
        dir:   match.aim.dir,
    });
}

function sendReady(ready) {
    if (net.client) net.client.send('ready', { ready });
}
function sendStart() { if (net.client) net.client.send('start'); }
function sendBot()   { if (net.client) net.client.send('addBot'); }

function leaveMatch() {
    if (net.client) { try { net.client.close(); } catch (e) {} }
    net.client = null;
    net.myId = null;
    match.active = false;
    Hud.hide('#hud');
    screens.switchTo('title');
}

// ─── Lobby UI ───────────────────────────────────────────────────────────────

function renderLobby() {
    const el = document.getElementById('lobby-players');
    el.innerHTML = match.lobby.entries.map(p => `
        <div class="lobby-row">
            <span class="lobby-swatch" style="background:${p.color}"></span>
            <span>${escapeHtml(p.name)}</span>
            ${p.bot ? '<span class="lobby-bot">(bot)</span>' : ''}
            ${p.id === match.lobby.hostId ? '<span class="lobby-bot">(host)</span>' : ''}
            <span class="lobby-ready ${p.ready ? '' : 'not-ready'}">${p.ready ? 'READY' : 'not ready'}</span>
        </div>
    `).join('');

    // Enable/disable host-only actions.
    const startItem = [...document.querySelectorAll('#screen-lobby .menu-item')]
        .find(it => it.dataset.action === 'start');
    const botItem = [...document.querySelectorAll('#screen-lobby .menu-item')]
        .find(it => it.dataset.action === 'bot');
    const humansReady = match.lobby.entries.filter(p => !p.bot).every(p => p.ready);
    const canStart = isHost() && match.lobby.entries.length >= 2 && humansReady;
    startItem.classList.toggle('disabled', !canStart);
    botItem.classList.toggle('disabled', !isHost());
}

// ─── Screens ────────────────────────────────────────────────────────────────

screens.define('title', {
    enter() {
        showError('');
        screens.showOverlay('title');
        screens.updateSelection('title');
    },
    keydown(key) {
        screens.menuNav('title', key, (idx, item) => {
            const a = item && item.dataset.action;
            if (a === 'connect') connect();
            else if (a === 'howto') screens.switchTo('howto');
            else if (a === 'quit')  window.close();
        });
    },
});

screens.define('howto', {
    enter() { screens.showOverlay('howto'); screens.updateSelection('howto'); },
    keydown(key) {
        screens.menuNav('howto', key, () => screens.switchTo('title'),
            { onBack: () => screens.switchTo('title') });
    },
});

screens.define('lobby', {
    enter() {
        screens.showOverlay('lobby');
        renderLobby();
        screens.updateSelection('lobby');
    },
    keydown(key) {
        screens.menuNav('lobby', key, (idx, item) => {
            const a = item && item.dataset.action;
            if (a === 'ready') {
                const me = match.lobby.entries.find(p => p.id === net.myId);
                sendReady(me ? !me.ready : true);
            } else if (a === 'bot' && isHost()) {
                sendBot();
            } else if (a === 'start' && isHost()) {
                sendStart();
            } else if (a === 'leave') {
                leaveMatch();
            }
        });
    },
});

screens.define('pause', {
    enter() { screens.showOverlay('pause'); screens.updateSelection('pause'); },
    keydown(key) {
        screens.menuNav('pause', key, (idx, item) => {
            const a = item && item.dataset.action;
            if (a === 'resume')      { screens.hideOverlay(); }
            else if (a === 'leave')  { leaveMatch(); }
        }, { onBack: () => screens.hideOverlay() });
    },
});

screens.define('gameover', {
    enter() { screens.showOverlay('gameover'); screens.updateSelection('gameover'); },
    keydown(key) {
        screens.menuNav('gameover', key, (idx, item) => {
            const a = item && item.dataset.action;
            if (a === 'lobby')       { screens.switchTo('lobby'); renderLobby(); }
            else if (a === 'leave')  { leaveMatch(); }
        });
    },
});

document.body.addEventListener('keydown', (e) => {
    // Don't steal input from the name/address fields.
    if (document.activeElement && document.activeElement.tagName === 'INPUT') {
        if (e.key === 'Enter' && screens.name() === 'title') connect();
        return;
    }
    if (overlayOpen()) {
        screens.keydown(e.key);
        return;
    }
    // In-game: Esc opens the pause menu.
    if (e.key === 'Escape' && match.active && !match.projectile) {
        screens.switchTo('pause');
    }
});

// ─── Boot ───────────────────────────────────────────────────────────────────

screens.switchTo('title');

GameLoop.create({
    tick: (dt) => tick(dt),
    draw: () => draw(),
}).start();
