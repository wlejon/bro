// AgentBinding lifetime pins. The binding used to keep RAW pointers to the
// navmesh / agent / AI world whose only keep-alive was a `__navMesh`-style
// property on whichever transient node wrapper attachAgent happened to be
// called on — drop that wrapper and your own handles, and the periodic
// repath dereferenced a freed NavMesh. Now the binding takes SHARED
// ownership of the navmesh (navMeshSharedFromJS) and pins the agent/world JS
// wrappers on itself; the graph pins the attachAIWorld world the same way.
// This test drops every JS reference, forces GC (advanceTime's periodic
// sweep), and keeps navigating with repath enabled.

const G = bro.ai.game;
if (G.navMeshAvailable !== true) {
    console.log('test_agent_pins: navmesh not compiled in, skipping');
} else {
    const canvas = document.createElement('canvas');
    canvas.setAttribute('width', '128');
    canvas.setAttribute('height', '128');
    document.body.appendChild(canvas);
    flush();
    const scene = canvas.getContext('scene');
    assert(scene !== null, 'scene context');

    // Flat 40x40 ground quad.
    const verts = new Float32Array([
        -20, 0, -20,   -20, 0, 20,   20, 0, -20,   20, 0, 20,
    ]);
    const idx = new Uint32Array([0, 1, 2, 2, 1, 3]);

    let world = G.createWorld();
    scene.attachAIWorld(world, { stepHz: 60 });

    // Agent 1: navmesh via attachAgent opts. Agent 2: navmesh via
    // navigateTo opts (the other pin path), from a separate bake.
    let a1 = G.createAgent({ x: -15, z: -15, speed: 4, radius: 0.4 });
    let a2 = G.createAgent({ x: 15, z: -15, speed: 4, radius: 0.4 });
    world.addAgent(a1);
    world.addAgent(a2);

    let mesh = G.bakeNavMesh({ positions: verts, indices: idx });
    let mesh2 = G.bakeNavMesh({ positions: verts, indices: idx });
    assert(mesh.valid === true && mesh2.valid === true, 'bakes succeeded');

    let n1 = scene.createNode('agent1');
    let n2 = scene.createNode('agent2');
    const n1Id = n1.id, n2Id = n2.id;
    n1.attachAgent(world, a1, { navMesh: mesh });
    n2.attachAgent(world, a2, {});
    assert(n1.navigateTo({ x: 15, y: 0, z: 15 }, { repathInterval: 0.2 }) === true,
        'navigateTo #1 starts');
    assert(n2.navigateTo({ x: -15, y: 0, z: 15 }, { navMesh: mesh2, repathInterval: 0.2 }) === true,
        'navigateTo #2 starts');

    // Drop every JS reference the old pin scheme depended on: the navmesh
    // handles, the attach-time node wrappers (which carried the old
    // `__navMesh` pins), the agent handles, and the world handle.
    mesh = null; mesh2 = null;
    n1 = null; n2 = null;
    a1 = null; a2 = null;
    world = null;

    // ~13 s of virtual time (the diagonal is ~42 units at speed 4): repath
    // fires every 0.2 s throughout, and the headless periodic GC (every
    // ~1 s) sweeps the dropped wrappers. A dangling mesh/world/agent faults
    // here pre-fix.
    for (let t = 0; t < 260; t++) advanceTime(50);

    // Re-fetch through the graph (fresh wrappers — no pins of their own)
    // and confirm both agents actually travelled on their meshes.
    const n1b = scene.findById(n1Id);
    const n2b = scene.findById(n2Id);
    assert(n1b !== null && n2b !== null, 'agent nodes still in the graph');
    const p1 = n1b.position, p2 = n2b.position;
    assert(Math.hypot(p1[0] - 15, p1[2] - 15) < 1.5,
        'agent 1 arrived after all JS refs were dropped, at (' +
        p1[0].toFixed(2) + ',' + p1[2].toFixed(2) + ')');
    assert(Math.hypot(p2[0] - (-15), p2[2] - 15) < 1.5,
        'agent 2 arrived via navigateTo-opts navmesh, at (' +
        p2[0].toFixed(2) + ',' + p2[2].toFixed(2) + ')');

    // Explicit detach releases one binding's pins while the graph lives...
    n2b.detachAgent();
    advanceTime(1100);   // GC can collect whatever only that binding pinned

    // ...and the other stays attached and navigating while the whole canvas
    // is detached: the graph prune destroys the binding + ticker + pins, and
    // engine teardown must be leak-clean (Debug QuickJS assert is the gate).
    document.body.removeChild(canvas);
    flush();
    advanceTime(300);

    console.log('agent pins: navmesh/agent/world survive dropped JS refs');
}
