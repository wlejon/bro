// game.js — Touchdown core simulation: terrain, lander, physics
var T = T || {};

T.Game = (function() {
    // Physics constants (units: pixels, frames at ~60fps, where "per frame" means per 16.67ms step)
    var GRAVITY = 0.06;          // px/frame^2 added to vy each frame
    var THRUST  = 0.14;          // px/frame^2 applied along lander facing
    var ROT_SPEED = 0.065;       // radians per frame while rotating
    var FUEL_BURN = 0.65;        // fuel units per frame of thrust
    var MAX_SAFE_VY = 2.4;
    var MAX_SAFE_VX = 1.8;
    var MAX_SAFE_TILT = 0.22;    // ~12.6 degrees from upright

    var LANDER_W = 14;
    var LANDER_H = 16;

    var state = null;

    // Particle pool (simple, for thrust + crash)
    var particles = [];

    function rand(a, b) { return a + Math.random() * (b - a); }

    // --- Terrain ---
    // Builds mountainous polyline spanning 0..W across the bottom portion.
    // Returns { points: [{x,y}], pads: [{x1,x2,y,width,bonus}] }
    function buildTerrain(W, H, level) {
        var points = [];
        var pads = [];
        var segWidth = 18;                       // finer = more detail
        var ground = H * 0.78;
        var amp = 50 + level * 14;
        if (amp > 180) amp = 180;
        var jag = 0.35 + Math.min(0.45, level * 0.06);

        // Decide where pads go (flat segments). Fewer pads as level rises.
        var numPads = Math.max(2, 5 - Math.floor(level / 2));
        var padRanges = [];
        for (var p = 0; p < numPads; p++) {
            var padWidth = Math.max(40, 120 - level * 10);
            padWidth = Math.floor(padWidth + rand(-10, 10));
            if (padWidth < 36) padWidth = 36;
            var margin = 60;
            var px = Math.floor(rand(margin, W - margin - padWidth));
            padRanges.push({ x1: px, x2: px + padWidth });
        }

        // Build heightmap
        var x = 0;
        var y = ground + rand(-20, 20);
        while (x <= W + segWidth) {
            // Check if current x inside a pad range
            var inPad = -1;
            for (var i = 0; i < padRanges.length; i++) {
                if (x >= padRanges[i].x1 && x <= padRanges[i].x2) { inPad = i; break; }
            }
            if (inPad !== -1) {
                // Walk the pad at constant y
                var pr = padRanges[inPad];
                var padY = y;
                var padStartIdx = points.length;
                // Ensure entry point lines up at pr.x1
                if (points.length === 0 || points[points.length - 1].x < pr.x1) {
                    points.push({ x: pr.x1, y: padY });
                }
                points.push({ x: pr.x2, y: padY });
                pads.push({
                    x1: pr.x1,
                    x2: pr.x2,
                    y: padY,
                    width: pr.x2 - pr.x1,
                    // Narrower pads = higher bonus; scaled roughly 50..400
                    bonus: Math.floor(50 + (140 - Math.min(140, pr.x2 - pr.x1)) * 2.8)
                });
                x = pr.x2 + segWidth;
                // perturb height after pad so rough terrain continues
                y = padY + rand(-amp * jag, amp * jag);
                continue;
            }
            points.push({ x: x, y: y });
            x += segWidth + rand(-5, 5);
            y += rand(-amp * jag, amp * jag);
            // clamp within screen range
            if (y < H * 0.40) y = H * 0.40 + rand(0, 10);
            if (y > H - 30)   y = H - 30 - rand(0, 10);
        }
        // Ensure endpoints reach screen edges
        if (points.length && points[0].x > 0) points.unshift({ x: 0, y: points[0].y });
        if (points[points.length - 1].x < W) points.push({ x: W, y: points[points.length - 1].y });

        return { points: points, pads: pads };
    }

    // Find terrain Y at column x via linear interpolation across the polyline
    function terrainY(terrain, x) {
        var pts = terrain.points;
        if (x <= pts[0].x) return pts[0].y;
        if (x >= pts[pts.length - 1].x) return pts[pts.length - 1].y;
        // linear scan (polyline small enough)
        for (var i = 1; i < pts.length; i++) {
            if (pts[i].x >= x) {
                var a = pts[i - 1], b = pts[i];
                var t = (x - a.x) / (b.x - a.x || 1);
                return a.y + (b.y - a.y) * t;
            }
        }
        return pts[pts.length - 1].y;
    }

    function onFlatPad(terrain, x) {
        for (var i = 0; i < terrain.pads.length; i++) {
            var p = terrain.pads[i];
            if (x >= p.x1 && x <= p.x2) return p;
        }
        return null;
    }

    // --- Lander lifecycle ---
    function newLander(W, H, level) {
        var fuel = Math.max(350, 1000 - (level - 1) * 110);
        return {
            x: W * 0.5 + rand(-W * 0.25, W * 0.25),
            y: 70,
            vx: rand(-0.9, 0.9) + (level - 1) * 0.1,
            vy: 0,
            angle: 0,          // 0 = pointing up; positive = tilted clockwise (right)
            thrusting: false,
            fuel: fuel,
            fuelMax: fuel,
            alive: true,
            landed: false,
            landingResult: null
        };
    }

    function start(W, H) {
        var level = 1;
        var terrain = buildTerrain(W, H, level);
        state = {
            W: W,
            H: H,
            level: level,
            score: 0,
            landings: 0,
            totalFuelUsed: 0,
            terrain: terrain,
            lander: newLander(W, H, level),
            input: { left: false, right: false, thrust: false },
            mouse: { x: W/2, y: H/2, held: false },
            paused: false,
            gameOver: false,
            status: "flying",      // flying | landed | crashed
            statusMsg: "",
            lastLandingBonus: 0,
            lastLandingPadWidth: 0,
            stars: makeStars(W, H),
            shake: 0
        };
        particles = [];
    }

    function makeStars(W, H) {
        var s = [];
        for (var i = 0; i < 60; i++) {
            s.push({
                x: Math.random() * W,
                y: Math.random() * H * 0.75,
                b: 0.2 + Math.random() * 0.6
            });
        }
        return s;
    }

    function advanceLevel() {
        if (!state) return;
        state.level += 1;
        state.terrain = buildTerrain(state.W, state.H, state.level);
        state.lander = newLander(state.W, state.H, state.level);
        state.status = "flying";
        state.statusMsg = "";
        state.shake = 0;
        particles = [];
    }

    // --- Input (sampled each frame from lib/input) ---
    function sampleInput() {
        if (!state) return;
        state.input.left   = Input.down("left");
        state.input.right  = Input.down("right");
        state.input.thrust = Input.down("thrust");
    }

    function setPaused(p) { if (state) state.paused = p; }
    function isPaused() { return state ? state.paused : false; }

    // --- Particles ---
    function spawnThrust(lander) {
        // Exhaust out the back (bottom) of the lander
        var ang = lander.angle;
        var ex = lander.x + Math.sin(ang) * 10;
        var ey = lander.y + Math.cos(ang) * 10;
        // Direction opposite thrust (thrust is -sin,-cos; exhaust is +sin,+cos from tail)
        var spread = (Math.random() - 0.5) * 0.6;
        var speed = 2.5 + Math.random() * 1.5;
        var dx = Math.sin(ang + spread) * speed + lander.vx * 0.3;
        var dy = Math.cos(ang + spread) * speed + lander.vy * 0.3;
        particles.push({
            x: ex, y: ey, vx: dx, vy: dy,
            life: 280 + Math.random() * 120,
            age: 0,
            color: "#ff9955",
            size: 1 + Math.random() * 1.5
        });
    }

    function spawnCrash(lander) {
        for (var i = 0; i < 40; i++) {
            var a = Math.random() * Math.PI * 2;
            var sp = 0.8 + Math.random() * 4.5;
            particles.push({
                x: lander.x,
                y: lander.y,
                vx: Math.cos(a) * sp,
                vy: Math.sin(a) * sp - 0.5,
                life: 600 + Math.random() * 400,
                age: 0,
                color: i % 3 === 0 ? "#ffbb55" : "#ffffff",
                size: 1 + Math.random() * 2
            });
        }
    }

    function updateParticles(dt) {
        var kept = [];
        for (var i = 0; i < particles.length; i++) {
            var p = particles[i];
            p.age += dt;
            if (p.age >= p.life) continue;
            var f = dt / 16.67;
            p.x += p.vx * f;
            p.y += p.vy * f;
            p.vy += 0.02 * f;
            kept.push(p);
        }
        particles = kept;
    }

    function drawParticles(ctx) {
        for (var i = 0; i < particles.length; i++) {
            var p = particles[i];
            var a = 1 - (p.age / p.life);
            if (a < 0) a = 0;
            ctx.globalAlpha = a;
            ctx.fillStyle = p.color;
            ctx.fillRect(p.x - p.size * 0.5, p.y - p.size * 0.5, p.size, p.size);
        }
        ctx.globalAlpha = 1;
    }

    // --- Physics ---
    function update(dt) {
        if (!state || state.paused || state.gameOver) return;
        sampleInput();
        var f = dt / 16.67; // normalize so constants behave at 60fps
        if (f > 3) f = 3;

        var L = state.lander;

        if (state.status === "flying") {
            var mouseSteer = state.mouse && state.mouse.held;
            if (mouseSteer) {
                var dx = state.mouse.x - L.x;
                var dy = state.mouse.y - L.y;
                if (dx * dx + dy * dy > 16) {
                    var target = Math.atan2(dx, -dy);
                    var cur = L.angle;
                    var diff = target - cur;
                    while (diff > Math.PI) diff -= Math.PI * 2;
                    while (diff < -Math.PI) diff += Math.PI * 2;
                    var step = ROT_SPEED * f;
                    if (diff > step) L.angle += step;
                    else if (diff < -step) L.angle -= step;
                    else L.angle = target;
                }
            } else {
                if (state.input.left) L.angle -= ROT_SPEED * f;
                if (state.input.right) L.angle += ROT_SPEED * f;
            }

            var wantThrust = (state.input.thrust || mouseSteer) && L.fuel > 0;
            L.thrusting = wantThrust;

            if (wantThrust) {
                // Forward direction (nose of lander): (sin(angle), -cos(angle))
                var fx = Math.sin(L.angle);
                var fy = -Math.cos(L.angle);
                L.vx += fx * THRUST * f;
                L.vy += fy * THRUST * f;
                L.fuel -= FUEL_BURN * f;
                if (L.fuel < 0) L.fuel = 0;
                state.totalFuelUsed += FUEL_BURN * f;
                // Spawn exhaust particles
                if (Math.random() < 0.8) spawnThrust(L);
                T.Audio.startThrust();
            } else {
                T.Audio.stopThrust();
            }

            // Gravity
            L.vy += GRAVITY * f;

            // Integrate
            L.x += L.vx * f;
            L.y += L.vy * f;

            // Wrap horizontally (classic lunar lander convention)
            if (L.x < 0) L.x += state.W;
            else if (L.x > state.W) L.x -= state.W;

            // Shake decay
            if (state.shake > 0) state.shake -= 0.2 * f;
            if (state.shake < 0) state.shake = 0;

            // Collision: check if lander bottom is below terrain height at its x
            var groundY = terrainY(state.terrain, L.x);
            // Lander "footprint" bottom: half-height rotated. Use a simple center + radius check
            // plus a tilt-aware foot position.
            var bottomY = L.y + Math.abs(Math.cos(L.angle)) * LANDER_H * 0.55;

            if (bottomY >= groundY) {
                // Snap to surface
                L.y = groundY - Math.abs(Math.cos(L.angle)) * LANDER_H * 0.55;
                var pad = onFlatPad(state.terrain, L.x);
                var vxAbs = Math.abs(L.vx);
                var vyAbs = Math.abs(L.vy);
                var tilt = Math.abs(((L.angle + Math.PI) % (Math.PI * 2)) - Math.PI);
                // normalize angle to [-pi, pi]
                var norm = L.angle;
                while (norm > Math.PI) norm -= Math.PI * 2;
                while (norm < -Math.PI) norm += Math.PI * 2;
                tilt = Math.abs(norm);

                var safe = pad && vxAbs <= MAX_SAFE_VX && vyAbs <= MAX_SAFE_VY && tilt <= MAX_SAFE_TILT;
                if (safe) {
                    var bonus = pad.bonus;
                    // Fuel bonus: leftover fuel contributes
                    var fuelBonus = Math.floor(L.fuel * 0.2);
                    var softLand = Math.max(0, Math.round((MAX_SAFE_VY - vyAbs) * 50));
                    var gained = bonus + fuelBonus + softLand;
                    state.score += gained;
                    state.landings += 1;
                    state.lastLandingBonus = gained;
                    state.lastLandingPadWidth = pad.width;
                    state.status = "landed";
                    L.landed = true;
                    L.vx = 0; L.vy = 0;
                    T.Audio.stopThrust();
                    T.Audio.sfxLanded();
                } else {
                    L.alive = false;
                    state.status = "crashed";
                    // Explain failure
                    var reasons = [];
                    if (!pad) reasons.push("NOT ON A FLAT PAD");
                    if (vyAbs > MAX_SAFE_VY) reasons.push("DESCENT TOO FAST");
                    if (vxAbs > MAX_SAFE_VX) reasons.push("LATERAL DRIFT TOO HIGH");
                    if (tilt > MAX_SAFE_TILT) reasons.push("NOT UPRIGHT");
                    state.statusMsg = reasons.join(" · ");
                    spawnCrash(L);
                    state.shake = 16;
                    T.Audio.stopThrust();
                    T.Audio.sfxCrash();
                    state.gameOver = true;
                }
            }

            // Out of fuel + still falling far above ground is fine; only crash on contact
        }

        updateParticles(dt);
    }

    // --- Rendering ---
    function drawStars(ctx) {
        for (var i = 0; i < state.stars.length; i++) {
            var s = state.stars[i];
            ctx.globalAlpha = s.b;
            ctx.fillStyle = "#ffffff";
            ctx.fillRect(s.x, s.y, 1, 1);
        }
        ctx.globalAlpha = 1;
    }

    function drawTerrain(ctx, W, H) {
        var pts = state.terrain.points;
        ctx.strokeStyle = "#ffffff";
        ctx.lineWidth = 1.5;
        ctx.beginPath();
        ctx.moveTo(pts[0].x, pts[0].y);
        for (var i = 1; i < pts.length; i++) {
            ctx.lineTo(pts[i].x, pts[i].y);
        }
        ctx.stroke();

        // Highlight pads
        ctx.lineWidth = 3;
        for (var j = 0; j < state.terrain.pads.length; j++) {
            var p = state.terrain.pads[j];
            ctx.strokeStyle = "#66ff99";
            ctx.beginPath();
            ctx.moveTo(p.x1, p.y);
            ctx.lineTo(p.x2, p.y);
            ctx.stroke();

            // Pad bonus label
            ctx.globalAlpha = 0.8;
            ctx.fillStyle = "#66ff99";
            ctx.font = "11px Consolas, monospace";
            ctx.textAlign = "center";
            ctx.fillText("+" + p.bonus, (p.x1 + p.x2) * 0.5, p.y - 6);
            ctx.globalAlpha = 1;
        }
        ctx.lineWidth = 1;
    }

    function drawLander(ctx, L) {
        ctx.save();
        ctx.translate(L.x, L.y);
        ctx.rotate(L.angle);
        ctx.strokeStyle = "#ffffff";
        ctx.lineWidth = 1.5;

        // Body (triangular nose + square body)
        ctx.beginPath();
        ctx.moveTo(0, -LANDER_H * 0.6);             // nose up
        ctx.lineTo(-LANDER_W * 0.5, -LANDER_H * 0.1);
        ctx.lineTo(-LANDER_W * 0.5, LANDER_H * 0.35);
        ctx.lineTo(LANDER_W * 0.5, LANDER_H * 0.35);
        ctx.lineTo(LANDER_W * 0.5, -LANDER_H * 0.1);
        ctx.closePath();
        ctx.stroke();

        // Landing legs
        ctx.beginPath();
        ctx.moveTo(-LANDER_W * 0.5, LANDER_H * 0.35);
        ctx.lineTo(-LANDER_W * 0.85, LANDER_H * 0.6);
        ctx.moveTo(LANDER_W * 0.5, LANDER_H * 0.35);
        ctx.lineTo(LANDER_W * 0.85, LANDER_H * 0.6);
        // Foot pads
        ctx.moveTo(-LANDER_W * 1.0, LANDER_H * 0.6);
        ctx.lineTo(-LANDER_W * 0.7, LANDER_H * 0.6);
        ctx.moveTo(LANDER_W * 0.7, LANDER_H * 0.6);
        ctx.lineTo(LANDER_W * 1.0, LANDER_H * 0.6);
        ctx.stroke();

        // Thrust flame
        if (L.thrusting && L.alive) {
            var flicker = 0.6 + Math.random() * 0.8;
            ctx.strokeStyle = "#ffaa44";
            ctx.beginPath();
            ctx.moveTo(-LANDER_W * 0.25, LANDER_H * 0.35);
            ctx.lineTo(0, LANDER_H * 0.35 + 10 * flicker);
            ctx.lineTo(LANDER_W * 0.25, LANDER_H * 0.35);
            ctx.stroke();
        }

        ctx.restore();
    }

    function draw(ctx, W, H) {
        if (!state) return;

        ctx.save();
        if (state.shake > 0) {
            ctx.translate((Math.random() - 0.5) * state.shake, (Math.random() - 0.5) * state.shake);
        }

        drawStars(ctx);
        drawTerrain(ctx, W, H);

        // Center dashed line for pads above (altitude marker optional — skipped for clarity)
        drawParticles(ctx);

        if (state.lander.alive || state.status === "landed") {
            drawLander(ctx, state.lander);
        }

        ctx.restore();

        // Status banner (landed / crashed)
        if (state.status === "landed") {
            ctx.fillStyle = "#66ff99";
            ctx.font = "bold 22px Consolas, monospace";
            ctx.textAlign = "center";
            ctx.fillText("TOUCHDOWN  +" + state.lastLandingBonus, W * 0.5, H * 0.28);
            ctx.font = "13px Consolas, monospace";
            ctx.fillStyle = "#aaaaaa";
            ctx.fillText("Press ENTER for next level", W * 0.5, H * 0.28 + 22);
        } else if (state.status === "crashed") {
            ctx.fillStyle = "#ff5555";
            ctx.font = "bold 22px Consolas, monospace";
            ctx.textAlign = "center";
            ctx.fillText("CRASHED", W * 0.5, H * 0.28);
            if (state.statusMsg) {
                ctx.font = "12px Consolas, monospace";
                ctx.fillStyle = "#aa8888";
                ctx.fillText(state.statusMsg, W * 0.5, H * 0.28 + 20);
            }
        }
    }

    function getState() { return state; }
    function isGameOver() { return state && state.gameOver; }
    function isLanded() { return state && state.status === "landed"; }

    return {
        start: start,
        advanceLevel: advanceLevel,
        update: update,
        draw: draw,
        sampleInput: sampleInput,
        setMouse: function(x, y, held) {
            if (!state) return;
            if (x !== undefined) state.mouse.x = x;
            if (y !== undefined) state.mouse.y = y;
            if (held !== undefined) state.mouse.held = !!held;
        },
        setPaused: setPaused,
        isPaused: isPaused,
        isGameOver: isGameOver,
        isLanded: isLanded,
        getState: getState
    };
})();
