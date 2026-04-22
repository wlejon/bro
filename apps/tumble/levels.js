// levels.js — piece defs, cell/world helpers, and the level progression.
//
// Worldspace conventions
//   • Right-handed, Y-up. 1 cell = 1 world unit.
//   • Cell (cx, cy, cz) centers at world (cx + 0.5, cy + 0.5, cz + 0.5).
//     The grid is integer-indexed so placement, serialization, and
//     collision filters stay identical regardless of bounds origin.
//   • A piece fills the volume of exactly one cell (centered on the cell
//     center). Rotations are multiples of 90° about +Y (`rot` ∈ {0..3}).
//
// Piece definition
//   Every entry describes geometry + physics shape + placement metadata
//   for a single type. The factories take ({x,y,z}, rot) in world space
//   (cell centers) and return scene node + physics body info. The "ghost"
//   hue is used for the unplaced preview.
//
// Levels
//   Each level bundles a spawner position, a goal AABB, the world bounds
//   the cursor may place in, a piece budget (map of type → count), par
//   times, and a starting layout (pre-placed pieces the player can't
//   remove — "level furniture"). Piece counts are per-type (so Level 1
//   might hand out 5 blocks and 2 ramps and nothing else).

(function (global) {
    'use strict';

    // ── Piece types ──────────────────────────────────────────────────────────
    // `build(world, cellWorld, rot)` creates scene mesh + physics body.
    // Returns { node, body, marker? } where `body` is a Jolt tag or null.
    // Rotation is in radians (rot * 90° when rot is an int).
    //
    // All geometry stays inside a 1x1x1 cell (with some slight overshoot on
    // ramps — that's intentional so they join neighbouring cells).

    function rotY(rot) { return rot * Math.PI / 2; }

    // Reuse apps/lib/camera.js for quaternion math. Camera uses [x,y,z,w]
    // arrays; Jolt's JS binding wants {x,y,z,w} objects, so we adapt at
    // the boundary.
    const { quatFromAxis, quatMul } = Camera;
    function quatArrToObj(q) { return { x: q[0], y: q[1], z: q[2], w: q[3] }; }

    // Pure-Y quat around Y axis (tight spinner path — avoids the
    // allocation of a 4-element array + object conversion).
    function quatY(angle) {
        const s = Math.sin(angle * 0.5), c = Math.cos(angle * 0.5);
        return { x: 0, y: s, z: 0, w: c };
    }

    // Extract Euler angles in the form the engine's fromEuler consumes
    // (Z*Y*X intrinsic composition — see src/scene/scene_node.h). Given a
    // quat `q`, setting a SceneNode's rx/ry/rz to these values (converted
    // to degrees) reproduces `q` (subject to the usual ±π/2 gimbal lock).
    // We need this because the scene node API only exposes Euler, but the
    // natural way to author "tilt in body frame, then yaw" is via
    // composed quaternion — the direct-Euler form `ry=yaw, rz=tilt` puts
    // tilt in the world frame and vanishes when yaw rotates the ramp's
    // long axis onto world Z.
    function quatToEuler(q) {
        const sinr_cosp = 2 * (q[3] * q[0] + q[1] * q[2]);
        const cosr_cosp = 1 - 2 * (q[0] * q[0] + q[1] * q[1]);
        const rx = Math.atan2(sinr_cosp, cosr_cosp);
        const sinp = Math.max(-1, Math.min(1, 2 * (q[3] * q[1] - q[2] * q[0])));
        const ry = Math.asin(sinp);
        const siny_cosp = 2 * (q[3] * q[2] + q[0] * q[1]);
        const cosy_cosp = 1 - 2 * (q[1] * q[1] + q[2] * q[2]);
        const rz = Math.atan2(siny_cosp, cosy_cosp);
        return { rx, ry, rz };
    }
    const RAD2DEG = 180 / Math.PI;

    const PIECES = {
        block: {
            label:    'Block',
            key:      '1',
            color:    '#5a6375',
            ghost:    'rgba(140, 170, 220, 0.35)',
            rotatable: false,
            describe: 'Solid 1×1 cube. The staple floor/wall unit.',
            build(scene, cw, rot) {
                const node = scene.createMesh({
                    mesh: 'box', halfW: 0.5, halfH: 0.5, halfD: 0.5,
                    x: cw.x, y: cw.y, z: cw.z,
                    color: '#5a6375', metallic: 0.05, roughness: 0.82,
                });
                const body = Physics.createBody({
                    shape: 'box', static: true,
                    halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
                    position: { x: cw.x, y: cw.y, z: cw.z },
                    friction: 0.55, restitution: 0.15,
                });
                return { node, body };
            },
        },

        ramp: {
            label:    'Ramp',
            key:      '2',
            color:    '#6a8dbf',
            ghost:    'rgba(130, 180, 240, 0.35)',
            rotatable: true,
            describe: 'Sloped plank. Rotation picks which way it points down.',
            build(scene, cw, rot) {
                // We want a slab whose +X end is lifted by `tilt`, then the
                // whole thing yawed around world Y by `rot * 90°` so the
                // high end faces +X, +Z, -X, -Z for rot ∈ {0..3}.
                //
                // The engine's scene node rotation is `Z(rz) * Y(ry) * X(rx)`
                // — i.e. `rz` is applied AFTER `ry`, in world frame. Naive
                // `ry=yaw, rz=tilt` puts the tilt around world Z for every
                // yaw, so at rot=1/3 (ramp long-axis rotated onto world Z)
                // the tilt is around the ramp's length and becomes
                // invisible. Instead, compose the desired quaternion in
                // body-local order (tilt first, then yaw) and extract the
                // engine-equivalent Euler angles.
                const tilt  = 28 * Math.PI / 180;
                const yaw   = rotY(rot);
                const qTilt = quatFromAxis(0, 0, 1, tilt);
                const qYaw  = quatFromAxis(0, 1, 0, yaw);
                const q     = quatMul(qYaw, qTilt);           // yaw ∘ tilt
                const e     = quatToEuler(q);
                const node  = scene.createMesh({
                    mesh: 'box', halfW: 0.58, halfH: 0.04, halfD: 0.45,
                    x: cw.x, y: cw.y, z: cw.z,
                    rx: e.rx * RAD2DEG,
                    ry: e.ry * RAD2DEG,
                    rz: e.rz * RAD2DEG,
                    color: '#6a8dbf', metallic: 0.1, roughness: 0.6,
                });
                const body = Physics.createBody({
                    shape: 'box', static: true,
                    halfExtents: { x: 0.58, y: 0.04, z: 0.45 },
                    position: { x: cw.x, y: cw.y, z: cw.z },
                    rotation: quatArrToObj(q),
                    friction: 0.28, restitution: 0.12,
                });
                return { node, body };
            },
        },

        wall: {
            label:    'Wall',
            key:      '3',
            color:    '#8b6ec4',
            ghost:    'rgba(180, 150, 235, 0.35)',
            rotatable: true,
            describe: 'Thin vertical panel. Blocks one side of a cell.',
            build(scene, cw, rot) {
                // Panel sits on one face of the cell. rot picks which face:
                // 0 = +X face, 1 = +Z, 2 = -X, 3 = -Z.
                const offsets = [
                    { x:  0.45, z:  0 },
                    { x:  0,    z:  0.45 },
                    { x: -0.45, z:  0 },
                    { x:  0,    z: -0.45 },
                ];
                const o = offsets[rot & 3];
                // Wall long edge lies along the cell face (perpendicular to
                // normal). When normal is ±X, long edge is along Z.
                const longAxis = (rot & 1) === 0 ? 'z' : 'x';
                const hW = longAxis === 'x' ? 0.48 : 0.05;
                const hD = longAxis === 'x' ? 0.05 : 0.48;
                const node = scene.createMesh({
                    mesh: 'box', halfW: hW, halfH: 0.4, halfD: hD,
                    x: cw.x + o.x, y: cw.y - 0.1, z: cw.z + o.z,
                    color: '#8b6ec4', metallic: 0.1, roughness: 0.7,
                });
                const body = Physics.createBody({
                    shape: 'box', static: true,
                    halfExtents: { x: hW, y: 0.4, z: hD },
                    position: { x: cw.x + o.x, y: cw.y - 0.1, z: cw.z + o.z },
                    friction: 0.5, restitution: 0.25,
                });
                return { node, body };
            },
        },

        bumper: {
            label:    'Bumper',
            key:      '4',
            color:    '#e25a8f',
            ghost:    'rgba(240, 130, 180, 0.4)',
            rotatable: false,
            describe: 'Springy sphere. Bounces marbles in lively directions.',
            build(scene, cw, rot) {
                const node = scene.createMesh({
                    mesh: 'sphere', radius: 0.32,
                    segments: 20, rings: 16,
                    x: cw.x, y: cw.y, z: cw.z,
                    color: '#e25a8f', metallic: 0.15, roughness: 0.35,
                    emissive: 0.6, emissiveColor: [1.0, 0.4, 0.7],
                });
                const body = Physics.createBody({
                    shape: 'sphere', static: true, radius: 0.32,
                    position: { x: cw.x, y: cw.y, z: cw.z },
                    friction: 0.1, restitution: 0.92,
                });
                return { node, body };
            },
        },

        spinner: {
            label:    'Spinner',
            key:      '5',
            color:    '#5acc88',
            ghost:    'rgba(120, 240, 170, 0.35)',
            rotatable: true,
            describe: 'Kinematic paddle. Whacks marbles along.',
            build(scene, cw, rot) {
                // Paddle: long X, short Z, medium Y. We spin around Y in the
                // sim loop. Static body that we rotate by setRotation each
                // frame — Jolt treats static-with-setRotation as moving, but
                // it'll do for our needs at this scale.
                const node = scene.createMesh({
                    mesh: 'box', halfW: 0.46, halfH: 0.15, halfD: 0.07,
                    x: cw.x, y: cw.y, z: cw.z,
                    ry: rot * 90,
                    color: '#5acc88', metallic: 0.25, roughness: 0.45,
                    emissive: 0.25, emissiveColor: [0.5, 1.0, 0.7],
                });
                const body = Physics.createBody({
                    shape: 'box', static: true,
                    halfExtents: { x: 0.46, y: 0.15, z: 0.07 },
                    position: { x: cw.x, y: cw.y, z: cw.z },
                    rotation: quatY(rotY(rot)),
                    friction: 0.5, restitution: 0.5,
                });
                // phase stored on the returned record so the game loop can
                // keep each spinner individually animated.
                return { node, body, anim: { kind: 'spinner', rot, phase: 0 } };
            },
        },

        booster: {
            label:    'Booster',
            key:      '6',
            color:    '#ffc34a',
            ghost:    'rgba(255, 210, 110, 0.45)',
            rotatable: true,
            describe: 'Conveyor pad. Any marble on top is shoved along its arrow.',
            build(scene, cw, rot) {
                // Flat pad near the bottom of the cell. Rotation = direction
                // of the impulse (in world XZ).
                const node = scene.createMesh({
                    mesh: 'box', halfW: 0.45, halfH: 0.05, halfD: 0.45,
                    x: cw.x, y: cw.y - 0.42, z: cw.z,
                    ry: rot * 90,
                    color: '#ffc34a', metallic: 0.05, roughness: 0.35,
                    emissive: 0.5, emissiveColor: [1.0, 0.75, 0.2],
                });
                const body = Physics.createBody({
                    shape: 'box', static: true,
                    halfExtents: { x: 0.45, y: 0.05, z: 0.45 },
                    position: { x: cw.x, y: cw.y - 0.42, z: cw.z },
                    rotation: quatY(rotY(rot)),
                    friction: 0.9, restitution: 0.05,
                });
                // Add a small arrow on top to show direction
                const arrow = scene.createMesh({
                    mesh: 'box', halfW: 0.22, halfH: 0.02, halfD: 0.05,
                    x: cw.x, y: cw.y - 0.34, z: cw.z,
                    ry: rot * 90,
                    color: '#1a1209', metallic: 0.0, roughness: 0.95,
                });
                return { node, body, extras: [arrow],
                         anim: { kind: 'booster', rot } };
            },
        },

        chute: {
            label:    'Chute',
            key:      '7',
            color:    '#4ec2d6',
            ghost:    'rgba(120, 220, 240, 0.35)',
            rotatable: false,
            describe: 'Downward funnel — four angled ramps converging to the centre.',
            build(scene, cw, rot) {
                // Four tilted slabs forming an inverted pyramid. Each slab
                // tilts so its centre-facing edge is low (outer edge high),
                // then yaws around the cell centre to face N/E/S/W.
                // Same body-local quaternion compose-then-extract trick as
                // the ramp so the tilt is visible at every yaw.
                const nodes = [];
                const bodies = [];
                const tilt = 34 * Math.PI / 180;
                const half = { x: 0.48, y: 0.03, z: 0.22 };
                for (let i = 0; i < 4; i++) {
                    const a  = i * Math.PI / 2;
                    const dx = Math.cos(a) * 0.28;
                    const dz = Math.sin(a) * 0.28;
                    const qTilt = quatFromAxis(0, 0, 1, -tilt);    // outer edge up
                    const qYaw  = quatFromAxis(0, 1, 0,  a);
                    const q     = quatMul(qYaw, qTilt);
                    const e     = quatToEuler(q);
                    const px = cw.x + dx, pz = cw.z + dz;
                    const py = cw.y - 0.05;
                    const node = scene.createMesh({
                        mesh: 'box', halfW: half.x, halfH: half.y, halfD: half.z,
                        x: px, y: py, z: pz,
                        rx: e.rx * RAD2DEG,
                        ry: e.ry * RAD2DEG,
                        rz: e.rz * RAD2DEG,
                        color: '#4ec2d6', metallic: 0.2, roughness: 0.4,
                    });
                    const body = Physics.createBody({
                        shape: 'box', static: true,
                        halfExtents: half,
                        position: { x: px, y: py, z: pz },
                        rotation: quatArrToObj(q),
                        friction: 0.22, restitution: 0.1,
                    });
                    nodes.push(node);
                    bodies.push(body);
                }
                return { node: nodes[0], extras: nodes.slice(1),
                         body: bodies[0], extraBodies: bodies.slice(1) };
            },
        },
    };

    const PIECE_ORDER = ['block', 'ramp', 'wall', 'bumper', 'spinner', 'booster', 'chute'];

    // ── Levels ───────────────────────────────────────────────────────────────
    // Each level: title, tagline, spawner (world point above the play field),
    //             goal AABB (world), place-volume bounds (cell-indexed),
    //             budget (per-piece-type), par times (seconds), layout.
    //
    // Bounds use cell indices; place-volume is inclusive. Goal AABB is world.
    // `furniture` is a list of pre-placed piece specs the player can't touch.

    function mkLevel(def) {
        return Object.assign({
            marbleGoalCount: 1,
            gravity:         -9.81,
            spawnInterval:   650,   // ms between marbles while running
            maxMarbles:      8,
            marbleBurst:     false, // drop all at once
            furniture:       [],
        }, def);
    }

    const LEVELS = [
        mkLevel({
            id:      'drop-in',
            name:    'Drop-In',
            tagline: 'Catch the marble. That\'s all.',
            spawner: { x: 0.5, y: 6.5, z: 0.5 },
            goal:    { min: [-0.5, 0, -0.5], max: [1.5, 1, 1.5], center: [0.5, 0.5, 0.5] },
            bounds:  { x: [-2, 3], y: [0, 5], z: [-2, 3] },
            budget:  { block: 4 },
            par:     { gold: 2.5, silver: 3.5, bronze: 5.0 },
        }),

        mkLevel({
            id:      'offset',
            name:    'Sideways',
            tagline: 'Spout above, cup to the side. Plank it.',
            spawner: { x: -2.5, y: 6.5, z: 0.5 },
            goal:    { min: [2.5, 0, -0.5], max: [4.5, 1, 1.5], center: [3.5, 0.5, 0.5] },
            bounds:  { x: [-3, 5], y: [0, 5], z: [-2, 3] },
            budget:  { block: 5, ramp: 3 },
            par:     { gold: 3.0, silver: 4.5, bronze: 7.0 },
        }),

        mkLevel({
            id:      'bank',
            name:    'Bank Shot',
            tagline: 'Wall it, ramp it, drop it home.',
            spawner: { x: -2.5, y: 7.5, z: -2.5 },
            goal:    { min: [2.5, 0, 2.5], max: [4.5, 1, 4.5], center: [3.5, 0.5, 3.5] },
            bounds:  { x: [-3, 5], y: [0, 6], z: [-3, 5] },
            budget:  { block: 6, ramp: 4, wall: 2 },
            par:     { gold: 4.0, silver: 6.0, bronze: 9.0 },
        }),

        mkLevel({
            id:      'bounce',
            name:    'Springboard',
            tagline: 'Bumpers hate ground control.',
            spawner: { x: 0.5, y: 8.5, z: 0.5 },
            goal:    { min: [3.5, 0, -0.5], max: [5.5, 1, 1.5], center: [4.5, 0.5, 0.5] },
            bounds:  { x: [-3, 6], y: [0, 7], z: [-3, 3] },
            budget:  { block: 4, ramp: 3, bumper: 2, wall: 2 },
            par:     { gold: 4.5, silver: 6.5, bronze: 10.0 },
        }),

        mkLevel({
            id:      'chute',
            name:    'Funnel Vision',
            tagline: 'Wide mouth, narrow target.',
            spawner: { x: 0.5, y: 9.5, z: 0.5 },
            goal:    { min: [-0.5, 0, -0.5], max: [1.5, 1, 1.5], center: [0.5, 0.5, 0.5] },
            bounds:  { x: [-3, 4], y: [0, 8], z: [-3, 4] },
            budget:  { chute: 2, block: 3, ramp: 2 },
            par:     { gold: 5.0, silver: 7.0, bronze: 10.0 },
        }),

        mkLevel({
            id:      'conveyor',
            name:    'Long Haul',
            tagline: 'Boosters do the talking.',
            spawner: { x: -4.5, y: 6.5, z: 0.5 },
            goal:    { min: [4.5, 0, -0.5], max: [6.5, 1, 1.5], center: [5.5, 0.5, 0.5] },
            bounds:  { x: [-5, 7], y: [0, 5], z: [-2, 3] },
            budget:  { block: 8, ramp: 2, booster: 3 },
            par:     { gold: 6.0, silver: 8.5, bronze: 12.0 },
        }),

        mkLevel({
            id:      'spin',
            name:    'Helicopters',
            tagline: 'Time the spinners, feed the cup.',
            spawner: { x: -3.5, y: 8.5, z: 0.5 },
            goal:    { min: [2.5, 0, -0.5], max: [4.5, 1, 1.5], center: [3.5, 0.5, 0.5] },
            bounds:  { x: [-4, 5], y: [0, 7], z: [-3, 3] },
            budget:  { block: 6, ramp: 3, wall: 2, spinner: 2 },
            par:     { gold: 6.0, silver: 9.0, bronze: 13.0 },
            spawnInterval: 450,
            maxMarbles: 10,
        }),

        mkLevel({
            id:      'gauntlet',
            name:    'Grand Tour',
            tagline: 'Every piece on the table. Use them.',
            spawner: { x: -4.5, y: 9.5, z: -3.5 },
            goal:    { min: [4.5, 0, 3.5], max: [6.5, 1, 5.5], center: [5.5, 0.5, 4.5] },
            bounds:  { x: [-5, 7], y: [0, 8], z: [-4, 6] },
            budget:  { block: 8, ramp: 5, wall: 3, bumper: 2, booster: 2, spinner: 1, chute: 1 },
            par:     { gold: 9.0, silver: 13.0, bronze: 20.0 },
            spawnInterval: 500,
            maxMarbles: 12,
        }),
    ];

    function medalFor(time, level) {
        if (time <= level.par.gold)   return 'gold';
        if (time <= level.par.silver) return 'silver';
        if (time <= level.par.bronze) return 'bronze';
        return 'none';
    }

    // Format seconds as "0.00s" or "—"
    function fmt(t) {
        if (!isFinite(t) || t == null) return '—';
        return t.toFixed(2) + 's';
    }

    global.TumbleLevels = {
        PIECES, PIECE_ORDER, LEVELS,
        medalFor, fmt,
        rotY, quatY,
    };
})(typeof window !== 'undefined' ? window : globalThis);
