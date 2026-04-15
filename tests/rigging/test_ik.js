// IK — twoBone, FABRIK, lookAt solvers.

// Helper: world-space tip position of a bone chain after computing world matrices.
function tipPosition(skel, pose, boneIndex) {
    const w = pose.computeWorldMatrices(skel);
    const off = boneIndex * 16;
    return [w[off + 12], w[off + 13], w[off + 14]];
}

// ── Two-bone IK on a 3-bone chain (root, mid, end) ──────────────────────────
{
    // shoulder at origin -> elbow at +y 1 -> wrist at +y 1 (chain length 2 along y)
    const skel = Skeleton.fromBones([
        { name: 'shoulder', parent: -1, localT: [0, 0, 0] },
        { name: 'elbow',    parent:  0, localT: [0, 1, 0] },
        { name: 'wrist',    parent:  1, localT: [0, 1, 0] },
    ]);
    const pose = skel.bindPose();

    // Reachable target.
    const target = [1.0, 1.5, 0.0];
    const ok = IK.twoBone(skel, pose, 0, 1, 2, target);
    assert(ok === true, 'twoBone solver returned true');

    const tip = tipPosition(skel, pose, 2);
    const dx = tip[0] - target[0];
    const dy = tip[1] - target[1];
    const dz = tip[2] - target[2];
    const dist = Math.sqrt(dx*dx + dy*dy + dz*dz);
    assert(dist < 0.05, 'twoBone tip near target: dist=' + dist);
}

// ── FABRIK on a 4-bone chain ────────────────────────────────────────────────
{
    const skel = Skeleton.fromBones([
        { name: 'b0', parent: -1, localT: [0, 0, 0] },
        { name: 'b1', parent:  0, localT: [0, 1, 0] },
        { name: 'b2', parent:  1, localT: [0, 1, 0] },
        { name: 'b3', parent:  2, localT: [0, 1, 0] },
    ]);
    const pose = skel.bindPose();
    const target = [2.0, 1.0, 0.0];
    const ok = IK.FABRIK(skel, pose, [0, 1, 2, 3], target, { iterations: 20, tolerance: 1e-4 });
    assert(ok === true, 'FABRIK returned true');

    const tip = tipPosition(skel, pose, 3);
    const dx = tip[0] - target[0];
    const dy = tip[1] - target[1];
    const dz = tip[2] - target[2];
    const dist = Math.sqrt(dx*dx + dy*dy + dz*dz);
    assert(dist < 0.05, 'FABRIK tip near target: dist=' + dist);
}

// ── Look-at on a single bone ────────────────────────────────────────────────
{
    const skel = Skeleton.fromBones([
        { name: 'head', parent: -1, localT: [0, 0, 0] },
    ]);
    const pose = skel.bindPose();
    const ok = IK.lookAt(skel, pose, 0, [1, 0, 0]);
    assert(ok === true, 'lookAt returned true');
    // Pose data should now contain a non-identity rotation on bone 0.
    const qx = pose.data[3], qy = pose.data[4], qz = pose.data[5], qw = pose.data[6];
    const isIdentity = Math.abs(qx) < 1e-5 && Math.abs(qy) < 1e-5 && Math.abs(qz) < 1e-5 && Math.abs(qw - 1) < 1e-5;
    assert(!isIdentity, 'lookAt rotated the bone');
    // Quaternion should still be unit length.
    const len = Math.sqrt(qx*qx + qy*qy + qz*qz + qw*qw);
    assert(Math.abs(len - 1) < 1e-3, 'lookAt produced unit quaternion: ' + len);
}

// ── Custom forward / up axes ────────────────────────────────────────────────
{
    const skel = Skeleton.fromBones([{ name: 'turret', parent: -1 }]);
    const pose = skel.bindPose();
    const ok = IK.lookAt(skel, pose, 0, [0, 0, 5], { localForward: [0, 0, 1], localUp: [0, 1, 0] });
    assert(ok === true, 'lookAt with custom axes returned true');
}

console.log('PASS test_ik');
