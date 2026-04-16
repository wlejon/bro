// scene_setup.js — 3D scene scaffold for the arena: ground, translucent
// boundary walls, obstacle boxes, capsule units. Per-frame `update()` reads
// each agent's x/z/yaw and writes it to the matching capsule node. Camera is
// an orbit rig tilted MOBA-style with right-drag to rotate and wheel to zoom.
//
// Phase 1: visuals only. AI still runs through the old loop; Scene3D just
// mirrors agent state onto scene nodes. Projectiles / FX / gizmos land in
// later phases.
var Scene3D = {};
(function () {
    "use strict";

    Scene3D.scene = null;
    Scene3D.canvas = null;
    Scene3D.cam = null;
    Scene3D.ground = null;
    Scene3D.walls = [];
    Scene3D.obstacles = [];
    Scene3D.units = {};                  // agentId → capsule node
    Scene3D.unitFovs = {};               // agentId → faint per-unit FOV mesh
    Scene3D.projPool = [[], []];         // [team] → sphere nodes (pooled)
    Scene3D.explosionPool = [];          // { node, t, maxT, r } entries
    Scene3D.dmgOverlay = null;           // HTML container for floating #s
    Scene3D.dmgDivs = [];                // pool of reusable divs
    Scene3D.gizmos = {                   // focused-unit worldspace gizmos
        focusRing: null, rangeRing: null, fovCone: null,
        targetLine: null, intentDiv: null,
    };

    Scene3D.UNIT_Y = 0.9;    // capsule center height (radius + halfHeight)
    var CAPSULE_R = 0.4;
    var CAPSULE_HALF_H = 0.5;
    var WALL_H = 2.5;
    var WALL_THICK = 0.4;
    var OBSTACLE_H = 1.8;

    Scene3D.init = function (canvas) {
        Scene3D.canvas = canvas;
        Scene3D.scene = canvas.getContext("scene");
        Scene3D.dmgOverlay = document.getElementById("dmg-overlay");
        buildGizmos();

        Scene3D.cam = Camera.createOrbit({
            pivot: [0, 0, 0],
            dist: 58,
            fov: 45,
            // Pitch the camera down ~55° for a MOBA-ish isometric view.
            // Negative angle around +X tilts camera above the pivot looking
            // down; positive would flip it under the ground plane.
            rot: quatFromAxisAngle(1, 0, 0, -0.95),
        });
        Scene3D.applyCamera();
        wireCameraInput(canvas);
        wireClickFocus(canvas);
    };

    // Left-click on a unit capsule → focus it. Tolerates small drag (6px).
    // Uses screen-space nearest-hit against projected unit positions rather
    // than full ray casting — good enough for capsules at this zoom.
    function wireClickFocus(canvas) {
        var downX = 0, downY = 0, armed = false;
        canvas.addEventListener("mousedown", function (ev) {
            if (ev.button !== 0) return;
            armed = true; downX = ev.clientX; downY = ev.clientY;
        });
        canvas.addEventListener("mouseup", function (ev) {
            if (ev.button !== 0 || !armed) return;
            armed = false;
            if (Math.hypot(ev.clientX - downX, ev.clientY - downY) > 6) return;
            var state = (typeof App !== "undefined") ? App.state : null;
            if (!state) return;
            var rect = canvas.getBoundingClientRect();
            var cx = ev.clientX - rect.left;
            var cy = ev.clientY - rect.top;
            var best = null, bestD = 40; // px radius of effective hit
            for (var i = 0; i < state.agents.length; i++) {
                var a = state.agents[i];
                if (!a.unit.alive) continue;
                var sp = Scene3D.projectToScreen(a.x, Scene3D.UNIT_Y, a.z);
                if (!sp) continue;
                var d = Math.hypot(sp.x - cx, sp.y - cy);
                if (d < bestD) { bestD = d; best = a; }
            }
            if (!best) return;
            state.focusId = best.unit.id;
            var sel = document.getElementById("sel-focus");
            if (sel) sel.value = String(best.unit.id);
        });
    }

    // Duplicated from apps/lib/camera.js (not exported there). Small enough
    // to inline rather than re-export.
    function quatFromAxisAngle(ax, ay, az, angle) {
        var s = Math.sin(angle * 0.5), c = Math.cos(angle * 0.5);
        return [ax * s, ay * s, az * s, c];
    }

    Scene3D.applyCamera = function () {
        Scene3D.scene.setCamera(Camera.orbitViewOpts(Scene3D.cam, Scene3D.canvas));
    };

    function wireCameraInput(canvas) {
        var dragging = false, lastX = 0, lastY = 0;
        var panning = false;

        canvas.addEventListener("mousedown", function (ev) {
            if (ev.button === 2)      { dragging = true; panning = false; }
            else if (ev.button === 1) { dragging = true; panning = true; }
            else return;
            lastX = ev.clientX; lastY = ev.clientY;
            ev.preventDefault();
        });
        window.addEventListener("mouseup", function () { dragging = false; });
        window.addEventListener("mousemove", function (ev) {
            if (!dragging) return;
            var dx = ev.clientX - lastX, dy = ev.clientY - lastY;
            lastX = ev.clientX; lastY = ev.clientY;
            if (panning) Camera.orbitPan(Scene3D.cam, dx, dy);
            else         Camera.orbitLook(Scene3D.cam, dx, dy);
            Scene3D.applyCamera();
        });
        canvas.addEventListener("wheel", function (ev) {
            var step = Math.sign(ev.deltaY) * Math.max(1, Scene3D.cam.dist * 0.1);
            Scene3D.cam.dist = Math.max(8, Math.min(120, Scene3D.cam.dist + step));
            Scene3D.applyCamera();
            ev.preventDefault();
        }, { passive: false });
        canvas.addEventListener("contextmenu", function (ev) { ev.preventDefault(); });
    }

    function destroyList(list) {
        for (var i = 0; i < list.length; i++) list[i].destroy();
        list.length = 0;
    }
    function destroyMap(map) {
        for (var k in map) if (map[k]) map[k].destroy();
        for (var k2 in map) delete map[k2];
    }

    Scene3D.destroy = function () {
        if (Scene3D.ground) { Scene3D.ground.destroy(); Scene3D.ground = null; }
        destroyList(Scene3D.walls);
        destroyList(Scene3D.obstacles);
        destroyMap(Scene3D.units);
        destroyMap(Scene3D.unitFovs);
        for (var t = 0; t < Scene3D.projPool.length; t++) {
            destroyList(Scene3D.projPool[t]);
        }
        for (var e = 0; e < Scene3D.explosionPool.length; e++) {
            Scene3D.explosionPool[e].node.destroy();
        }
        Scene3D.explosionPool.length = 0;
    };

    Scene3D.build = function (scenario) {
        Scene3D.destroy();
        var B = scenario.bounds;
        var spanX = B.maxX - B.minX;
        var spanZ = B.maxZ - B.minZ;

        // Ground — matte grey plane sized to the scenario bounds.
        Scene3D.ground = Scene3D.scene.createMesh({
            mesh: "plane",
            halfW: spanX / 2, halfD: spanZ / 2,
            color: [0.14, 0.16, 0.18, 1.0],
            x: (B.minX + B.maxX) / 2, y: 0, z: (B.minZ + B.maxZ) / 2,
            name: "ground",
        });

        // Boundary walls — translucent (alpha 0.3) so the arena reads as a
        // walled box without occluding the action.
        var wallColor = [0.85, 0.92, 1.0, 0.3];
        var wy = WALL_H / 2;
        function makeWall(x, z, hw, hh, hd) {
            return Scene3D.scene.createMesh({
                mesh: "box", halfW: hw, halfH: hh, halfD: hd,
                color: wallColor, x: x, y: wy, z: z, name: "wall",
            });
        }
        Scene3D.walls.push(makeWall(
            (B.minX + B.maxX) / 2, B.minZ - WALL_THICK,
            spanX / 2 + WALL_THICK, WALL_H / 2, WALL_THICK));
        Scene3D.walls.push(makeWall(
            (B.minX + B.maxX) / 2, B.maxZ + WALL_THICK,
            spanX / 2 + WALL_THICK, WALL_H / 2, WALL_THICK));
        Scene3D.walls.push(makeWall(
            B.minX - WALL_THICK, (B.minZ + B.maxZ) / 2,
            WALL_THICK, WALL_H / 2, spanZ / 2 + WALL_THICK));
        Scene3D.walls.push(makeWall(
            B.maxX + WALL_THICK, (B.minZ + B.maxZ) / 2,
            WALL_THICK, WALL_H / 2, spanZ / 2 + WALL_THICK));

        // Obstacle boxes — opaque dark; height arbitrary since sim is 2D.
        for (var i = 0; i < scenario.obstacles.length; i++) {
            var o = scenario.obstacles[i];
            var node = Scene3D.scene.createMesh({
                mesh: "box",
                halfW: o.hw, halfH: OBSTACLE_H / 2, halfD: o.hd,
                color: [0.05, 0.06, 0.07, 1.0],
                x: o.x, y: OBSTACLE_H / 2, z: o.z,
                name: "obstacle",
            });
            Scene3D.obstacles.push(node);
        }

        // Capsule per roster entry + a faint team-colored FOV cone pinned to
        // each one. Capsules get replaced with animated meshes later.
        var fovMesh = buildFovMesh(1.0);
        for (var j = 0; j < scenario.roster.length; j++) {
            var r = scenario.roster[j];
            var c = r.teamId === 0 ? [0.90, 0.30, 0.24, 1.0]
                                   : [0.20, 0.60, 0.85, 1.0];
            var node2 = Scene3D.scene.createMesh({
                mesh: "capsule",
                radius: CAPSULE_R, halfHeight: CAPSULE_HALF_H,
                color: c,
                x: r.x, y: Scene3D.UNIT_Y, z: r.z,
                name: "unit-" + r.id,
            });
            Scene3D.units[r.id] = node2;

            var fovCol = r.teamId === 0 ? [0.95, 0.35, 0.28, 0.10]
                                        : [0.28, 0.66, 0.95, 0.10];
            Scene3D.unitFovs[r.id] = Scene3D.scene.createMesh({
                positions: fovMesh.positions,
                indices: fovMesh.indices,
                normals: fovMesh.normals,
                color: fovCol,
                emissive: 0.15,
                name: "unit-fov-" + r.id,
            });
        }
    };

    // The AgentBinding writes position + rotation each frame. We only manage
    // visibility (so fallen units don't clutter the battlefield) and drive
    // projectile / explosion / damage-number visuals.
    Scene3D.update = function (state, dt) {
        var agents = state.agents;
        for (var i = 0; i < agents.length; i++) {
            var a = agents[i];
            var node = Scene3D.units[a.unit.id];
            if (!node) continue;
            node.visible = !!a.unit.alive;
        }
        syncProjectiles(state.world.projectiles);
        syncExplosions(dt);
        syncDamageNumbers();
        syncUnitFovs(state);
        syncGizmos(state);
    };

    // Per-unit faint FOV triangles — one per roster entry, hidden when dead
    // or when the unit is the focused one (the bright gold gizmo covers it).
    function syncUnitFovs(state) {
        var focusId = state.focusId;
        for (var i = 0; i < state.agents.length; i++) {
            var a = state.agents[i];
            var cone = Scene3D.unitFovs[a.unit.id];
            if (!cone) continue;
            if (!a.unit.alive || a.unit.id === focusId) { cone.visible = false; continue; }
            var range = a.unit.attackRange || 9;
            cone.visible = true;
            cone.x = a.x; cone.y = 0.03; cone.z = a.z;
            cone.scaleX = range; cone.scaleZ = range;
            var mem = AI.memory[a.unit.id];
            var aimYaw;
            if (mem && mem.aim) {
                var f = BotAim.forward(mem.aim);
                aimYaw = Math.atan2(f.x, -f.z);
            } else aimYaw = a.yaw;
            cone.rotationY = -aimYaw;
        }
    }

    // ─── Projectiles ──────────────────────────────────────────────────
    // One pool per team. Ability projectiles are team-colored too — a
    // slight readability loss vs. per-kind color but avoids per-projectile
    // node churn and there's no runtime mesh-color setter today.
    var PROJ_COLORS = [
        [1.00, 0.45, 0.35, 1.0],   // red team
        [0.40, 0.75, 1.00, 1.0],   // blue team
    ];

    function growProjPool(team, to) {
        var pool = Scene3D.projPool[team];
        while (pool.length < to) {
            pool.push(Scene3D.scene.createMesh({
                mesh: "sphere", radius: 0.15, segments: 10, rings: 6,
                color: PROJ_COLORS[team],
                emissive: 0.8,
                name: "proj-" + team,
            }));
            pool[pool.length - 1].visible = false;
        }
    }
    function syncProjectiles(projs) {
        var counts = [0, 0];
        for (var i = 0; i < projs.length; i++) {
            var p = projs[i];
            var team = p.teamId === 1 ? 1 : 0;
            growProjPool(team, counts[team] + 1);
            var node = Scene3D.projPool[team][counts[team]];
            node.visible = true;
            node.x = p.x; node.y = 1.1; node.z = p.z;
            // Grenades / pierces get a bigger reticle so they're legible.
            var s = (p.mode === "aoe") ? 2.5 : (p.mode === "pierce" ? 1.6 : 1.0);
            node.scaleX = s; node.scaleY = s; node.scaleZ = s;
            counts[team]++;
        }
        for (var t = 0; t < 2; t++) {
            var pool = Scene3D.projPool[t];
            for (var j = counts[t]; j < pool.length; j++) pool[j].visible = false;
        }
    }

    // ─── Explosions ───────────────────────────────────────────────────
    // Each Render.fx.rings entry borrows a sphere node; the node expands +
    // fades by scaling since emissive/color isn't runtime-mutable.
    function growExplosionPool() {
        var node = Scene3D.scene.createMesh({
            mesh: "sphere", radius: 0.5, segments: 14, rings: 8,
            color: [1.0, 0.55, 0.15, 0.6],
            emissive: 0.9,
            name: "explosion",
        });
        node.visible = false;
        Scene3D.explosionPool.push({ node: node, inUse: false });
        return Scene3D.explosionPool[Scene3D.explosionPool.length - 1];
    }
    function acquireExplosion() {
        for (var i = 0; i < Scene3D.explosionPool.length; i++) {
            if (!Scene3D.explosionPool[i].inUse) return Scene3D.explosionPool[i];
        }
        return growExplosionPool();
    }
    function syncExplosions() {
        // Release any explosion nodes no longer referenced. We re-bind from
        // Render.fx.rings every frame — simpler than tracking a mapping.
        for (var i = 0; i < Scene3D.explosionPool.length; i++) {
            Scene3D.explosionPool[i].inUse = false;
            Scene3D.explosionPool[i].node.visible = false;
        }
        var rings = Render.fx.rings;
        for (var r = 0; r < rings.length; r++) {
            var ring = rings[r];
            var e = acquireExplosion();
            e.inUse = true;
            var frac = Math.min(1, ring.t / ring.maxT);
            var scale = (ring.r * (0.3 + frac * 2.5));
            e.node.x = ring.x; e.node.y = 1.0; e.node.z = ring.z;
            e.node.scaleX = scale; e.node.scaleY = scale; e.node.scaleZ = scale;
            e.node.visible = true;
        }
    }

    // ─── Damage numbers (HTML overlay) ────────────────────────────────
    function ensureDmgDiv(i) {
        while (Scene3D.dmgDivs.length <= i) {
            var d = document.createElement("div");
            d.className = "dmg";
            Scene3D.dmgOverlay.appendChild(d);
            Scene3D.dmgDivs.push(d);
        }
        return Scene3D.dmgDivs[i];
    }
    function syncDamageNumbers() {
        var floats = Render.fx.floats;
        for (var i = 0; i < floats.length; i++) {
            var f = floats[i];
            // Float rises over its lifetime.
            var y = 1.8 + f.t * 1.2;
            var sp = Scene3D.projectToScreen(f.x, y, f.z);
            var div = ensureDmgDiv(i);
            if (!sp) { div.style.display = "none"; continue; }
            div.style.display = "block";
            div.style.left = sp.x + "px";
            div.style.top  = sp.y + "px";
            div.style.color = f.color;
            div.style.opacity = String(Math.max(0, 1 - f.t));
            if (div.textContent !== f.text) div.textContent = f.text;
        }
        for (var j = floats.length; j < Scene3D.dmgDivs.length; j++) {
            Scene3D.dmgDivs[j].style.display = "none";
        }
    }

    // ─── Focused-unit gizmos ──────────────────────────────────────────
    // One of each; hidden when there's no live focus. Transforms are
    // rewritten each frame off the focused agent's state.
    var FOV = Math.PI / 2.2;

    function buildFovMesh(range) {
        var half = FOV / 2;
        // Local frame: forward = -Z (FPS convention). Triangle fan is on
        // XZ plane, apex at origin.
        var sR = Math.sin(+half) * range, cR = -Math.cos(+half) * range;
        var sL = Math.sin(-half) * range, cL = -Math.cos(-half) * range;
        return {
            positions: new Float32Array([
                0, 0, 0,
                sR, 0, cR,
                sL, 0, cL,
            ]),
            // CCW when viewed from +Y so the +Y normal is the visible face.
            indices: new Uint32Array([0, 1, 2]),
            normals: new Float32Array([0,1,0, 0,1,0, 0,1,0]),
        };
    }

    function buildGizmos() {
        var g = Scene3D.gizmos;
        g.focusRing = Scene3D.scene.createMesh({
            mesh: "torus", majorRadius: 0.7, minorRadius: 0.07,
            majorSegments: 28, minorSegments: 8,
            color: [1.0, 0.82, 0.29, 1.0],
            emissive: 0.9,
            name: "gizmo-focus",
        });
        g.rangeRing = Scene3D.scene.createMesh({
            mesh: "torus", majorRadius: 1.0, minorRadius: 0.04,
            majorSegments: 48, minorSegments: 6,
            color: [1.0, 0.82, 0.29, 0.35],
            emissive: 0.2,
            name: "gizmo-range",
        });
        // FOV triangle is built at attackRange=1 then scaled per-frame to
        // the agent's actual range.
        var mesh = buildFovMesh(1.0);
        g.fovCone = Scene3D.scene.createMesh({
            positions: mesh.positions,
            indices: mesh.indices,
            normals: mesh.normals,
            color: [1.0, 0.82, 0.29, 0.28],
            emissive: 0.35,
            name: "gizmo-fov",
        });
        // A thin unit-length box (halfW=0.5 is default for "box"). We scaleX
        // to span the A→B distance and rotateY to point.
        g.targetLine = Scene3D.scene.createMesh({
            mesh: "box", halfW: 0.5, halfH: 0.04, halfD: 0.04,
            color: [0.30, 0.86, 0.47, 0.8],
            emissive: 0.6,
            name: "gizmo-target",
        });
        g.focusRing.visible = false;
        g.rangeRing.visible = false;
        g.fovCone.visible = false;
        g.targetLine.visible = false;

        g.intentDiv = document.createElement("div");
        g.intentDiv.className = "dmg";
        g.intentDiv.style.color = "#ffd24a";
        g.intentDiv.style.display = "none";
        Scene3D.dmgOverlay.appendChild(g.intentDiv);
    }

    function syncGizmos(state) {
        var g = Scene3D.gizmos;
        var focus = state.byId[state.focusId];
        if (!focus || !focus.unit.alive) {
            g.focusRing.visible = false;
            g.rangeRing.visible = false;
            g.fovCone.visible = false;
            g.targetLine.visible = false;
            g.intentDiv.style.display = "none";
            return;
        }
        var mem = AI.memory[focus.unit.id];
        var range = focus.unit.attackRange || 9;

        // Focus ring at the capsule base.
        g.focusRing.visible = true;
        g.focusRing.x = focus.x; g.focusRing.y = 0.02; g.focusRing.z = focus.z;
        g.focusRing.rotationX = Math.PI / 2; // lay flat on XZ

        // Range ring — torus's majorRadius=1, scale XZ by range.
        g.rangeRing.visible = true;
        g.rangeRing.x = focus.x; g.rangeRing.y = 0.02; g.rangeRing.z = focus.z;
        g.rangeRing.rotationX = Math.PI / 2;
        g.rangeRing.scaleX = range;
        g.rangeRing.scaleZ = range;

        // FOV cone — rotate to match aim (BotAim.forward if available, else
        // agent yaw). FOV mesh was built for range=1, so scale to actual range.
        g.fovCone.visible = true;
        g.fovCone.x = focus.x; g.fovCone.y = 0.04; g.fovCone.z = focus.z;
        g.fovCone.scaleX = range;
        g.fovCone.scaleZ = range;
        var aimYaw;
        if (mem && mem.aim) {
            var f = BotAim.forward(mem.aim);
            aimYaw = Math.atan2(f.x, -f.z);
        } else aimYaw = focus.yaw;
        // Node rotation is CCW around +Y; agent FPS yaw is CW, so negate.
        g.fovCone.rotationY = -aimYaw;

        // Target line — only if we know the current target and it's alive.
        var tid = mem && mem.targetId;
        var tgt = tid != null ? state.byId[tid] : null;
        if (tgt && tgt.unit.alive) {
            var dx = tgt.x - focus.x, dz = tgt.z - focus.z;
            var d = Math.hypot(dx, dz);
            if (d > 0.05) {
                var los = bro.ai.game.hasLineOfSight(
                    focus.x, focus.z, tgt.x, tgt.z, Arena.OBSTACLES);
                // No runtime color setter on meshes — hide line when no LOS
                // rather than maintaining two nodes.
                g.targetLine.visible = los;
                if (los) {
                    g.targetLine.x = (focus.x + tgt.x) / 2;
                    g.targetLine.y = 1.1;
                    g.targetLine.z = (focus.z + tgt.z) / 2;
                    g.targetLine.scaleX = d;
                    g.targetLine.rotationY = Math.atan2(-dz, dx);
                }
            } else g.targetLine.visible = false;
        } else g.targetLine.visible = false;

        // Intent label — float above capsule.
        if (mem && mem.intent) {
            var sp = Scene3D.projectToScreen(focus.x, 2.8, focus.z);
            if (sp) {
                g.intentDiv.style.display = "block";
                g.intentDiv.style.left = sp.x + "px";
                g.intentDiv.style.top = sp.y + "px";
                g.intentDiv.style.opacity = "1";
                if (g.intentDiv.textContent !== mem.intent) g.intentDiv.textContent = mem.intent;
            } else g.intentDiv.style.display = "none";
        } else g.intentDiv.style.display = "none";
    }

    // World (x, y, z) → canvas pixel coords; null if behind camera.
    Scene3D.projectToScreen = function (wx, wy, wz) {
        var opts = Camera.orbitViewOpts(Scene3D.cam, Scene3D.canvas);
        var ex = opts.position[0], ey = opts.position[1], ez = opts.position[2];
        var fx = opts.target[0] - ex, fy = opts.target[1] - ey, fz = opts.target[2] - ez;
        var fl = Math.hypot(fx, fy, fz) || 1;
        fx /= fl; fy /= fl; fz /= fl;
        var up = opts.up;
        // right = normalize(cross(forward, up))
        var rx = fy*up[2] - fz*up[1];
        var ry = fz*up[0] - fx*up[2];
        var rz = fx*up[1] - fy*up[0];
        var rl = Math.hypot(rx, ry, rz) || 1;
        rx /= rl; ry /= rl; rz /= rl;
        // trueUp = cross(right, forward)
        var ux = ry*fz - rz*fy;
        var uy = rz*fx - rx*fz;
        var uz = rx*fy - ry*fx;
        var dx = wx - ex, dy = wy - ey, dz = wz - ez;
        var xc = rx*dx + ry*dy + rz*dz;
        var yc = ux*dx + uy*dy + uz*dz;
        var zc = fx*dx + fy*dy + fz*dz; // positive = in front
        if (zc <= 0.01) return null;
        var tanHalf = Math.tan(opts.fov * Math.PI / 180 * 0.5);
        var aspect = opts.aspect;
        var ndcX = xc / (zc * aspect * tanHalf);
        var ndcY = yc / (zc * tanHalf);
        var w = Scene3D.canvas.clientWidth || Scene3D.canvas.width;
        var h = Scene3D.canvas.clientHeight || Scene3D.canvas.height;
        return {
            x: (ndcX + 1) * 0.5 * w,
            y: (1 - ndcY) * 0.5 * h,
        };
    };
})();
