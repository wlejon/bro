// Per-axis rotation properties (rotationX/rotationY/rotationZ) are exact and
// composable: a write to one axis reads back unchanged and leaves the other
// two as they were last set. They used to decompose the quaternion on every
// set, and qtoEuler folds a yaw past +/-90 deg into (pi, pi - yaw, pi), so a
// second write to rotationY mirrored the node to pi - value.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const EPS = 1e-5;
const near = (a, b, msg, eps) => assert(Math.abs(a - b) <= (eps === undefined ? EPS : eps),
    `${msg}: got ${a}, expected ${b}`);

const eulerToQuat = (rx, ry, rz) => {
    const cx = Math.cos(rx / 2), sx = Math.sin(rx / 2);
    const cy = Math.cos(ry / 2), sy = Math.sin(ry / 2);
    const cz = Math.cos(rz / 2), sz = Math.sin(rz / 2);
    return [
        sx * cy * cz - cx * sy * sz,
        cx * sy * cz + sx * cy * sz,
        cx * cy * sz - sx * sy * cz,
        cx * cy * cz + sx * sy * sz,
    ];
};

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available (no GPU)');
} else {
    // --- yaw past 90 deg then back ------------------------------------
    const n = scene.createMesh({ mesh: 'box' });

    n.rotationY = 2.0;
    near(n.rotationX, 0, 'yaw 2.0 leaves X at 0');
    near(n.rotationY, 2.0, 'yaw 2.0 reads back');
    near(n.rotationZ, 0, 'yaw 2.0 leaves Z at 0');

    n.rotationY = 0.5;
    near(n.rotationX, 0, 'yaw 0.5 after 2.0 leaves X at 0');
    near(n.rotationY, 0.5, 'yaw 0.5 after 2.0 reads back');
    near(n.rotationZ, 0, 'yaw 0.5 after 2.0 leaves Z at 0');

    // --- three axes in sequence ---------------------------------------
    const m = scene.createMesh({ mesh: 'box' });
    m.rotationX = 0.3;
    m.rotationY = 2.4;
    m.rotationZ = -1.1;
    near(m.rotationX, 0.3, 'X survives the Y and Z writes');
    near(m.rotationY, 2.4, 'Y survives the Z write');
    near(m.rotationZ, -1.1, 'Z reads back');

    m.rotationY = 0.2;
    near(m.rotationX, 0.3, 'X untouched by a Y rewrite');
    near(m.rotationY, 0.2, 'Y rewrite reads back');
    near(m.rotationZ, -1.1, 'Z untouched by a Y rewrite');

    // --- quaternion is the atomic alternative --------------------------
    // It drops the stored triple, so a per-axis read decomposes it.
    const q = scene.createMesh({ mesh: 'box' });
    q.quaternion = eulerToQuat(0.4, 0.9, -0.25);
    near(q.rotationX, 0.4, 'quaternion write decomposes to X', 1e-4);
    near(q.rotationY, 0.9, 'quaternion write decomposes to Y', 1e-4);
    near(q.rotationZ, -0.25, 'quaternion write decomposes to Z', 1e-4);

    q.rotationY = 0.1;
    near(q.rotationX, 0.4, 'X kept across a quaternion then axis write', 1e-4);
    near(q.rotationY, 0.1, 'Y written after a quaternion write', 1e-4);
    near(q.rotationZ, -0.25, 'Z kept across a quaternion then axis write', 1e-4);

    // --- the transform matches a fresh node at the same yaw ------------
    const fresh = scene.createMesh({ mesh: 'box' });
    fresh.rotationY = 0.5;
    const probes = [[1, 0, 0], [0, 1, 0], [0, 0, 1], [0.3, -0.7, 2]];
    for (const [px, py, pz] of probes) {
        const a = n.localToWorld(px, py, pz);
        const b = fresh.localToWorld(px, py, pz);
        near(a.x, b.x, `localToWorld x at ${px},${py},${pz}`, 1e-5);
        near(a.y, b.y, `localToWorld y at ${px},${py},${pz}`, 1e-5);
        near(a.z, b.z, `localToWorld z at ${px},${py},${pz}`, 1e-5);
    }
    // And it is the real yaw of 0.5, not pi - 0.5.
    const zAxis = n.localToWorld(0, 0, 1);
    near(zAxis.x, Math.sin(0.5), 'yaw 0.5 turns +Z toward +X', 1e-5);
    near(zAxis.z, Math.cos(0.5), 'yaw 0.5 keeps +Z forward', 1e-5);

    // --- the 2D rotation property stays Z-only -------------------------
    const two = scene.createShape();
    if (two) {
        two.rotation = 1.9;
        near(two.rotation, 1.9, '2D rotation reads back');
        near(two.rotationX, 0, '2D rotation leaves X at 0');
        near(two.rotationY, 0, '2D rotation leaves Y at 0');
        near(two.rotationZ, 1.9, '2D rotation is rotationZ');
    }

    console.log('rotation axis tests passed');
}
