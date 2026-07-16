// Polygon navmesh — bro.ai.game.bakeNavMesh / loadNavMesh (Recast/Detour via
// brogameagent::NavMesh) plus node.navigateTo agent routing. Exercises raw
// triangle-soup bakes, fromPhysics static-geometry collection
// (PhysicsWorld::collectStaticTriangles), fromTerrain height sampling,
// multi-level paths (the NavGrid-impossible case), save/load round-trips,
// query methods, and AgentBinding navmesh route-following composed with ORCA
// avoidance. (src/js/ai_bindings.cpp, src/js/ai_binding_integration.cpp,
// src/scene/agent_binding.cpp, src/physics/physics_world.cpp)

const G = bro.ai.game;
if (G.navMeshAvailable !== true) {
    // Soft-disabled build — recastnavigation wasn't installed, so the
    // binding stubs the navmesh surface out. Nothing to test.
    console.log('test_navmesh: navmesh not compiled in, skipping');
} else {
    runNavMeshTests();
}

function runNavMeshTests() {
    assert(typeof G.bakeNavMesh === 'function', 'bakeNavMesh exists');
    assert(typeof G.loadNavMesh === 'function', 'loadNavMesh exists');

    // --- Soup builders ---------------------------------------------------------

    // Horizontal quad (CCW from above) at height y covering [x0,x1]x[z0,z1].
    function pushQuad(verts, idx, x0, z0, x1, z1, y) {
        const b = verts.length / 3;
        verts.push(x0, y, z0,  x0, y, z1,  x1, y, z0,  x1, y, z1);
        idx.push(b, b + 1, b + 2,  b + 2, b + 1, b + 3);
    }

    // Sloped quad from (x0, y0) to (x1, y1) along X, covering [z0,z1].
    function pushRamp(verts, idx, x0, y0, x1, y1, z0, z1) {
        const b = verts.length / 3;
        verts.push(x0, y0, z0,  x0, y0, z1,  x1, y1, z0,  x1, y1, z1);
        idx.push(b, b + 1, b + 2,  b + 2, b + 1, b + 3);
    }

    // Closed axis-aligned box (12 tris, CCW from outside).
    function pushBox(verts, idx, cx, cy, cz, hx, hy, hz) {
        const b = verts.length / 3;
        const x0 = cx - hx, x1 = cx + hx, y0 = cy - hy, y1 = cy + hy, z0 = cz - hz, z1 = cz + hz;
        verts.push(
            x0, y0, z0,  x1, y0, z0,  x1, y1, z0,  x0, y1, z0,   // -Z face verts
            x0, y0, z1,  x1, y0, z1,  x1, y1, z1,  x0, y1, z1);  // +Z face verts
        const q = (a, b2, c, d) => idx.push(b + a, b + b2, b + c, b + a, b + c, b + d);
        q(0, 1, 2, 3);  // -Z
        q(5, 4, 7, 6);  // +Z
        q(4, 0, 3, 7);  // -X
        q(1, 5, 6, 2);  // +X
        q(3, 2, 6, 7);  // top (+Y)
        q(4, 5, 1, 0);  // bottom (-Y)
    }

    function pathLen(p) {
        let len = 0;
        for (let i = 3; i < p.length; i += 3) {
            len += Math.hypot(p[i] - p[i - 3], p[i + 1] - p[i - 2], p[i + 2] - p[i - 1]);
        }
        return len;
    }

    // =========================================================================
    // Raw soup bake — floor + box obstacle, routing, unreachable, save/load
    // =========================================================================
    {
        const verts = [], idx = [];
        pushQuad(verts, idx, -12, -12, 12, 12, 0);        // floor
        pushBox(verts, idx, 0, 1, 0, 2, 1, 2);            // obstacle astride the midline
        pushQuad(verts, idx, 30, -3, 36, 3, 0);           // disconnected island

        const mesh = G.bakeNavMesh({
            positions: new Float32Array(verts),
            indices: new Uint32Array(idx),
            agentRadius: 0.5,
        });
        assert(mesh.valid, 'raw bake valid');

        // Straight line pierces the box → path must detour around the inflated
        // footprint (2 + agentRadius on each side).
        const p = mesh.findPath({ x: -9, y: 0, z: 0 }, { x: 9, y: 0, z: 0 });
        assert(p !== null, 'path around obstacle found');
        assert(p instanceof Float32Array, 'findPath returns Float32Array');
        assert(p.length >= 6 && p.length % 3 === 0, 'xyz triples, got ' + p.length);
        assert(Math.hypot(p[0] - (-9), p[2] - 0) < 1.0, 'path starts near start');
        assert(Math.hypot(p[p.length - 3] - 9, p[p.length - 1] - 0) < 1.0, 'path ends near goal');
        assert(pathLen(p) > 18, 'detour is longer than the straight line, got ' + pathLen(p).toFixed(2));
        for (let i = 0; i < p.length; i += 3) {
            const inside = Math.abs(p[i]) < 2.3 && Math.abs(p[i + 2]) < 2.3;
            assert(!inside, 'no waypoint inside the inflated obstacle footprint: (' +
                p[i].toFixed(2) + ',' + p[i + 2].toFixed(2) + ')');
        }

        // Unreachable: disconnected island → null (never a truncated partial path).
        assert(mesh.findPath({ x: 0, y: 0, z: 8 }, { x: 33, y: 0, z: 0 }) === null,
            'unreachable island returns null');
        // Off-mesh endpoint (fails to snap) → null.
        assert(mesh.findPath({ x: 0, y: 0, z: 8 }, { x: 100, y: 0, z: 100 }) === null,
            'unsnappable endpoint returns null');

        // nearestPoint snaps onto the mesh (within default extents {2,1,2});
        // far-away point returns null. Widened y extents reach farther.
        const np = mesh.nearestPoint({ x: 5, y: 0.8, z: 5 });
        assert(np !== null && Math.abs(np.y) < 0.5, 'nearestPoint snaps to floor height, y=' + (np && np.y));
        assert(mesh.nearestPoint({ x: 5, y: 3, z: 5 }) === null, 'y=3 outside default y-extent 1');
        const npWide = mesh.nearestPoint({ x: 5, y: 3, z: 5 }, { x: 2, y: 4, z: 2 });
        assert(npWide !== null, 'widened extents reach the floor from y=3');
        assert(mesh.nearestPoint({ x: 500, y: 0, z: 500 }) === null, 'nearestPoint far away returns null');

        // raycast: open run is unobstructed, run into the box footprint hits.
        const clear = mesh.raycast({ x: -9, y: 0, z: 8 }, { x: 9, y: 0, z: 8 });
        assert(!clear.hit && clear.t >= 1.0, 'open raycast unobstructed');
        const blocked = mesh.raycast({ x: -9, y: 0, z: 0 }, { x: 9, y: 0, z: 0 });
        assert(blocked.hit && blocked.t < 1.0, 'raycast into obstacle hits, t=' + blocked.t.toFixed(3));
        assert(blocked.point.x < -2.0, 'hit point is before the box, x=' + blocked.point.x.toFixed(2));

        // randomPoint is deterministic per seed and lands on the mesh.
        const r1 = mesh.randomPoint(7), r2 = mesh.randomPoint(7);
        assert(r1 !== null && r1.x === r2.x && r1.z === r2.z, 'randomPoint deterministic per seed');

        // save → load → identical path (bit-exact: same blob, deterministic query).
        const blob = mesh.save();
        assert(blob instanceof ArrayBuffer && blob.byteLength > 0, 'save yields bytes');
        const loaded = G.loadNavMesh(blob);
        assert(loaded.valid, 'loaded mesh valid');
        const p2 = loaded.findPath({ x: -9, y: 0, z: 0 }, { x: 9, y: 0, z: 0 });
        assert(p2 !== null && p2.length === p.length, 'loaded path same length');
        for (let i = 0; i < p.length; i++) assert(p[i] === p2[i], 'loaded path identical at ' + i);

        // Malformed blob throws.
        let threw = false;
        try { G.loadNavMesh(new Uint8Array([1, 2, 3, 4]).buffer); } catch (e) { threw = true; }
        assert(threw, 'loadNavMesh rejects malformed data');

        // Bake failure surfaces lastError() text.
        threw = false;
        try {
            G.bakeNavMesh({ positions: new Float32Array([0, 0, 0]), indices: new Uint32Array([0, 0, 0]) });
        } catch (e) { threw = true; assert(String(e.message).length > 0, 'bake error has message'); }
        assert(threw, 'degenerate bake throws');
    }

    // =========================================================================
    // fromPhysics — static mesh-shape + primitive box block; sensors don't
    // =========================================================================
    {
        Physics.destroyAll();
        // Floor slab (primitive box → Jolt 12-tri tessellation, top face walkable).
        Physics.createBody({
            shape: 'box', halfExtents: { x: 12, y: 0.5, z: 12 },
            position: { x: 0, y: -0.5, z: 0 }, static: true,
        });
        // Mesh-shape obstacle: a real triangle box astride the route.
        const mv = [], mi = [];
        pushBox(mv, mi, 0, 1, 0, 1.5, 1, 1.5);
        Physics.createBody({
            shape: 'mesh', positions: mv, indices: mi,
            position: { x: 0, y: 0, z: 0 }, static: true,
        });
        // Primitive box obstacle offset in z.
        Physics.createBody({
            shape: 'box', halfExtents: { x: 1.5, y: 1, z: 1.5 },
            position: { x: 0, y: 1, z: 6 }, static: true,
        });
        // Static sensor — must NOT block.
        Physics.createBody({
            shape: 'box', halfExtents: { x: 1.5, y: 1, z: 1.5 },
            position: { x: 0, y: 1, z: -6 }, static: true, sensor: true,
        });

        const mesh = G.bakeNavMesh({ fromPhysics: Physics, agentRadius: 0.4 });
        assert(mesh.valid, 'fromPhysics bake valid');

        // Route past the mesh-shape obstacle detours.
        const p1 = mesh.findPath({ x: -9, y: 0, z: 0 }, { x: 9, y: 0, z: 0 });
        assert(p1 !== null, 'path exists past mesh-shape obstacle');
        let maxAbsZ = 0;
        for (let i = 0; i < p1.length; i += 3) maxAbsZ = Math.max(maxAbsZ, Math.abs(p1[i + 2]));
        assert(maxAbsZ > 1.5, 'mesh-shape obstacle forces a detour, maxAbsZ=' + maxAbsZ.toFixed(2));

        // Primitive box also blocks.
        const p2 = mesh.findPath({ x: -9, y: 0, z: 6 }, { x: 9, y: 0, z: 6 });
        assert(p2 !== null, 'path exists past primitive box');
        let detoured = false;
        for (let i = 0; i < p2.length; i += 3) if (Math.abs(p2[i + 2] - 6) > 1.6) detoured = true;
        assert(detoured, 'primitive box forces a detour');

        // Sensor does not block: straight walkability ray across its footprint.
        const ray = mesh.raycast({ x: -9, y: 0, z: -6 }, { x: 9, y: 0, z: -6 });
        assert(!ray.hit, 'static sensor is not baked as an obstacle');

        Physics.destroyAll();
    }

    // =========================================================================
    // Multi-level — floor + elevated platform + ramp (NavGrid-impossible)
    // =========================================================================
    const twoLevel = (() => {
        const verts = [], idx = [];
        pushQuad(verts, idx, -12, -12, 12, 12, 0);       // ground floor
        pushQuad(verts, idx, 4, -4, 12, 4, 3);           // platform at y=3 (over the floor)
        pushRamp(verts, idx, -4, 0, 4, 3, -2, 2);        // ramp up (~20.6 deg)
        return G.bakeNavMesh({
            positions: new Float32Array(verts),
            indices: new Uint32Array(idx),
            agentRadius: 0.4,
        });
    })();
    {
        assert(twoLevel.valid, 'two-level bake valid');

        // Floor → platform resolves THROUGH the ramp: waypoints climb from 0 to 3.
        const up = twoLevel.findPath({ x: -8, y: 0, z: 0 }, { x: 10, y: 3, z: 0 });
        assert(up !== null, 'ramp traversal path found');
        let minY = 1e9, maxY = -1e9;
        for (let i = 1; i < up.length; i += 3) { minY = Math.min(minY, up[i]); maxY = Math.max(maxY, up[i]); }
        assert(minY < 0.5 && maxY > 2.5, 'path spans both levels, y in [' +
            minY.toFixed(2) + ',' + maxY.toFixed(2) + ']');

        // Stacked levels resolve per level: the platform overlaps the floor in
        // XZ, so the same XZ point snaps to whichever level the query Y is near
        // (default extents keep Y tight at 1).
        const onFloor = twoLevel.nearestPoint({ x: 8, y: 0.2, z: 0 });
        const onPlatform = twoLevel.nearestPoint({ x: 8, y: 3.2, z: 0 });
        assert(onFloor !== null && Math.abs(onFloor.y) < 0.6, 'y≈0 query snaps to the floor');
        assert(onPlatform !== null && Math.abs(onPlatform.y - 3) < 0.6, 'y≈3 query snaps to the platform');

        // Same-level path on the floor beneath the platform stays low.
        const flat = twoLevel.findPath({ x: 5, y: 0, z: -8 }, { x: 10, y: 0, z: 8 });
        assert(flat !== null, 'floor-level path under the platform found');
        for (let i = 1; i < flat.length; i += 3) {
            assert(flat[i] < 1.0, 'floor path stays on the floor, y=' + flat[i].toFixed(2));
        }
    }

    // =========================================================================
    // Agent routing — navigateTo across the ramp; RVO composition
    // =========================================================================
    {
        const canvas = document.createElement('canvas');
        canvas.setAttribute('width', '128');
        canvas.setAttribute('height', '128');
        document.body.appendChild(canvas);
        flush();
        const scene = canvas.getContext('scene');
        assert(scene !== null, 'scene context');

        const world = G.createWorld();
        world.setAvoidance(true);
        scene.attachAIWorld(world, { stepHz: 60 });

        const mkAgent = (x, z) => {
            const a = G.createAgent({ x, z, speed: 4, radius: 0.4, avoidance: true });
            world.addAgent(a);
            return a;
        };

        // Agent 1: attachAgent({navMesh}) + navigateTo across the ramp.
        const a1 = mkAgent(-8, -1);
        const n1 = scene.createMesh({ mesh: 'box', color: 'red' });
        n1.attachAgent(world, a1, { navMesh: twoLevel, yOffset: 0.5 });
        // Agent 2: navMesh passed via navigateTo opts instead.
        const a2 = mkAgent(-8, 1);
        const n2 = scene.createMesh({ mesh: 'box', color: 'blue' });
        n2.attachAgent(world, a2, { yOffset: 0.5 });

        assert(n1.navigateTo({ x: 10, y: 3, z: -1 }) === true, 'navigateTo #1 finds a route');
        assert(n2.navigateTo({ x: 10, y: 3, z: 1 }, { navMesh: twoLevel }) === true,
            'navigateTo #2 with opts.navMesh finds a route');

        // Unreachable target reports false and doesn't start moving.
        const a3 = mkAgent(0, -8);
        const n3 = scene.createMesh({ mesh: 'box' });
        n3.attachAgent(world, a3, { navMesh: twoLevel });
        assert(n3.navigateTo({ x: 100, y: 0, z: 100 }) === false, 'unreachable navigateTo returns false');

        // Drive the sim; track the inter-agent distance for RVO composition.
        let minSep = 1e9, sawClimb = false;
        for (let t = 0; t < 160; t++) {
            advanceTime(50);
            minSep = Math.min(minSep, Math.hypot(a1.x - a2.x, a1.z - a2.z));
            if (n1.position[1] > 1.0 && n1.position[1] < 3.0) sawClimb = true;
        }

        // Both arrive across the ramp (XZ convergence).
        assert(Math.hypot(a1.x - 10, a1.z - (-1)) < 1.0,
            'agent 1 arrives, at (' + a1.x.toFixed(2) + ',' + a1.z.toFixed(2) + ')');
        assert(Math.hypot(a2.x - 10, a2.z - 1) < 1.0,
            'agent 2 arrives, at (' + a2.x.toFixed(2) + ',' + a2.z.toFixed(2) + ')');
        // Unreachable agent never moved.
        assert(Math.hypot(a3.x - 0, a3.z - (-8)) < 0.01, 'failed navigateTo does not move the agent');

        // Node Y followed the route: passed through ramp heights, ends at
        // platform height + yOffset (no groundFollow set → waypoint Y drives).
        assert(sawClimb, 'node Y interpolated through ramp heights');
        assert(Math.abs(n1.position[1] - 3.5) < 0.6,
            'node 1 Y ends at platform + yOffset, got ' + n1.position[1].toFixed(2));

        // RVO composed: parallel routed agents never interpenetrated
        // (sum of radii = 0.8).
        assert(minSep > 0.7, 'avoidance kept routed agents apart, minSep=' + minSep.toFixed(2));

        // stopNavigation halts the route.
        const a4 = mkAgent(-8, 5);
        const n4 = scene.createMesh({ mesh: 'box' });
        n4.attachAgent(world, a4, { navMesh: twoLevel });
        assert(n4.navigateTo({ x: 8, y: 0, z: 5 }) === true, 'navigateTo #4 starts');
        advanceTime(500);
        n4.stopNavigation();
        const hx = a4.x, hz = a4.z;
        advanceTime(500);
        assert(Math.hypot(a4.x - hx, a4.z - hz) < 0.2, 'stopNavigation halts the agent');

        n1.detachAgent(); n2.detachAgent(); n3.detachAgent(); n4.detachAgent();
        scene.detachAIWorld();
    }

    // =========================================================================
    // fromTerrain — height-sampled voxel terrain surface
    // =========================================================================
    {
        const canvas2 = document.createElement('canvas');
        canvas2.setAttribute('width', '64');
        canvas2.setAttribute('height', '64');
        document.body.appendChild(canvas2);
        flush();
        const scene2 = canvas2.getContext('scene');

        const terrain = scene2.createTerrain({
            chunkSize: [16, 16, 16],
            cellSize: 1.0,
            loadRadius: 2,
            unloadRadius: 3,
            maxLoadsPerUpdate: 64,
            seed: 42,
            noise: { frequency: 0.05, octaves: 3, gain: 0.5, lacunarity: 2.0 },
            baseHeight: 6,
            heightAmplitude: 4,
            seaLevel: 0,
            meshMode: 0,
        });
        for (let i = 0; i < 8; i++) terrain.update(0, 8, 0);

        const mesh = G.bakeNavMesh({
            fromTerrain: terrain,
            terrainBounds: { minX: -10, minZ: -10, maxX: 10, maxZ: 10 },
            terrainStep: 1.0,
            agentRadius: 0.4,
        });
        assert(mesh.valid, 'terrain bake valid');

        // The mesh sits at the terrain surface: nearestPoint height matches the
        // terrain's own raycast probe (within a cell of tolerance).
        const hit = terrain.raycast([0, 100, 0], [0, -1, 0], 200);
        assert(hit !== null, 'terrain probe hit');
        const np = mesh.nearestPoint({ x: 0, y: hit.position[1], z: 0 });
        assert(np !== null, 'terrain navmesh snaps at origin');
        assert(Math.abs(np.y - hit.position[1]) < 1.5,
            'navmesh height matches terrain surface: ' + np.y.toFixed(2) +
            ' vs ' + hit.position[1].toFixed(2));

        // Paths route across the sampled surface.
        const s = mesh.nearestPoint({ x: -7, y: hit.position[1], z: -7, });
        const e = mesh.nearestPoint({ x: 7, y: hit.position[1], z: 7 });
        assert(s !== null && e !== null, 'terrain endpoints snap');
        const p = mesh.findPath(s, e, { x: 2, y: 4, z: 2 });
        assert(p !== null && p.length >= 6, 'terrain path found');

        terrain.destroy();
    }

    console.log('test_navmesh: OK');

}
