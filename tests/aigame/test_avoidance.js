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

console.log('test_avoidance: OK');
