// ORCA local avoidance — world.setAvoidance() makes agents flow around each
// other instead of walking through. Exercises brogameagent::AvoidanceSim +
// World::tick's avoidance pass through the bro.ai.game bindings
// (src/js/ai_bindings.cpp) and the scene attachAgent path
// (src/js/ai_binding_integration.cpp).

const G = bro.ai.game;
const dt = 1 / 60;

function dist(a, b) { return Math.hypot(a.x - b.x, a.z - b.z); }

// =========================================================================
// Two agents, swapped goals: they pass each other, never overlap
// =========================================================================
{
    const world = G.createWorld();
    world.setAvoidance(true);
    assert(world.avoidanceEnabled === true, 'avoidanceEnabled reads back true');

    const a = G.createAgent({ id: 1, x: -5, z: 0, speed: 4, radius: 0.5 });
    const b = G.createAgent({ id: 2, x: 5, z: 0, speed: 4, radius: 0.5 });
    world.addAgent(a);
    world.addAgent(b);
    a.setTarget(5, 0);
    b.setTarget(-5, 0);

    let minDist = Infinity;
    for (let i = 0; i < 15 * 60; i++) {
        world.tick(dt);
        minDist = Math.min(minDist, dist(a, b));
        if (a.atTarget && b.atTarget) break;
    }
    assert(a.atTarget, 'agent a reached its goal, at (' + a.x.toFixed(2) + ',' + a.z.toFixed(2) + ')');
    assert(b.atTarget, 'agent b reached its goal, at (' + b.x.toFixed(2) + ',' + b.z.toFixed(2) + ')');
    assert(minDist >= 1.0 * 0.9,
        'head-on pair never overlapped: minDist=' + minDist.toFixed(3) + ' (sum radii 1.0)');
}

// =========================================================================
// Regression: avoidance off (default) keeps pass-through behavior
// =========================================================================
{
    const world = G.createWorld();
    assert(world.avoidanceEnabled === false, 'avoidance defaults off');

    const a = G.createAgent({ id: 1, x: -5, z: 0, speed: 4, radius: 0.5 });
    const b = G.createAgent({ id: 2, x: 5, z: 0, speed: 4, radius: 0.5 });
    world.addAgent(a);
    world.addAgent(b);
    a.setTarget(5, 0);
    b.setTarget(-5, 0);

    let minDist = Infinity;
    for (let i = 0; i < 15 * 60; i++) {
        world.tick(dt);
        minDist = Math.min(minDist, dist(a, b));
        if (a.atTarget && b.atTarget) break;
    }
    assert(a.atTarget && b.atTarget, 'legacy agents still reach goals');
    assert(minDist < 0.5,
        'legacy agents walked through each other: minDist=' + minDist.toFixed(3));
}

// =========================================================================
// 10-agent crossing: everyone arrives, zero hard overlaps
// =========================================================================
{
    const world = G.createWorld();
    world.setAvoidance(true);

    const N = 10;
    const R = 8;
    const agents = [];
    const goals = [];
    for (let i = 0; i < N; i++) {
        // Ring with a slight angular stagger; goals are the antipodes, so
        // every path crosses the center — the worst case for local avoidance.
        const ang = (i / N) * 2 * Math.PI + 0.013 * i;
        const x = R * Math.cos(ang), z = R * Math.sin(ang);
        const ag = G.createAgent({ id: i + 1, x: x, z: z, speed: 4, radius: 0.4 });
        world.addAgent(ag);
        agents.push(ag);
        goals.push({ x: -x, z: -z });
    }
    for (let i = 0; i < N; i++) agents[i].setTarget(goals[i].x, goals[i].z);

    let minDist = Infinity;
    let allArrived = false;
    for (let step = 0; step < 30 * 60; step++) {
        world.tick(dt);
        for (let i = 0; i < N; i++)
            for (let j = i + 1; j < N; j++)
                minDist = Math.min(minDist, dist(agents[i], agents[j]));
        allArrived = agents.every(a => a.atTarget);
        if (allArrived) break;
    }
    assert(allArrived, 'all ' + N + ' crossing agents arrived');
    assert(minDist >= 0.8 * 0.9,
        'crossing crowd had no hard overlap: minDist=' + minDist.toFixed(3) + ' (sum radii 0.8)');
}

// =========================================================================
// NavGrid obstacle bridge: setAvoidance({navGrid}) — the wall the path
// planner routes around is also locally respected
// =========================================================================
{
    const nav = G.createNavGrid({
        minX: -20, minZ: -20, maxX: 20, maxZ: 20, cellSize: 0.5,
        obstacles: [{ x: 0, z: 0, hw: 2, hd: 0.25 }],
    });
    const world = G.createWorld();
    world.setAvoidance({ navGrid: nav });
    assert(world.avoidanceEnabled === true, 'setAvoidance({navGrid}) enables');

    const a = G.createAgent({ navGrid: nav, id: 1, x: -4, z: -2, speed: 4, radius: 0.4 });
    world.addAgent(a);
    a.setTarget(4, 2);   // straight line would cross the wall

    let minClearance = Infinity;
    for (let i = 0; i < 20 * 60; i++) {
        world.tick(dt);
        const dx = Math.max(Math.abs(a.x - 0) - 2, 0);
        const dz = Math.max(Math.abs(a.z - 0) - 0.25, 0);
        minClearance = Math.min(minClearance, Math.hypot(dx, dz));
        if (a.atTarget) break;
    }
    assert(a.atTarget, 'agent rounded the wall and arrived at (' + a.x.toFixed(2) + ',' + a.z.toFixed(2) + ')');
    assert(minClearance >= 0.4 * 0.9,
        'wall clearance kept: ' + minClearance.toFixed(3) + ' (radius 0.4)');
}

// =========================================================================
// Per-agent opt-out: avoidance:false agent keeps its ground, others yield
// =========================================================================
{
    const world = G.createWorld();
    world.setAvoidance(true);

    const blocker = G.createAgent({ id: 1, x: 0, z: 0.01, radius: 0.5, avoidance: false });
    const mover = G.createAgent({ id: 2, x: -6, z: 0, speed: 4, radius: 0.5 });
    world.addAgent(blocker);
    world.addAgent(mover);
    mover.setTarget(6, 0);

    let minDist = Infinity;
    for (let i = 0; i < 15 * 60; i++) {
        world.tick(dt);
        minDist = Math.min(minDist, dist(mover, blocker));
        if (mover.atTarget) break;
    }
    assert(mover.atTarget, 'mover reached its goal around the blocker');
    assert(minDist >= 1.0 * 0.9, 'mover kept clear of the blocker: ' + minDist.toFixed(3));
    assert(Math.abs(blocker.x) < 1e-3 && Math.abs(blocker.z - 0.01) < 1e-3,
        'opted-out blocker never moved: (' + blocker.x + ',' + blocker.z + ')');

    // Runtime retune via agent.setAvoidance is accepted.
    mover.setAvoidance({ radius: 0.6, timeHorizon: 3 });
    mover.setAvoidance(true);
}

// =========================================================================
// Multi-level: vertically separated agents ignore each other (elevation
// filter — bridge over tunnel), same-level agents still avoid
// =========================================================================
{
    // Different levels: |dy| = 6 far exceeds (2 + 2) / 2 with the default
    // avoidance height 2 — the crossing pair must pass straight through in
    // XZ with no swerve (only the tiny ORCA symmetry dither bends paths).
    const world = G.createWorld();
    world.setAvoidance(true);

    const lower = G.createAgent({ id: 1, x: -5, z: 0, speed: 4, radius: 0.5, elevation: 0 });
    const upper = G.createAgent({ id: 2, x: 5, z: 0, speed: 4, radius: 0.5, elevation: 6 });
    assert(Math.abs(upper.elevation - 6) < 1e-6, 'elevation opt lands: ' + upper.elevation);
    upper.elevation = 7;
    assert(Math.abs(upper.elevation - 7) < 1e-6, 'elevation is assignable');
    world.addAgent(lower);
    world.addAgent(upper);
    lower.setTarget(5, 0);
    upper.setTarget(-5, 0);

    let maxLateral = 0;
    let minDist = Infinity;
    for (let i = 0; i < 10 * 60; i++) {
        world.tick(dt);
        maxLateral = Math.max(maxLateral, Math.abs(lower.z), Math.abs(upper.z));
        minDist = Math.min(minDist, dist(lower, upper));
        if (lower.atTarget && upper.atTarget) break;
    }
    assert(lower.atTarget && upper.atTarget, 'stacked-level agents reached their goals');
    assert(minDist < 0.5,
        'stacked-level agents passed straight through in XZ: minDist=' + minDist.toFixed(3));
    assert(maxLateral < 0.2,
        'stacked-level agents never swerved: maxLateral=' + maxLateral.toFixed(3));
}
{
    // Overlapping spans: |dy| = 3 < (4 + 4) / 2 with avoidance.height 4 —
    // the pair still counts as neighbors and must avoid. Also proves the
    // height option reaches the solver through the bindings.
    const world = G.createWorld();
    world.setAvoidance(true);

    const a = G.createAgent({ id: 1, x: -5, z: 0, speed: 4, radius: 0.5,
                              elevation: 0, avoidance: { height: 4 } });
    const b = G.createAgent({ id: 2, x: 5, z: 0, speed: 4, radius: 0.5,
                              elevation: 3, avoidance: { height: 4 } });
    world.addAgent(a);
    world.addAgent(b);
    a.setTarget(5, 0);
    b.setTarget(-5, 0);

    let minDist = Infinity;
    for (let i = 0; i < 15 * 60; i++) {
        world.tick(dt);
        minDist = Math.min(minDist, dist(a, b));
        if (a.atTarget && b.atTarget) break;
    }
    assert(a.atTarget && b.atTarget, 'overlapping-span agents reached their goals');
    assert(minDist >= 1.0 * 0.9,
        'overlapping vertical spans still avoid: minDist=' + minDist.toFixed(3));
}

// =========================================================================
// Scene-attached agents: think() + moveTo flow around each other
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

    const a = G.createAgent({ id: 1, x: -5, z: 0, speed: 4, radius: 0.5 });
    const b = G.createAgent({ id: 2, x: 5, z: 0, speed: 4, radius: 0.5 });
    world.addAgent(a);
    world.addAgent(b);

    const nodeA = scene.createMesh({ mesh: 'box', color: 'red' });
    const nodeB = scene.createMesh({ mesh: 'box', color: 'blue' });
    nodeA.attachAgent(world, a, {
        avoidance: { radius: 0.5 },
        think(self) { self.moveTo(5, 0); },
    });
    nodeB.attachAgent(world, b, {
        avoidance: { radius: 0.5 },
        think(self) { self.moveTo(-5, 0); },
    });

    let minDist = Infinity;
    let done = false;
    for (let i = 0; i < 150 && !done; i++) {
        advanceTime(100);
        minDist = Math.min(minDist, dist(a, b));
        done = a.atTarget && b.atTarget;
    }
    // advanceTime samples positions coarsely (every 100 ms) — the fine-grained
    // guarantee is covered above; here we check the scene path passes cleanly.
    assert(a.atTarget && b.atTarget, 'scene-attached agents reached swapped goals');
    assert(minDist >= 1.0 * 0.8,
        'scene-attached agents did not overlap (coarse sampling): ' + minDist.toFixed(3));

    // Node transforms track the agents.
    const pA = nodeA.position;
    assert(Math.abs(pA[0] - a.x) < 0.01 && Math.abs(pA[2] - a.z) < 0.01,
        'node follows agent after avoidance-driven movement');

    nodeA.detachAgent();
    nodeB.detachAgent();
    scene.detachAIWorld();
}

// =========================================================================
// Priority: the lower-priority agent takes (nearly) all the avoidance
// effort; the pair still never overlaps and both arrive
// =========================================================================
{
    const world = G.createWorld();
    world.setAvoidance(true);

    const boss = G.createAgent({
        id: 1, x: -5, z: 0, speed: 4, radius: 0.5,
        avoidance: { priority: 1.0 },
    });
    const minion = G.createAgent({
        id: 2, x: 5, z: 0, speed: 4, radius: 0.5,
        avoidance: { priority: 0.0 },
    });
    world.addAgent(boss);
    world.addAgent(minion);
    boss.setTarget(5, 0);
    minion.setTarget(-5, 0);

    let minDist = Infinity, latBoss = 0, latMinion = 0;
    for (let i = 0; i < 15 * 60; i++) {
        world.tick(dt);
        minDist = Math.min(minDist, dist(boss, minion));
        latBoss = Math.max(latBoss, Math.abs(boss.z));
        latMinion = Math.max(latMinion, Math.abs(minion.z));
        if (boss.atTarget && minion.atTarget) break;
    }
    assert(boss.atTarget && minion.atTarget, 'priority pair both arrive');
    assert(minDist >= 1.0 * 0.9,
        'priority pair never overlapped: minDist=' + minDist.toFixed(3));
    assert(latMinion > 2 * latBoss,
        'low-priority agent did the swerving: minion=' + latMinion.toFixed(3) +
        ' vs boss=' + latBoss.toFixed(3));
}

// =========================================================================
// Layers/mask: disjoint groups ghost through each other; runtime
// setAvoidance updates take effect
// =========================================================================
{
    const world = G.createWorld();
    world.setAvoidance(true);

    const a = G.createAgent({
        id: 1, x: -5, z: 0, speed: 4, radius: 0.5,
        avoidance: { layers: 1, mask: 1 },
    });
    const b = G.createAgent({ id: 2, x: 5, z: 0, speed: 4, radius: 0.5 });
    // Runtime setter: move b onto layer 2, only seeing layer 2.
    b.setAvoidance({ layers: 2, mask: 2 });
    world.addAgent(a);
    world.addAgent(b);
    a.setTarget(5, 0);
    b.setTarget(-5, 0);

    let minDist = Infinity;
    for (let i = 0; i < 15 * 60; i++) {
        world.tick(dt);
        minDist = Math.min(minDist, dist(a, b));
        if (a.atTarget && b.atTarget) break;
    }
    assert(a.atTarget && b.atTarget, 'masked-out pair both arrive');
    assert(minDist < 0.5,
        'disjoint layers ghost through each other: minDist=' + minDist.toFixed(3));

    // Flip b back onto the shared layer: they avoid again on a fresh run.
    a.setPosition(-5, 0);
    b.setPosition(5, 0);
    b.setAvoidance({ layers: 1, mask: 1 });
    a.setTarget(5, 0);
    b.setTarget(-5, 0);
    minDist = Infinity;
    for (let i = 0; i < 15 * 60; i++) {
        world.tick(dt);
        minDist = Math.min(minDist, dist(a, b));
        if (a.atTarget && b.atTarget) break;
    }
    assert(a.atTarget && b.atTarget, 'shared-layer pair both arrive');
    assert(minDist >= 1.0 * 0.9,
        'shared layer avoids again: minDist=' + minDist.toFixed(3));
}

console.log('test_avoidance: OK');
