// Levels of detail on instanced meshes:
//  - rows held on the GPU (unchanged for a few renders) still move when
//    setInstances replaces them;
//  - fadeInDist / fadeOutDist split a band between two nodes pixel for
//    pixel: before the band only the near node draws, past it only the far
//    one, and inside it the two together cover every pixel (no gap);
//  - setInstanceCullSpheres' minDistance culls instances wholly nearer.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) missingGpuContext('scene');
else runWithScene();

function runWithScene() {
scene.setToneMap({ mode: 'linear', gamma: 1.0 });
const look = (z) => scene.setCamera({ fov: 30, near: 0.1, far: 500, position: [0, 0, z], target: [0, 0, 0], up: [0, 1, 0] });

const row = (x, r, g, b) => [1, 0, 0, x, 0, 1, 0, 0, 0, 0, 1, 0, r, g, b, 1];
// The share of the centre 16x16 that is red, green, or covered at all.
function centre() {
    const img = scene.captureFrame();
    let red = 0, green = 0, any = 0;
    for (let y = 56; y < 72; y++) for (let x = 56; x < 72; x++) {
        const i = (y * img.width + x) * 4;
        const r = img.data[i], g = img.data[i + 1], b = img.data[i + 2], a = img.data[i + 3];
        if (a > 0) any++;
        if (r > 4 * g && r > 4 * b && r > 20) red++;
        if (g > 4 * r && g > 4 * b && g > 20) green++;
    }
    return { red: red / 256, green: green / 256, any: any / 256 };
}

// Rows held on the GPU move when replaced.
look(10);
const moving = scene.createInstancedMesh({ mesh: Mesh.box(1, 1, 1), color: [1, 1, 1, 1], instances: new Float32Array(row(0, 1, 0, 0)) });
for (let i = 0; i < 6; i++) centre();
assert(centre().red > 0.9, 'the held instance draws at the centre');
moving.setInstances(new Float32Array(row(20, 1, 0, 0)));
assert(centre().any === 0, 'replaced rows move it off the centre at once');
for (let i = 0; i < 6; i++) centre();
moving.setInstances(new Float32Array(row(0, 1, 0, 0)));
assert(centre().red > 0.9, 'and back');
moving.destroy();

// A near (red) and far (green) level of the same box, handing over at 40 m.
const near = scene.createInstancedMesh({ mesh: Mesh.box(3, 3, 3), color: [1, 1, 1, 1], instances: new Float32Array(row(0, 1, 0, 0)) });
const far = scene.createInstancedMesh({ mesh: Mesh.box(3, 3, 3), color: [1, 1, 1, 1], instances: new Float32Array(row(0, 0, 1, 0)) });
near.fadeOutDist = 40;
far.fadeInDist = 40;
far.fadeOutDist = 200;
assert(far.fadeInDist === 40, 'fadeInDist reads back');
let c;
look(30); c = centre();
assert(c.red > 0.95 && c.green === 0, `nearer than the band: the near level alone (${JSON.stringify(c)})`);
look(65); c = centre();
assert(c.green > 0.95 && c.red === 0, `past the band: the far level alone (${JSON.stringify(c)})`);
look(46); c = centre();
assert(c.red > 0.1 && c.green > 0.1 && c.any > 0.99, `in the band: both, covering every pixel (${JSON.stringify(c)})`);

// minDistance: the far level's instance, wholly nearer than 40 m, is culled
// (with its fade it drew nothing there anyway); past it, drawn.
const sph = bro.kernel.createBuffer(new Float32Array([0, 0, 0, 6]));
far.fadeInDist = 0;
far.setInstanceCullSpheres(sph, { minDistance: 40 });
near.visible = false;
look(20); c = centre();
assert(c.green === 0, `an instance wholly nearer than minDistance is culled (${JSON.stringify(c)})`);
look(60); c = centre();
assert(c.green > 0.95, `past minDistance it draws (${JSON.stringify(c)})`);

console.log('test_instance_lod_fades: OK');
}
