// node.rotation = [rx, ry, rz] sets Euler radians (it was silently ignored:
// only a 4-element quaternion took effect).
const canvas = document.createElement('canvas');
document.body.appendChild(canvas);
flush();
const scene = canvas.getContext('scene');
if (!scene) missingGpuContext('scene');
else {
    const n = scene.createGroup ? scene.createGroup() : scene.createInteriorVolume({});
    n.rotation = [0, 0.5, 0];
    assert(Math.abs(n.rotationY - 0.5) < 1e-5, `Euler array sets rotationY (${n.rotationY})`);
    const q = n.quaternion;
    assert(Math.abs(q[1] - Math.sin(0.25)) < 1e-5 && Math.abs(q[3] - Math.cos(0.25)) < 1e-5, `and the quaternion (${q})`);
    n.rotation = [0, 0, 0, 1];
    assert(Math.abs(n.rotationY) < 1e-6, 'a quaternion still sets it');
    console.log('test_node_rotation_euler: OK');
}
