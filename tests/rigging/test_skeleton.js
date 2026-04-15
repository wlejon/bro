// Skeleton — bones, sockets, lookups, bindPose.

// Build a 3-bone chain: root -> spine -> head.
const skel = Skeleton.fromBones([
    { name: 'root',  parent: -1, localT: [0, 0, 0] },
    { name: 'spine', parent:  0, localT: [0, 1, 0] },
    { name: 'head',  parent:  1, localT: [0, 0.5, 0] },
]);

assert(skel.boneCount === 3,   'boneCount');
assert(skel.socketCount === 0, 'no sockets');

// Round-trip via property getter.
const bones = skel.bones;
assert(bones.length === 3,       'bones.length');
assert(bones[0].name === 'root', 'bone[0].name');
assert(bones[1].parent === 0,    'bone[1].parent');
assert(Math.abs(bones[1].localT[1] - 1) < 1e-6, 'bone[1].localT[1]');
assert(bones[2].localR.length === 4,            'localR has 4 components');
assert(Math.abs(bones[2].localR[3] - 1) < 1e-6, 'localR defaults to identity (qw=1)');
assert(bones[0].localS[0] === 1,                'localS defaults to 1');

// findBone / findSocket.
assert(skel.findBone('spine') === 1,    'findBone present');
assert(skel.findBone('missing') === -1, 'findBone missing');
assert(skel.findSocket('any') === -1,   'findSocket on empty list');

// addSocket attaches at an existing bone.
const idx = skel.addSocket({ name: 'helm', bone: 2, offset: [
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
]});
assert(idx === 0,                                  'first socket index');
assert(skel.socketCount === 1,                     'socketCount after add');
assert(skel.findSocket('helm') === 0,              'findSocket after add');
assert(skel.sockets[0].bone === 2,                 'socket bound to head');
assert(skel.sockets[0].name === 'helm',            'socket name');

// bindPose returns a Pose with one entry per bone (stride 10).
const pose = skel.bindPose();
assert(pose.boneCount === 3,        'bindPose boneCount');
assert(pose.data.length === 30,     'bindPose data length (10 floats/bone)');
// translation of bone[1] should equal localT
assert(Math.abs(pose.data[10 + 1] - 1) < 1e-6, 'bone[1] T.y from bind');
// rotation is unit quat
assert(Math.abs(pose.data[10 + 6] - 1) < 1e-6, 'bone[1] R.w from bind');
// scale is 1
assert(Math.abs(pose.data[10 + 7] - 1) < 1e-6, 'bone[1] S.x from bind');

// Constructor with no args = empty skeleton.
const empty = new Skeleton();
assert(empty.boneCount === 0,   'empty bones');
assert(empty.socketCount === 0, 'empty sockets');

// Constructor with { bones, sockets }.
const built = new Skeleton({
    bones: [
        { name: 'a', parent: -1 },
        { name: 'b', parent:  0 },
    ],
    sockets: [
        { name: 'tip', bone: 1 },
    ],
});
assert(built.boneCount === 2,        'ctor bones');
assert(built.socketCount === 1,      'ctor sockets');
assert(built.findBone('b') === 1,    'ctor lookup');

console.log('PASS test_skeleton');
