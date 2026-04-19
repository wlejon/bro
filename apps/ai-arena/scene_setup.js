// scene_setup.js — 3D scene scaffold for the arena: ground, translucent
// boundary walls, obstacle boxes, capsule units. Per-frame `update()` reads
// each agent's x/z/yaw and writes it to the matching capsule node. Camera is
// an orbit rig tilted MOBA-style with right-drag to rotate and wheel to zoom.
//
// HP bars, intent labels, and floating damage numbers are world-anchored
// scene nodes (ShapeNode + HtmlNode) — no HTML overlay, no projectToScreen.
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
    Scene3D.hpBars = {};                 // agentId → { bg, fill, lastFrac, maxHp }
    Scene3D.projPool = [[], []];         // [team] → sphere nodes (pooled)
    Scene3D.explosionPool = [];          // { node, t, maxT, r } entries
    Scene3D.dmgPool = [];                // pool of HtmlNode floating-damage nodes
    Scene3D.gizmos = {                   // focused-unit worldspace gizmos
        focusRing: null, rangeRing: null, fovCone: null,
        targetLine: null, intentLabel: null,
    };

    Scene3D.UNIT_Y = 0.9;    // capsule center height (radius + halfHeight)
    var CAPSULE_R = 0.4;
    var CAPSULE_HALF_H = 0.5;
    var WALL_H = 2.5;
    var WALL_THICK = 0.4;
    var OBSTACLE_H = 1.8;

    // World-space HP bar dimensions. Rendered as billboards (ylock) so they
    // track the camera horizontally while always staying Y-up. Size is fixed
    // in world units — perspective handles zoom naturally.
    var HP_BAR_W = 1.3;
    var HP_BAR_H = 0.18;
    var HP_BAR_Y = 2.2;      // a bit above UNIT_Y + halfHeight + radius (1.8)

    Scene3D.init = function (canvas) {
        Scene3D.canvas = canvas;
        Scene3D.scene = canvas.getContext("scene");
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
        // First applyCamera() above runs before htmlayout has computed the
        // canvas's box, so clientWidth/Height fall back to the canvas
        // intrinsic 300×150 default (see element_bindings.cpp clientWidth
        // → js_element_get_width). That bakes aspect=2.0 into the camera
        // and the scene renders stretched until the next user input fires
        // applyCamera again. Re-apply after the first frame (layout done)
        // and on every window resize.
        requestAnimationFrame(Scene3D.applyCamera);
        window.addEventListener("resize", Scene3D.applyCamera);
        wireCameraInput(canvas);
        wireClickFocus(canvas);
    };

    // Left-click on a unit capsule → focus it. Tolerates small drag (6px).
    // Uses scene.raycast against the capsule meshes so the test works from
    // any camera angle.
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
            // Pick by nearest capsule world-position projected manually —
            // the engine's raycast would also work but this keeps the radius
            // tolerance that felt right before.
            var best = null, bestD = 40;
            for (var i = 0; i < state.agents.length; i++) {
                var a = state.agents[i];
                if (!a.unit.alive) continue;
                var node = Scene3D.units[a.unit.id];
                if (!node) continue;
                // Screen-project via the scene's own camera helper.
                var sp = worldToScreen(a.x, Scene3D.UNIT_Y, a.z);
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

    // Local screen-project helper kept only for click picking. Damage
    // numbers / HP bars / intent labels no longer need it — they render as
    // world-anchored scene nodes.
    function worldToScreen(wx, wy, wz) {
        var opts = Camera.orbitViewOpts(Scene3D.cam, Scene3D.canvas);
        var ex = opts.position[0], ey = opts.position[1], ez = opts.position[2];
        var fx = opts.target[0] - ex, fy = opts.target[1] - ey, fz = opts.target[2] - ez;
        var fl = Math.hypot(fx, fy, fz) || 1;
        fx /= fl; fy /= fl; fz /= fl;
        var up = opts.up;
        var rx = fy*up[2] - fz*up[1];
        var ry = fz*up[0] - fx*up[2];
        var rz = fx*up[1] - fy*up[0];
        var rl = Math.hypot(rx, ry, rz) || 1;
        rx /= rl; ry /= rl; rz /= rl;
        var ux = ry*fz - rz*fy;
        var uy = rz*fx - rx*fz;
        var uz = rx*fy - ry*fx;
        var dx = wx - ex, dy = wy - ey, dz = wz - ez;
        var xc = rx*dx + ry*dy + rz*dz;
        var yc = ux*dx + uy*dy + uz*dz;
        var zc = fx*dx + fy*dy + fz*dz;
        if (zc <= 0.01) return null;
        var tanHalf = Math.tan(opts.fov * Math.PI / 180 * 0.5);
        var aspect = opts.aspect;
        var ndcX = xc / (zc * aspect * tanHalf);
        var ndcY = yc / (zc * tanHalf);
        var w = Scene3D.canvas.clientWidth || Scene3D.canvas.width;
        var h = Scene3D.canvas.clientHeight || Scene3D.canvas.height;
        return { x: (ndcX + 1) * 0.5 * w, y: (1 - ndcY) * 0.5 * h };
    }

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
        for (var hid in Scene3D.hpBars) {
            var hb = Scene3D.hpBars[hid];
            if (hb) {
                if (hb.bg)   hb.bg.destroy();
                if (hb.fill) hb.fill.destroy();
            }
        }
        Scene3D.hpBars = {};
        for (var t = 0; t < Scene3D.projPool.length; t++) {
            destroyList(Scene3D.projPool[t]);
        }
        for (var e = 0; e < Scene3D.explosionPool.length; e++) {
            Scene3D.explosionPool[e].node.destroy();
        }
        Scene3D.explosionPool.length = 0;
        for (var d = 0; d < Scene3D.dmgPool.length; d++) {
            Scene3D.dmgPool[d].destroy();
        }
        Scene3D.dmgPool.length = 0;
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
        // each one. HP bar = two stacked world-anchored rects (bg + fill),
        // ylock-billboard so they face camera but always stay Y-up.
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

            // HP bar: dark team-tinted background rect.
            var bg = Scene3D.scene.createShape({
                shape: "rect", width: HP_BAR_W, height: HP_BAR_H,
                fill: r.teamId === 0 ? "#5a1a14" : "#0e3a5c",
                worldAnchor: [r.x, HP_BAR_Y, r.z],
                billboard: "ylock",
                name: "hp-bg-" + r.id,
            });
            var fill = Scene3D.scene.createShape({
                shape: "rect",
                width: HP_BAR_W - 0.06, height: HP_BAR_H - 0.04,
                fill: "#4ae04a",
                worldAnchor: [r.x, HP_BAR_Y, r.z],
                billboard: "ylock",
                name: "hp-fill-" + r.id,
            });
            Scene3D.hpBars[r.id] = {
                bg: bg, fill: fill,
                lastFrac: -1,
                maxHp: r.maxHp || 1,
                baseW: HP_BAR_W - 0.06,
            };
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
        syncHpBars(state);
        syncUnitFovs(state);
        syncGizmos(state);
    };

    // Per-unit HP bar — now two stacked world-anchored rects above the
    // capsule. Fill scaleX shrinks with HP fraction (center-to-center, which
    // looks fine at these sizes). Color ramps green→yellow→red.
    function hpColor(frac) {
        if (frac >= 0.55) return "#4ae04a";
        if (frac >= 0.25) return "#e6c64a";
        return "#e74c3c";
    }
    function syncHpBars(state) {
        for (var i = 0; i < state.agents.length; i++) {
            var a = state.agents[i];
            var hb = Scene3D.hpBars[a.unit.id];
            if (!hb) continue;
            var alive = !!a.unit.alive;
            hb.bg.visible   = alive;
            hb.fill.visible = alive;
            if (!alive) continue;

            // Track the authoritative scene node position — matches what the
            // engine actually renders for the capsule.
            var node = Scene3D.units[a.unit.id];
            var wx = node ? node.x : a.x;
            var wz = node ? node.z : a.z;
            hb.bg.worldAnchor   = [wx, HP_BAR_Y, wz];
            hb.fill.worldAnchor = [wx, HP_BAR_Y + 0.001, wz];

            var max = a.unit.maxHp || hb.maxHp || 1;
            var frac = Math.max(0, Math.min(1, a.unit.hp / max));
            if (frac !== hb.lastFrac) {
                hb.fill.scaleX = Math.max(0.0001, frac);
                hb.fill.fillColor = hpColor(frac);
                hb.lastFrac = frac;
            }
        }
    }

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
            var aim = mem ? mem.aim : null;
            var aimYaw;
            if (aim) {
                var f = BotAim.forward(aim);
                aimYaw = Math.atan2(f.x, -f.z);
            } else aimYaw = a.yaw;
            cone.rotationY = -aimYaw;
        }
    }

    // ─── Projectiles ──────────────────────────────────────────────────
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

    // ─── Floating damage numbers (HtmlNodes, world-anchored) ─────────
    // One HtmlNode per slot in Render.fx.floats. Styled via inline CSS so
    // they read as bold white/red/yellow text over the scene. Pool grows
    // as needed; unused slots get hidden.
    function makeDmgNode() {
        var node = Scene3D.scene.createHtmlNode({
            width: 120, height: 40,
            pxPerUnit: 90,   // ~1.33 world-units wide
            billboard: "full",
            html: "<div></div>",
            name: "dmg-float",
        });
        node.visible = false;
        return node;
    }
    function ensureDmgNode(i) {
        while (Scene3D.dmgPool.length <= i) {
            Scene3D.dmgPool.push(makeDmgNode());
        }
        return Scene3D.dmgPool[i];
    }
    function syncDamageNumbers() {
        var floats = Render.fx.floats;
        for (var i = 0; i < floats.length; i++) {
            var f = floats[i];
            var node = ensureDmgNode(i);
            // Float rises over its lifetime.
            var y = 1.8 + f.t * 1.2;
            node.worldAnchor = [f.x, y, f.z];
            node.visible = true;
            // Re-use the same inner div; setHtml re-parses so only touch it
            // when the text or color actually changes.
            var opacity = Math.max(0, 1 - f.t).toFixed(2);
            var html = "<div style=\"font:bold 18px Consolas, monospace; color:" + f.color
                     + "; text-shadow:0 0 3px #000,0 0 3px #000; text-align:center; opacity:" + opacity + ";\">"
                     + f.text + "</div>";
            node.setHtml(html);
        }
        for (var j = floats.length; j < Scene3D.dmgPool.length; j++) {
            Scene3D.dmgPool[j].visible = false;
        }
    }

    // ─── Focused-unit gizmos ──────────────────────────────────────────
    var FOV = Math.PI / 2.2;

    function buildFovMesh(range) {
        var half = FOV / 2;
        var sR = Math.sin(+half) * range, cR = -Math.cos(+half) * range;
        var sL = Math.sin(-half) * range, cL = -Math.cos(-half) * range;
        return {
            positions: new Float32Array([
                0, 0, 0,
                sR, 0, cR,
                sL, 0, cL,
            ]),
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
        var mesh = buildFovMesh(1.0);
        g.fovCone = Scene3D.scene.createMesh({
            positions: mesh.positions,
            indices: mesh.indices,
            normals: mesh.normals,
            color: [1.0, 0.82, 0.29, 0.28],
            emissive: 0.35,
            name: "gizmo-fov",
        });
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

        // Intent label — an HtmlNode billboard (full, not ylock, so it stays
        // readable from any angle).
        g.intentLabel = Scene3D.scene.createHtmlNode({
            width: 180, height: 36,
            pxPerUnit: 90,
            billboard: "full",
            html: "<div></div>",
            name: "gizmo-intent",
        });
        g.intentLabel.visible = false;
    }

    function syncGizmos(state) {
        var g = Scene3D.gizmos;
        var focus = state.byId[state.focusId];
        if (!focus || !focus.unit.alive) {
            g.focusRing.visible = false;
            g.rangeRing.visible = false;
            g.fovCone.visible = false;
            g.targetLine.visible = false;
            g.intentLabel.visible = false;
            return;
        }
        var mem = AI.memory[focus.unit.id];
        var range = focus.unit.attackRange || 9;

        g.focusRing.visible = true;
        g.focusRing.x = focus.x; g.focusRing.y = 0.02; g.focusRing.z = focus.z;

        g.rangeRing.visible = true;
        g.rangeRing.x = focus.x; g.rangeRing.y = 0.02; g.rangeRing.z = focus.z;
        g.rangeRing.scaleX = range;
        g.rangeRing.scaleZ = range;

        g.fovCone.visible = true;
        g.fovCone.x = focus.x; g.fovCone.y = 0.04; g.fovCone.z = focus.z;
        g.fovCone.scaleX = range;
        g.fovCone.scaleZ = range;
        var focusAim = mem ? mem.aim : null;
        var aimYaw;
        if (focusAim) {
            var f = BotAim.forward(focusAim);
            aimYaw = Math.atan2(f.x, -f.z);
        } else aimYaw = focus.yaw;
        g.fovCone.rotationY = -aimYaw;

        var tid = mem && mem.targetId;
        var tgt = tid != null ? state.byId[tid] : null;
        if (tgt && tgt.unit.alive) {
            var dx = tgt.x - focus.x, dz = tgt.z - focus.z;
            var d = Math.hypot(dx, dz);
            if (d > 0.05) {
                var los = bro.ai.game.hasLineOfSight(
                    focus.x, focus.z, tgt.x, tgt.z, Arena.OBSTACLES);
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

        // Intent label as a world-anchored HtmlNode floating above the
        // focused unit. setHtml re-parses the subtree — only call it when
        // the intent actually changes so we don't thrash the raster thread.
        if (mem && mem.intent) {
            g.intentLabel.visible = true;
            g.intentLabel.worldAnchor = [focus.x, 2.8, focus.z];
            if (g._lastIntent !== mem.intent) {
                g.intentLabel.setHtml(
                    "<div style=\"font:bold 14px Consolas, monospace; color:#ffd24a;"
                    + " text-shadow:0 0 3px #000,0 0 3px #000; text-align:center;\">"
                    + mem.intent + "</div>");
                g._lastIntent = mem.intent;
            }
        } else {
            g.intentLabel.visible = false;
            g._lastIntent = null;
        }
    }
})();
