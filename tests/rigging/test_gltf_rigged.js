// Rigged glTF round-trip — autoRig a procedural mesh, save with skin +
// skeleton + animation, reload, and verify the rigging payload survives.

const fs = require('fs');
const tmpDir   = 'tests/tmp';
const tmpPath  = tmpDir + '/rigged_test.gltf';
if (!fs.existsSync(tmpDir)) fs.mkdirSync(tmpDir, { recursive: true });

try {

// Build a minimal rigged scene from a capsule.
const mesh = Mesh.capsule(0.3, 1.0, 16, 8);
const spec = Rig.spec('humanoid');
const lm   = Rig.detectHumanoid(mesh);
const r    = Rig.autoRig(mesh, spec, lm, { method: 'voxelBind', smoothIterations: 0 });
assert(r.skeleton.boneCount > 0, 'autoRig produced skeleton');

// One trivial animation with a single rotation channel on bone 0.
const anim = new Animation({
    name: 'idle',
    duration: 1.0,
    channels: [{
        boneIndex: 0,
        path: 'rotation',
        interp: 'linear',
        times:  new Float32Array([0, 1]),
        values: new Float32Array([0, 0, 0, 1,  0, 0, 0, 1]), // identity quat at both keys
    }],
});

// Save with rigging payload.
const ok = mesh.saveGLTF(tmpPath, { skin: r.skin, skeleton: r.skeleton, animations: [anim] });
assert(ok === true, 'saveGLTF rigged returned true');

// Reload and inspect.
const scene = Mesh.loadGLTF(tmpPath);
assert(Array.isArray(scene.meshes),     'loadGLTF: meshes is array');
assert(scene.meshes.length >= 1,        'at least one mesh');
assert(Array.isArray(scene.skins),      'loadGLTF: skins is array');
assert(scene.skins.length >= 1,         'reload: skin present');
assert(Array.isArray(scene.skeletons),  'loadGLTF: skeletons is array');
assert(scene.skeletons.length >= 1,     'reload: skeleton present');
assert(Array.isArray(scene.animations), 'loadGLTF: animations is array');
assert(scene.animations.length >= 1,    'reload: animation present');
assert(Array.isArray(scene.meshSkeleton),      'meshSkeleton index array');
assert(Array.isArray(scene.animationSkeleton), 'animationSkeleton index array');

// Skeleton bone count should match what we saved.
assert(scene.skeletons[0].boneCount === r.skeleton.boneCount, 'skeleton bones round-trip');

// Skin should cover the saved mesh.
assert(scene.skins[0].vertexCount === scene.meshes[0].vertexCount,
       'skin vertex count matches reloaded mesh');

// Animation has at least one channel and the duration we set.
assert(scene.animations[0].channelCount >= 1, 'animation has channels');
assert(Math.abs(scene.animations[0].duration - 1.0) < 1e-3, 'animation duration round-trip');

// Unskinned save still works (no opts arg).
{
    const plainPath = tmpDir + '/unskinned_test.gltf';
    const okPlain = mesh.saveGLTF(plainPath);
    assert(okPlain === true, 'unskinned saveGLTF still works');
    const reloaded = Mesh.loadGLTF(plainPath);
    assert(reloaded.meshes.length >= 1, 'unskinned reload');
    assert(reloaded.skins.length === 0 || reloaded.skins[0].boneCount === 0,
           'unskinned reload has no skin payload');
}

console.log('PASS test_gltf_rigged');

} finally {
    // Always clean up — leftover files in repo root were getting picked up
    // by `git add -A`. The tmp dir is gitignored so the dir itself can stay.
    for (const name of ['rigged_test.gltf', 'unskinned_test.gltf']) {
        const p = tmpDir + '/' + name;
        if (fs.existsSync(p)) fs.unlinkSync(p);
    }
}
