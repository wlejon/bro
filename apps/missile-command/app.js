// Missile Command — single-file game logic
(function() {
"use strict";

var canvas = document.getElementById("game");
var ctx = canvas.getContext("2d");
var getW = function() { return Canvas.w(ctx, 900); };
var getH = function() { return Canvas.h(ctx, 700); };

// ---------------- Audio ----------------
var Audio = {
    init:    function() { SFX.init(); },
    launch:  function() { SFX.tone(220, 0.08, "sawtooth", 0.4); },
    explode: function() { SFX.tone(90 + Math.random()*40, 0.25, "sawtooth", 0.7); },
    cityHit: function() { SFX.tone(60, 0.5, "sawtooth", 0.9); },
    siloHit: function() { SFX.tone(80, 0.4, "sawtooth", 0.8); },
    menu:    function() { SFX.tone(500, 0.05, "square", 0.3); },
    select:  function() { SFX.tone(700, 0.1, "square", 0.4); },
    waveEnd: function() { SFX.sequence([[523,0.12,"square",0.6],[659,0.12,"square",0.6],[784,0.16,"square",0.7]]); },
    gameOver:function() { SFX.sequence([[220,0.3,"sawtooth",0.6],[180,0.3,"sawtooth",0.6],[140,0.6,"sawtooth",0.7]]); },
};

// ---------------- Storage ----------------
var _store = Storage.create("missilecommand");
var MC_Storage = {
    get highScore() { return _store.get("highScore") || 0; },
    set highScore(v) { _store.set("highScore", v); },
    load: function() { _store.load({ highScore: 0 }); },
    save: function() { _store.save(); },
};

// ---------------- Game constants ----------------
var NUM_CITIES = 6;
var NUM_SILOS = 3;
var AMMO_PER_SILO = 10;
var GROUND_Y_FRAC = 0.88;   // ground line
var SILO_Y_FRAC = 0.85;     // silo top
var CITY_Y_FRAC = 0.88;
var MAX_EXPLOSION_R = 48;
var EXPLOSION_GROW = 140;   // px/sec
var EXPLOSION_SHRINK = 60;  // px/sec
var PLAYER_MISSILE_SPEED = 520; // px/sec
var BOMB_SCORE = 25;
var CITY_BONUS = 100;
var AMMO_BONUS = 5;

// ---------------- Game state ----------------
var state = {
    screen: "title",
    score: 0,
    wave: 1,
    cities: [],     // {x, y, alive}
    silos: [],      // {x, y, ammo, alive}
    enemies: [],    // {x,y,tx,ty,vx,vy,alive,split,splitAt}
    players: [],    // {x,y,tx,ty,vx,vy,alive}
    explosions: [], // {x,y,r,maxR,growing,alive,fromEnemy}
    mouse: {x: 0, y: 0},
    waveTimer: 0,
    spawnQueue: [], // pending enemy spawns (t, count)
    spawnCursor: 0,
    waveDuration: 0,
    waveEnemiesRemaining: 0,
    waveOverTimer: 0,
    menuSel: 0,
    menuItems: []
};

// ---------------- Level setup ----------------
function resetBattlefield() {
    var W = getW(), H = getH();
    state.cities = [];
    state.silos = [];

    // Layout: silo, 2 cities, silo, 2 cities, silo, 2 cities OR 3 silos spread out with 6 cities between
    // Classic arrangement: silo-cities-cities-silo-cities-cities-silo-cities-cities
    // Simpler: 3 silos at 1/6, 1/2, 5/6 ; cities at .15,.22,.3, then .55,.62,.7 of width
    var siloXs = [W * 0.08, W * 0.5, W * 0.92];
    var cityXs = [W*0.20, W*0.28, W*0.36, W*0.64, W*0.72, W*0.80];
    var groundY = H * GROUND_Y_FRAC;
    for (var i = 0; i < siloXs.length; i++) {
        state.silos.push({ x: siloXs[i], y: H * SILO_Y_FRAC, ammo: AMMO_PER_SILO, alive: true });
    }
    for (var j = 0; j < cityXs.length; j++) {
        state.cities.push({ x: cityXs[j], y: H * CITY_Y_FRAC, alive: true });
    }
    state.enemies.length = 0;
    state.players.length = 0;
    state.explosions.length = 0;
}

function startGame() {
    state.score = 0;
    state.wave = 1;
    for (var i = 0; i < NUM_CITIES; i++) {}
    resetBattlefield();
    // All cities start alive (resetBattlefield already creates alive ones)
    beginWave();
    setScreen("playing");
}

function beginWave() {
    // Refill ammo on surviving silos
    for (var i = 0; i < state.silos.length; i++) {
        if (state.silos[i].alive) state.silos[i].ammo = AMMO_PER_SILO;
    }
    // Build enemy spawn schedule
    var count = 10 + state.wave * 4;
    var duration = 28; // seconds
    state.spawnQueue = [];
    for (var k = 0; k < count; k++) {
        var t = Math.random() * duration * 0.85;
        state.spawnQueue.push(t);
    }
    state.spawnQueue.sort(function(a, b) { return a - b; });
    state.spawnCursor = 0;
    state.waveTimer = 0;
    state.waveDuration = duration;
    state.waveEnemiesRemaining = count;
    state.waveOverTimer = 0;
    state.enemies.length = 0;
    state.players.length = 0;
    state.explosions.length = 0;
}

function enemySpeed() {
    // pixels per second vertical equivalent; actual scalar along path
    var base = 40 + state.wave * 8;
    if (base > 140) base = 140;
    return base;
}

function spawnEnemy() {
    var W = getW(), H = getH();
    var x = Math.random() * W;
    // Pick random target: cities mostly, silos sometimes
    var targets = [];
    for (var i = 0; i < state.cities.length; i++) {
        if (state.cities[i].alive) targets.push({ x: state.cities[i].x, y: state.cities[i].y });
    }
    for (var j = 0; j < state.silos.length; j++) {
        if (state.silos[j].alive) targets.push({ x: state.silos[j].x, y: state.silos[j].y });
    }
    if (targets.length === 0) return;
    var tgt = targets[(Math.random() * targets.length) | 0];
    var dx = tgt.x - x, dy = tgt.y - 0;
    var len = Math.sqrt(dx*dx + dy*dy) || 1;
    var spd = enemySpeed();
    var splitChance = Math.min(0.05 + state.wave * 0.02, 0.35);
    var split = (state.wave >= 3 && Math.random() < splitChance);
    var splitAt = split ? (0.25 + Math.random() * 0.35) : -1; // fraction of vertical distance
    state.enemies.push({
        x: x, y: 0,
        sx: x, sy: 0, // start
        tx: tgt.x, ty: tgt.y,
        vx: dx / len * spd,
        vy: dy / len * spd,
        alive: true,
        split: split,
        splitAt: splitAt,
        hasSplit: false
    });
}

function splitEnemy(e) {
    var W = getW(), H = getH();
    for (var k = 0; k < 2; k++) {
        var targets = [];
        for (var i = 0; i < state.cities.length; i++) {
            if (state.cities[i].alive) targets.push({ x: state.cities[i].x, y: state.cities[i].y });
        }
        for (var j = 0; j < state.silos.length; j++) {
            if (state.silos[j].alive) targets.push({ x: state.silos[j].x, y: state.silos[j].y });
        }
        if (targets.length === 0) return;
        var tgt = targets[(Math.random() * targets.length) | 0];
        var dx = tgt.x - e.x, dy = tgt.y - e.y;
        var len = Math.sqrt(dx*dx + dy*dy) || 1;
        var spd = enemySpeed();
        state.enemies.push({
            x: e.x, y: e.y,
            sx: e.x, sy: e.y,
            tx: tgt.x, ty: tgt.y,
            vx: dx / len * spd,
            vy: dy / len * spd,
            alive: true,
            split: false,
            splitAt: -1,
            hasSplit: true
        });
    }
}

// ---------------- Player missile launch ----------------
function launchPlayerMissile(targetX, targetY) {
    // Find nearest silo with ammo
    var best = -1, bestD = Infinity;
    for (var i = 0; i < state.silos.length; i++) {
        var s = state.silos[i];
        if (!s.alive || s.ammo <= 0) continue;
        var dx = s.x - targetX, dy = s.y - targetY;
        var d = dx*dx + dy*dy;
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0) return false;
    var silo = state.silos[best];
    silo.ammo--;
    var dx = targetX - silo.x, dy = targetY - silo.y;
    var len = Math.sqrt(dx*dx + dy*dy) || 1;
    state.players.push({
        sx: silo.x, sy: silo.y,
        x: silo.x, y: silo.y,
        tx: targetX, ty: targetY,
        vx: dx / len * PLAYER_MISSILE_SPEED,
        vy: dy / len * PLAYER_MISSILE_SPEED,
        alive: true
    });
    Audio.launch();
    return true;
}

function createExplosion(x, y, fromEnemy) {
    state.explosions.push({
        x: x, y: y, r: 2, maxR: MAX_EXPLOSION_R,
        growing: true, alive: true, fromEnemy: !!fromEnemy
    });
    Audio.explode();
}

// ---------------- Update ----------------
function update(dt) {
    if (state.screen !== "playing") return;
    var W = getW(), H = getH();
    var ds = dt / 1000;

    state.waveTimer += ds;

    // Spawn enemies from queue
    while (state.spawnCursor < state.spawnQueue.length &&
           state.spawnQueue[state.spawnCursor] <= state.waveTimer) {
        spawnEnemy();
        state.spawnCursor++;
    }

    // Update enemies
    for (var i = 0; i < state.enemies.length; i++) {
        var e = state.enemies[i];
        if (!e.alive) continue;
        e.x += e.vx * ds;
        e.y += e.vy * ds;

        // Split logic
        if (e.split && !e.hasSplit) {
            var progY = (e.y) / (e.ty || 1);
            if (progY >= e.splitAt) {
                e.hasSplit = true;
                e.alive = false;
                splitEnemy(e);
                state.waveEnemiesRemaining += 1; // net +1 (two new, one consumed)
                continue;
            }
        }

        // Did it hit the ground?
        if (e.y >= e.ty || e.y >= H * GROUND_Y_FRAC) {
            e.alive = false;
            state.waveEnemiesRemaining--;
            // damage city or silo near impact
            var impactX = e.x;
            var cRange = 22;
            var hit = false;
            for (var ci = 0; ci < state.cities.length; ci++) {
                var c = state.cities[ci];
                if (c.alive && Math.abs(c.x - impactX) < cRange) {
                    c.alive = false; hit = true;
                    Audio.cityHit();
                    // small explosion visual
                    createExplosion(c.x, c.y, true);
                    break;
                }
            }
            if (!hit) {
                for (var si = 0; si < state.silos.length; si++) {
                    var s = state.silos[si];
                    if (s.alive && Math.abs(s.x - impactX) < 28 && Math.abs(e.y - s.y) < 40) {
                        s.alive = false; s.ammo = 0; hit = true;
                        Audio.siloHit();
                        createExplosion(s.x, s.y, true);
                        break;
                    }
                }
            }
            if (!hit) {
                // just ground impact
                createExplosion(e.x, H * GROUND_Y_FRAC, true);
            }
        }
    }

    // Update player missiles
    for (var p = 0; p < state.players.length; p++) {
        var pm = state.players[p];
        if (!pm.alive) continue;
        pm.x += pm.vx * ds;
        pm.y += pm.vy * ds;
        // Reached target?
        var dx = pm.tx - pm.x, dy = pm.ty - pm.y;
        // Check if we passed target: if sign of (tx-sx) vs (tx-x) flipped, done
        var reached = false;
        if (pm.vx >= 0 ? pm.x >= pm.tx : pm.x <= pm.tx) {
            if (pm.vy >= 0 ? pm.y >= pm.ty : pm.y <= pm.ty) {
                reached = true;
            }
        }
        // Or distance small
        if (dx*dx + dy*dy < 25) reached = true;
        if (reached) {
            pm.alive = false;
            createExplosion(pm.tx, pm.ty, false);
        }
    }

    // Update explosions
    for (var ex = 0; ex < state.explosions.length; ex++) {
        var xp = state.explosions[ex];
        if (!xp.alive) continue;
        if (xp.growing) {
            xp.r += EXPLOSION_GROW * ds;
            if (xp.r >= xp.maxR) { xp.r = xp.maxR; xp.growing = false; }
        } else {
            xp.r -= EXPLOSION_SHRINK * ds;
            if (xp.r <= 0) { xp.alive = false; xp.r = 0; }
        }
        // Hit check against enemies (only player explosions kill; enemy explosions also chain)
        if (xp.alive) {
            for (var ei = 0; ei < state.enemies.length; ei++) {
                var ee = state.enemies[ei];
                if (!ee.alive) continue;
                var ddx = ee.x - xp.x, ddy = ee.y - xp.y;
                if (ddx*ddx + ddy*ddy <= xp.r * xp.r) {
                    ee.alive = false;
                    state.waveEnemiesRemaining--;
                    if (!xp.fromEnemy) {
                        state.score += BOMB_SCORE;
                    }
                    // chain reaction
                    createExplosion(ee.x, ee.y, false);
                }
            }
        }
    }

    // Cull dead from arrays occasionally
    state.enemies = state.enemies.filter(function(e) { return e.alive; });
    state.players = state.players.filter(function(p) { return p.alive; });
    state.explosions = state.explosions.filter(function(x) { return x.alive; });

    // HUD update
    updateHud();

    // Wave end detection
    var allSpawned = state.spawnCursor >= state.spawnQueue.length;
    var cleared = allSpawned && state.enemies.length === 0 && state.players.length === 0 && state.explosions.length === 0;

    // Check for game over (no cities)
    var anyCity = false;
    for (var cc = 0; cc < state.cities.length; cc++) {
        if (state.cities[cc].alive) { anyCity = true; break; }
    }
    if (!anyCity) {
        // small delay for final explosions
        state.waveOverTimer += ds;
        if (state.waveOverTimer > 1.5) {
            endGame();
        }
        return;
    }

    if (cleared) {
        state.waveOverTimer += ds;
        if (state.waveOverTimer > 0.8) {
            completeWave();
        }
    }
}

function completeWave() {
    // Bonuses
    var surviving = 0;
    for (var i = 0; i < state.cities.length; i++) if (state.cities[i].alive) surviving++;
    var ammoLeft = 0;
    for (var j = 0; j < state.silos.length; j++) if (state.silos[j].alive) ammoLeft += state.silos[j].ammo;
    var cityBonus = surviving * CITY_BONUS;
    var ammoBonus = ammoLeft * AMMO_BONUS;
    state.score += cityBonus + ammoBonus;

    // Restore one destroyed city every 2 waves (classic MC style)
    if (state.wave % 2 === 0) {
        for (var c = 0; c < state.cities.length; c++) {
            if (!state.cities[c].alive) { state.cities[c].alive = true; break; }
        }
    }

    Audio.waveEnd();
    var stats = document.getElementById("wave-stats");
    if (stats) {
        stats.innerHTML =
            "Cities saved: " + surviving + " &times; " + CITY_BONUS + " = " + cityBonus + "<br>" +
            "Unused missiles: " + ammoLeft + " &times; " + AMMO_BONUS + " = " + ammoBonus + "<br>" +
            "Total score: <strong>" + state.score + "</strong>";
    }
    setScreen("wavecomplete");
}

function endGame() {
    if (state.score > MC_Storage.highScore) {
        MC_Storage.highScore = state.score;
        MC_Storage.save();
    }
    Audio.gameOver();
    var stats = document.getElementById("gameover-stats");
    if (stats) {
        stats.innerHTML = "Final score: <strong>" + state.score + "</strong><br>" +
                          "Wave reached: " + state.wave + "<br>" +
                          "High score: " + MC_Storage.highScore;
    }
    setScreen("gameover");
}

function updateHud() {
    var s = document.getElementById("hud-score");
    var w = document.getElementById("hud-wave");
    var h = document.getElementById("hud-hi");
    if (s) s.textContent = String(state.score);
    if (w) w.textContent = String(state.wave);
    if (h) h.textContent = String(MC_Storage.highScore);
}

// ---------------- Rendering ----------------
function draw() {
    var W = getW(), H = getH();
    ctx.fillStyle = "#05050c";
    ctx.fillRect(0, 0, W, H);

    // Stars
    drawStars(W, H);

    var groundY = H * GROUND_Y_FRAC;

    // Ground
    ctx.fillStyle = "#2a1a05";
    ctx.fillRect(0, groundY, W, H - groundY);
    ctx.strokeStyle = "#ff8000";
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(0, groundY);
    ctx.lineTo(W, groundY);
    ctx.stroke();

    // Cities
    for (var i = 0; i < state.cities.length; i++) {
        var c = state.cities[i];
        if (!c.alive) {
            // rubble
            ctx.fillStyle = "#442";
            ctx.fillRect(c.x - 18, groundY - 6, 36, 6);
            continue;
        }
        drawCity(c.x, groundY);
    }

    // Silos
    for (var s = 0; s < state.silos.length; s++) {
        var silo = state.silos[s];
        drawSilo(silo, groundY);
    }

    // Enemy missile trails + heads
    for (var ei = 0; ei < state.enemies.length; ei++) {
        var e = state.enemies[ei];
        if (!e.alive) continue;
        // trail from spawn origin to current
        ctx.strokeStyle = "rgba(255,80,80,0.9)";
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(e.sx, e.sy);
        ctx.lineTo(e.x, e.y);
        ctx.stroke();
        // head
        ctx.fillStyle = "#ffef7a";
        ctx.beginPath();
        ctx.arc(e.x, e.y, 2.5, 0, Math.PI * 2);
        ctx.fill();
    }

    // Player missile trails + heads
    for (var pi = 0; pi < state.players.length; pi++) {
        var p = state.players[pi];
        if (!p.alive) continue;
        ctx.strokeStyle = "rgba(120,220,255,0.95)";
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(p.sx, p.sy);
        ctx.lineTo(p.x, p.y);
        ctx.stroke();
        // mark target with small X
        ctx.strokeStyle = "rgba(120,220,255,0.5)";
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(p.tx - 4, p.ty - 4); ctx.lineTo(p.tx + 4, p.ty + 4);
        ctx.moveTo(p.tx + 4, p.ty - 4); ctx.lineTo(p.tx - 4, p.ty + 4);
        ctx.stroke();
        // head
        ctx.fillStyle = "#cfefff";
        ctx.beginPath();
        ctx.arc(p.x, p.y, 2.5, 0, Math.PI * 2);
        ctx.fill();
    }

    // Explosions
    for (var xi = 0; xi < state.explosions.length; xi++) {
        var xp = state.explosions[xi];
        if (!xp.alive) continue;
        var hue = xp.fromEnemy ? "#ff4040" : "#ffef7a";
        var outer = xp.fromEnemy ? "rgba(255,64,64,0.15)" : "rgba(255,220,120,0.2)";
        ctx.fillStyle = outer;
        ctx.beginPath();
        ctx.arc(xp.x, xp.y, xp.r, 0, Math.PI * 2);
        ctx.fill();
        ctx.strokeStyle = hue;
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.arc(xp.x, xp.y, xp.r * 0.7, 0, Math.PI * 2);
        ctx.stroke();
        ctx.fillStyle = hue;
        ctx.beginPath();
        ctx.arc(xp.x, xp.y, Math.max(1, xp.r * 0.3), 0, Math.PI * 2);
        ctx.fill();
    }

    // Crosshair
    if (state.screen === "playing") {
        drawCrosshair(state.mouse.x, state.mouse.y);
    }
}

var _stars = null;
function drawStars(W, H) {
    if (!_stars || _stars.w !== W || _stars.h !== H) {
        _stars = { w: W, h: H, pts: [] };
        for (var i = 0; i < 80; i++) {
            _stars.pts.push({
                x: Math.random() * W,
                y: Math.random() * H * GROUND_Y_FRAC,
                b: 0.3 + Math.random() * 0.7
            });
        }
    }
    for (var j = 0; j < _stars.pts.length; j++) {
        var s = _stars.pts[j];
        ctx.fillStyle = "rgba(255,255,255," + s.b.toFixed(2) + ")";
        ctx.fillRect(s.x, s.y, 1, 1);
    }
}

function drawCity(cx, gy) {
    ctx.fillStyle = "#6ad3ff";
    // four blocks of varying heights
    var heights = [10, 16, 12, 18, 14];
    var bw = 6;
    var total = heights.length * bw;
    var x0 = cx - total / 2;
    for (var i = 0; i < heights.length; i++) {
        var h = heights[i];
        ctx.fillRect(x0 + i * bw + 1, gy - h, bw - 2, h);
    }
    // glow outline
    ctx.strokeStyle = "rgba(106,211,255,0.4)";
    ctx.lineWidth = 1;
    ctx.strokeRect(x0, gy - 18, total, 18);
}

function drawSilo(silo, gy) {
    var x = silo.x;
    if (!silo.alive) {
        ctx.fillStyle = "#332";
        ctx.fillRect(x - 20, gy - 6, 40, 6);
        return;
    }
    // triangular base
    ctx.fillStyle = "#ffb060";
    ctx.beginPath();
    ctx.moveTo(x - 22, gy);
    ctx.lineTo(x + 22, gy);
    ctx.lineTo(x + 14, gy - 18);
    ctx.lineTo(x - 14, gy - 18);
    ctx.closePath();
    ctx.fill();
    // ammo dots
    ctx.fillStyle = "#ffef7a";
    var ammo = silo.ammo;
    // stack of dots in a triangle pattern, up to AMMO_PER_SILO
    var dotR = 2;
    var slots = [
        [ 0, -24],
        [-5, -20], [5, -20],
        [-10, -16], [0, -16], [10, -16],
        [-12, -12], [-4, -12], [4, -12], [12, -12]
    ];
    for (var i = 0; i < ammo && i < slots.length; i++) {
        var d = slots[i];
        ctx.beginPath();
        ctx.arc(x + d[0], gy + d[1], dotR, 0, Math.PI * 2);
        ctx.fill();
    }
}

function drawCrosshair(x, y) {
    ctx.strokeStyle = "rgba(180,255,180,0.8)";
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(x - 10, y); ctx.lineTo(x - 3, y);
    ctx.moveTo(x + 3, y);  ctx.lineTo(x + 10, y);
    ctx.moveTo(x, y - 10); ctx.lineTo(x, y - 3);
    ctx.moveTo(x, y + 3);  ctx.lineTo(x, y + 10);
    ctx.stroke();
}

// ---------------- Screens / menus ----------------
function setScreen(name) {
    state.screen = name;
    var screens = document.querySelectorAll(".screen");
    for (var i = 0; i < screens.length; i++) screens[i].classList.remove("active");
    var overlay = document.getElementById("overlay");
    var hud = document.getElementById("hud");

    if (name === "playing") {
        overlay.style.display = "none";
        if (hud) hud.style.display = "flex";
        updateHud();
    } else {
        overlay.style.display = "flex";
        if (hud) hud.style.display = "none";
        var target = document.getElementById("screen-" + name);
        if (target) target.classList.add("active");
        refreshTitleScreen();
        setupMenu(name);
    }
}

function refreshTitleScreen() {
    var t = document.getElementById("title-hi");
    if (t) t.textContent = String(MC_Storage.highScore);
}

function setupMenu(name) {
    var screen = document.getElementById("screen-" + name);
    if (!screen) { state.menuItems = []; return; }
    var items = screen.querySelectorAll(".menu-item");
    state.menuItems = [];
    for (var i = 0; i < items.length; i++) state.menuItems.push(items[i]);
    state.menuSel = 0;
    // Keep the one already marked selected if any
    for (var j = 0; j < state.menuItems.length; j++) {
        if (state.menuItems[j].classList.contains("selected")) { state.menuSel = j; break; }
    }
    updateMenuSelection();
}

function updateMenuSelection() {
    for (var i = 0; i < state.menuItems.length; i++) {
        if (i === state.menuSel) state.menuItems[i].classList.add("selected");
        else state.menuItems[i].classList.remove("selected");
    }
}

function activateMenu() {
    if (!state.menuItems.length) return;
    var item = state.menuItems[state.menuSel];
    var action = item.getAttribute("data-action");
    Audio.select();
    handleMenuAction(action);
}

function handleMenuAction(action) {
    if (state.screen === "title") {
        if (action === "play") startGame();
        else if (action === "howto") setScreen("howto");
    } else if (state.screen === "howto") {
        if (action === "back") setScreen("title");
    } else if (state.screen === "wavecomplete") {
        if (action === "continue") {
            state.wave++;
            beginWave();
            setScreen("playing");
        }
    } else if (state.screen === "gameover") {
        if (action === "restart") startGame();
        else if (action === "quit") setScreen("title");
    }
}

// ---------------- Input ----------------
function toCanvasCoords(ev) {
    var rect = canvas.getBoundingClientRect();
    var W = getW(), H = getH();
    var sx = W / rect.width;
    var sy = H / rect.height;
    return {
        x: (ev.clientX - rect.left) * sx,
        y: (ev.clientY - rect.top) * sy
    };
}

canvas.addEventListener("mousemove", function(ev) {
    var p = toCanvasCoords(ev);
    state.mouse.x = p.x; state.mouse.y = p.y;
});

canvas.addEventListener("mousedown", function(ev) {
    if (state.screen !== "playing") return;
    var p = toCanvasCoords(ev);
    state.mouse.x = p.x; state.mouse.y = p.y;
    // Don't let them target below ground level
    var H = getH();
    var y = Math.min(p.y, H * GROUND_Y_FRAC - 8);
    launchPlayerMissile(p.x, y);
});

// Input bindings (bro.settings-backed).
Input.init([
    { name: "primary", label: "Fire",      defaults: ["Mouse0", " "] },
    { name: "up",      label: "Menu Up",   defaults: ["w", "ArrowUp"] },
    { name: "down",    label: "Menu Down", defaults: ["s", "ArrowDown"] },
    { name: "confirm", label: "Confirm",   defaults: ["Enter"] },
    { name: "pause",   label: "Back",      defaults: ["Escape"] },
]);
Input.attach(window);

Input.onAction(function(action, phase) {
    if (phase !== "down" || !action) return;
    if (state.screen === "playing") return; // mouse handled separately
    if (!state.menuItems.length) return;
    if (action === "up") {
        state.menuSel = (state.menuSel - 1 + state.menuItems.length) % state.menuItems.length;
        updateMenuSelection(); Audio.menu();
    } else if (action === "down") {
        state.menuSel = (state.menuSel + 1) % state.menuItems.length;
        updateMenuSelection(); Audio.menu();
    } else if (action === "confirm") {
        activateMenu();
    } else if (action === "pause") {
        if (state.screen === "howto") setScreen("title");
    }
});

// Click on menu items
document.getElementById("overlay").addEventListener("click", function(ev) {
    var el = ev.target;
    while (el && el !== document.body) {
        if (el.classList && el.classList.contains("menu-item")) {
            for (var i = 0; i < state.menuItems.length; i++) {
                if (state.menuItems[i] === el) { state.menuSel = i; break; }
            }
            updateMenuSelection();
            activateMenu();
            return;
        }
        el = el.parentNode;
    }
});

// ---------------- Boot ----------------
MC_Storage.load();
Audio.init();
setScreen("title");

GameLoop.create({ tick: update, draw: draw }).start();

console.log("Missile Command loaded");
})();
