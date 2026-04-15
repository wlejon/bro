// Animation — construct, evaluate, retarget.

const skel = Skeleton.fromBones([
    { name: 'root', parent: -1 },
    { name: 'tip',  parent:  0, localT: [0, 1, 0] },
]);

// One channel: bone 1 translation, two keyframes at t=0 and t=2 going from
// (0, 0, 0) to (0, 0, 10).
const anim = new Animation({
    name: 'jump',
    duration: 2.0,
    channels: [{
        boneIndex: 1,
        path: 'translation',
        interp: 'linear',
        times:  new Float32Array([0, 2]),
        values: new Float32Array([0, 0, 0,  0, 0, 10]),
    }],
});

assert(anim.name === 'jump',                'name preserved');
assert(Math.abs(anim.duration - 2) < 1e-6,  'duration');
assert(anim.channelCount === 1,             'channelCount');
assert(anim.channels[0].path === 'translation', 'path round-trip');
assert(anim.channels[0].interp === 'linear',     'interp round-trip');
assert(anim.channels[0].times.length === 2,      'times length');

// ── Evaluate at t=0 -> bone 1 translation = (0,0,0) ─────────────────────────
{
    const pose = anim.evaluate(skel, 0);
    // Pose layout: bone i at offset i*10; translation at slots 0..2
    const tx = pose.data[10 + 0];
    const ty = pose.data[10 + 1];
    const tz = pose.data[10 + 2];
    assert(Math.abs(tx) < 1e-5,  'eval@0 bone1 tx=0: ' + tx);
    assert(Math.abs(ty) < 1e-5,  'eval@0 bone1 ty=0: ' + ty);
    assert(Math.abs(tz) < 1e-5,  'eval@0 bone1 tz=0: ' + tz);
}

// ── Evaluate at t=1 -> bone 1 translation = (0,0,5) (linear midpoint) ──────
{
    const pose = anim.evaluate(skel, 1.0);
    const tz = pose.data[10 + 2];
    assert(Math.abs(tz - 5) < 1e-5, 'eval@1 bone1 tz=5: ' + tz);
}

// ── Evaluate at t=2 (end) without looping -> bone 1 translation = (0,0,10) ─
{
    const pose = anim.evaluate(skel, 2.0, { loop: false });
    const tz = pose.data[10 + 2];
    assert(Math.abs(tz - 10) < 1e-5, 'eval@2 bone1 tz=10 (no loop): ' + tz);
}

// ── Bone 0 (no channel) keeps bind transform — root T=(0,0,0) ───────────────
{
    const pose = anim.evaluate(skel, 1.0);
    assert(Math.abs(pose.data[0]) < 1e-5,     'bone 0 unchanged tx');
    assert(Math.abs(pose.data[6] - 1) < 1e-5, 'bone 0 keeps identity quaternion');
}

// ── evaluateInto reuses an existing pose ────────────────────────────────────
{
    const pose = skel.bindPose();
    const ret = anim.evaluateInto(skel, 1.0, pose);
    assert(Math.abs(pose.data[10 + 2] - 5) < 1e-5, 'evaluateInto wrote into pose');
    assert(ret === pose,                            'evaluateInto returns the pose');
}

// ── Loop wraps t past duration ──────────────────────────────────────────────
{
    // With loop=true, t=4 wraps to 0; at t=0 tz should be 0.
    const pose = anim.evaluate(skel, 4.0, { loop: true });
    assert(Math.abs(pose.data[10 + 2]) < 1e-5, 'loop wraps to t=0');
}

// ── Retarget by name ────────────────────────────────────────────────────────
{
    const dst = Skeleton.fromBones([
        // Different layout, but bones 'root' and 'tip' have matching names.
        { name: 'extra', parent: -1 },
        { name: 'tip',   parent:  0 },
        { name: 'root',  parent: -1 },
    ]);
    const r = Animation.retarget(anim, skel, dst);
    assert(r.channelCount >= 1, 'retarget kept the tip channel');
    // bone index for tip in dst is 1
    assert(r.channels[0].boneIndex === 1, 'retargeted to dst bone index');
}

// ── findBoneBySuffix utility ────────────────────────────────────────────────
{
    const s = Skeleton.fromBones([
        { name: 'mixamorig:Hips' },
        { name: 'DEF-spine.001' },
        { name: 'foo' },
    ]);
    assert(s.findBoneBySuffix('Hips') === 0,        'suffix match Hips');
    assert(s.findBoneBySuffix('spine.001') === 1,   'suffix match spine.001');
    assert(s.findBoneBySuffix('missing') === -1,    'no suffix match');
}

console.log('PASS test_animation');
