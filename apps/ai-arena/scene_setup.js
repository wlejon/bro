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
    Scene3D.units = {};  // agentId → capsule node

    var UNIT_Y = 0.9;        // capsule center height (radius + halfHeight)
    var CAPSULE_R = 0.4;
    var CAPSULE_HALF_H = 0.5;
    var WALL_H = 2.5;
    var WALL_THICK = 0.4;
    var OBSTACLE_H = 1.8;

    Scene3D.init = function (canvas) {
        Scene3D.canvas = canvas;
        Scene3D.scene = canvas.getContext("scene");

        Scene3D.cam = Camera.createOrbit({
            pivot: [0, 0, 0],
            dist: 58,
            fov: 45,
            // Pitch the camera down ~48° for a MOBA-ish view — readable now
            // and friendlier to animated meshes later.
            rot: quatFromAxisAngle(1, 0, 0, 0.85),
        });
        Scene3D.applyCamera();
        wireCameraInput(canvas);
    };

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

        // Capsule per roster entry. Colored by team; will be replaced with
        // animated meshes later. Capsule standing upright on Y axis.
        for (var j = 0; j < scenario.roster.length; j++) {
            var r = scenario.roster[j];
            var c = r.teamId === 0 ? [0.90, 0.30, 0.24, 1.0]
                                   : [0.20, 0.60, 0.85, 1.0];
            var node2 = Scene3D.scene.createMesh({
                mesh: "capsule",
                radius: CAPSULE_R, halfHeight: CAPSULE_HALF_H,
                color: c,
                x: r.x, y: UNIT_Y, z: r.z,
                name: "unit-" + r.id,
            });
            Scene3D.units[r.id] = node2;
        }
    };

    // Per-frame transform sync. Phase 2 will flip this around — node.attachAgent
    // writes transforms automatically — but for Phase 1 (sim still driven by the
    // old loop) we push agent.x/z/yaw onto the capsule nodes manually.
    Scene3D.update = function (state, dt) {
        var agents = state.agents;
        for (var i = 0; i < agents.length; i++) {
            var a = agents[i];
            var node = Scene3D.units[a.unit.id];
            if (!node) continue;
            if (!a.unit.alive) {
                // Sink dead units below the floor so the HUD can still see
                // them in the roster without cluttering the battlefield.
                node.visible = false;
                continue;
            }
            node.visible = true;
            node.x = a.x;
            node.z = a.z;
            node.rotationY = a.yaw;
        }
    };
})();
