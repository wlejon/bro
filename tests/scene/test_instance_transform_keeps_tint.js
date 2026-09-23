// setInstanceTransform replaces only an instance's transform: the tint in
// floats 12-15 of its record (from `instances` or setInstanceColor) is kept.
// It used to write 1,1,1,1 there, so moving a tinted instance turned it white.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
assert(scene !== null, 'scene context');
scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
scene.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 6], target: [0, 0, 0], up: [0, 1, 0] });

function centerPixel() {
    const img = scene.captureFrame();
    const i = (64 * img.width + 64) * 4;
    return { r: img.data[i], g: img.data[i + 1], b: img.data[i + 2], a: img.data[i + 3] };
}
const fmt = (p) => `rgb(${p.r},${p.g},${p.b})`;
// The lit path multiplies the tint in (the emissive term does not), so a
// tinted white box shows the tint's hue under the default light.
const isRed = (p) => p.r > 40 && p.r > 4 * p.g && p.r > 4 * p.b;
const isGreen = (p) => p.g > 40 && p.g > 4 * p.r && p.g > 4 * p.b;

// One instance, parked off to the side, tinted red.
const rec = new Float32Array([
    1, 0, 0, 5,
    0, 1, 0, 0,
    0, 0, 1, 0,
    1, 0, 0, 1,
]);
const inst = scene.createInstancedMesh({ mesh: Mesh.box(), color: [1, 1, 1, 1], instances: rec });

let p = centerPixel();
assert(p.a === 0, 'the instance starts off-centre, centre is ' + fmt(p));

// Move it to the origin with a column-major 4x4: still red.
inst.setInstanceTransform(0, [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
p = centerPixel();
assert(isRed(p), 'the tint from `instances` survives setInstanceTransform, got ' + fmt(p));

// A tint set with setInstanceColor survives a later move too.
inst.setInstanceColor(0, [0, 1, 0, 1]);
inst.setInstanceTransform(0, [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.1, 0, 0, 1]);
p = centerPixel();
assert(isGreen(p), 'the tint from setInstanceColor survives a move, got ' + fmt(p));

console.log('test_instance_transform_keeps_tint: OK');
