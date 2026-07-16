// Test agent ground-follow — node.attachAgent(..., { groundFollow }) makes
// the node's Y track the ground under the agent's (x, z), with yOffset as a
// clearance above it. 'terrain' mode samples a scene terrain via a downward
// terrain raycast; 'raycast' mode uses a physics down-raycast against the
// default world. Exercises scene::AgentBinding::syncToNode +
// js_node_attachAgent (src/scene/agent_binding.cpp +
// src/js/ai_binding_integration.cpp) and js::terrainSampleHeight.

const G = bro.ai.game;

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
assert(scene !== null, 'scene context');

// =========================================================================
// Terrain mode — node Y follows the voxel terrain surface
// =========================================================================
{
    const terrain = scene.createTerrain({
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
    // Stream in the chunks around the origin so heights are queryable.
    for (let i = 0; i < 8; i++) terrain.update(0, 8, 0);

    // Ground truth from the terrain's own raycast at two probe points.
    function terrainY(x, z) {
        const hit = terrain.raycast([x, 100, z], [0, -1, 0], 200);
        assert(hit !== null, 'terrain hit at (' + x + ',' + z + ')');
        return hit.position[1];
    }
    const yA = terrainY(0, 0);
    const yB = terrainY(6, 6);

    const world = G.createWorld();
    const agent = G.createAgent({ x: 0, z: 0, speed: 4 });
    scene.attachAIWorld(world, { stepHz: 60 });

    const node = scene.createMesh({ mesh: 'box', color: 'red' });
    const ret = node.attachAgent(world, agent, {
        yOffset: 0.5,
        groundFollow: { mode: 'terrain', terrain: terrain, rayStart: 100, rayLength: 200 },
        think(self) { self.hold(1); },
    });
    assert(ret !== undefined, 'attachAgent with groundFollow returns node');

    advanceTime(100);
    let p = node.position;
    assert(Math.abs(p[1] - (yA + 0.5)) < 0.01,
        'terrain mode: node Y = surface + yOffset at (0,0): ' + p[1] + ' vs ' + (yA + 0.5));

    // Teleport the agent; the node's Y must re-track the new column height.
    agent.setPosition(6, 6);
    advanceTime(100);
    p = node.position;
    assert(Math.abs(p[0] - 6) < 0.01 && Math.abs(p[2] - 6) < 0.01, 'node follows agent x/z');
    assert(Math.abs(p[1] - (yB + 0.5)) < 0.01,
        'terrain mode: node Y re-tracks at (6,6): ' + p[1] + ' vs ' + (yB + 0.5));

    node.detachAgent();
    scene.detachAIWorld();
    terrain.destroy();
}

// =========================================================================
// Raycast mode — node Y follows static physics floors at varying heights
// =========================================================================
{
    Physics.destroyAll();
    // Low floor (top y = 0.5) on the -X side, high platform (top y = 3) on +X.
    Physics.createBody({
        shape: 'box', halfExtents: { x: 10, y: 0.25, z: 10 },
        position: { x: -10, y: 0.25, z: 0 }, static: true,
    });
    Physics.createBody({
        shape: 'box', halfExtents: { x: 10, y: 0.5, z: 10 },
        position: { x: 10, y: 2.5, z: 0 }, static: true,
    });

    const world = G.createWorld();
    const agent = G.createAgent({ x: -5, z: 0, speed: 4 });
    scene.attachAIWorld(world, { stepHz: 60 });

    const node = scene.createMesh({ mesh: 'box', color: 'blue' });
    node.attachAgent(world, agent, {
        yOffset: 1.0,
        groundFollow: { mode: 'raycast', rayStart: 50, rayLength: 100 },
        think(self) { self.hold(1); },
    });

    advanceTime(100);
    let p = node.position;
    assert(Math.abs(p[1] - 1.5) < 0.01,
        'raycast mode: node Y = floor top (0.5) + yOffset (1.0), got ' + p[1]);

    // Move over the high platform.
    agent.setPosition(5, 0);
    advanceTime(100);
    p = node.position;
    assert(Math.abs(p[1] - 4.0) < 0.01,
        'raycast mode: node Y re-tracks platform top (3.0) + yOffset, got ' + p[1]);

    // Off both floors: no hit → keeps the last known ground height.
    agent.setPosition(0, 50);
    advanceTime(100);
    p = node.position;
    assert(Math.abs(p[1] - 4.0) < 0.01,
        'raycast mode: no hit keeps last ground height, got ' + p[1]);

    node.detachAgent();
    scene.detachAIWorld();
    Physics.destroyAll();
}

// =========================================================================
// Without groundFollow, yOffset stays absolute (regression guard)
// =========================================================================
{
    const world = G.createWorld();
    const agent = G.createAgent({ x: 1, z: 2 });
    scene.attachAIWorld(world, { stepHz: 60 });
    const node = scene.createMesh({ mesh: 'box' });
    node.attachAgent(world, agent, { yOffset: 2.5, think(self) { self.hold(1); } });
    advanceTime(50);
    const p = node.position;
    assert(Math.abs(p[1] - 2.5) < 0.01, 'yOffset is absolute without groundFollow, got ' + p[1]);
    node.detachAgent();
    scene.detachAIWorld();
}

// Bad mode is rejected.
{
    const world = G.createWorld();
    const agent = G.createAgent({ x: 0, z: 0 });
    const node = scene.createMesh({ mesh: 'box' });
    let threw = false;
    try {
        node.attachAgent(world, agent, { groundFollow: { mode: 'nope' } });
    } catch (e) {
        threw = true;
    }
    assert(threw, 'invalid groundFollow.mode throws');
}

console.log('test_agent_groundfollow: OK');
