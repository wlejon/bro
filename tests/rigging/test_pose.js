// Pose — bindPose, world matrices, skinning matrices, blend, socketWorld.

const skel = Skeleton.fromBones([
    { name: 'root', parent: -1, localT: [1, 0, 0] },
    { name: 'tip',  parent:  0, localT: [0, 2, 0] },
]);

// ── computeWorldMatrices on bind pose ───────────────────────────────────────
{
    const pose  = skel.bindPose();
    const world = pose.computeWorldMatrices(skel);
    assert(world.length === 32, 'world matrices: 16 floats * 2 bones');

    // Root world matrix should equal the bind translation (1, 0, 0). Column-
    // major: translation lives in indices 12, 13, 14.
    assert(Math.abs(world[12] - 1) < 1e-5, 'root world tx');
    assert(Math.abs(world[13] - 0) < 1e-5, 'root world ty');
    assert(Math.abs(world[14] - 0) < 1e-5, 'root world tz');

    // Tip world is root * (0,2,0) -> (1, 2, 0).
    assert(Math.abs(world[16 + 12] - 1) < 1e-5, 'tip world tx');
    assert(Math.abs(world[16 + 13] - 2) < 1e-5, 'tip world ty');
    assert(Math.abs(world[16 + 14] - 0) < 1e-5, 'tip world tz');
}

// ── computeSkinningMatrices = world * inverseBind (identity IBM here) ───────
{
    const pose = skel.bindPose();
    const skin = pose.computeSkinningMatrices(skel);
    assert(skin.length === 32, 'skinning matrices length');
    // With identity inverse-bind, skinning == world.
    const world = pose.computeWorldMatrices(skel);
    for (let i = 0; i < skin.length; i++)
        assert(Math.abs(skin[i] - world[i]) < 1e-5, 'skinning == world at ' + i);
}

// ── Blend two poses ─────────────────────────────────────────────────────────
{
    const a = skel.bindPose();
    const b = skel.bindPose();
    // Shift bone 0 of b by (10, 0, 0) in translation slots.
    const bdata = b.data;
    bdata[0] = 10;
    b.data = bdata;
    Pose.blend(a, b, 0.5);
    // bone 0 bind T.x = 1, override to 10 in b -> 0.5 * 1 + 0.5 * 10 = 5.5
    assert(Math.abs(a.data[0] - 5.5) < 1e-5, 'blend lerps translation: got ' + a.data[0]);
    // bone 1 bind T = (0, 2, 0); b unchanged; lerp(2, 2, 0.5) = 2.
    assert(Math.abs(a.data[10 + 1] - 2) < 1e-5, 'blend leaves bone 1');
}

// ── Socket world matrix ─────────────────────────────────────────────────────
{
    const skel2 = Skeleton.fromBones([
        { name: 'root', parent: -1, localT: [0, 0, 0] },
    ]);
    skel2.addSocket({ name: 'mount', bone: 0, offset: [
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        3, 4, 5, 1,
    ]});
    const pose = skel2.bindPose();
    const m = pose.socketWorld(skel2, 'mount');
    assert(m !== null,            'socketWorld found');
    assert(m.length === 16,       'socketWorld is mat4');
    assert(Math.abs(m[12] - 3) < 1e-5, 'socket tx');
    assert(Math.abs(m[13] - 4) < 1e-5, 'socket ty');
    assert(Math.abs(m[14] - 5) < 1e-5, 'socket tz');

    const miss = pose.socketWorld(skel2, 'nope');
    assert(miss === null, 'missing socket -> null');
}

console.log('PASS test_pose');
