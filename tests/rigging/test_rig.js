// Rig pipeline smoke test — built-in specs, landmark detection, fitSkeleton,
// autoRig, locomotion. Uses primitives as stand-in meshes.

// ── Built-in specs load ─────────────────────────────────────────────────────
{
    const specs = ['humanoid', 'quadruped', 'hexapod', 'octopod'];
    for (const name of specs) {
        const s = Rig.spec(name);
        assert(s !== null,                        name + ' loads');
        assert(s.name === name,                   name + '.name');
        assert(s.boneCount > 0,                   name + ' has bones');
        assert(s.landmarkCount > 0,               name + ' has landmarks');
    }
}

// Unknown spec returns an empty one.
{
    const s = Rig.spec('nope');
    assert(s.boneCount === 0, 'unknown spec is empty');
}

// ── toJSON / specFromJSON round-trip ────────────────────────────────────────
{
    const s = Rig.spec('humanoid');
    const json = s.toJSON();
    assert(typeof json === 'string' && json.length > 0, 'toJSON produces a string');
    const round = Rig.specFromJSON(json);
    assert(round.boneCount === s.boneCount,         'round-tripped boneCount');
    assert(round.landmarkCount === s.landmarkCount, 'round-tripped landmarkCount');
}

// ── landmarkNames / boneNames return string[] ──────────────────────────────
{
    const s = Rig.spec('humanoid');
    const lms = s.landmarkNames();
    const bones = s.boneNames();
    assert(Array.isArray(lms),                    'landmarkNames is array');
    assert(lms.length === s.landmarkCount,        'landmarkNames length matches');
    assert(typeof lms[0] === 'string',            'landmark names are strings');
    assert(bones.length === s.boneCount,          'boneNames length matches');
}

// ── Landmark detection on a humanoid stand-in (capsule) ────────────────────
{
    // A vertically-stretched capsule is a passable T-pose proxy for the
    // detector to chew on. We do not assert specific landmarks — only that
    // the call runs and returns an object.
    const mesh = Mesh.capsule(0.3, 1.0, 16, 8);
    const lm = Rig.detectHumanoid(mesh);
    assert(typeof lm === 'object' && lm !== null, 'detectHumanoid returns object');
}

// ── missingLandmarks against an empty input ─────────────────────────────────
{
    const s = Rig.spec('humanoid');
    const missing = Rig.missingLandmarks(s, {});
    assert(Array.isArray(missing),                'missingLandmarks is array');
    assert(missing.length === s.landmarkCount,    'all landmarks missing on empty input');
    assert(typeof missing[0] === 'string',        'missing names are strings');
}

// missingLandmarks shrinks when we provide some landmarks
{
    const s = Rig.spec('humanoid');
    const partial = {};
    partial[s.landmarkNames()[0]] = [0, 0, 0];
    const missing = Rig.missingLandmarks(s, partial);
    assert(missing.length === s.landmarkCount - 1, 'one fewer missing');
}

// ── fitSkeleton runs end-to-end on a procedural humanoid ────────────────────
{
    const mesh = Mesh.capsule(0.3, 1.0, 16, 8);
    const spec = Rig.spec('humanoid');
    const lm   = Rig.detectHumanoid(mesh);
    const skel = Rig.fitSkeleton(spec, lm, mesh);
    assert(skel.boneCount > 0, 'fitSkeleton produced a skeleton');
}

// ── autoRig runs end-to-end and reports a method ────────────────────────────
{
    const mesh = Mesh.capsule(0.3, 1.0, 16, 8);
    const spec = Rig.spec('humanoid');
    const lm   = Rig.detectHumanoid(mesh);
    const r = Rig.autoRig(mesh, spec, lm, { method: 'voxelBind', smoothIterations: 1 });
    assert(typeof r === 'object',                    'autoRig returns object');
    assert(r.skeleton.boneCount > 0,                 'autoRig produced skeleton');
    assert(typeof r.methodUsed === 'string',         'methodUsed set');
    assert(r.methodUsed !== 'auto',                  'methodUsed never Auto on return');
    assert(Array.isArray(r.missingLandmarks),        'missingLandmarks array');
    assert(Array.isArray(r.warnings),                'warnings array');
    // skin should at least cover the mesh vertex count.
    assert(r.skin.vertexCount === mesh.vertexCount,  'skin covers mesh');
}

// ── generateLocomotionCycle runs (may return empty Animation if no
//    grounded legs identified — accept either, just no crash). ──────────────
{
    const mesh = Mesh.capsule(0.3, 1.0, 16, 8);
    const spec = Rig.spec('humanoid');
    const lm   = Rig.detectHumanoid(mesh);
    const skel = Rig.fitSkeleton(spec, lm, mesh);
    const anim = Rig.generateLocomotionCycle(skel, spec, {
        strideLength: 0.3, cycleDuration: 1.0, footLiftHeight: 0.05, keyframesPerCycle: 12,
    });
    assert(typeof anim === 'object',          'locomotion returns Animation');
    assert(typeof anim.duration === 'number', 'locomotion has duration');
}

console.log('PASS test_rig');
