// game.js — Asteroids game world: ship, asteroids, bullets, physics, rendering
var A = A || {};

A.Game = (function() {
    "use strict";

    // --- Constants ---
    var SHIP_ROT_SPEED = 0.005;       // radians / ms
    var SHIP_THRUST    = 0.00018;     // units / ms^2 (px / ms^2)
    var SHIP_MAX_SPEED = 0.5;         // px / ms
    var SHIP_DRAG      = 0.9995;      // per ms (very light)
    var SHIP_RADIUS    = 12;
    var BULLET_SPEED   = 0.6;         // px / ms
    var BULLET_LIFE    = 900;         // ms
    var BULLET_COOLDOWN = 160;        // ms between shots
    var MAX_BULLETS    = 5;
    var RESPAWN_DELAY  = 1200;        // ms before respawn after death
    var INVULN_TIME    = 2500;        // ms of invulnerability after respawn
    var EXTRA_LIFE_AT  = 10000;       // award every 10k

    var AST_SIZES = {
        large:  { r: 42, score: 20, next: "medium" },
        medium: { r: 22, score: 50, next: "small"  },
        small:  { r: 12, score: 100, next: null    }
    };

    // Asteroid base speeds (px/ms); larger = slower
    var AST_SPEEDS = { large: 0.04, medium: 0.07, small: 0.1 };

    // --- Ship shape (triangle points, in local space, nose at +X) ---
    var SHIP_SHAPE = [
        { x: 14, y: 0 },
        { x: -10, y: -9 },
        { x: -6,  y: 0 },
        { x: -10, y: 9 }
    ];

    // --- Utility ---
    function wrap(v, max) {
        if (v < 0) return v + max;
        if (v >= max) return v - max;
        return v;
    }

    function distSq(ax, ay, bx, by, W, H) {
        // shortest distance considering wrap
        var dx = Math.abs(ax - bx);
        var dy = Math.abs(ay - by);
        if (dx > W * 0.5) dx = W - dx;
        if (dy > H * 0.5) dy = H - dy;
        return dx*dx + dy*dy;
    }

    function makeAsteroidShape(radius) {
        var pts = [];
        var n = 10 + Math.floor(Math.random() * 4);
        for (var i = 0; i < n; i++) {
            var a = (i / n) * Math.PI * 2;
            var r = radius * (0.75 + Math.random() * 0.45);
            pts.push({ x: Math.cos(a) * r, y: Math.sin(a) * r });
        }
        return pts;
    }

    function createAsteroid(x, y, size, W, H) {
        var info = AST_SIZES[size];
        var angle = Math.random() * Math.PI * 2;
        var speed = AST_SPEEDS[size] * (0.6 + Math.random() * 0.8);
        return {
            x: x, y: y,
            vx: Math.cos(angle) * speed,
            vy: Math.sin(angle) * speed,
            rot: 0,
            rotSpeed: (Math.random() - 0.5) * 0.0015,
            radius: info.r,
            size: size,
            shape: makeAsteroidShape(info.r)
        };
    }

    // --- State ---
    var state = null;

    function newState() {
        return {
            W: 900, H: 800,
            ship: null,
            asteroids: [],
            bullets: [],
            score: 0,
            lives: 3,
            wave: 1,
            paused: false,
            gameOver: false,
            respawnTimer: 0,
            invulnTimer: 0,
            cooldown: 0,
            nextExtraLife: EXTRA_LIFE_AT,
            keys: {},
            mouse: { x: 0, y: 0, held: false },
            running: false
        };
    }

    function setMouse(x, y, held) {
        if (!state) return;
        if (x !== undefined) state.mouse.x = x;
        if (y !== undefined) state.mouse.y = y;
        if (held !== undefined) state.mouse.held = !!held;
    }

    function makeShip(W, H) {
        return {
            x: W / 2, y: H / 2,
            vx: 0, vy: 0,
            angle: -Math.PI / 2,  // pointing up
            thrusting: false,
            alive: true
        };
    }

    function startGame(W, H) {
        state = newState();
        state.W = W; state.H = H;
        state.ship = makeShip(W, H);
        state.invulnTimer = INVULN_TIME;
        state.running = true;
        spawnWave(1);
    }

    function spawnWave(waveNum) {
        state.wave = waveNum;
        state.asteroids.length = 0;
        var count = 3 + waveNum;
        if (count > 10) count = 10;
        var W = state.W, H = state.H;
        var cx = state.ship ? state.ship.x : W/2;
        var cy = state.ship ? state.ship.y : H/2;
        var safe = 180;
        for (var i = 0; i < count; i++) {
            var x, y, tries = 0;
            do {
                x = Math.random() * W;
                y = Math.random() * H;
                tries++;
            } while (tries < 20 && distSq(x, y, cx, cy, W, H) < safe*safe);
            state.asteroids.push(createAsteroid(x, y, "large", W, H));
        }
        if (A.Audio) A.Audio.sfxWave();
    }

    function fireBullet() {
        if (!state.ship || !state.ship.alive) return;
        if (state.bullets.length >= MAX_BULLETS) return;
        if (state.cooldown > 0) return;
        var s = state.ship;
        var nx = Math.cos(s.angle);
        var ny = Math.sin(s.angle);
        state.bullets.push({
            x: s.x + nx * 14,
            y: s.y + ny * 14,
            vx: nx * BULLET_SPEED + s.vx,
            vy: ny * BULLET_SPEED + s.vy,
            life: BULLET_LIFE
        });
        state.cooldown = BULLET_COOLDOWN;
        if (A.Audio) A.Audio.sfxFire();
    }

    function explodeAsteroid(ast, bulletVx, bulletVy) {
        var info = AST_SIZES[ast.size];
        state.score += info.score;
        // particle burst
        var color = "#ffffff";
        var count = ast.size === "large" ? 22 : (ast.size === "medium" ? 14 : 8);
        if (A.FX) A.FX.spawn(ast.x, ast.y, count, { speed: 0.2, life: 500, lifeVar: 400, color: color });
        if (A.Audio) {
            if (ast.size === "large") A.Audio.sfxBangLarge();
            else if (ast.size === "medium") A.Audio.sfxBangMed();
            else A.Audio.sfxBangSmall();
        }
        if (info.next) {
            for (var i = 0; i < 2; i++) {
                var child = createAsteroid(ast.x, ast.y, info.next, state.W, state.H);
                // slight nudge in bullet direction
                var a = Math.random() * Math.PI * 2;
                var sp = AST_SPEEDS[info.next] * (0.8 + Math.random() * 0.6);
                child.vx = Math.cos(a) * sp + (bulletVx || 0) * 0.2;
                child.vy = Math.sin(a) * sp + (bulletVy || 0) * 0.2;
                state.asteroids.push(child);
            }
        }
        // extra life
        if (state.score >= state.nextExtraLife) {
            state.lives++;
            state.nextExtraLife += EXTRA_LIFE_AT;
            if (A.Audio) A.Audio.sfxExtraLife();
        }
    }

    function killShip() {
        if (!state.ship || !state.ship.alive) return;
        var s = state.ship;
        s.alive = false;
        if (A.FX) A.FX.spawn(s.x, s.y, 30, { speed: 0.25, life: 700, lifeVar: 500, color: "#ffffff" });
        if (A.Audio) A.Audio.sfxShipExplode();
        state.lives--;
        if (state.lives <= 0) {
            state.gameOver = true;
            state.running = false;
        } else {
            state.respawnTimer = RESPAWN_DELAY;
        }
    }

    function respawnShip() {
        var cx = state.W / 2, cy = state.H / 2;
        // check area clear
        var safe = 100;
        for (var i = 0; i < state.asteroids.length; i++) {
            var a = state.asteroids[i];
            if (distSq(a.x, a.y, cx, cy, state.W, state.H) < (safe + a.radius) * (safe + a.radius)) {
                // delay a bit more
                state.respawnTimer = 200;
                return;
            }
        }
        state.ship = makeShip(state.W, state.H);
        state.invulnTimer = INVULN_TIME;
    }

    function update(dt, W, H) {
        if (!state || !state.running || state.paused) return;
        state.W = W; state.H = H;

        sampleInput();
        var keys = state.keys;
        var s = state.ship;

        if (state.cooldown > 0) state.cooldown -= dt;
        if (state.invulnTimer > 0) state.invulnTimer -= dt;

        // --- Ship control / physics ---
        if (s && s.alive) {
            var mouseSteer = state.mouse && state.mouse.held;
            if (mouseSteer) {
                var dx = state.mouse.x - s.x;
                var dy = state.mouse.y - s.y;
                if (dx * dx + dy * dy > 16) {
                    var target = Math.atan2(dy, dx);
                    var diff = target - s.angle;
                    while (diff > Math.PI) diff -= Math.PI * 2;
                    while (diff < -Math.PI) diff += Math.PI * 2;
                    var step = SHIP_ROT_SPEED * dt;
                    if (diff > step) s.angle += step;
                    else if (diff < -step) s.angle -= step;
                    else s.angle = target;
                }
            } else {
                if (keys.left)  s.angle -= SHIP_ROT_SPEED * dt;
                if (keys.right) s.angle += SHIP_ROT_SPEED * dt;
            }
            s.thrusting = mouseSteer || !!keys.up;
            if (s.thrusting) {
                s.vx += Math.cos(s.angle) * SHIP_THRUST * dt;
                s.vy += Math.sin(s.angle) * SHIP_THRUST * dt;
                var sp = Math.sqrt(s.vx*s.vx + s.vy*s.vy);
                if (sp > SHIP_MAX_SPEED) {
                    s.vx = s.vx / sp * SHIP_MAX_SPEED;
                    s.vy = s.vy / sp * SHIP_MAX_SPEED;
                }
                // thrust particles
                if (A.FX && Math.random() < 0.6) {
                    var bx = s.x - Math.cos(s.angle) * 10;
                    var by = s.y - Math.sin(s.angle) * 10;
                    A.FX.spawn(bx, by, 1, {
                        speed: 0.05,
                        vx: -Math.cos(s.angle) * 0.15,
                        vy: -Math.sin(s.angle) * 0.15,
                        life: 250, lifeVar: 150,
                        color: "#ffaa44"
                    });
                }
            }
            // very light drag
            var drag = Math.pow(SHIP_DRAG, dt);
            s.vx *= drag;
            s.vy *= drag;
            s.x = wrap(s.x + s.vx * dt, W);
            s.y = wrap(s.y + s.vy * dt, H);
        } else if (!state.gameOver) {
            state.respawnTimer -= dt;
            if (state.respawnTimer <= 0) respawnShip();
        }

        // --- Bullets ---
        for (var i = state.bullets.length - 1; i >= 0; i--) {
            var b = state.bullets[i];
            b.life -= dt;
            if (b.life <= 0) { state.bullets.splice(i, 1); continue; }
            b.x = wrap(b.x + b.vx * dt, W);
            b.y = wrap(b.y + b.vy * dt, H);
        }

        // --- Asteroids ---
        for (var j = 0; j < state.asteroids.length; j++) {
            var a = state.asteroids[j];
            a.x = wrap(a.x + a.vx * dt, W);
            a.y = wrap(a.y + a.vy * dt, H);
            a.rot += a.rotSpeed * dt;
        }

        // --- Bullet vs asteroid collisions ---
        for (var bi = state.bullets.length - 1; bi >= 0; bi--) {
            var bul = state.bullets[bi];
            for (var ai = state.asteroids.length - 1; ai >= 0; ai--) {
                var ast = state.asteroids[ai];
                var d2 = distSq(bul.x, bul.y, ast.x, ast.y, W, H);
                if (d2 < ast.radius * ast.radius) {
                    state.bullets.splice(bi, 1);
                    state.asteroids.splice(ai, 1);
                    explodeAsteroid(ast, bul.vx, bul.vy);
                    break;
                }
            }
        }

        // --- Ship vs asteroid ---
        if (s && s.alive && state.invulnTimer <= 0) {
            for (var ci = 0; ci < state.asteroids.length; ci++) {
                var ca = state.asteroids[ci];
                var rr = (ca.radius + SHIP_RADIUS * 0.7);
                if (distSq(s.x, s.y, ca.x, ca.y, W, H) < rr * rr) {
                    killShip();
                    // Also split the asteroid we hit
                    var astCopy = ca;
                    state.asteroids.splice(ci, 1);
                    explodeAsteroid(astCopy, s.vx, s.vy);
                    break;
                }
            }
        }

        // --- Wave clear ---
        if (state.asteroids.length === 0 && !state.gameOver) {
            spawnWave(state.wave + 1);
        }
    }

    // --- Drawing ---
    function drawPolygon(ctx, pts, x, y, rot, close) {
        ctx.beginPath();
        var c = Math.cos(rot), si = Math.sin(rot);
        for (var i = 0; i < pts.length; i++) {
            var p = pts[i];
            var px = x + p.x * c - p.y * si;
            var py = y + p.x * si + p.y * c;
            if (i === 0) ctx.moveTo(px, py);
            else ctx.lineTo(px, py);
        }
        if (close) ctx.closePath();
        ctx.stroke();
    }

    // Draw with wrap — if shape crosses edges, draw ghost copies
    function drawWrapped(ctx, W, H, radius, x, y, drawFn) {
        drawFn(x, y);
        var ox = 0, oy = 0;
        if (x < radius) ox = W;
        else if (x > W - radius) ox = -W;
        if (y < radius) oy = H;
        else if (y > H - radius) oy = -H;
        if (ox !== 0) drawFn(x + ox, y);
        if (oy !== 0) drawFn(x, y + oy);
        if (ox !== 0 && oy !== 0) drawFn(x + ox, y + oy);
    }

    function drawShip(ctx, s, invuln) {
        if (!s.alive) return;
        // blink during invuln
        if (invuln > 0 && Math.floor(invuln / 100) % 2 === 0) return;
        ctx.strokeStyle = "#ffffff";
        ctx.lineWidth = 1.5;
        drawPolygon(ctx, SHIP_SHAPE, s.x, s.y, s.angle, true);
        if (s.thrusting && Math.random() < 0.7) {
            // flame
            var flame = [
                { x: -6, y: -4 },
                { x: -14, y: 0 },
                { x: -6, y: 4 }
            ];
            ctx.strokeStyle = "#ff9933";
            drawPolygon(ctx, flame, s.x, s.y, s.angle, false);
        }
    }

    function drawAsteroid(ctx, a) {
        ctx.strokeStyle = "#ffffff";
        ctx.lineWidth = 1.5;
        drawPolygon(ctx, a.shape, a.x, a.y, a.rot, true);
    }

    function drawBullet(ctx, b) {
        ctx.fillStyle = "#ffffff";
        ctx.fillRect(b.x - 1.5, b.y - 1.5, 3, 3);
    }

    function draw(ctx, W, H) {
        if (!state) return;

        // Asteroids
        for (var i = 0; i < state.asteroids.length; i++) {
            var a = state.asteroids[i];
            (function(ast) {
                drawWrapped(ctx, W, H, ast.radius + 2, ast.x, ast.y, function(cx, cy) {
                    ctx.save();
                    var saved = { x: ast.x, y: ast.y };
                    ast.x = cx; ast.y = cy;
                    drawAsteroid(ctx, ast);
                    ast.x = saved.x; ast.y = saved.y;
                    ctx.restore();
                });
            })(a);
        }

        // Bullets
        for (var j = 0; j < state.bullets.length; j++) {
            drawBullet(ctx, state.bullets[j]);
        }

        // Ship
        if (state.ship) {
            var s = state.ship;
            drawWrapped(ctx, W, H, 20, s.x, s.y, function(cx, cy) {
                var saved = { x: s.x, y: s.y };
                s.x = cx; s.y = cy;
                drawShip(ctx, s, state.invulnTimer);
                s.x = saved.x; s.y = saved.y;
            });
        }
    }

    // --- Input sampling from lib/input each frame ---
    function sampleInput() {
        if (!state) return;
        state.keys.left  = Input.down("left");
        state.keys.right = Input.down("right");
        state.keys.up    = Input.down("up");
        // primary action rising edge = fire bullet (consumed here)
        if (Input.pressed("primary")) fireBullet();
    }

    function clearKeys() {
        if (state) state.keys = {};
        Input.clear();
    }

    return {
        start: startGame,
        update: update,
        draw: draw,
        sampleInput: sampleInput,
        clearKeys: clearKeys,
        setMouse: setMouse,
        getState: function() { return state; },
        setPaused: function(p) { if (state) state.paused = p; },
        isGameOver: function() { return state ? state.gameOver : false; }
    };
})();
