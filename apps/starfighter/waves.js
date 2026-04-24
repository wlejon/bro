// waves.js — Wave orchestration for the 3-sector loop.
//
// Each wave exports { init, update, draw, isComplete } and is driven by
// game.js. The wave is responsible for scheduling enemy spawns and wave-
// specific geometry (star field vs surface grid vs trench); game.js owns
// enemy lists, projectiles, collisions, and HUD.
var N = N || {};

N.Waves = (function() {
    "use strict";

    var SPACE = "space", SURFACE = "surface", TRENCH = "trench";

    function startingWave() { return SPACE; }

    function nextWave(current) {
        if (current === SPACE)   return SURFACE;
        if (current === SURFACE) return TRENCH;
        return null;
    }

    function loopScale(loop) { return Math.min(3.0, 1 + (loop - 1) * 0.35); }
    function waveCompleteBonusShields() { return 1; }

    // --- Space wave ---------------------------------------------------------
    // Schedules waves of fighters, paced with gaps. First loop: light
    // pressure, ~6 fighters. Scales up with loop number.
    function createSpaceWave(game) {
        var loop = game.getLoop();
        var scale = loopScale(loop);
        var schedule = [];
        var totalFighters = Math.round(6 + (loop - 1) * 2);

        var t = 1500;
        var perWave = [Math.ceil(totalFighters / 3), Math.ceil(totalFighters / 3)];
        perWave.push(totalFighters - perWave[0] - perWave[1]);
        for (var w = 0; w < perWave.length; w++) {
            for (var i = 0; i < perWave[w]; i++) {
                schedule.push({ type: "fighter", at: t + i * 650 });
            }
            t += perWave[w] * 650 + 1500;
        }

        // Fireball pacing: a couple of dodge-only threats between waves.
        schedule.push({ type: "fireball", at: schedule[Math.floor(schedule.length * 0.3)].at + 900 });
        schedule.push({ type: "fireball", at: schedule[Math.floor(schedule.length * 0.6)].at + 900 });

        // Black Ace: single appearance near the end of wave 1. On higher
        // loops it appears earlier and lingers longer.
        var aceAt = Math.max(4000, t - 3500 - (loop - 1) * 600);
        schedule.push({ type: "ace", at: aceAt });

        // Sort so time-order is respected.
        schedule.sort(function(a, b) { return a.at - b.at; });

        return {
            kind: SPACE,
            elapsed: 0,
            spawned: 0,
            totalFighters: totalFighters,
            schedule: schedule,
            finishAfterMs: t + 2800,
            loopScale: scale,
            aceAnnounced: false
        };
    }

    function updateSpaceWave(ws, dt, game) {
        ws.elapsed += dt;
        while (ws.spawned < ws.schedule.length && ws.schedule[ws.spawned].at <= ws.elapsed) {
            var entry = ws.schedule[ws.spawned++];
            if (entry.type === "fighter") {
                var f = N.Enemies.createFighter({
                    path: N.Enemies.randomSwoop(),
                    life: 4400 + Math.random() * 1200,
                    fireCount: 1 + (Math.random() < ws.loopScale - 0.8 ? 1 : 0)
                });
                game.addEnemy(f);
            } else if (entry.type === "fireball") {
                var fb = N.Enemies.createFireball({
                    x: (Math.random() * 2 - 1) * 14,
                    y: (Math.random() * 2 - 1) * 10,
                    z: 240,
                    vz: -0.14 - Math.random() * 0.05
                });
                game.addEnemy(fb);
                if (game.radio) game.radio("FIREBALL :: EVADE", 900);
            } else if (entry.type === "ace") {
                // Ace flies a slow, high-amplitude swoop directly at the player.
                var a = N.Enemies.createAce({
                    path: N.Enemies.swoopPath(55, 18, 4, 2, -55, -12),
                    life: 7000
                });
                game.addEnemy(a);
                ws.aceAnnounced = true;
                if (game.radio) game.radio("!! BLACK ACE INBOUND !!", 2000);
                N.Audio.sfxAceEngine();
            }
        }
    }

    function drawSpaceWave(ctx, game) {
        N.Render.drawStars(ctx);
    }

    function isSpaceComplete(ws, game) {
        // Complete when all scheduled enemies have spawned AND the world is clear,
        // OR when the grace-timer has expired (avoids softlock).
        if (ws.spawned < ws.schedule.length) return false;
        if (game.hasLiveEnemies()) return false;
        if (ws.elapsed < ws.finishAfterMs - 1000) {
            // Small grace to let last explosions breathe.
            return ws.elapsed > ws.schedule[ws.schedule.length - 1].at + 2000;
        }
        return true;
    }

    // --- Surface wave ------------------------------------------------------
    // The ground sits at y=-8 (below camera center). Ship must dodge towers,
    // bunkers, and catwalks while strafing. A "grid" of lines scrolls toward
    // the camera at rail speed.
    var GROUND_Y    = -8;
    var GRID_FAR    = 320;
    var GRID_STEP   = 20;       // spacing between grid lines (world units forward)
    var GRID_X_HALF = 90;       // lateral extent of grid
    var GRID_LONGX  = [-70, -40, -20, 0, 20, 40, 70]; // longitudinal lines

    function createSurfaceWave(game) {
        var loop = game.getLoop();
        var scale = loopScale(loop);

        // Feature schedule: stream towers, bunkers, and catwalks along the z
        // axis. Spacing tightens with loop. First feature appears ~2s in.
        var schedule = [];
        var z = 240;
        var featureCount = Math.round(14 + (loop - 1) * 3);
        for (var i = 0; i < featureCount; i++) {
            var r = Math.random();
            var laneX = (Math.random() * 2 - 1) * 40;
            if (r < 0.55) {
                schedule.push({ type: "tower", x: laneX, z: z });
            } else if (r < 0.85) {
                schedule.push({ type: "bunker", x: laneX, z: z });
            } else {
                // Catwalk spans two lanes — positioned at ship altitude.
                schedule.push({
                    type: "catwalk",
                    x: laneX * 0.4,
                    z: z,
                    spanX: 14 + Math.random() * 8,
                    y: (Math.random() * 2 - 1) * 6
                });
            }
            z += 18 + Math.random() * 22 - scale * 4;
        }

        // Grid-line state: positions scroll and wrap.
        var gridZ = [];
        for (var g = 0; g < GRID_FAR / GRID_STEP; g++) gridZ.push(g * GRID_STEP);

        return {
            kind: SURFACE,
            elapsed: 0,
            schedule: schedule,
            spawnCursor: 0,
            railSpeed: 0.085 * Math.min(1.4, 1 + (loop - 1) * 0.08),
            gridZ: gridZ,
            loopScale: scale,
            endSignalAt: 0,   // set when schedule exhausted
            featuresRemaining: schedule.length
        };
    }

    function updateSurfaceWave(ws, dt, game) {
        ws.elapsed += dt;
        // Advance grid — lines scroll toward the camera at rail speed.
        var adv = ws.railSpeed * dt;
        for (var i = 0; i < ws.gridZ.length; i++) {
            ws.gridZ[i] -= adv;
            if (ws.gridZ[i] < 0) ws.gridZ[i] += GRID_FAR;
        }

        // Features: spawn the ones that have scrolled close enough to be
        // visible. We treat each schedule entry as anchored at its initial
        // z and subtract elapsed·railSpeed to get current z.
        // Simpler: spawn everything up-front; towers already handle their own
        // z-advance via updateSurface. Spawn when schedule entry's scheduled z
        // minus (elapsed * rail) is less than the spawn horizon.
        var horizon = 280;
        while (ws.spawnCursor < ws.schedule.length) {
            var entry = ws.schedule[ws.spawnCursor];
            var liveZ = entry.z - ws.elapsed * ws.railSpeed;
            if (liveZ > horizon) break;
            if (entry.type === "tower") {
                game.addEnemy(N.Enemies.createTower({ x: entry.x, y: GROUND_Y, z: liveZ }));
            } else if (entry.type === "bunker") {
                game.addEnemy(N.Enemies.createBunker({ x: entry.x, y: GROUND_Y, z: liveZ }));
            } else if (entry.type === "catwalk") {
                game.addEnemy(N.Enemies.createCatwalk({
                    x: entry.x, y: entry.y, z: liveZ, spanX: entry.spanX
                }));
            }
            ws.spawnCursor++;
        }

        if (ws.spawnCursor >= ws.schedule.length && !ws.endSignalAt) {
            ws.endSignalAt = ws.elapsed + 2800;
        }
    }

    function drawSurfaceWave(ctx, game) {
        var ws = game.getWaveScript();
        if (!ws) return;

        // Sky gradient via horizon band (lighter above horizon, black below).
        var W = N.Render.width(), H = N.Render.height();
        ctx.fillStyle = "#000";
        ctx.fillRect(0, 0, W, H);

        // Subtle horizon glow.
        var horizonP = N.Render.project(0, GROUND_Y, GRID_FAR);
        if (horizonP.visible) {
            var grd = ctx.createLinearGradient(0, horizonP.y - 60, 0, horizonP.y + 4);
            grd.addColorStop(0, "rgba(60, 40, 80, 0)");
            grd.addColorStop(1, "rgba(120, 60, 80, 0.25)");
            ctx.fillStyle = grd;
            ctx.fillRect(0, horizonP.y - 60, W, 64);
        }

        // Grid: longitudinal lines (fixed X, running far to near).
        ctx.lineWidth = 1.2;
        for (var lx = 0; lx < GRID_LONGX.length; lx++) {
            var x = GRID_LONGX[lx];
            N.Render.line(ctx,
                x, GROUND_Y, N.Render.NEAR_Z + 0.1,
                x, GROUND_Y, GRID_FAR,
                "#3a6", 0.7);
        }

        // Transverse lines (fixed Z, spanning X range), scrolling.
        for (var ti = 0; ti < ws.gridZ.length; ti++) {
            var tz = ws.gridZ[ti];
            if (tz < N.Render.NEAR_Z + 0.2 || tz > GRID_FAR) continue;
            var fade = 1 - tz / GRID_FAR;
            N.Render.line(ctx,
                -GRID_X_HALF, GROUND_Y, tz,
                 GRID_X_HALF, GROUND_Y, tz,
                "#3a6", 0.25 + fade * 0.6);
        }
    }

    function isSurfaceComplete(ws, game) {
        if (ws.spawnCursor < ws.schedule.length) return false;
        if (game.hasLiveEnemies()) return ws.elapsed > ws.endSignalAt + 4000;
        return ws.elapsed > ws.endSignalAt;
    }

    // --- Trench wave -------------------------------------------------------
    // The trench is a long rectangular channel: walls at x=±TRENCH_HALF_W,
    // floor at y=FLOOR_Y, ceiling at y=CEIL_Y. Transverse ribs scroll toward
    // the camera at rail speed. Pylons, catwalks, and turrets stream in as
    // obstacles. The exhaust port spawns at a fixed distance from the start.
    var TRENCH_HALF_W = 12;
    var FLOOR_Y = -9;
    var CEIL_Y  = 9;
    var TRENCH_FAR = 360;
    var TRENCH_RIB_STEP = 16;

    function createTrenchWave(game) {
        var loop = game.getLoop();
        var scale = loopScale(loop);

        var schedule = [];
        var featureCount = Math.round(10 + (loop - 1) * 2);
        var z = 240;
        for (var i = 0; i < featureCount; i++) {
            var r = Math.random();
            if (r < 0.35) {
                // Catwalk: horizontal hazard across the trench
                schedule.push({
                    type: "catwalk",
                    x: 0,
                    y: (Math.random() * 2 - 1) * 6,
                    z: z,
                    spanX: TRENCH_HALF_W + 1
                });
            } else if (r < 0.70) {
                // Pylon: vertical column, blocks one side
                schedule.push({
                    type: "pylon",
                    x: (Math.random() * 2 - 1) * (TRENCH_HALF_W - 2.5),
                    z: z,
                    halfWidth: 1.4 + Math.random() * 1.4
                });
            } else {
                // Turret: wall-mounted
                var side = Math.random() < 0.5 ? -1 : 1;
                schedule.push({
                    type: "turret",
                    x: side * (TRENCH_HALF_W - 0.6),
                    y: (Math.random() * 2 - 1) * 6,
                    z: z,
                    side: side
                });
            }
            z += 18 + Math.random() * 16 - scale * 3;
        }

        // Exhaust port appears at the far end after all other features.
        var portZ = z + 80;
        schedule.push({ type: "port", x: 0, y: 0, z: portZ });

        // Transverse rib state.
        var ribZ = [];
        for (var g = 0; g < TRENCH_FAR / TRENCH_RIB_STEP; g++) {
            ribZ.push(g * TRENCH_RIB_STEP);
        }

        return {
            kind: TRENCH,
            elapsed: 0,
            schedule: schedule,
            spawnCursor: 0,
            railSpeed: 0.09 * Math.min(1.45, 1 + (loop - 1) * 0.09),
            ribZ: ribZ,
            loopScale: scale,
            portReached: false,
            portResolved: false,
            portZStart: portZ
        };
    }

    function updateTrenchWave(ws, dt, game) {
        ws.elapsed += dt;

        var adv = ws.railSpeed * dt;
        for (var i = 0; i < ws.ribZ.length; i++) {
            ws.ribZ[i] -= adv;
            if (ws.ribZ[i] < 0) ws.ribZ[i] += TRENCH_FAR;
        }

        var horizon = 280;
        while (ws.spawnCursor < ws.schedule.length) {
            var entry = ws.schedule[ws.spawnCursor];
            var liveZ = entry.z - ws.elapsed * ws.railSpeed;
            if (liveZ > horizon) break;
            if (entry.type === "catwalk") {
                game.addEnemy(N.Enemies.createCatwalk({
                    x: entry.x, y: entry.y, z: liveZ, spanX: entry.spanX
                }));
            } else if (entry.type === "pylon") {
                game.addEnemy(N.Enemies.createPylon({
                    x: entry.x, z: liveZ, halfWidth: entry.halfWidth
                }));
            } else if (entry.type === "turret") {
                game.addEnemy(N.Enemies.createTurret({
                    x: entry.x, y: entry.y, z: liveZ, side: entry.side
                }));
            } else if (entry.type === "port") {
                // Port shrinks with each loop — bullseye tolerance tightens.
                var loopShrink = Math.max(0.45, 1 - (game.getLoop() - 1) * 0.15);
                var p = N.Enemies.createPort({ x: entry.x, y: entry.y, z: liveZ });
                p.radius *= loopShrink;
                p.innerRadius *= loopShrink;
                game.addEnemy(p);
                ws.port = p;
                if (game.radio) game.radio("VENT AHEAD  ::  STAY CENTERED", 2400);
            }
            ws.spawnCursor++;
        }

        // Lock-on: active when the port is within the lock window.
        if (ws.port && !ws.port.resolved) {
            var pz = ws.port.z;
            var inLock = pz > 10 && pz < 85;
            if (inLock && !ws._lastLock) {
                // Lock tone pulses periodically while active.
                ws._lockTonePulse = 0;
            }
            if (inLock) {
                ws._lockTonePulse -= dt;
                if (ws._lockTonePulse <= 0) {
                    N.Audio.sfxLockTone();
                    // Pulse faster as port approaches.
                    var tt = (pz - 10) / 75; // 0..1, 0=closest
                    ws._lockTonePulse = 90 + tt * 260;
                }
            }
            ws._lastLock = inLock;
            if (game._setLock) game._setLock(inLock);
        }
    }

    function drawTrenchWave(ctx, game) {
        var ws = game.getWaveScript();
        if (!ws) return;
        var W = N.Render.width(), H = N.Render.height();
        ctx.fillStyle = "#000";
        ctx.fillRect(0, 0, W, H);

        // Left + right wall long runners (top/bottom edges) running far to near.
        var col = "#68f";
        N.Render.line(ctx, -TRENCH_HALF_W, FLOOR_Y, N.Render.NEAR_Z + 0.2,
                           -TRENCH_HALF_W, FLOOR_Y, TRENCH_FAR, col, 0.9);
        N.Render.line(ctx, -TRENCH_HALF_W, CEIL_Y,  N.Render.NEAR_Z + 0.2,
                           -TRENCH_HALF_W, CEIL_Y,  TRENCH_FAR, col, 0.9);
        N.Render.line(ctx,  TRENCH_HALF_W, FLOOR_Y, N.Render.NEAR_Z + 0.2,
                            TRENCH_HALF_W, FLOOR_Y, TRENCH_FAR, col, 0.9);
        N.Render.line(ctx,  TRENCH_HALF_W, CEIL_Y,  N.Render.NEAR_Z + 0.2,
                            TRENCH_HALF_W, CEIL_Y,  TRENCH_FAR, col, 0.9);
        // Floor center longitudinal for depth cue.
        N.Render.line(ctx,  0, FLOOR_Y, N.Render.NEAR_Z + 0.2,
                            0, FLOOR_Y, TRENCH_FAR, col, 0.35);

        // Transverse ribs on each wall — four vertical segments per rib position.
        for (var ti = 0; ti < ws.ribZ.length; ti++) {
            var rz = ws.ribZ[ti];
            if (rz < N.Render.NEAR_Z + 0.3 || rz > TRENCH_FAR) continue;
            var fade = 1 - rz / TRENCH_FAR;
            var a = 0.25 + fade * 0.6;
            N.Render.line(ctx, -TRENCH_HALF_W, FLOOR_Y, rz,
                               -TRENCH_HALF_W, CEIL_Y,  rz, col, a);
            N.Render.line(ctx,  TRENCH_HALF_W, FLOOR_Y, rz,
                                TRENCH_HALF_W, CEIL_Y,  rz, col, a);
            // Floor rib spanning trench width (bottom).
            N.Render.line(ctx, -TRENCH_HALF_W, FLOOR_Y, rz,
                                TRENCH_HALF_W, FLOOR_Y, rz, col, a * 0.6);
        }
    }

    function isTrenchComplete(ws, game) {
        return !!ws.portResolved;
    }

    // --- Dispatcher --------------------------------------------------------
    function create(name, game) {
        if (name === SPACE)   return createSpaceWave(game);
        if (name === SURFACE) return createSurfaceWave(game);
        if (name === TRENCH)  return createTrenchWave(game);
        return null;
    }
    function update(ws, dt, game) {
        if (!ws) return;
        if (ws.kind === SPACE)   updateSpaceWave(ws, dt, game);
        else if (ws.kind === SURFACE) updateSurfaceWave(ws, dt, game);
        else if (ws.kind === TRENCH)  updateTrenchWave(ws, dt, game);
    }
    function draw(ws, ctx, game) {
        if (!ws) return;
        if (ws.kind === SPACE)   drawSpaceWave(ctx, game);
        else if (ws.kind === SURFACE) drawSurfaceWave(ctx, game);
        else if (ws.kind === TRENCH)  drawTrenchWave(ctx, game);
    }
    function isComplete(ws, game) {
        if (!ws) return false;
        if (ws.kind === SPACE)   return isSpaceComplete(ws, game);
        if (ws.kind === SURFACE) return isSurfaceComplete(ws, game);
        if (ws.kind === TRENCH)  return isTrenchComplete(ws, game);
        return true;
    }

    return {
        SPACE: SPACE, SURFACE: SURFACE, TRENCH: TRENCH,
        startingWave: startingWave,
        nextWave: nextWave,
        loopScale: loopScale,
        waveCompleteBonusShields: waveCompleteBonusShields,
        create: create,
        update: update,
        draw: draw,
        isComplete: isComplete
    };
})();
