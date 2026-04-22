// =============================================================================
// Tumble — main client
// =============================================================================
//
// Single-player 3D marble-run puzzle. Place pieces on an integer cell grid to
// route a marble from a floating spawner into a goal cup. Scored on time to
// first marble in the cup. See levels.js for the piece/level definitions.
//
// High-level structure
//   • scene         — bro scene graph, set up per level (lighting, ground,
//                      spawner/goal markers, grid helper).
//   • Physics       — Jolt via bro's singleton. One static body per placed
//                      piece; one dynamic sphere per marble. Spinners and
//                      boosters post-process contact events to fake
//                      kinematic behaviour.
//   • orbit camera  — Camera.createOrbit from apps/lib/camera.js. Right-
//                      drag orbits, middle-drag pans, wheel zooms.
//   • build mode    — left-click places the current piece on the active
//                      layer at the cell the cursor is hovering. Right-
//                      click removes whatever piece is under the cursor.
//                      Ghost preview mesh tracks the cursor continuously.
//   • run mode      — spawner drops marbles at a level-specified cadence;
//                      first marble to enter the goal AABB wins and logs
//                      its arrival time.
//
// Nothing here is networked — the game is fully offline. `Storage.create`
// keeps best times per level, medals, and the current selected level.

'use strict';

// ── External singletons exposed by the libs ─────────────────────────────────
const { PIECES, PIECE_ORDER, LEVELS, medalFor, fmt, quatY, rotY }
    = TumbleLevels;

// ── DOM plumbing ────────────────────────────────────────────────────────────
const canvas = document.querySelector('#view');
const scene  = canvas.getContext('scene');

function resizeCanvas() {
    const dpr = window.devicePixelRatio || 1;
    const w = Math.floor(window.innerWidth  * dpr);
    const h = Math.floor(window.innerHeight * dpr);
    if (canvas.width !== w)  canvas.width  = w;
    if (canvas.height !== h) canvas.height = h;
}
window.addEventListener('resize', resizeCanvas);
resizeCanvas();

// ── Persistent settings & progress ──────────────────────────────────────────
const store = Storage.create('tumble');
store.load({
    sfxVol:     0.6,
    best:       {},       // levelId → best time (seconds)
    lastLevel:  0,        // index into LEVELS
    unlocked:   1,        // 1 = only level 0 unlocked; incremented as levels clear
});

// ── Input / audio / screens ─────────────────────────────────────────────────
Input.init([
    { name: 'run_reset',  label: 'Run / Reset',  defaults: [' '] },
    { name: 'rotate',     label: 'Rotate Piece', defaults: ['r'] },
    { name: 'layer_up',   label: 'Layer Up',     defaults: ['e'] },
    { name: 'layer_down', label: 'Layer Down',   defaults: ['q'] },
    { name: 'pause',      label: 'Pause',        defaults: ['Escape'] },
    { name: 'p1', label: 'Piece 1', defaults: ['1'] },
    { name: 'p2', label: 'Piece 2', defaults: ['2'] },
    { name: 'p3', label: 'Piece 3', defaults: ['3'] },
    { name: 'p4', label: 'Piece 4', defaults: ['4'] },
    { name: 'p5', label: 'Piece 5', defaults: ['5'] },
    { name: 'p6', label: 'Piece 6', defaults: ['6'] },
    { name: 'p7', label: 'Piece 7', defaults: ['7'] },
], { storageKey: 'tumble:controls' });
Input.attach();

SFX.init({ sfxVol: store.get('sfxVol') });
const sfx = {
    menu:   () => SFX.tone(440, 0.03, 'sine',     0.3),
    select: () => SFX.tone(620, 0.06, 'square',   0.35),
    place:  () => SFX.tone(540, 0.04, 'triangle', 0.3),
    remove: () => SFX.tone(200, 0.06, 'square',   0.3),
    drop:   () => SFX.tone(320, 0.04, 'sine',     0.25),
    clink:  () => SFX.tone(880, 0.03, 'triangle', 0.18),
    goal:   () => SFX.sequence([[523,0.09,'square',0.55], [659,0.09,'square',0.6],
                                [784,0.1, 'square',0.65], [1047,0.22,'square',0.7]]),
    fail:   () => SFX.sequence([[220,0.12,'sawtooth',0.45], [160,0.2,'sawtooth',0.5]]),
};

const screens = Screens.create({
    overlay:      '#overlay',
    onMenuMove:   sfx.menu,
    onMenuSelect: sfx.select,
});

// ── Scene globals: kept at module scope so screen handlers can tear down
//    and rebuild on level switch without piping state around.

const SCENE = {
    groundNode:      null,   // ground plate mesh
    goalMarker:      null,   // rim ring around goal AABB
    goalFill:        null,   // translucent goal box
    spoutNode:       null,   // marker above spawn point
    layerPlane:      null,   // semi-transparent "build plane" indicator
    ghostNode:       null,   // translucent preview mesh
    ghostCellKey:    null,   // current hovered cell key ("x,y,z")
    outOfBoundsGhost:false,  // hovered cell outside level bounds
    staticDecor:     [],     // misc decorative nodes (spout base, etc.)
};

// ── Game state ─────────────────────────────────────────────────────────────
const game = {
    level:       null,     // current level def
    levelIdx:    0,
    mode:        'build',  // 'build' | 'run'
    placed:      new Map(),       // cellKey ("x,y,z") → piece record
    meshToCell:  new Map(),       // scene node.id → cellKey
    bodyToCell:  new Map(),       // Physics body tag → cellKey
    animatedCells: [],            // cellKeys whose anim tick needs running
    marbles:     [],              // [{ body, node }]
    marblesSpawned: 0,
    marblesRemoved: 0,
    nextSpawnAt: 0,               // performance.now() timestamp
    startMs:     0,               // run-mode timer origin
    resultMs:    null,            // null until first marble scores
    paused:      false,
    runtime:     0,               // ms since mode=run (for spinner anim)
    pendingFail: null,            // timeout id watching for unsolvable runs
    build:       {
        selected: 'block',
        rot:      0,
        layer:    0,      // y index of active placement layer
    },
};

// Each level owns its own budget counter: { type: { used, limit } }. Built
// fresh when the level is loaded.
let budget = {};

// ── Camera ──────────────────────────────────────────────────────────────────
const cam = Camera.createOrbit({
    target: [0, 3, 0],
    dist:   12,
    fov:    50,
    near:   0.1,
    far:    400,
});

function applyCamera() {
    scene.setCamera(Camera.orbitViewOpts(cam, canvas));
}

// Right-drag orbit, middle-drag pan, wheel zoom. Left-click is reserved
// for placement, right-click for removal — so orbit is on right-drag
// (which also fires contextmenu; we suppress it).
(function wireCamera() {
    let dragging = null;
    let lastX = 0, lastY = 0;
    canvas.addEventListener('mousedown', (e) => {
        if (e.button === 2)      dragging = 'orbit';
        else if (e.button === 1) dragging = 'pan';
        else                     dragging = null;
        lastX = e.clientX; lastY = e.clientY;
    });
    window.addEventListener('mouseup', () => { dragging = null; });
    window.addEventListener('mousemove', (e) => {
        if (!dragging) return;
        const dx = e.clientX - lastX;
        const dy = e.clientY - lastY;
        lastX = e.clientX; lastY = e.clientY;
        if (dragging === 'orbit') Camera.orbitLook(cam, dx, dy);
        else if (dragging === 'pan') Camera.orbitPan(cam, dx, dy);
    });
    canvas.addEventListener('wheel', (e) => {
        const s = Math.exp(e.deltaY * 0.001);
        cam.dist = Math.max(4, Math.min(60, cam.dist * s));
    }, { passive: true });
    canvas.addEventListener('contextmenu', (e) => e.preventDefault());
})();

// ── Scene setup / teardown ──────────────────────────────────────────────────
function buildEnvironment(level) {
    // Ambient + tonemap + fog chosen once so it looks decent against our
    // varied piece palette without ballooning into a full lighting demo.
    scene.setAmbient([0.05, 0.055, 0.065]);
    scene.setToneMap({ mode: 'aces', exposure: 1.1 });
    scene.setFog({ start: 25, end: 60, color: [0.04, 0.05, 0.09] });

    // Key directional "sun".
    scene.createLight({
        type: 'directional',
        direction: [-0.45, -1.0, -0.35],
        color:     [1.0, 0.97, 0.9],
        intensity: 2.6,
        name:      'sun',
    });
    // Warm fill from camera side.
    scene.createLight({
        type: 'point',
        position:  [0, 5, 8],
        color:     [1.0, 0.75, 0.55],
        intensity: 14,
        range:     18,
        name:      'warm-fill',
    });
    // Cool rim.
    scene.createLight({
        type: 'point',
        position:  [-6, 4, -6],
        color:     [0.5, 0.7, 1.0],
        intensity: 10,
        range:     14,
        name:      'cool-rim',
    });

    // Ground plate sized to fit the level bounds with a little surround.
    const bx = level.bounds.x, bz = level.bounds.z;
    const w  = (bx[1] - bx[0] + 3);
    const d  = (bz[1] - bz[0] + 3);
    const cx = (bx[0] + bx[1]) * 0.5;
    const cz = (bz[0] + bz[1]) * 0.5;
    SCENE.groundNode = scene.createMesh({
        mesh: 'plane',
        halfW: w * 0.5, halfD: d * 0.5,
        x: cx, y: -0.02, z: cz,
        color: '#1a2236', metallic: 0.0, roughness: 0.92,
        name: 'ground',
    });
    // Ground physics plate so marbles that miss don't fall forever near the
    // cells (but we still drop them out below y=-3 explicitly).
    Physics.createBody({
        shape: 'box', static: true,
        halfExtents: { x: w * 0.5, y: 0.02, z: d * 0.5 },
        position: { x: cx, y: -0.02, z: cz },
        friction: 0.6, restitution: 0.12,
    });

    // Spawner base: a floating emissive torus-ish marker (two stacked rings
    // made from box primitives to avoid needing a mesh import path).
    const sp = level.spawner;
    SCENE.spoutNode = scene.createMesh({
        mesh: 'cylinder',
        radius: 0.35, halfHeight: 0.08, segments: 24,
        x: sp.x, y: sp.y + 0.18, z: sp.z,
        color: '#ffd466', metallic: 0.1, roughness: 0.3,
        emissive: 1.5, emissiveColor: [1.0, 0.82, 0.4],
        name: 'spawner',
    });
    // Column of light leading up to the spawner
    SCENE.staticDecor.push(scene.createMesh({
        mesh: 'cylinder', radius: 0.04, halfHeight: 0.6,
        segments: 12,
        x: sp.x, y: sp.y - 0.5, z: sp.z,
        color: '#ffd466', emissive: 2.0, emissiveColor: [1.0, 0.85, 0.4],
        metallic: 0.0, roughness: 1.0,
    }));

    // Goal: a rectangular rim plus a translucent fill plane at its top.
    const g = level.goal;
    const gcx = (g.min[0] + g.max[0]) * 0.5;
    const gcy = (g.min[1] + g.max[1]) * 0.5;
    const gcz = (g.min[2] + g.max[2]) * 0.5;
    const ghw = (g.max[0] - g.min[0]) * 0.5;
    const ghh = (g.max[1] - g.min[1]) * 0.5;
    const ghd = (g.max[2] - g.min[2]) * 0.5;
    // Four rim walls around the top, as emissive strips.
    const rimColor = [0.3, 1.0, 0.6];
    const rimY = g.max[1] + 0.02;
    SCENE.goalMarker = scene.createNode('goal-rim');
    const rims = [
        { x: gcx - ghw, y: rimY, z: gcz, hw: 0.04, hd: ghd + 0.04, hh: 0.04 },
        { x: gcx + ghw, y: rimY, z: gcz, hw: 0.04, hd: ghd + 0.04, hh: 0.04 },
        { x: gcx, y: rimY, z: gcz - ghd, hw: ghw + 0.04, hd: 0.04, hh: 0.04 },
        { x: gcx, y: rimY, z: gcz + ghd, hw: ghw + 0.04, hd: 0.04, hh: 0.04 },
    ];
    for (const r of rims) {
        const n = scene.createMesh({
            mesh: 'box', halfW: r.hw, halfH: r.hh, halfD: r.hd,
            x: r.x, y: r.y, z: r.z,
            color: '#4eff8f', metallic: 0.0, roughness: 0.5,
            emissive: 2.2, emissiveColor: rimColor,
        });
        SCENE.goalMarker.add(n);
    }
    // Floor of the goal: a dim translucent pad (not physical) so the eye
    // reads it as a cup.
    SCENE.goalFill = scene.createMesh({
        mesh: 'box', halfW: ghw, halfH: 0.02, halfD: ghd,
        x: gcx, y: g.min[1] + 0.02, z: gcz,
        color: '#0a3a1e',
        emissive: 0.6, emissiveColor: [0.2, 0.9, 0.5],
        metallic: 0.0, roughness: 0.8,
    });
    // Physical goal floor so marbles that enter do stop inside and we can
    // see them settle (not strictly needed — we detect by AABB — but nicer).
    Physics.createBody({
        shape: 'box', static: true,
        halfExtents: { x: ghw, y: 0.02, z: ghd },
        position: { x: gcx, y: g.min[1] + 0.02, z: gcz },
        friction: 0.8, restitution: 0.1,
    });
    // Goal walls (physical) so marbles don't roll out once they've scored.
    const wallH = (g.max[1] - g.min[1]) * 0.8;
    const walls = [
        { x: g.min[0], y: g.min[1] + wallH * 0.5, z: gcz, hw: 0.04, hh: wallH * 0.5, hd: ghd },
        { x: g.max[0], y: g.min[1] + wallH * 0.5, z: gcz, hw: 0.04, hh: wallH * 0.5, hd: ghd },
        { x: gcx, y: g.min[1] + wallH * 0.5, z: g.min[2], hw: ghw, hh: wallH * 0.5, hd: 0.04 },
        { x: gcx, y: g.min[1] + wallH * 0.5, z: g.max[2], hw: ghw, hh: wallH * 0.5, hd: 0.04 },
    ];
    for (const w of walls) {
        Physics.createBody({
            shape: 'box', static: true,
            halfExtents: { x: w.hw, y: w.hh, z: w.hd },
            position:   { x: w.x,  y: w.y,  z: w.z  },
            friction: 0.5, restitution: 0.2,
        });
        // Also a translucent wall visual
        SCENE.staticDecor.push(scene.createMesh({
            mesh: 'box', halfW: w.hw, halfH: w.hh, halfD: w.hd,
            x: w.x, y: w.y, z: w.z,
            color: '#1e4a2e',
            emissive: 0.25, emissiveColor: [0.2, 0.9, 0.5],
            metallic: 0.05, roughness: 0.7,
        }));
    }

    // Grid hint: a lightly-emissive rectangle at the current build layer
    // so the player can see where placements will land.
    SCENE.layerPlane = scene.createMesh({
        mesh: 'plane',
        halfW: (bx[1] - bx[0] + 1) * 0.5,
        halfD: (bz[1] - bz[0] + 1) * 0.5,
        x: cx, y: 0, z: cz,
        color: '#2a3458',
        emissive: 0.15, emissiveColor: [0.4, 0.5, 0.9],
        metallic: 0.0, roughness: 1.0,
        name: 'layer-plane',
    });
    SCENE.layerPlane.visible = false; // only visible in build mode

    // Ghost preview parts — an array of { node, bx, by, bz } rebuilt from
    // the selected piece's own build() factory in rebuildGhost(). Each
    // part keeps its baseline offset (relative to cell centre) so the
    // ghost can be repositioned without re-running the factory.
    SCENE.ghostParts = [];
    SCENE.ghostIds   = new Set();

    // Camera framing: centre on level, zoom based on footprint.
    const diag = Math.sqrt(
        (bx[1] - bx[0]) * (bx[1] - bx[0]) +
        (bz[1] - bz[0]) * (bz[1] - bz[0])
    );
    Camera.orbitReframe(cam,
        [cx, (level.bounds.y[0] + level.bounds.y[1]) * 0.35, cz],
        Math.max(10, diag * 1.4));
}

function teardownScene() {
    // Nuke every node under scene.root and every Jolt body. This is the
    // lazy path — rebuild-from-scratch per level — but it's robust and
    // keeps state exactly consistent between runs.
    const rootChildren = scene.root.children.slice();
    for (const n of rootChildren) scene.destroyNode(n);
    // Destroy every body we know about. There may be more (ground plate,
    // goal floor, goal walls) — we destroy them by retrieving them from
    // getAllTransforms.
    const all = Physics.getAllTransforms();
    for (let i = 0; i < all.length; i += 8) {
        const tag = all[i] | 0;
        Physics.destroyBody(tag);
    }
    SCENE.groundNode = null;
    SCENE.goalMarker = null;
    SCENE.goalFill   = null;
    SCENE.spoutNode  = null;
    SCENE.layerPlane = null;
    if (SCENE.ghostParts) SCENE.ghostParts.length = 0;
    if (SCENE.ghostIds)   SCENE.ghostIds.clear();
    SCENE.staticDecor.length = 0;
    game.placed.clear();
    game.meshToCell.clear();
    game.bodyToCell.clear();
    game.animatedCells.length = 0;
    game.marbles.length = 0;
    game.marblesSpawned = 0;
    game.marblesRemoved = 0;
    game.resultMs       = null;
    game.runtime        = 0;
    if (game.pendingFail) { clearTimeout(game.pendingFail); game.pendingFail = null; }
}

// ── Level load / reset ──────────────────────────────────────────────────────
function loadLevel(idx) {
    teardownScene();
    game.levelIdx = idx;
    game.level    = LEVELS[idx];
    store.set('lastLevel', idx);
    store.save();

    Physics.createWorld({ maxBodies: 4096 });
    Physics.setGravity(0, game.level.gravity, 0);

    buildEnvironment(game.level);

    // Budget + palette defaults
    budget = {};
    for (const t of PIECE_ORDER) {
        const lim = game.level.budget[t] || 0;
        budget[t] = { used: 0, limit: lim };
    }
    // Default to the first piece type with a non-zero budget.
    game.build.selected = PIECE_ORDER.find(t => (budget[t].limit || 0) > 0) || 'block';
    game.build.rot      = 0;
    game.build.layer    = Math.max(0, game.level.bounds.y[0]);

    // Optional furniture (pre-placed pieces)
    for (const f of (game.level.furniture || [])) {
        placePiece(f.type, f.cell[0], f.cell[1], f.cell[2], f.rot || 0, { furniture: true });
    }

    enterBuildMode();
    refreshPalette();
    refreshHud();
}

function restartLevel() { loadLevel(game.levelIdx); }

// ── Piece placement ─────────────────────────────────────────────────────────
function cellKey(cx, cy, cz) { return cx + ',' + cy + ',' + cz; }

function inBounds(cx, cy, cz) {
    const b = game.level.bounds;
    return cx >= b.x[0] && cx <= b.x[1]
        && cy >= b.y[0] && cy <= b.y[1]
        && cz >= b.z[0] && cz <= b.z[1];
}

// Some cells are reserved — the goal volume and the spawner's column.
// Placing on them would interfere with the game's core affordances.
function cellReserved(cx, cy, cz) {
    const g = game.level.goal;
    const wx = cx + 0.5, wy = cy + 0.5, wz = cz + 0.5;
    if (wx >= g.min[0] && wx <= g.max[0] &&
        wy >= g.min[1] && wy <= g.max[1] &&
        wz >= g.min[2] && wz <= g.max[2]) return true;
    const s = game.level.spawner;
    if (Math.floor(s.x) === cx &&
        Math.floor(s.z) === cz &&
        Math.abs(wy - s.y) < 1.0) return true;
    return false;
}

function placePiece(type, cx, cy, cz, rot, opts) {
    opts = opts || {};
    const key  = cellKey(cx, cy, cz);
    if (game.placed.has(key)) return false;
    if (!inBounds(cx, cy, cz)) return false;
    if (cellReserved(cx, cy, cz)) return false;
    const def = PIECES[type];
    if (!def) return false;
    if (!opts.furniture) {
        const bud = budget[type];
        if (!bud || bud.used >= bud.limit) return false;
    }
    const cw = { x: cx + 0.5, y: cy + 0.5, z: cz + 0.5 };
    const built = def.build(scene, cw, rot | 0);
    const rec = {
        type, rot: rot | 0, cell: [cx, cy, cz],
        node:        built.node,
        body:        built.body,
        extras:      built.extras || [],
        extraBodies: built.extraBodies || [],
        anim:        built.anim || null,
        furniture:   !!opts.furniture,
    };
    game.placed.set(key, rec);
    if (!opts.furniture) budget[type].used += 1;
    // Reverse maps for raycast removal + contact event handling
    if (rec.node) game.meshToCell.set(rec.node.id, key);
    for (const ex of rec.extras)   game.meshToCell.set(ex.id, key);
    if (rec.body != null) game.bodyToCell.set(rec.body, key);
    for (const eb of rec.extraBodies) game.bodyToCell.set(eb, key);
    if (rec.anim) game.animatedCells.push(key);
    return true;
}

function removePiece(key) {
    const rec = game.placed.get(key);
    if (!rec) return false;
    if (rec.furniture) return false;
    if (rec.node)      { game.meshToCell.delete(rec.node.id); scene.destroyNode(rec.node); }
    for (const ex of rec.extras) {
        game.meshToCell.delete(ex.id);
        scene.destroyNode(ex);
    }
    if (rec.body != null) { game.bodyToCell.delete(rec.body); Physics.destroyBody(rec.body); }
    for (const eb of rec.extraBodies) {
        game.bodyToCell.delete(eb);
        Physics.destroyBody(eb);
    }
    const idx = game.animatedCells.indexOf(key);
    if (idx >= 0) game.animatedCells.splice(idx, 1);
    game.placed.delete(key);
    budget[rec.type].used = Math.max(0, budget[rec.type].used - 1);
    return true;
}

// ── Ghost preview ──────────────────────────────────────────────────────────
// The ghost uses the actual piece's build() factory so rotations and multi-
// part shapes (chute) are faithfully previewed. We destroy the physics body
// immediately and crank emissive on the visuals so it reads as a hologram.
// Geometry is built around the origin so we can reposition by just moving
// the parent group — no need to re-call build() on mousemove.

function destroyGhost() {
    if (!SCENE.ghostParts) return;
    for (const p of SCENE.ghostParts) {
        if (p.node) scene.destroyNode(p.node);
    }
    SCENE.ghostParts.length = 0;
    if (SCENE.ghostIds) SCENE.ghostIds.clear();
}

function setGhostVisible(v) {
    if (!SCENE.ghostParts) return;
    for (const p of SCENE.ghostParts) if (p.node) p.node.visible = v;
}

function moveGhost(cx, cy, cz) {
    if (!SCENE.ghostParts) return;
    const wx = cx + 0.5, wy = cy + 0.5, wz = cz + 0.5;
    for (const p of SCENE.ghostParts) {
        if (!p.node) continue;
        p.node.x = wx + p.bx;
        p.node.y = wy + p.by;
        p.node.z = wz + p.bz;
    }
}

function setGhostEmissive(em) {
    if (!SCENE.ghostParts) return;
    for (const p of SCENE.ghostParts) {
        if (!p.node) continue;
        try { p.node.emissive = em; } catch (e) {}
    }
}

function rebuildGhost() {
    destroyGhost();
    const def = PIECES[game.build.selected];
    if (!def) return;
    // Build the piece at world origin; record each visual's baseline
    // offset (for compound pieces like the chute where parts are spread
    // across the cell). Then destroy the physics bodies the factory
    // opened — ghosts are visual-only.
    const built = def.build(scene, { x: 0, y: 0, z: 0 }, game.build.rot | 0);
    if (built.body != null) Physics.destroyBody(built.body);
    for (const eb of (built.extraBodies || [])) Physics.destroyBody(eb);
    const visuals = [built.node, ...(built.extras || [])].filter(Boolean);
    for (const n of visuals) {
        const bx = n.x, by = n.y, bz = n.z;
        try { n.emissive = 1.6; }  catch (e) {}
        try { n.roughness = 0.4; } catch (e) {}
        n.visible = false;
        SCENE.ghostParts.push({ node: n, bx, by, bz });
        SCENE.ghostIds.add(n.id);
    }
}

// ── Cursor picking ──────────────────────────────────────────────────────────
// Returns {cx, cy, cz} for the cell the cursor is hovering. Prefers a mesh
// raycast so clicking on top of an existing piece gives the cell on the
// face struck (natural stacking). Falls back to intersecting the active
// build-layer plane when no piece is hit.
function cellUnderCursor(e) {
    const rect = canvas.getBoundingClientRect();
    const mx = (e.clientX - rect.left) * (canvas.width  / rect.width);
    const my = (e.clientY - rect.top)  * (canvas.height / rect.height);
    const ray = scene.unprojectLocal(mx, my);
    if (!ray) return null;

    // Hide the ghost before raycasting so it doesn't pick itself.
    const wasVisible = SCENE.ghostParts.length > 0 &&
                       SCENE.ghostParts[0].node && SCENE.ghostParts[0].node.visible;
    if (wasVisible) setGhostVisible(false);
    const hit = scene.raycast(ray.origin, ray.dir, 200);
    if (wasVisible) setGhostVisible(true);

    if (hit && hit.node) {
        const key = game.meshToCell.get(hit.node.id);
        if (key) {
            const rec = game.placed.get(key);
            if (rec && hit.normal) {
                // Snap the hit normal to its dominant axis; place in the
                // adjacent cell along that axis. Falling back to +Y when
                // the normal is ambiguous gives a sensible "put it on top"
                // default for tilted ramps.
                const n = hit.normal;
                let ax = 0, ay = 0, az = 0;
                const bx = Math.abs(n[0]), by = Math.abs(n[1]), bz = Math.abs(n[2]);
                if (bx >= by && bx >= bz)      ax = Math.sign(n[0]);
                else if (by >= bz)             ay = Math.sign(n[1]);
                else                           az = Math.sign(n[2]);
                if (ax === 0 && ay === 0 && az === 0) ay = 1;
                return {
                    cx: rec.cell[0] + ax,
                    cy: rec.cell[1] + ay,
                    cz: rec.cell[2] + az,
                    ray,
                    onPiece: true,
                };
            }
        }
    }

    // Fallback: layer plane at the active build layer.
    const y   = game.build.layer;
    const top = y + 1;
    const o   = ray.origin, d = ray.dir;
    function planeHit(py) {
        if (Math.abs(d[1]) < 1e-6) return null;
        const t = (py - o[1]) / d[1];
        if (t < 0) return null;
        return { wx: o[0] + d[0] * t, wz: o[2] + d[2] * t };
    }
    const h = planeHit(top) || planeHit(y + 0.5) || planeHit(y);
    if (!h) return null;
    return { cx: Math.floor(h.wx), cy: y, cz: Math.floor(h.wz), ray };
}

function updateGhost(e) {
    if (game.mode !== 'build' || SCENE.ghostParts.length === 0) {
        setGhostVisible(false);
        return;
    }
    const c = cellUnderCursor(e);
    if (!c) { setGhostVisible(false); return; }
    const key      = cellKey(c.cx, c.cy, c.cz);
    const inb      = inBounds(c.cx, c.cy, c.cz);
    const reserved = cellReserved(c.cx, c.cy, c.cz);
    const occupied = game.placed.has(key);
    const bud      = budget[game.build.selected];
    const noBudget = !bud || bud.used >= bud.limit;
    const valid    = inb && !reserved && !occupied && !noBudget;

    SCENE.ghostCellKey     = key;
    SCENE.outOfBoundsGhost = !valid;
    moveGhost(c.cx, c.cy, c.cz);
    setGhostVisible(true);
    // Dim when placement would be rejected so the player sees at a glance
    // that clicking won't succeed.
    setGhostEmissive(valid ? 1.6 : 0.2);
}

// ── Click handlers ──────────────────────────────────────────────────────────
canvas.addEventListener('mousemove', (e) => { updateGhost(e); });

canvas.addEventListener('click', (e) => {
    if (e.button !== 0) return;
    if (game.mode !== 'build' || overlayOpen()) return;
    const c = cellUnderCursor(e);
    if (!c) return;
    const placed = placePiece(game.build.selected, c.cx, c.cy, c.cz, game.build.rot);
    if (placed) {
        sfx.place();
        refreshPalette();
        updateGhost(e); // refresh ghost state (budget dimming, occupancy)
    } else {
        Hud.toast('Cannot place there.', 900,
            { id: 'tumble-toast', className: 'tumble-toast',
              container: document.getElementById('hud-toast-area') });
    }
});

// We use contextmenu (right-click) for removal since right-drag is orbit;
// pure right-click without drag still fires contextmenu.
canvas.addEventListener('contextmenu', (e) => {
    e.preventDefault();
    if (game.mode !== 'build' || overlayOpen()) return;
    // Prefer a mesh raycast — it finds a piece even if the cursor isn't
    // hovering exactly on the layer plane.
    const rect = canvas.getBoundingClientRect();
    const mx = (e.clientX - rect.left) * (canvas.width  / rect.width);
    const my = (e.clientY - rect.top)  * (canvas.height / rect.height);
    const ray = scene.unprojectLocal(mx, my);
    if (!ray) return;
    const hit = scene.raycast(ray.origin, ray.dir, 200);
    if (hit && hit.node) {
        const key = game.meshToCell.get(hit.node.id);
        if (key && removePiece(key)) {
            sfx.remove();
            refreshPalette();
            return;
        }
    }
    // Fallback: remove by cell.
    const c = cellUnderCursor(e);
    if (c) {
        const key = cellKey(c.cx, c.cy, c.cz);
        if (removePiece(key)) { sfx.remove(); refreshPalette(); }
    }
});

// ── Build/Run mode transitions ──────────────────────────────────────────────
function enterBuildMode() {
    game.mode     = 'build';
    game.paused   = false;
    game.resultMs = null;
    game.runtime  = 0;
    if (game.pendingFail) { clearTimeout(game.pendingFail); game.pendingFail = null; }
    // Wipe any marbles
    for (const m of game.marbles) {
        if (m.body != null) Physics.destroyBody(m.body);
        if (m.node) scene.destroyNode(m.node);
    }
    game.marbles.length = 0;
    game.marblesSpawned = 0;
    game.marblesRemoved = 0;
    if (SCENE.layerPlane) {
        SCENE.layerPlane.visible = true;
        SCENE.layerPlane.y = game.build.layer + 0.001;
    }
    // Ghost is rebuilt here so it reflects the currently-selected piece
    // and starts hidden until the cursor moves onto a valid cell.
    rebuildGhost();
    Hud.text('#hud-mode', 'BUILD');
    document.getElementById('hud-mode').classList.remove('run');
}

function enterRunMode() {
    // Can we run? At least one placed piece should exist, but empty runs are
    // allowed (gravity-only puzzle). We do require the goal to be reachable
    // to not instantly spam 'fail' — but we don't simulate that offline, so
    // just start running.
    game.mode     = 'run';
    game.paused   = false;
    game.resultMs = null;
    game.startMs  = performance.now();
    game.runtime  = 0;
    game.nextSpawnAt = game.startMs;   // first marble drops immediately
    game.marblesSpawned = 0;
    game.marblesRemoved = 0;
    if (SCENE.layerPlane) SCENE.layerPlane.visible = false;
    setGhostVisible(false);
    Hud.text('#hud-mode', 'RUN');
    document.getElementById('hud-mode').classList.add('run');
    sfx.drop();
}

function toggleMode() {
    if (game.mode === 'build') enterRunMode();
    else                        enterBuildMode();
}

// ── Marble lifecycle ────────────────────────────────────────────────────────
function spawnMarble() {
    const sp = game.level.spawner;
    const body = Physics.createBody({
        shape: 'sphere', radius: 0.17,
        position: { x: sp.x, y: sp.y, z: sp.z },
        friction:    0.18,
        restitution: 0.4,
    });
    const node = scene.createMesh({
        mesh: 'sphere', radius: 0.17,
        segments: 20, rings: 14,
        x: sp.x, y: sp.y, z: sp.z,
        color: '#f5f0ff', metallic: 1.0, roughness: 0.15,
        emissive: 0.2, emissiveColor: [0.6, 0.75, 1.0],
    });
    game.marbles.push({ body, node });
    game.marblesSpawned += 1;
    game.bodyToCell.set(body, '__marble');
}

function destroyMarble(m) {
    if (m.body != null) { game.bodyToCell.delete(m.body); Physics.destroyBody(m.body); }
    if (m.node) scene.destroyNode(m.node);
    game.marblesRemoved += 1;
}

function marbleInGoal(pos) {
    const g = game.level.goal;
    return pos.x >= g.min[0] && pos.x <= g.max[0] &&
           pos.y >= g.min[1] && pos.y <= g.max[1] &&
           pos.z >= g.min[2] && pos.z <= g.max[2];
}

// ── Simulation tick ─────────────────────────────────────────────────────────
function simTick(dt) {
    if (game.mode !== 'run' || game.paused) return;
    game.runtime += dt;
    const now = game.startMs + game.runtime;

    // Spawn marbles until the level cap.
    if (game.marblesSpawned < game.level.maxMarbles && now >= game.nextSpawnAt) {
        spawnMarble();
        sfx.drop();
        game.nextSpawnAt = now + game.level.spawnInterval;
    }

    // Sync marble visuals from physics, detect scoring / out-of-bounds.
    for (let i = game.marbles.length - 1; i >= 0; i--) {
        const m  = game.marbles[i];
        const tf = Physics.getTransform(m.body);
        if (!tf) { game.marbles.splice(i, 1); continue; }
        const p = tf.position;
        m.node.x = p.x;
        m.node.y = p.y;
        m.node.z = p.z;
        if (game.resultMs == null && marbleInGoal(p)) {
            game.resultMs = game.runtime;
            onLevelCompleted();
        }
        if (p.y < -6) {
            destroyMarble(m);
            game.marbles.splice(i, 1);
        }
    }

    // Animate spinners / process boosters.
    for (const key of game.animatedCells) {
        const rec = game.placed.get(key);
        if (!rec || !rec.anim) continue;
        if (rec.anim.kind === 'spinner') {
            rec.anim.phase += dt * 0.004;    // rad / ms ≈ 230°/s
            const yaw = rotY(rec.anim.rot) + rec.anim.phase;
            const q = quatY(yaw);
            Physics.setRotation(rec.body, q.x, q.y, q.z, q.w);
            rec.node.rotationY = yaw;
        }
    }

    // Contact-driven impulses: boosters push marbles along their axis;
    // spinners impart a tangential whack.
    const events = Physics.getContacts();
    for (const ev of events) {
        if (ev.type !== 'added') continue;
        let marbleId = null, pieceKey = null;
        if (game.bodyToCell.get(ev.body1) === '__marble') {
            marbleId = ev.body1;
            pieceKey = game.bodyToCell.get(ev.body2);
        } else if (game.bodyToCell.get(ev.body2) === '__marble') {
            marbleId = ev.body2;
            pieceKey = game.bodyToCell.get(ev.body1);
        }
        if (marbleId == null || !pieceKey || pieceKey === '__marble') continue;
        const rec = game.placed.get(pieceKey);
        if (!rec) continue;
        if (rec.type === 'booster') {
            // impulse along rec.rot * 90° (world XZ)
            const yaw = rotY(rec.rot || 0);
            const fx = Math.cos(yaw);
            const fz = Math.sin(yaw);
            Physics.addImpulse(marbleId, fx * 0.22, 0.05, fz * 0.22);
            sfx.clink();
        } else if (rec.type === 'spinner') {
            const v = Physics.getVelocity(marbleId);
            if (v) {
                // Enhance their tangential speed so the hit reads as the
                // paddle flinging the marble off.
                Physics.addImpulse(marbleId, v.linear.x * 0.5, 0.1, v.linear.z * 0.5);
            }
            sfx.clink();
        } else if (rec.type === 'bumper') {
            sfx.clink();
        }
    }

    // Fail-safe: if we've spent everything and no result yet, give a grace
    // window for the last marble to settle, then show 'fail'.
    if (game.resultMs == null &&
        game.marblesSpawned >= game.level.maxMarbles &&
        game.marblesRemoved >= game.marblesSpawned &&
        game.pendingFail == null) {
        game.pendingFail = setTimeout(() => {
            if (game.resultMs == null && game.mode === 'run') {
                onLevelFailed();
            }
        }, 900);
    }
}

// ── Level completion ────────────────────────────────────────────────────────
function onLevelCompleted() {
    const t = game.resultMs / 1000;
    const level = game.level;
    const bestMap = store.get('best') || {};
    const prev = bestMap[level.id];
    if (prev == null || t < prev) {
        bestMap[level.id] = t;
        store.set('best', bestMap);
    }
    // Unlock the next level
    const nextIdx = game.levelIdx + 1;
    const unlocked = store.get('unlocked') || 1;
    if (nextIdx + 1 > unlocked && nextIdx < LEVELS.length) {
        store.set('unlocked', nextIdx + 1);
    }
    store.save();
    sfx.goal();

    const medal = medalFor(t, level);
    const title = document.getElementById('complete-title');
    title.textContent = (medal === 'gold' ? 'Gold — '
                       : medal === 'silver' ? 'Silver — '
                       : medal === 'bronze' ? 'Bronze — '
                       : 'Complete — ') + level.name;
    document.getElementById('complete-time').textContent = fmt(t);
    const medalEl = document.getElementById('complete-medal');
    medalEl.textContent = medal === 'none' ? 'Complete' : medal.toUpperCase();
    medalEl.className = 'medal ' + (medal === 'none' ? '' : medal);
    const best = bestMap[level.id];
    document.getElementById('complete-detail').textContent =
        `Par: gold ${fmt(level.par.gold)} · silver ${fmt(level.par.silver)} · bronze ${fmt(level.par.bronze)}` +
        `  —  Best ${fmt(best)}`;
    screens.switchTo('complete');
}

function onLevelFailed() {
    sfx.fail();
    Hud.toast('No marbles reached the cup. Rebuild and try again.', 2400,
        { id: 'tumble-toast', className: 'tumble-toast',
          container: document.getElementById('hud-toast-area') });
    enterBuildMode();
    refreshPalette();
}

// ── HUD / palette ───────────────────────────────────────────────────────────
function refreshPalette() {
    const el = document.getElementById('hud-palette');
    if (!el) return;
    const available = PIECE_ORDER.filter(t => (budget[t] || { limit: 0 }).limit > 0);
    el.innerHTML = available.map((t, i) => {
        const def  = PIECES[t];
        const b    = budget[t];
        const left = b.limit - b.used;
        const cls  = ['palette-item',
                      t === game.build.selected ? 'selected' : '',
                      left <= 0 ? 'disabled' : ''].filter(Boolean).join(' ');
        return `
            <div class="${cls}" data-piece="${t}">
                <div class="palette-key">${def.key}</div>
                <div class="palette-swatch" style="background:${def.color};
                    box-shadow: 0 0 6px ${def.color}88;"></div>
                <div class="palette-name">${def.label}</div>
                <div class="palette-count">${left}/${b.limit}</div>
            </div>`;
    }).join('');
    // Click-to-select piece
    for (const item of el.querySelectorAll('.palette-item')) {
        item.addEventListener('click', () => {
            game.build.selected = item.getAttribute('data-piece');
            sfx.select();
            refreshPalette();
            rebuildGhost();
        });
    }
    // Totals
    let used = 0, limit = 0;
    for (const t of PIECE_ORDER) {
        used  += budget[t].used;
        limit += budget[t].limit;
    }
    Hud.text('#hud-budget', used + ' / ' + limit);
}

function refreshHud() {
    if (!game.level) return;
    Hud.text('#hud-level', 'Level ' + (game.levelIdx + 1) + ' — ' + game.level.name);
    Hud.text('#hud-par', fmt(game.level.par.bronze) + ' / ' + fmt(game.level.par.gold));
    const bestMap = store.get('best') || {};
    Hud.text('#hud-best', fmt(bestMap[game.level.id]));
}

// Called every draw frame to keep per-tick counters fresh.
function tickHud() {
    if (!game.level) return;
    Hud.text('#hud-marbles',
        game.marblesSpawned + '/' + game.level.maxMarbles +
        (game.marblesRemoved ? '  (' + game.marblesRemoved + ' cleared)' : ''));
    if (game.mode === 'run') {
        Hud.text('#hud-timer',
            (game.resultMs != null ? (game.resultMs / 1000) : (game.runtime / 1000))
                .toFixed(2) + 's');
    } else {
        Hud.text('#hud-timer', '—');
    }
}

// ── Screens ─────────────────────────────────────────────────────────────────
function overlayOpen() {
    const el = document.getElementById('overlay');
    return el && el.style.display !== 'none';
}

screens.define('title', {
    enter() {
        Hud.hide('#hud');
        screens.showOverlay('title');
        screens.updateSelection('title');
    },
    keydown(key) {
        screens.menuNav('title', key, (idx, item) => {
            const a = item && item.dataset.action;
            if      (a === 'play')   { loadLevel(Math.min(store.get('lastLevel') || 0, LEVELS.length - 1));
                                        screens.hideOverlay(); Hud.show('#hud'); }
            else if (a === 'levels') { screens.switchTo('levels'); }
            else if (a === 'howto')  { screens.switchTo('howto');  }
            else if (a === 'reset')  {
                store.set('best', {}); store.set('unlocked', 1); store.set('lastLevel', 0);
                store.save();
                Hud.toast('Progress reset.', 1200, {
                    id: 'tumble-toast', className: 'tumble-toast',
                    container: document.body });
            }
            else if (a === 'quit')   { window.close(); }
        });
    },
});

screens.define('levels', {
    enter() {
        renderLevelTiles();
        screens.showOverlay('levels');
        screens.updateSelection('levels');
    },
    keydown(key) {
        screens.menuNav('levels', key, (idx, item) => {
            const a = item && item.dataset.action;
            if (a === 'back') screens.switchTo('title');
        }, { onBack: () => screens.switchTo('title') });
    },
});

function renderLevelTiles() {
    const grid = document.getElementById('levels-grid');
    if (!grid) return;
    const bestMap  = store.get('best')     || {};
    const unlocked = store.get('unlocked') || 1;
    grid.innerHTML = LEVELS.map((lv, i) => {
        const locked = i >= unlocked;
        const best   = bestMap[lv.id];
        const medal  = best != null ? medalFor(best, lv) : 'none';
        const medalGlyph = medal === 'gold'   ? '\u2605'
                         : medal === 'silver' ? '\u2605'
                         : medal === 'bronze' ? '\u2605'
                         : '\u2606';
        return `
            <div class="level-tile ${locked ? 'locked' : ''}" data-idx="${i}">
                <div class="level-num">Level ${i + 1}</div>
                <div class="level-name">${lv.name}</div>
                <div class="level-best">${best != null ? fmt(best) : (locked ? 'locked' : '')}</div>
                <div class="level-medal ${medal}" style="color:${
                    medal === 'gold'   ? '#ffd84a' :
                    medal === 'silver' ? '#d8dce4' :
                    medal === 'bronze' ? '#c88a5a' : '#444'
                }">${medalGlyph}</div>
            </div>`;
    }).join('');
    for (const tile of grid.querySelectorAll('.level-tile')) {
        tile.addEventListener('click', () => {
            const i = parseInt(tile.getAttribute('data-idx'), 10);
            const unlocked = store.get('unlocked') || 1;
            if (i >= unlocked) return;
            loadLevel(i);
            screens.hideOverlay();
            Hud.show('#hud');
        });
    }
}

screens.define('howto', {
    enter() { screens.showOverlay('howto'); screens.updateSelection('howto'); },
    keydown(key) {
        screens.menuNav('howto', key,
            () => screens.switchTo('title'),
            { onBack: () => screens.switchTo('title') });
    },
});

screens.define('pause', {
    enter() { screens.showOverlay('pause'); screens.updateSelection('pause'); },
    keydown(key) {
        screens.menuNav('pause', key, (idx, item) => {
            const a = item && item.dataset.action;
            if      (a === 'resume')   { screens.hideOverlay(); Hud.show('#hud'); }
            else if (a === 'restart')  { screens.hideOverlay(); Hud.show('#hud'); restartLevel(); }
            else if (a === 'levels')   { screens.switchTo('levels'); }
            else if (a === 'title')    { screens.switchTo('title'); }
        }, { onBack: () => { screens.hideOverlay(); Hud.show('#hud'); } });
    },
});

screens.define('complete', {
    enter() {
        Hud.hide('#hud');
        screens.showOverlay('complete');
        screens.updateSelection('complete');
    },
    keydown(key) {
        screens.menuNav('complete', key, (idx, item) => {
            const a = item && item.dataset.action;
            if (a === 'next') {
                const nxt = Math.min(game.levelIdx + 1, LEVELS.length - 1);
                loadLevel(nxt);
                screens.hideOverlay();
                Hud.show('#hud');
            } else if (a === 'retry') {
                restartLevel();
                screens.hideOverlay();
                Hud.show('#hud');
            } else if (a === 'levels') {
                screens.switchTo('levels');
            }
        });
    },
});

// ── Keyboard ────────────────────────────────────────────────────────────────
window.addEventListener('keydown', (e) => {
    if (overlayOpen()) {
        screens.keydown(e.key);
        return;
    }
    if (!game.level) return;

    // Piece selection hotkeys 1..7
    if (/^[1-7]$/.test(e.key)) {
        const idx = parseInt(e.key, 10) - 1;
        const available = PIECE_ORDER.filter(t => (budget[t] || { limit: 0 }).limit > 0);
        if (idx < available.length) {
            game.build.selected = available[idx];
            sfx.select();
            refreshPalette();
            rebuildGhost();
        }
        return;
    }

    if (e.key === 'r' || e.key === 'R') {
        const def = PIECES[game.build.selected];
        if (def && def.rotatable) {
            game.build.rot = (game.build.rot + 1) & 3;
            sfx.menu();
            rebuildGhost();
        }
        return;
    }
    if (e.key === 'q' || e.key === 'Q') {
        if (game.mode !== 'build') return;
        const b = game.level.bounds.y;
        game.build.layer = Math.max(b[0], game.build.layer - 1);
        if (SCENE.layerPlane) SCENE.layerPlane.y = game.build.layer + 0.001;
        sfx.menu();
        return;
    }
    if (e.key === 'e' || e.key === 'E') {
        if (game.mode !== 'build') return;
        const b = game.level.bounds.y;
        game.build.layer = Math.min(b[1], game.build.layer + 1);
        if (SCENE.layerPlane) SCENE.layerPlane.y = game.build.layer + 0.001;
        sfx.menu();
        return;
    }
    if (e.key === ' ') {
        e.preventDefault();
        toggleMode();
        return;
    }
    if (e.key === 'Escape') {
        if (game.mode === 'run') enterBuildMode();
        screens.switchTo('pause');
        return;
    }
});

// ── Main loop ──────────────────────────────────────────────────────────────
GameLoop.create({
    tick: (dt) => { simTick(dt); },
    draw: ()   => {
        applyCamera();
        tickHud();
    },
}).start();

// ── Boot ───────────────────────────────────────────────────────────────────
screens.switchTo('title');
