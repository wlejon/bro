// Dynamic navmesh obstacles — bakeNavMesh({dynamicObstacles: true}) builds a
// tiled dtTileCache-backed mesh whose walkable surface can change at runtime:
// addObstacle (cylinder / AABB box / Y-rotated box) and removeObstacle queue
// changes, incremental tile rebuilds apply them (mesh.update() pump or the
// engine's per-frame auto-pump), generation bumps once per applied batch, and
// navigating agents repath automatically when the surface changes under their
// active route. (brogameagent/src/nav_mesh.cpp, src/js/ai_bindings.cpp,
// src/scene/agent_binding.cpp, pumpNavMeshObstacles in engine_frame.cpp /
// headless_api.cpp)

const G = bro.ai.game;
if (G.navMeshAvailable !== true) {
    // Soft-disabled build — recastnavigation wasn't installed, so the
    // binding stubs the navmesh surface out. Nothing to test.
    console.log('test_navmesh_obstacles: navmesh not compiled in, skipping');
} else {
    runObstacleTests();
}

function runObstacleTests() {
    // --- Soup builders ---------------------------------------------------------

    function pushQuad(verts, idx, x0, z0, x1, z1, y) {
        const b = verts.length / 3;
        verts.push(x0, y, z0,  x0, y, z1,  x1, y, z0,  x1, y, z1);
        idx.push(b, b + 1, b + 2,  b + 2, b + 1, b + 3);
    }

    function bakeFloor(x0, z0, x1, z1, extraOpts) {
        const verts = [], idx = [];
        pushQuad(verts, idx, x0, z0, x1, z1, 0);
        return G.bakeNavMesh(Object.assign({
            positions: new Float32Array(verts),
            indices: new Uint32Array(idx),
            agentRadius: 0.5,
            dynamicObstacles: true,
            tileSize: 8,
        }, extraOpts || {}));
    }

    function pathLen(p) {
        let len = 0;
        for (let i = 3; i < p.length; i += 3) {
            len += Math.hypot(p[i] - p[i - 3], p[i + 1] - p[i - 2], p[i + 2] - p[i - 1]);
        }
        return len;
    }

    // Pump update() until the tile cache settles; returns the call count.
    function settle(mesh) {
        let n = 0;
        while (!mesh.update() && n < 64) n++;
        assert(n < 64, 'update pump converges, took ' + n + ' calls');
        return n + 1;
    }

    // =========================================================================
    // API gating + cylinder obstacle block/detour/restore (explicit pump)
    // =========================================================================
    {
        const mesh = bakeFloor(-12, -12, 12, 12);
        assert(mesh.valid, 'tiled bake valid');
        assert(mesh.supportsObstacles === true, 'supportsObstacles on tiled mesh');
        assert(mesh.obstacleCount === 0, 'no obstacles at bake');
        assert(mesh.obstaclesPending === false, 'nothing pending at bake');
        assert(mesh.update() === true, 'update with nothing pending is up to date');
        assert(typeof mesh.generation === 'number' && mesh.generation >= 1,
            'bake counts as a surface generation');

        const gen0 = mesh.generation;
        const open = mesh.findPath({ x: -9, y: 0, z: 0 }, { x: 9, y: 0, z: 0 });
        assert(open !== null, 'open path exists');
        const openLen = pathLen(open);

        // Queue a cylinder astride the route: nothing changes until the pump.
        const h = mesh.addObstacle({
            type: 'cylinder', pos: { x: 0, y: -0.5, z: 0 }, radius: 2, height: 3,
        });
        assert(typeof h === 'number' && h > 0, 'addObstacle returns a handle');
        assert(mesh.obstacleCount === 1, 'obstacleCount tracks the add');
        assert(mesh.obstaclesPending === true, 'change is pending before update');
        assert(mesh.generation === gen0, 'generation unchanged before update');
        const still = mesh.findPath({ x: -9, y: 0, z: 0 }, { x: 9, y: 0, z: 0 });
        assert(still !== null && Math.abs(pathLen(still) - openLen) < 0.01,
            'path unchanged until tiles rebuild');

        settle(mesh);
        assert(mesh.obstaclesPending === false, 'settled after pump');
        assert(mesh.generation === gen0 + 1, 'applied batch bumps generation once');

        const blocked = mesh.findPath({ x: -9, y: 0, z: 0 }, { x: 9, y: 0, z: 0 });
        assert(blocked !== null, 'detour path exists');
        assert(blocked.length >= 9, 'detour bends, ' + (blocked.length / 3) + ' waypoints');
        assert(pathLen(blocked) > openLen + 0.2, 'detour is longer, ' +
            pathLen(blocked).toFixed(2) + ' vs ' + openLen.toFixed(2));
        for (let i = 0; i < blocked.length; i += 3) {
            assert(Math.hypot(blocked[i], blocked[i + 2]) > 1.9,
                'no waypoint inside the cylinder: (' + blocked[i].toFixed(2) + ',' +
                blocked[i + 2].toFixed(2) + ')');
        }

        // Remove → surface restores, one more generation.
        assert(mesh.removeObstacle(h) === true, 'removeObstacle');
        assert(mesh.obstacleCount === 0, 'count back to 0');
        settle(mesh);
        assert(mesh.generation === gen0 + 2, 'removal bumps generation');
        const restored = mesh.findPath({ x: -9, y: 0, z: 0 }, { x: 9, y: 0, z: 0 });
        assert(restored !== null && pathLen(restored) < openLen + 0.5, 'path restored');

        // Stale/double handles are clean no-ops.
        assert(mesh.removeObstacle(h) === false, 'double remove returns false');
        assert(mesh.removeObstacle(0) === false, 'zero handle returns false');
        assert(mesh.removeObstacle(123456789) === false, 'garbage handle returns false');

        // Tiled meshes do not serialize.
        let threw = false;
        try { mesh.save(); } catch (e) {
            threw = true;
            assert(String(e.message).indexOf('dynamicObstacles') >= 0, 'save error names the cause');
        }
        assert(threw, 'save() on a tiled mesh throws');

        // Bad descriptors throw.
        threw = false;
        try { mesh.addObstacle({ type: 'cylinder', pos: { x: 0, y: 0, z: 0 }, radius: -1, height: 2 }); }
        catch (e) { threw = true; }
        assert(threw, 'negative radius throws');
        threw = false;
        try { mesh.addObstacle({ type: 'box' }); } catch (e) { threw = true; }
        assert(threw, 'box without min/max or center/halfExtents throws');
        threw = false;
        try { mesh.addObstacle({ type: 'sphere' }); } catch (e) { threw = true; }
        assert(threw, 'unknown obstacle type throws');
    }

    // =========================================================================
    // Static (non-tiled) meshes reject the obstacle API
    // =========================================================================
    {
        const verts = [], idx = [];
        pushQuad(verts, idx, -12, -12, 12, 12, 0);
        const mesh = G.bakeNavMesh({
            positions: new Float32Array(verts), indices: new Uint32Array(idx),
        });
        assert(mesh.supportsObstacles === false, 'static mesh has no obstacle support');
        assert(mesh.obstacleCount === 0 && mesh.obstaclesPending === false,
            'static mesh obstacle queries are inert');
        assert(mesh.update() === true, 'static mesh update is a no-op no-fail');
        let threw = false;
        try {
            mesh.addObstacle({ type: 'cylinder', pos: { x: 0, y: 0, z: 0 }, radius: 1, height: 2 });
        } catch (e) {
            threw = true;
            assert(String(e.message).indexOf('dynamicObstacles') >= 0,
                'error explains the dynamicObstacles bake option');
        }
        assert(threw, 'addObstacle on a static mesh throws');
        assert(mesh.removeObstacle(1) === false, 'removeObstacle on a static mesh is false');
        assert(mesh.save() instanceof ArrayBuffer, 'static mesh still serializes');
    }

    // =========================================================================
    // Box obstacle severs a corridor (path fails), restore brings it back
    // =========================================================================
    {
        const mesh = bakeFloor(-10, -2, 10, 2);   // 4 m wide corridor
        assert(mesh.findPath({ x: -8, y: 0, z: 0 }, { x: 8, y: 0, z: 0 }) !== null,
            'corridor is open');

        const h = mesh.addObstacle({
            type: 'box', min: { x: -1, y: -1, z: -3 }, max: { x: 1, y: 3, z: 3 },
        });
        assert(h > 0, 'box obstacle added');
        settle(mesh);
        // Severed corridor → clamped partial path ending before the box.
        const sev = mesh.findPath({ x: -8, y: 0, z: 0 }, { x: 8, y: 0, z: 0 });
        assert(sev !== null && sev.partial === true,
            'full-width box severs the corridor → partial path');
        assert(sev[sev.length - 3] < -0.9,
            'partial path stops at the box\'s near face, x=' + sev[sev.length - 3].toFixed(2));
        assert(mesh.findPath({ x: -8, y: 0, z: 0 }, { x: 8, y: 0, z: 0 },
            { requireFullPath: true }) === null,
            'requireFullPath: severed corridor hard-fails');
        // Endpoints still snap: the failure is connectivity, not snapping.
        assert(mesh.nearestPoint({ x: -8, y: 0, z: 0 }) !== null, 'start still on mesh');
        assert(mesh.nearestPoint({ x: 8, y: 0, z: 0 }) !== null, 'goal still on mesh');

        assert(mesh.removeObstacle(h) === true, 'box removed');
        settle(mesh);
        const restored = mesh.findPath({ x: -8, y: 0, z: 0 }, { x: 8, y: 0, z: 0 });
        assert(restored !== null && restored.partial === false,
            'corridor restored after removal (complete path again)');
    }

    // =========================================================================
    // Y-rotated box carves a rotated footprint
    // =========================================================================
    {
        const mesh = bakeFloor(-12, -12, 12, 12);
        const open = mesh.findPath({ x: -9, y: 0, z: 0 }, { x: 9, y: 0, z: 0 });
        const openLen = pathLen(open);
        const h = mesh.addObstacle({
            type: 'box', center: { x: 0, y: 0.5, z: 0 },
            halfExtents: { x: 3, y: 1.5, z: 0.75 }, yaw: Math.PI / 4,
        });
        assert(h > 0, 'oriented box added');
        settle(mesh);
        const blocked = mesh.findPath({ x: -9, y: 0, z: 0 }, { x: 9, y: 0, z: 0 });
        assert(blocked !== null && pathLen(blocked) > openLen + 0.2,
            'oriented box forces a detour');
    }

    // =========================================================================
    // Agent repath — engine auto-pump: obstacle dropped mid-route makes the
    // walking agent detour, with NO manual mesh.update() calls
    // =========================================================================
    {
        const canvas = document.createElement('canvas');
        canvas.setAttribute('width', '128');
        canvas.setAttribute('height', '128');
        document.body.appendChild(canvas);
        flush();
        const scene = canvas.getContext('scene');
        assert(scene !== null, 'scene context');

        const mesh = bakeFloor(-12, -6, 12, 6);
        const world = G.createWorld();
        scene.attachAIWorld(world, { stepHz: 60 });

        const agent = G.createAgent({ x: -10, z: 0, speed: 4, radius: 0.4 });
        world.addAgent(agent);
        const node = scene.createMesh({ mesh: 'box', color: 'red' });
        node.attachAgent(world, agent, { navMesh: mesh, yOffset: 0.5 });

        assert(node.navigateTo({ x: 10, y: 0, z: 0 }) === true, 'route starts');
        const genBefore = mesh.generation;

        // Let the agent get going, then drop a box across the straight line
        // ahead of it (leaves side gaps at |z| in [2.5, 6]).
        advanceTime(500);
        assert(agent.x > -9.5, 'agent is moving');
        assert(agent.x < -2, 'agent has not passed the drop point yet');
        const h = mesh.addObstacle({
            type: 'box', min: { x: -1, y: -1, z: -2.5 }, max: { x: 1, y: 3, z: 2.5 },
        });
        assert(h > 0, 'mid-route obstacle added');

        // Drive ONLY virtual time: the engine's per-frame pump must apply the
        // tiles and the binding's generation check must repath the agent.
        let maxAbsZ = 0;
        for (let t = 0; t < 140; t++) {
            advanceTime(50);
            maxAbsZ = Math.max(maxAbsZ, Math.abs(agent.z));
        }
        assert(mesh.generation > genBefore, 'engine auto-pump applied the batch');
        assert(mesh.obstaclesPending === false, 'no pending work left');
        assert(Math.hypot(agent.x - 10, agent.z) < 1.0,
            'agent arrived via the detour, at (' + agent.x.toFixed(2) + ',' +
            agent.z.toFixed(2) + ')');
        assert(maxAbsZ > 2.0,
            'agent path bent around the obstacle, maxAbsZ=' + maxAbsZ.toFixed(2));

        // ── Full block: repath clamps to the closest reachable point — the
        //    agent walks up to the wall, halts there, and the route reads
        //    partial (never ghost-walks the stale path through the wall). ──
        const agent2 = G.createAgent({ x: -10, z: 0, speed: 4, radius: 0.4 });
        world.addAgent(agent2);
        const node2 = scene.createMesh({ mesh: 'box', color: 'blue' });
        node2.attachAgent(world, agent2, { navMesh: mesh, yOffset: 0.5 });
        assert(node2.navigateTo({ x: 10, y: 0, z: 0 }) === true, 'route 2 starts');
        assert(node2.navigationInfo().active === true &&
               node2.navigationInfo().partial === false, 'route 2 starts complete');
        advanceTime(400);
        const wall = mesh.addObstacle({
            type: 'box', min: { x: 0, y: -1, z: -7 }, max: { x: 2, y: 3, z: 7 },
        });
        assert(wall > 0, 'full-width wall added');
        advanceTime(1500);
        assert(node2.navigationInfo().partial === true,
            'severed route re-planned as partial');
        // Let the clamped route finish, then verify the agent halted before
        // the wall and stays put.
        for (let t = 0; t < 200 && node2.navigationInfo().active; t++) advanceTime(50);
        assert(node2.navigationInfo().active === false, 'clamped route completed');
        const hx = agent2.x, hz = agent2.z;
        assert(hx < 0, 'blocked agent never crossed the wall, x=' + hx.toFixed(2));
        advanceTime(1000);
        assert(Math.hypot(agent2.x - hx, agent2.z - hz) < 0.2,
            'blocked agent halted at the clamped end, not ghost-walked');
        assert(agent2.atTarget === false,
            'atTarget stays false at a clamped route end');

        // Removing the first obstacle while the wall stays: still blocked.
        assert(mesh.removeObstacle(h) === true, 'first obstacle removed');
        // Removing the wall re-opens the corridor for a fresh navigateTo.
        assert(mesh.removeObstacle(wall) === true, 'wall removed');
        advanceTime(500);   // engine pump applies the removals
        assert(mesh.obstaclesPending === false, 'removals applied by auto-pump');
        assert(node2.navigateTo({ x: 10, y: 0, z: 0 }) === true, 'route re-plans after removal');
        assert(node2.navigationInfo().partial === false, 'fresh route is complete again');
        for (let t = 0; t < 140; t++) advanceTime(50);
        assert(Math.hypot(agent2.x - 10, agent2.z) < 1.0,
            'previously blocked agent arrives after removal, at (' +
            agent2.x.toFixed(2) + ',' + agent2.z.toFixed(2) + ')');

        node.detachAgent();
        node2.detachAgent();
        scene.detachAIWorld();
    }

    console.log('test_navmesh_obstacles: OK');
}
