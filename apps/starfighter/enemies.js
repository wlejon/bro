// enemies.js — Enemy types, meshes, and behaviors.
//
// Each enemy is a plain object with a shared update/draw dispatch by
// `.kind`. We avoid prototypal classes to stay consistent with the rest
// of the arcade apps (IIFE + factory pattern).
var N = N || {};

N.Enemies = (function() {
    "use strict";

    // --- Fighter wireframe (legally-distinct "twin-vane" silhouette) -------
    var FIGHTER_VERTS = [
        { x:  0.0, y:  0.0, z:  0.6 }, // 0 nose
        { x:  0.0, y:  0.0, z: -0.6 }, // 1 tail
        { x:  1.2, y:  0.9, z:  0.0 }, // 2 R upper
        { x:  1.2, y: -0.9, z:  0.0 }, // 3 R lower
        { x: -1.2, y:  0.9, z:  0.0 }, // 4 L upper
        { x: -1.2, y: -0.9, z:  0.0 }, // 5 L lower
        { x:  0.35, y: 0.0, z:  0.0 }, // 6 R pod attach
        { x: -0.35, y: 0.0, z:  0.0 }  // 7 L pod attach
    ];
    var FIGHTER_EDGES = [
        [0, 1], [0, 6], [0, 7], [1, 6], [1, 7],
        [2, 3], [2, 6], [3, 6],
        [4, 5], [4, 7], [5, 7]
    ];

    // --- Ace (asymmetric heavy fighter) ------------------------------------
    var ACE_VERTS = [
        { x:  0.0, y:  0.0, z:  0.9 },
        { x:  0.0, y:  0.0, z: -0.8 },
        { x:  1.6, y:  0.4, z:  0.2 },
        { x:  0.6, y: -0.3, z: -0.3 },
        { x: -1.6, y:  0.4, z:  0.2 },
        { x: -0.6, y: -0.3, z: -0.3 },
        { x:  0.0, y:  0.6, z:  0.0 }
    ];
    var ACE_EDGES = [
        [0, 1], [0, 2], [0, 4], [0, 6],
        [1, 3], [1, 5], [1, 6],
        [2, 3], [4, 5], [3, 5]
    ];

    // Transform a vertex list by yaw (around Y) + pitch (around X) + scale.
    function _xform(verts, yaw, pitch, scale) {
        var cy = Math.cos(yaw),  sy = Math.sin(yaw);
        var cp = Math.cos(pitch), sp = Math.sin(pitch);
        var out = new Array(verts.length);
        for (var i = 0; i < verts.length; i++) {
            var v = verts[i];
            // Pitch first (X-axis), then yaw (Y-axis).
            var y1 = v.y * cp - v.z * sp;
            var z1 = v.y * sp + v.z * cp;
            var x2 = v.x * cy - z1 * sy;
            var z2 = v.x * sy + z1 * cy;
            out[i] = { x: x2 * scale, y: y1 * scale, z: z2 * scale };
        }
        return out;
    }

    function drawMesh(ctx, verts, edges, ox, oy, oz, color, alpha) {
        N.Render.edges(ctx, verts, edges, color, alpha != null ? alpha : 1,
            { ox: ox, oy: oy, oz: oz });
    }

    // --- Fighter factory ---------------------------------------------------
    // path: function(t) → {x,y,z} in world space, for t in [0,1] over `life`.
    function createFighter(opts) {
        opts = opts || {};
        return {
            kind: "fighter",
            path: opts.path,
            life: opts.life || 5200,   // total ms the flight path takes
            t: 0,
            x: 0, y: 0, z: 300,
            radius: 1.6,
            scale: 1.5,
            yaw: 0, pitch: 0,
            color: "#6f6",
            hp: 1,
            score: 1000,
            dead: false,
            fireAt: opts.fireAt != null ? opts.fireAt : 0.45, // normalized along path
            fireCount: opts.fireCount || 2,
            _fired: 0,
            _lastX: 0, _lastY: 0, _lastZ: 300
        };
    }

    // --- Ace factory -------------------------------------------------------
    function createAce(opts) {
        opts = opts || {};
        return {
            kind: "ace",
            path: opts.path,
            life: opts.life || 6500,
            t: 0,
            x: 0, y: 0, z: 300,
            radius: 2.2,
            scale: 1.9,
            yaw: 0, pitch: 0,
            color: "#f84",
            hp: 999,                   // invulnerable
            score: 0,                  // no kill credit; flee-on-hit gives 2000
            flee: false,
            fleeTimer: 0,
            dead: false,
            fireAt: 0.4,
            fireCount: 3,
            _fired: 0,
            _lastX: 0, _lastY: 0, _lastZ: 300
        };
    }

    // --- Fireball factory --------------------------------------------------
    function createFireball(opts) {
        opts = opts || {};
        return {
            kind: "fireball",
            x: opts.x || 0,
            y: opts.y || 0,
            z: opts.z != null ? opts.z : 260,
            vz: opts.vz || -0.18,       // advances toward camera
            radius: 2.4,
            scale: 2.4,
            color: "#f66",
            t: 0,
            life: 9999,
            hp: 0,                      // cannot be shot — must be dodged
            score: 0,
            dead: false
        };
    }

    // --- Update dispatch ---------------------------------------------------
    function update(e, dt, game) {
        e.t += dt;
        e._lastX = e.x; e._lastY = e.y; e._lastZ = e.z;

        if (e.kind === "fighter" || e.kind === "ace") {
            var u = Math.min(1, e.t / e.life);
            if (e.kind === "ace" && e.flee) {
                // Fleeing ace accelerates away.
                e.fleeTimer += dt;
                e.z += 0.25 * dt;
                e.x += (e.x < 0 ? -0.05 : 0.05) * dt;
                if (e.z > 400) e.dead = true;
            } else if (e.path) {
                var p = e.path(u);
                e.x = p.x; e.y = p.y; e.z = p.z;
                // Yaw follows motion direction in XZ plane.
                var dx = e.x - e._lastX, dz = e.z - e._lastZ;
                if (Math.abs(dx) + Math.abs(dz) > 0.01) {
                    e.yaw = Math.atan2(dx, -dz);
                }
                e.pitch = Math.sin(e.t * 0.002) * 0.1;
                if (u >= 1) e.dead = true;
            }
            // Schedule fire events across the path.
            if (game && !e.flee && e._fired < e.fireCount) {
                var nextFireU = e.fireAt + (e._fired * 0.12);
                if (u >= nextFireU) {
                    game.spawnEnemyBolt(e.x, e.y, e.z);
                    e._fired++;
                }
            }
            return;
        }

        if (e.kind === "fireball") {
            e.z += e.vz * dt;
            if (e.z < N.Render.NEAR_Z) {
                // Made contact — deal damage and vanish.
                if (game) game.onFireballImpact(e);
                e.dead = true;
            }
        }
    }

    // --- Draw dispatch -----------------------------------------------------
    function draw(e, ctx) {
        if (e.kind === "fighter") {
            var v = _xform(FIGHTER_VERTS, e.yaw, e.pitch, e.scale);
            drawMesh(ctx, v, FIGHTER_EDGES, e.x, e.y, e.z, e.color);
            return;
        }
        if (e.kind === "ace") {
            var v2 = _xform(ACE_VERTS, e.yaw, e.pitch, e.scale);
            drawMesh(ctx, v2, ACE_EDGES, e.x, e.y, e.z, e.color);
            return;
        }
        if (e.kind === "fireball") {
            var pulse = 1 + 0.15 * Math.sin(e.t * 0.02);
            var r = e.scale * pulse;
            var verts = [
                { x: 0, y: r, z: 0 },  { x: 0, y: -r, z: 0 },
                { x: r, y: 0, z: 0 },  { x: -r, y: 0, z: 0 },
                { x: 0, y: 0, z: r },  { x: 0, y: 0, z: -r }
            ];
            var E = [[0,2],[0,3],[0,4],[0,5],[1,2],[1,3],[1,4],[1,5],
                     [2,4],[4,3],[3,5],[5,2]];
            drawMesh(ctx, verts, E, e.x, e.y, e.z, e.color);
        }
    }

    // --- Path factories ----------------------------------------------------
    // A swooping path: enters from a point near the Death-Star horizon,
    // arcs past the player, and exits off-screen. Cubic between three
    // waypoints: start (far, offset), pass (close, near player), end (behind).
    function swoopPath(startX, startY, passX, passY, endX, endY) {
        var s = { x: startX, y: startY, z: 260 };
        var p = { x: passX,  y: passY,  z: 35  };
        var e = { x: endX,   y: endY,   z: -40 };
        return function(u) {
            // Quadratic bezier (s, p, e).
            var iu = 1 - u;
            return {
                x: iu*iu*s.x + 2*iu*u*p.x + u*u*e.x,
                y: iu*iu*s.y + 2*iu*u*p.y + u*u*e.y,
                z: iu*iu*s.z + 2*iu*u*p.z + u*u*e.z
            };
        };
    }

    // Fly-by that arcs from one side to the other without getting close.
    // Used for "strafing run" enemies.
    function arcPath(startX, startY, endX, endY, passZ) {
        var s = { x: startX, y: startY, z: 220 };
        var p = { x: (startX + endX) * 0.5, y: (startY + endY) * 0.5 + 10, z: passZ || 60 };
        var e = { x: endX,   y: endY,   z: -20 };
        return function(u) {
            var iu = 1 - u;
            return {
                x: iu*iu*s.x + 2*iu*u*p.x + u*u*e.x,
                y: iu*iu*s.y + 2*iu*u*p.y + u*u*e.y,
                z: iu*iu*s.z + 2*iu*u*p.z + u*u*e.z
            };
        };
    }

    // Random swoop generator. Waypoints sized for the visible cone so
    // the enemy passes through screen space rather than the margins.
    // (Half-width ≈ 0.77·z, half-height ≈ 0.58·z at vfov=60 on 4:3.)
    function randomSwoop() {
        var side = Math.random() < 0.5 ? -1 : 1;
        var startX = side * (40 + Math.random() * 30);        // at z≈260: ~15-30% of cone
        var startY = (Math.random() * 2 - 1) * 30;
        var passX  = (Math.random() * 2 - 1) * 14;            // at z≈35: inside reticle envelope
        var passY  = (Math.random() * 2 - 1) * 10;
        var endX   = -side * (25 + Math.random() * 20);
        var endY   = startY + (Math.random() * 2 - 1) * 15;
        return swoopPath(startX, startY, passX, passY, endX, endY);
    }

    // --- Surface tower (tall, fires tracking bolts upward at the player) ---
    // Origin at the base; height extends upward. Glowing top is the scoring
    // hit zone.
    function createTower(opts) {
        opts = opts || {};
        var h = opts.h || 14;
        return {
            kind: "tower",
            x: opts.x || 0, y: opts.y || 0, z: opts.z || 300,
            height: h,
            radius: 3.5,
            hp: 1, score: 300,
            color: "#6a6",
            topColor: "#ff6",
            dead: false,
            fireCooldown: 900 + Math.random() * 1200,
            fireVariance: 700 + Math.random() * 600
        };
    }

    function drawTower(ctx, e) {
        var h = e.height;
        var verts = [
            // Square base (0..3) and top (4..7)
            { x: -1, y: 0, z: -1 }, { x:  1, y: 0, z: -1 },
            { x:  1, y: 0, z:  1 }, { x: -1, y: 0, z:  1 },
            { x: -0.6, y: h, z: -0.6 }, { x: 0.6, y: h, z: -0.6 },
            { x:  0.6, y: h, z:  0.6 }, { x: -0.6, y: h, z:  0.6 }
        ];
        var E = [
            [0,1],[1,2],[2,3],[3,0],           // base
            [4,5],[5,6],[6,7],[7,4],           // top
            [0,4],[1,5],[2,6],[3,7]            // posts
        ];
        N.Render.edges(ctx, verts, E, e.color, 1, { ox: e.x, oy: e.y, oz: e.z });
        // Glowing top — cross of lines inside the top square.
        var topVerts = [
            { x: -0.6, y: h, z: -0.6 }, { x:  0.6, y: h, z:  0.6 },
            { x:  0.6, y: h, z: -0.6 }, { x: -0.6, y: h, z:  0.6 }
        ];
        N.Render.edges(ctx, topVerts, [[0,1],[2,3]], e.topColor, 1,
            { ox: e.x, oy: e.y, oz: e.z });
    }

    // --- Bunker (low squat structure, non-firing, lower score) -------------
    function createBunker(opts) {
        opts = opts || {};
        return {
            kind: "bunker",
            x: opts.x || 0, y: opts.y || 0, z: opts.z || 300,
            radius: 2.2,
            hp: 1, score: 50,
            color: "#5a5",
            dead: false
        };
    }

    function drawBunker(ctx, e) {
        var verts = [
            { x: -2, y: 0,   z: -1.2 }, { x:  2, y: 0,   z: -1.2 },
            { x:  2, y: 0,   z:  1.2 }, { x: -2, y: 0,   z:  1.2 },
            { x: -1.5, y: 1.8, z: -1.0 }, { x: 1.5, y: 1.8, z: -1.0 },
            { x:  1.5, y: 1.8, z:  1.0 }, { x: -1.5, y: 1.8, z:  1.0 }
        ];
        var E = [
            [0,1],[1,2],[2,3],[3,0],
            [4,5],[5,6],[6,7],[7,4],
            [0,4],[1,5],[2,6],[3,7]
        ];
        N.Render.edges(ctx, verts, E, e.color, 1, { ox: e.x, oy: e.y, oz: e.z });
    }

    // --- Catwalk (horizontal hazard — collides with ship if in the way) ----
    // A rigid pipe spanning two towers at ship altitude. Non-destructible
    // structurally (you can shoot the towers, but the walkway itself is
    // bluntly collidable and hurts you if you clip it).
    function createCatwalk(opts) {
        opts = opts || {};
        return {
            kind: "catwalk",
            x: opts.x || 0, y: opts.y || 6, z: opts.z || 300,
            spanX: opts.spanX || 10,   // half-width
            radius: 4.0,               // bounding sphere for general culling
            hp: 99, score: 0,          // not really destructible
            color: "#a84",
            dead: false,
            hasHit: false              // one-hit-per-pass
        };
    }

    function drawCatwalk(ctx, e) {
        var s = e.spanX;
        var verts = [
            { x: -s, y: 0, z: -0.4 }, { x: s, y: 0, z: -0.4 },
            { x:  s, y: 0, z:  0.4 }, { x: -s, y: 0, z:  0.4 },
            { x: -s, y: 1, z: -0.4 }, { x: s, y: 1, z: -0.4 },
            { x:  s, y: 1, z:  0.4 }, { x: -s, y: 1, z:  0.4 }
        ];
        var E = [
            [0,1],[1,2],[2,3],[3,0],
            [4,5],[5,6],[6,7],[7,4],
            [0,4],[1,5],[2,6],[3,7]
        ];
        N.Render.edges(ctx, verts, E, e.color, 1, { ox: e.x, oy: e.y, oz: e.z });
    }

    // --- Unified update/draw for surface features ------------------------
    function updateSurface(e, dt, game) {
        // Surface features scroll backward (toward -Z) each frame. Fixed
        // rate — synced with the ground grid so they feel locked to terrain.
        e.z -= (game.getRailSpeed ? game.getRailSpeed() : 0.07) * dt;
        if (e.z < -30) { e.dead = true; return; }

        if (e.kind === "tower") {
            e.fireCooldown -= dt;
            if (e.fireCooldown <= 0 && e.z < 180 && e.z > 20) {
                // Fire from the tower top toward the player.
                game.spawnEnemyBolt(e.x, e.y + e.height, e.z);
                e.fireCooldown = e.fireVariance + Math.random() * 800;
            }
        } else if (e.kind === "catwalk") {
            // When passing the ship, test if the ship is within the walkway's
            // path (x range, y range).
            if (!e.hasHit && e.z > 0 && e.z < 4) {
                var ship = game.getShip();
                var dx = ship.x - e.x;
                if (Math.abs(dx) < e.spanX && Math.abs(ship.y - e.y) < 2.2) {
                    e.hasHit = true;
                    game.takeDamage(1);
                    if (game.radio) game.radio("CATWALK IMPACT", 900);
                }
            }
        }
    }

    function drawSurfaceFeature(ctx, e) {
        if (e.kind === "tower")   drawTower(ctx, e);
        else if (e.kind === "bunker")  drawBunker(ctx, e);
        else if (e.kind === "catwalk") drawCatwalk(ctx, e);
    }

    // Patch the generic update/draw dispatch to route surface kinds too.
    var origUpdate = update;
    update = function(e, dt, game) {
        if (e.kind === "tower" || e.kind === "bunker" || e.kind === "catwalk") {
            updateSurface(e, dt, game);
            return;
        }
        origUpdate(e, dt, game);
    };
    var origDraw = draw;
    draw = function(e, ctx) {
        if (e.kind === "tower" || e.kind === "bunker" || e.kind === "catwalk") {
            drawSurfaceFeature(ctx, e);
            return;
        }
        origDraw(e, ctx);
    };

    // --- Trench pylon (vertical column, blocks a lane) --------------------
    function createPylon(opts) {
        opts = opts || {};
        return {
            kind: "pylon",
            x: opts.x || 0, y: 0, z: opts.z || 300,
            radius: 2.0,
            halfWidth: opts.halfWidth || 1.6,
            hp: 2, score: 100,           // tough — shoot it if in the way
            color: "#88f",
            dead: false,
            hasHit: false
        };
    }
    function drawPylon(ctx, e) {
        var hw = e.halfWidth;
        var verts = [
            { x: -hw, y: -10, z: -0.5 }, { x: hw, y: -10, z: -0.5 },
            { x:  hw, y: -10, z:  0.5 }, { x: -hw, y: -10, z:  0.5 },
            { x: -hw, y:  10, z: -0.5 }, { x: hw, y:  10, z: -0.5 },
            { x:  hw, y:  10, z:  0.5 }, { x: -hw, y:  10, z:  0.5 }
        ];
        var E = [[0,1],[1,2],[2,3],[3,0],[4,5],[5,6],[6,7],[7,4],[0,4],[1,5],[2,6],[3,7]];
        N.Render.edges(ctx, verts, E, e.color, 1, { ox: e.x, oy: 0, oz: e.z });
    }

    // --- Trench turret (wall-mounted, fires across the trench) ------------
    function createTurret(opts) {
        opts = opts || {};
        return {
            kind: "turret",
            x: opts.x || 0, y: opts.y || 0, z: opts.z || 300,
            side: opts.side || 1,       // +1 right wall, -1 left wall
            radius: 1.8,
            hp: 1, score: 50,
            color: "#f88",
            dead: false,
            fireCooldown: 700 + Math.random() * 1100
        };
    }
    function drawTurret(ctx, e) {
        // Small angular mount sitting flush against the wall.
        var side = e.side;
        var verts = [
            { x: 0,         y: -0.8, z: -0.8 }, { x: 0,         y: -0.8, z: 0.8 },
            { x: 0,         y:  0.8, z:  0.8 }, { x: 0,         y:  0.8, z: -0.8 },
            { x: -side*1.4, y: 0,    z:  0 }
        ];
        var E = [[0,1],[1,2],[2,3],[3,0],[0,4],[1,4],[2,4],[3,4]];
        N.Render.edges(ctx, verts, E, e.color, 1, { ox: e.x, oy: e.y, oz: e.z });
    }

    // --- Exhaust port (the reactor vent — the objective) ------------------
    function createPort(opts) {
        opts = opts || {};
        return {
            kind: "port",
            x: opts.x || 0, y: opts.y || 0, z: opts.z || 420,
            radius: 2.8,
            innerRadius: 1.0,      // bullseye hit tolerance
            hp: 1, score: 25000,
            color: "#fe8",
            dead: false,
            resolved: false        // true after the hit-window passes
        };
    }
    function drawPort(ctx, e) {
        // Concentric squares — outer frame and inner target. Sizes scale
        // with the port's runtime radius so higher loops look as tight as
        // they play.
        var outer = e.radius * 0.78;
        var inner = e.innerRadius * 0.9;
        var o = [
            { x: -outer, y:  outer, z: 0 }, { x:  outer, y:  outer, z: 0 },
            { x:  outer, y: -outer, z: 0 }, { x: -outer, y: -outer, z: 0 }
        ];
        var i = [
            { x: -inner, y:  inner, z: 0 }, { x:  inner, y:  inner, z: 0 },
            { x:  inner, y: -inner, z: 0 }, { x: -inner, y: -inner, z: 0 }
        ];
        var E = [[0,1],[1,2],[2,3],[3,0]];
        N.Render.edges(ctx, o, E, e.color, 1, { ox: e.x, oy: e.y, oz: e.z });
        N.Render.edges(ctx, i, E, "#fff", 1, { ox: e.x, oy: e.y, oz: e.z });
        // Crosshair across the inner.
        N.Render.line(ctx,
            e.x - outer, e.y, e.z, e.x + outer, e.y, e.z, e.color, 0.6);
        N.Render.line(ctx,
            e.x, e.y - outer, e.z, e.x, e.y + outer, e.z, e.color, 0.6);
    }

    function updateTrench(e, dt, game) {
        e.z -= (game.getRailSpeed ? game.getRailSpeed() : 0.07) * dt;
        if (e.z < -30) { e.dead = true; return; }

        if (e.kind === "pylon") {
            if (!e.hasHit && e.z > 0 && e.z < 3) {
                var ship = game.getShip();
                if (Math.abs(ship.x - e.x) < e.halfWidth + 2.0) {
                    e.hasHit = true;
                    game.takeDamage(1);
                    if (game.radio) game.radio("PYLON IMPACT", 900);
                }
            }
        } else if (e.kind === "turret") {
            e.fireCooldown -= dt;
            if (e.fireCooldown <= 0 && e.z > 12 && e.z < 180) {
                game.spawnEnemyBolt(e.x, e.y, e.z);
                e.fireCooldown = 900 + Math.random() * 900;
            }
        } else if (e.kind === "port") {
            // Port is the objective. When within lock range, game state
            // shows lockActive. When it passes without being hit, resolve
            // as a miss and end the wave.
            if (!e.resolved) {
                if (e.z < 90 && e.z > 8) game._portLock = true;
                if (e.z < 6) {
                    // Port passed — if not hit, it's a miss.
                    e.resolved = true;
                    if (game.onPortMiss) game.onPortMiss();
                }
            }
        }
    }

    function drawTrenchFeature(ctx, e) {
        if (e.kind === "pylon")       drawPylon(ctx, e);
        else if (e.kind === "turret") drawTurret(ctx, e);
        else if (e.kind === "port")   drawPort(ctx, e);
    }

    // Extend update/draw dispatch for trench kinds.
    var priorUpdate = update;
    update = function(e, dt, game) {
        if (e.kind === "pylon" || e.kind === "turret" || e.kind === "port") {
            updateTrench(e, dt, game);
            return;
        }
        priorUpdate(e, dt, game);
    };
    var priorDraw = draw;
    draw = function(e, ctx) {
        if (e.kind === "pylon" || e.kind === "turret" || e.kind === "port") {
            drawTrenchFeature(ctx, e);
            return;
        }
        priorDraw(e, ctx);
    };

    return {
        createFighter: createFighter,
        createAce: createAce,
        createFireball: createFireball,
        createTower: createTower,
        createBunker: createBunker,
        createCatwalk: createCatwalk,
        createPylon: createPylon,
        createTurret: createTurret,
        createPort: createPort,
        update: function(e, dt, g) { return update(e, dt, g); },
        draw:   function(e, ctx)   { return draw(e, ctx); },
        swoopPath: swoopPath,
        arcPath: arcPath,
        randomSwoop: randomSwoop
    };
})();
