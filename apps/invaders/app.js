// app.js — Space Invaders clone for Bro
(function() {
"use strict";

// ---------- Canvas ----------
var canvas = document.getElementById("game");
var ctx = canvas.getContext("2d");
function getW() { return Canvas.w(ctx, 800); }
function getH() { return Canvas.h(ctx, 800); }

// ---------- Storage ----------
var store = Storage.create("invaders");
function loadHighScore() { store.load({ highScore: 0 }); return store.get("highScore") || 0; }
function saveHighScore(v) { store.set("highScore", v); store.save(); }

// ---------- Audio ----------
var Audio = {
    init: function() { SFX.init(); },
    shoot:      function() { SFX.tone(880, 0.08, "square", 0.4); },
    hit:        function() { SFX.tone(200, 0.12, "sawtooth", 0.6); },
    kill:       function() { SFX.tone(120, 0.18, "sawtooth", 0.7); },
    step:       function(n) { SFX.tone(90 + (n % 4) * 30, 0.08, "triangle", 0.5); },
    ufo:        function() { SFX.tone(660, 0.15, "square", 0.5); },
    die:        function() { SFX.sequence([[300,0.2,"sawtooth",0.6],[200,0.25,"sawtooth",0.6],[100,0.35,"sawtooth",0.6]]); },
    menuMove:   function() { SFX.tone(440, 0.03, "sine", 0.3); },
    menuSelect: function() { SFX.tone(660, 0.07, "square", 0.4); }
};

// ---------- Input ----------
Input.init([
    { name: "left",    label: "Left",    defaults: ["a", "ArrowLeft"] },
    { name: "right",   label: "Right",   defaults: ["d", "ArrowRight"] },
    { name: "primary", label: "Fire",    defaults: [" ", "Mouse0"] },
    { name: "up",      label: "Menu Up", defaults: ["w", "ArrowUp"] },
    { name: "down",    label: "Menu Dn", defaults: ["s", "ArrowDown"] },
    { name: "confirm", label: "Confirm", defaults: ["Enter"] },
    { name: "pause",   label: "Menu",    defaults: ["Escape"] },
]);
Input.attach(window);

// ---------- Game constants ----------
var ROWS = 5;
var COLS = 11;
var ENEMY_W = 32;
var ENEMY_H = 22;
var ENEMY_HGAP = 14;
var ENEMY_VGAP = 14;
var ENEMY_STEP_Y = 18;

var PLAYER_W = 42;
var PLAYER_H = 18;
var PLAYER_SPEED = 320; // px/s
var PLAYER_BULLET_SPEED = 560;
var ENEMY_BULLET_SPEED = 240;

var SHIELD_COUNT = 4;
var SHIELD_W = 72;
var SHIELD_H = 44;
var SHIELD_CELL = 4; // pixel size
var SHIELD_COLS = SHIELD_W / SHIELD_CELL; // 18
var SHIELD_ROWS = SHIELD_H / SHIELD_CELL; // 11

// ---------- State ----------
var state = {
    mode: "title", // "title", "playing", "dying", "gameover"
    score: 0,
    highScore: loadHighScore(),
    wave: 1,
    lives: 3,
    player: { x: 0, y: 0, alive: true, dieTimer: 0 },
    bullet: null, // {x,y,vy}
    enemies: [], // {col,row,x,y,alive,type}
    enemyDir: 1,
    enemyStepInterval: 700, // ms
    enemyStepTimer: 0,
    enemyStepPhase: 0,
    enemyBullets: [],
    enemyFireTimer: 0,
    enemyFireInterval: 900,
    ufo: null, // {x,y,vx,points}
    ufoTimer: 0,
    shields: [], // [{x,y,grid:[ [bool...], ... ]}]
    particles: [],
    stars: [],
    waveStart: 0,
    menuIndex: 0,
    activeScreen: "title"
};

// ---------- Stars (bg) ----------
function initStars(W, H) {
    state.stars = [];
    for (var i = 0; i < 60; i++) {
        state.stars.push({
            x: Math.random() * W,
            y: Math.random() * H,
            a: 0.2 + Math.random() * 0.6,
            s: 0.3 + Math.random() * 1.2
        });
    }
}

// ---------- Shields ----------
function buildShield(x, y) {
    var grid = [];
    for (var r = 0; r < SHIELD_ROWS; r++) {
        var row = [];
        for (var c = 0; c < SHIELD_COLS; c++) {
            var on = true;
            // Carve out arch bottom
            var cx = SHIELD_COLS / 2;
            // Notch at bottom-center
            if (r >= SHIELD_ROWS - 4) {
                var distFromCenter = Math.abs(c - cx + 0.5);
                if (distFromCenter < 3 - (SHIELD_ROWS - 1 - r) * 0.8) on = false;
            }
            // Rounded top corners
            if (r < 2) {
                if (c < 1 || c > SHIELD_COLS - 2) on = false;
            }
            row.push(on);
        }
        grid.push(row);
    }
    return { x: x, y: y, grid: grid };
}

function initShields(W, H) {
    state.shields = [];
    var baseY = H - 170;
    var span = W - 120;
    var startX = 60;
    var gap = (span - SHIELD_W * SHIELD_COUNT) / (SHIELD_COUNT - 1);
    for (var i = 0; i < SHIELD_COUNT; i++) {
        var x = startX + i * (SHIELD_W + gap);
        state.shields.push(buildShield(x, baseY));
    }
}

// Returns true if hit
function shieldHit(px, py, radius) {
    radius = radius || 0;
    for (var i = 0; i < state.shields.length; i++) {
        var s = state.shields[i];
        if (px < s.x - radius || px > s.x + SHIELD_W + radius) continue;
        if (py < s.y - radius || py > s.y + SHIELD_H + radius) continue;
        var lx = px - s.x;
        var ly = py - s.y;
        var cc = Math.floor(lx / SHIELD_CELL);
        var cr = Math.floor(ly / SHIELD_CELL);
        if (cr < 0 || cr >= SHIELD_ROWS || cc < 0 || cc >= SHIELD_COLS) continue;
        if (s.grid[cr][cc]) {
            // Carve small crater
            var cw = 2, ch = 2;
            for (var dr = -ch; dr <= ch; dr++) {
                for (var dc = -cw; dc <= cw; dc++) {
                    var rr = cr + dr, ccc = cc + dc;
                    if (rr >= 0 && rr < SHIELD_ROWS && ccc >= 0 && ccc < SHIELD_COLS) {
                        if (Math.abs(dr) + Math.abs(dc) <= 3) s.grid[rr][ccc] = false;
                    }
                }
            }
            return true;
        }
    }
    return false;
}

// ---------- Enemies ----------
function initEnemies(W, H) {
    state.enemies = [];
    var gridW = COLS * ENEMY_W + (COLS - 1) * ENEMY_HGAP;
    var startX = (W - gridW) / 2;
    var startY = 100 + Math.min(60, (state.wave - 1) * 12);
    for (var r = 0; r < ROWS; r++) {
        for (var c = 0; c < COLS; c++) {
            // type: 0 = back rows (squid, 30pts), 1 = mid (crab, 20), 2 = front (octopus, 10)
            var type;
            if (r === 0) type = 0;
            else if (r <= 2) type = 1;
            else type = 2;
            state.enemies.push({
                col: c, row: r,
                x: startX + c * (ENEMY_W + ENEMY_HGAP),
                y: startY + r * (ENEMY_H + ENEMY_VGAP),
                alive: true,
                type: type
            });
        }
    }
    state.enemyDir = 1;
    state.enemyStepTimer = 0;
    state.enemyStepPhase = 0;
    state.enemyBullets = [];
    state.enemyFireTimer = 0;
    state.enemyFireInterval = Math.max(280, 950 - (state.wave - 1) * 80);
    recomputeStepInterval();
}

function recomputeStepInterval() {
    var alive = 0;
    for (var i = 0; i < state.enemies.length; i++) if (state.enemies[i].alive) alive++;
    var total = ROWS * COLS;
    // Start slow, ramp to fast as fewer remain. Also waves speed up.
    var base = 760 - (state.wave - 1) * 60;
    if (base < 220) base = 220;
    var frac = alive / total;
    var ms = base * (0.15 + frac * 0.85);
    if (ms < 40) ms = 40;
    state.enemyStepInterval = ms;
}

function enemyStep(W, H) {
    var moveDown = false;
    var leftMost = Infinity, rightMost = -Infinity;
    for (var i = 0; i < state.enemies.length; i++) {
        var e = state.enemies[i];
        if (!e.alive) continue;
        if (e.x < leftMost) leftMost = e.x;
        if (e.x + ENEMY_W > rightMost) rightMost = e.x + ENEMY_W;
    }
    if (!isFinite(leftMost)) return;
    var dx = 10 + (state.wave - 1) * 1;
    var dir = state.enemyDir;
    if (dir > 0 && rightMost + dx > W - 20) { moveDown = true; dir = -1; }
    else if (dir < 0 && leftMost - dx < 20) { moveDown = true; dir = 1; }
    for (var j = 0; j < state.enemies.length; j++) {
        var en = state.enemies[j];
        if (!en.alive) continue;
        if (moveDown) en.y += ENEMY_STEP_Y;
        else en.x += dir * dx;
    }
    state.enemyDir = dir;
    state.enemyStepPhase = 1 - state.enemyStepPhase;
    Audio.step(state.enemyStepPhase);

    // Check if reached bottom
    var playerTop = state.player.y;
    for (var k = 0; k < state.enemies.length; k++) {
        var ek = state.enemies[k];
        if (!ek.alive) continue;
        if (ek.y + ENEMY_H >= playerTop) {
            state.lives = 0;
            triggerPlayerDeath();
            return;
        }
    }
}

function enemyFire() {
    // Pick a random column that has a living enemy, shoot from the lowest in that column
    var cols = {};
    for (var i = 0; i < state.enemies.length; i++) {
        var e = state.enemies[i];
        if (!e.alive) continue;
        if (!cols[e.col] || e.y > cols[e.col].y) cols[e.col] = e;
    }
    var list = [];
    for (var k in cols) list.push(cols[k]);
    if (list.length === 0) return;
    var pick = list[Math.floor(Math.random() * list.length)];
    state.enemyBullets.push({
        x: pick.x + ENEMY_W / 2,
        y: pick.y + ENEMY_H,
        vy: ENEMY_BULLET_SPEED + (state.wave - 1) * 10
    });
}

// ---------- UFO ----------
function maybeSpawnUFO(W) {
    if (state.ufo) return;
    state.ufoTimer -= 1;
    if (state.ufoTimer > 0) return;
    // Schedule next spawn
    state.ufoTimer = 15000 + Math.random() * 10000;
    var leftToRight = Math.random() < 0.5;
    state.ufo = {
        x: leftToRight ? -30 : W + 30,
        y: 70,
        vx: leftToRight ? 140 : -140,
        points: [50, 100, 150, 300][Math.floor(Math.random() * 4)]
    };
    Audio.ufo();
}

// ---------- Player ----------
function resetPlayer(W, H) {
    state.player.x = W / 2 - PLAYER_W / 2;
    state.player.y = H - 70;
    state.player.alive = true;
    state.player.dieTimer = 0;
    state.bullet = null;
}

function triggerPlayerDeath() {
    if (!state.player.alive) return;
    state.player.alive = false;
    state.player.dieTimer = 1200;
    state.mode = "dying";
    Audio.die();
    spawnExplosion(state.player.x + PLAYER_W / 2, state.player.y + PLAYER_H / 2, "#fff", 30);
}

// ---------- Particles ----------
function spawnExplosion(x, y, color, count) {
    count = count || 12;
    for (var i = 0; i < count; i++) {
        var ang = Math.random() * Math.PI * 2;
        var spd = 40 + Math.random() * 140;
        state.particles.push({
            x: x, y: y,
            vx: Math.cos(ang) * spd,
            vy: Math.sin(ang) * spd,
            life: 400 + Math.random() * 300,
            age: 0,
            color: color || "#fff"
        });
    }
}

// ---------- Game flow ----------
function startGame() {
    var W = getW(), H = getH();
    state.score = 0;
    state.wave = 1;
    state.lives = 3;
    state.ufo = null;
    state.ufoTimer = 12000 + Math.random() * 8000;
    state.particles = [];
    initStars(W, H);
    initShields(W, H);
    initEnemies(W, H);
    resetPlayer(W, H);
    state.mode = "playing";
    updateHUD();
}

function nextWave() {
    var W = getW(), H = getH();
    state.wave += 1;
    state.bullet = null;
    state.enemyBullets = [];
    initShields(W, H);
    initEnemies(W, H);
    resetPlayer(W, H);
    state.mode = "playing";
    updateHUD();
}

function gameOver() {
    if (state.score > state.highScore) {
        state.highScore = state.score;
        saveHighScore(state.highScore);
    }
    state.mode = "gameover";
    var stats = document.getElementById("gameover-stats");
    if (stats) {
        stats.innerHTML = "SCORE: <strong>" + state.score + "</strong><br>WAVE: " +
            state.wave + "<br>HIGH: <strong>" + state.highScore + "</strong>";
    }
    showOverlay("gameover");
    state.menuIndex = 0;
    updateSelection();
}

// ---------- HUD ----------
function updateHUD() {
    var s = document.getElementById("hud-score"); if (s) s.textContent = String(state.score);
    var hi = document.getElementById("hud-hiscore"); if (hi) hi.textContent = String(state.highScore);
    var w = document.getElementById("hud-wave"); if (w) w.textContent = String(state.wave);
    var l = document.getElementById("hud-lives"); if (l) l.textContent = String(state.lives);
}

// ---------- Overlay ----------
function showOverlay(screenId) {
    var overlay = document.getElementById("overlay");
    var children = overlay.children;
    for (var i = 0; i < children.length; i++) children[i].style.display = "none";
    var el = document.getElementById("screen-" + screenId);
    if (el) el.style.display = "block";
    overlay.style.display = "block";
    var hud = document.getElementById("hud");
    if (hud) hud.style.display = "none";
    state.activeScreen = screenId;
    state.menuIndex = 0;
    updateSelection();
}

function hideOverlay() {
    var overlay = document.getElementById("overlay");
    if (overlay) overlay.style.display = "none";
    var hud = document.getElementById("hud");
    if (hud) hud.style.display = "flex";
}

function getMenuItems() {
    var el = document.getElementById("screen-" + state.activeScreen);
    if (!el) return [];
    var items = [];
    var nodes = el.querySelectorAll(".menu-item");
    for (var i = 0; i < nodes.length; i++) items.push(nodes[i]);
    return items;
}

function updateSelection() {
    var items = getMenuItems();
    for (var i = 0; i < items.length; i++) {
        items[i].className = (i === state.menuIndex) ? "menu-item selected" : "menu-item";
    }
}

(function attachMenuMouse() {
    var overlay = document.getElementById("overlay");
    if (!overlay) return;
    function findMenuItem(t) {
        while (t && t !== overlay) {
            if (t.className && t.className.indexOf("menu-item") !== -1 &&
                t.className.indexOf("menu-items") === -1) return t;
            t = t.parentNode;
        }
        return null;
    }
    overlay.addEventListener("mousemove", function(e) {
        if (!state.activeScreen) return;
        var t = findMenuItem(e.target);
        if (!t) return;
        var items = getMenuItems();
        for (var i = 0; i < items.length; i++) {
            if (items[i] === t) {
                if (state.menuIndex !== i) {
                    state.menuIndex = i;
                    updateSelection();
                    Audio.menuMove();
                }
                return;
            }
        }
    });
    overlay.addEventListener("click", function(e) {
        if (!state.activeScreen) return;
        var t = findMenuItem(e.target);
        if (!t) return;
        var items = getMenuItems();
        for (var i = 0; i < items.length; i++) {
            if (items[i] === t) {
                state.menuIndex = i;
                updateSelection();
                activateMenuItem();
                return;
            }
        }
    });
})();

function activateMenuItem() {
    var items = getMenuItems();
    var item = items[state.menuIndex];
    if (!item) return;
    var action = item.getAttribute("data-action");
    Audio.menuSelect();
    handleMenuAction(action);
}

function handleMenuAction(action) {
    if (action === "play") { hideOverlay(); startGame(); }
    else if (action === "restart") { hideOverlay(); startGame(); }
    else if (action === "quit") { goToTitle(); }
    else if (action === "back") { goToTitle(); }
    else if (action === "howtoplay") { showOverlay("howtoplay"); }
    else if (action === "highscores") {
        var el = document.getElementById("hs-list");
        if (el) el.innerHTML = state.highScore > 0
            ? "<strong>" + state.highScore + "</strong>"
            : "No score yet";
        showOverlay("highscores");
    }
}

function goToTitle() {
    state.mode = "title";
    showOverlay("title");
}

// ---------- Input routing ----------
Input.onAction(function(action, phase) {
    if (!action) return;
    var overlay = document.getElementById("overlay");
    var overlayOpen = overlay && overlay.style.display !== "none";
    if (overlayOpen) {
        if (phase !== "down") return;
        if (action === "up") {
            var items = getMenuItems();
            state.menuIndex = (state.menuIndex - 1 + items.length) % items.length;
            updateSelection(); Audio.menuMove();
        } else if (action === "down") {
            var items2 = getMenuItems();
            state.menuIndex = (state.menuIndex + 1) % items2.length;
            updateSelection(); Audio.menuMove();
        } else if (action === "confirm") {
            activateMenuItem();
        } else if (action === "pause") {
            goToTitle();
        }
        return;
    }
    if (state.mode === "playing" && phase === "down") {
        if (action === "primary") fireBullet();
        else if (action === "pause") goToTitle();
    }
});

function fireBullet() {
    if (!state.player.alive) return;
    if (state.bullet) return; // only one at a time
    state.bullet = {
        x: state.player.x + PLAYER_W / 2,
        y: state.player.y,
        vy: -PLAYER_BULLET_SPEED
    };
    Audio.shoot();
}

// ---------- Update ----------
function update(dt, W, H) {
    // Stars twinkle
    for (var si = 0; si < state.stars.length; si++) {
        state.stars[si].a += (Math.random() - 0.5) * 0.03;
        if (state.stars[si].a < 0.15) state.stars[si].a = 0.15;
        if (state.stars[si].a > 0.9) state.stars[si].a = 0.9;
    }

    // Particles
    for (var pi = state.particles.length - 1; pi >= 0; pi--) {
        var p = state.particles[pi];
        p.age += dt;
        p.x += p.vx * dt / 1000;
        p.y += p.vy * dt / 1000;
        p.vx *= 0.96;
        p.vy *= 0.96;
        if (p.age >= p.life) state.particles.splice(pi, 1);
    }

    if (state.mode === "dying") {
        state.player.dieTimer -= dt;
        if (state.player.dieTimer <= 0) {
            state.lives -= 1;
            updateHUD();
            if (state.lives <= 0) { gameOver(); return; }
            resetPlayer(W, H);
            state.mode = "playing";
        }
        return;
    }
    if (state.mode !== "playing") return;

    // Player movement
    var pSpeed = PLAYER_SPEED;
    if (Input.down("left"))  state.player.x -= pSpeed * dt / 1000;
    if (Input.down("right")) state.player.x += pSpeed * dt / 1000;
    if (state.player.x < 20) state.player.x = 20;
    if (state.player.x + PLAYER_W > W - 20) state.player.x = W - 20 - PLAYER_W;

    // Player bullet
    if (state.bullet) {
        state.bullet.y += state.bullet.vy * dt / 1000;
        var bx = state.bullet.x, by = state.bullet.y;
        if (by < 0) state.bullet = null;
        else if (shieldHit(bx, by, 0)) { state.bullet = null; Audio.hit(); spawnExplosion(bx, by, "#4fff6a", 5); }
        else {
            // Check UFO
            if (state.ufo && bx >= state.ufo.x && bx <= state.ufo.x + 40 &&
                by >= state.ufo.y && by <= state.ufo.y + 16) {
                state.score += state.ufo.points;
                spawnExplosion(state.ufo.x + 20, state.ufo.y + 8, "#ff5080", 20);
                Audio.kill();
                state.ufo = null;
                state.bullet = null;
                updateHUD();
            } else {
                // Check enemies
                for (var ei = 0; ei < state.enemies.length; ei++) {
                    var en = state.enemies[ei];
                    if (!en.alive) continue;
                    if (bx >= en.x && bx <= en.x + ENEMY_W && by >= en.y && by <= en.y + ENEMY_H) {
                        en.alive = false;
                        var pts = [30, 20, 10][en.type];
                        state.score += pts;
                        spawnExplosion(en.x + ENEMY_W/2, en.y + ENEMY_H/2, "#4fff6a", 14);
                        Audio.kill();
                        state.bullet = null;
                        recomputeStepInterval();
                        updateHUD();
                        break;
                    }
                }
            }
        }
    }

    // Enemy bullets
    for (var bi = state.enemyBullets.length - 1; bi >= 0; bi--) {
        var eb = state.enemyBullets[bi];
        eb.y += eb.vy * dt / 1000;
        if (eb.y > H) { state.enemyBullets.splice(bi, 1); continue; }
        if (shieldHit(eb.x, eb.y, 0)) {
            spawnExplosion(eb.x, eb.y, "#ff8040", 4);
            state.enemyBullets.splice(bi, 1);
            continue;
        }
        // Cancel if hits player bullet
        if (state.bullet) {
            var dx = eb.x - state.bullet.x, dy = eb.y - state.bullet.y;
            if (dx*dx + dy*dy < 64) {
                state.bullet = null;
                state.enemyBullets.splice(bi, 1);
                spawnExplosion(eb.x, eb.y, "#fff", 6);
                continue;
            }
        }
        // Player hit
        if (state.player.alive &&
            eb.x >= state.player.x && eb.x <= state.player.x + PLAYER_W &&
            eb.y >= state.player.y && eb.y <= state.player.y + PLAYER_H) {
            state.enemyBullets.splice(bi, 1);
            triggerPlayerDeath();
            return;
        }
    }

    // Enemy step timer
    state.enemyStepTimer += dt;
    if (state.enemyStepTimer >= state.enemyStepInterval) {
        state.enemyStepTimer = 0;
        enemyStep(W, H);
    }

    // Enemy fire
    state.enemyFireTimer += dt;
    if (state.enemyFireTimer >= state.enemyFireInterval) {
        state.enemyFireTimer = 0;
        state.enemyFireInterval = 400 + Math.random() * (900 - (state.wave - 1) * 40);
        if (state.enemyFireInterval < 200) state.enemyFireInterval = 200;
        if (state.enemyBullets.length < 3) enemyFire();
    }

    // UFO
    if (state.ufo) {
        state.ufo.x += state.ufo.vx * dt / 1000;
        if ((state.ufo.vx > 0 && state.ufo.x > W + 40) ||
            (state.ufo.vx < 0 && state.ufo.x < -40)) {
            state.ufo = null;
        }
    } else {
        state.ufoTimer -= dt;
        if (state.ufoTimer <= 0) maybeSpawnUFO(W);
    }

    // Wave cleared?
    var anyAlive = false;
    for (var ci = 0; ci < state.enemies.length; ci++) {
        if (state.enemies[ci].alive) { anyAlive = true; break; }
    }
    if (!anyAlive) {
        nextWave();
    }
}

// ---------- Drawing ----------
function drawStars(ctx, W, H) {
    for (var i = 0; i < state.stars.length; i++) {
        var s = state.stars[i];
        ctx.fillStyle = "rgba(255,255,255," + s.a.toFixed(2) + ")";
        ctx.fillRect(s.x, s.y, s.s, s.s);
    }
}

function drawPlayer(ctx) {
    if (!state.player.alive && state.mode === "dying") {
        // Flicker
        var t = Math.floor(state.player.dieTimer / 60);
        if (t % 2 === 0) return;
        ctx.fillStyle = "#ff5050";
    } else {
        ctx.fillStyle = "#ffffff";
    }
    var x = state.player.x, y = state.player.y;
    // base
    ctx.fillRect(x, y + 10, PLAYER_W, 8);
    // mid
    ctx.fillRect(x + 6, y + 4, PLAYER_W - 12, 8);
    // barrel
    ctx.fillRect(x + PLAYER_W/2 - 2, y - 2, 4, 8);
}

// Invader drawn as pixel grid. Two-frame animation via phase.
var INV_PIXELS = {
    // type 2 (front, 10pts) — "octopus" 12x8
    2: [
        "001111110000",
        "011111111000",
        "111111111100",
        "110110011010",
        "111111111110",
        "001100110010",
        "010000001010",
        "001100110000"
    ],
    // type 1 (mid, 20pts) — "crab"
    1: [
        "000110011000",
        "001111111100",
        "011111111110",
        "110110011011",
        "111111111111",
        "010111111010",
        "100100001010",
        "010000001010"
    ],
    // type 0 (back, 30pts) — "squid"
    0: [
        "000011110000",
        "000111111000",
        "001111111100",
        "010110011010",
        "011111111110",
        "001101011010",
        "010100101010",
        "001000001000"
    ]
};
// Alternate frames for subtle animation
var INV_PIXELS_ALT = {
    2: [
        "001111110000",
        "011111111000",
        "111111111100",
        "110110011011",
        "111111111110",
        "011011011010",
        "110000000010",
        "001100110000"
    ],
    1: [
        "000110011000",
        "001111111100",
        "011111111110",
        "110110011010",
        "111111111110",
        "010111111010",
        "010100000100",
        "001010010100"
    ],
    0: [
        "000011110000",
        "000111111000",
        "001111111100",
        "010110011010",
        "011111111110",
        "010110101010",
        "100000010001",
        "010000000100"
    ]
};

function drawInvader(ctx, e, phase) {
    var frames = phase ? INV_PIXELS_ALT : INV_PIXELS;
    var grid = frames[e.type];
    var cellW = ENEMY_W / 12;
    var cellH = ENEMY_H / 8;
    ctx.fillStyle = e.type === 0 ? "#8fff8f" : (e.type === 1 ? "#5fff6a" : "#3fdd4a");
    for (var r = 0; r < 8; r++) {
        var row = grid[r];
        for (var c = 0; c < 12; c++) {
            if (row.charCodeAt(c) === 49) { // '1'
                ctx.fillRect(e.x + c * cellW, e.y + r * cellH, cellW + 0.5, cellH + 0.5);
            }
        }
    }
}

function drawShields(ctx) {
    ctx.fillStyle = "#6fff8a";
    for (var i = 0; i < state.shields.length; i++) {
        var s = state.shields[i];
        for (var r = 0; r < SHIELD_ROWS; r++) {
            for (var c = 0; c < SHIELD_COLS; c++) {
                if (s.grid[r][c]) {
                    ctx.fillRect(s.x + c * SHIELD_CELL, s.y + r * SHIELD_CELL, SHIELD_CELL, SHIELD_CELL);
                }
            }
        }
    }
}

function drawBullets(ctx) {
    if (state.bullet) {
        ctx.fillStyle = "#ffffff";
        ctx.fillRect(state.bullet.x - 1, state.bullet.y - 6, 2, 10);
    }
    ctx.fillStyle = "#ffcc40";
    for (var i = 0; i < state.enemyBullets.length; i++) {
        var b = state.enemyBullets[i];
        ctx.fillRect(b.x - 1, b.y - 4, 2, 8);
    }
}

function drawUFO(ctx) {
    if (!state.ufo) return;
    var x = state.ufo.x, y = state.ufo.y;
    ctx.fillStyle = "#ff5080";
    ctx.fillRect(x + 4, y + 6, 32, 6);
    ctx.fillRect(x + 8, y + 2, 24, 4);
    ctx.fillRect(x, y + 10, 40, 4);
    ctx.fillStyle = "#ffa0c0";
    ctx.fillRect(x + 14, y + 4, 4, 2);
    ctx.fillRect(x + 22, y + 4, 4, 2);
}

function drawParticles(ctx) {
    for (var i = 0; i < state.particles.length; i++) {
        var p = state.particles[i];
        var a = 1 - (p.age / p.life);
        if (a < 0) a = 0;
        ctx.globalAlpha = a;
        ctx.fillStyle = p.color;
        ctx.fillRect(p.x - 1.5, p.y - 1.5, 3, 3);
    }
    ctx.globalAlpha = 1;
}

function drawGround(ctx, W, H) {
    ctx.fillStyle = "#4fff6a";
    ctx.fillRect(20, H - 40, W - 40, 2);
}

function draw(ctx, W, H) {
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, W, H);
    drawStars(ctx, W, H);

    if (state.mode === "title") return;

    drawGround(ctx, W, H);
    drawShields(ctx);

    for (var i = 0; i < state.enemies.length; i++) {
        if (state.enemies[i].alive) drawInvader(ctx, state.enemies[i], state.enemyStepPhase);
    }

    drawUFO(ctx);
    drawBullets(ctx);

    if (state.player.alive || state.mode === "dying") drawPlayer(ctx);

    drawParticles(ctx);
}

// ---------- Boot ----------
Audio.init();
initStars(getW(), getH());
updateHUD();
showOverlay("title");
GameLoop.create({
    tick: function(dt) { update(dt, getW(), getH()); },
    draw: function() {
        var W = getW(), H = getH();
        ctx.clearRect(0, 0, W, H);
        draw(ctx, W, H);
    },
}).start();

console.log("Invaders loaded!");
})();
