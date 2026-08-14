// The instanced mesh probe: a high-instance-count Three.js scene testing
// InstancedMesh under load with dynamic matrix & color updates per frame.
//
// Proves that compiled JS can:
//  1. Import from bare 'three' package resolution.
//  2. Allocate and animate InstancedMesh with thousands of instances (2,500 instances).
//  3. Update instanceMatrix and instanceColor each frame via setMatrixAt/setColorAt.
//  4. Render instanced geometry through WebGL2 instanced draw calls.
//  5. Maintain correct render info statistics and render lit colored pixels.

import {
    Scene,
    PerspectiveCamera,
    WebGLRenderer,
    BoxGeometry,
    InstancedMesh,
    MeshStandardMaterial,
    DirectionalLight,
    AmbientLight,
    Object3D,
    Color
} from 'three';

const WIDTH = 320;
const HEIGHT = 240;
const INSTANCE_COUNT = 2500;

function say(label, value) {
    console.log('APP ' + label + '=' + value);
}

// --- Canvas & Renderer ----------------------------------------------------
const canvas = document.createElement('canvas');
canvas.width = WIDTH;
canvas.height = HEIGHT;
document.body.appendChild(canvas);

const renderer = new WebGLRenderer({ canvas: canvas, antialias: false });
renderer.setSize(WIDTH, HEIGHT, false);
renderer.setClearColor(0x101010, 1);

say('canvas.width', canvas.width);
say('canvas.height', canvas.height);
say('renderer.isWebGL2', renderer.capabilities.isWebGL2);

// --- Scene, Camera, Lights ------------------------------------------------
const scene = new Scene();
const camera = new PerspectiveCamera(60, WIDTH / HEIGHT, 0.1, 1000);
camera.position.set(0, 0, 18);
camera.lookAt(0, 0, 0);

const ambientLight = new AmbientLight(0x404040, 1.0);
scene.add(ambientLight);

const dirLight = new DirectionalLight(0xffffff, 1.5);
dirLight.position.set(5, 10, 7);
scene.add(dirLight);

// --- InstancedMesh Setup --------------------------------------------------
const geometry = new BoxGeometry(0.3, 0.3, 0.3);
const material = new MeshStandardMaterial({
    roughness: 0.5,
    metalness: 0.1
});

const instancedMesh = new InstancedMesh(geometry, material, INSTANCE_COUNT);
scene.add(instancedMesh);

const dummy = new Object3D();
const tempColor = new Color();

function updateInstances(frame) {
    const time = frame * 0.1;
    const gridDim = 50; // 50x50 = 2500 instances

    for (let i = 0; i < INSTANCE_COUNT; i++) {
        const gx = (i % gridDim) - (gridDim / 2);
        const gy = Math.floor(i / gridDim) - (gridDim / 2);

        const x = gx * 0.4 + Math.sin(time + i * 0.05) * 0.15;
        const y = gy * 0.4 + Math.cos(time + i * 0.05) * 0.15;
        const z = Math.sin(time + (gx + gy) * 0.1) * 0.5;

        dummy.position.set(x, y, z);
        dummy.rotation.set(time + i * 0.02, time * 0.5 + i * 0.03, 0);

        const s = 0.8 + 0.2 * Math.sin(time + i * 0.1);
        dummy.scale.set(s, s, s);
        dummy.updateMatrix();

        instancedMesh.setMatrixAt(i, dummy.matrix);

        const r = 0.5 + 0.5 * Math.sin(time + gx * 0.2);
        const g = 0.5 + 0.5 * Math.cos(time + gy * 0.2);
        const b = 0.5 + 0.5 * Math.sin(time + (gx + gy) * 0.2);
        tempColor.setRGB(r, g, b);
        instancedMesh.setColorAt(i, tempColor);
    }

    instancedMesh.instanceMatrix.needsUpdate = true;
    if (instancedMesh.instanceColor !== null) {
        instancedMesh.instanceColor.needsUpdate = true;
    }
}

// Initial instance setup
updateInstances(0);

say('instanced.count', instancedMesh.count === INSTANCE_COUNT);
say('instanced.matrixCount', instancedMesh.instanceMatrix.count === INSTANCE_COUNT);
say('instanced.colorCount', instancedMesh.instanceColor !== null && instancedMesh.instanceColor.count === INSTANCE_COUNT);

// --- Frame Loop & State Verification --------------------------------------
const gl = renderer.getContext();
const pixels = new Uint8Array(WIDTH * HEIGHT * 4);

let frame = 0;

function checkPixels(isFinal) {
    gl.readPixels(0, 0, WIDTH, HEIGHT, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    const centerIdx = (120 * WIDTH + 160) * 4;
    const cr = pixels[centerIdx], cg = pixels[centerIdx + 1], cb = pixels[centerIdx + 2];
    const cornerR = pixels[0], cornerG = pixels[1], cornerB = pixels[2];

    if (!isFinal) {
        say('pixel.instancedLit', (cr > 15 || cg > 15 || cb > 15));
    } else {
        const cornerClear = (cornerR > 8 && cornerR < 24 && cornerG > 8 && cornerG < 24 && cornerB > 8 && cornerB < 24);
        say('pixel.cornerClear', cornerClear);
        say('pixel.rendered', (cr !== cornerR || cg !== cornerG || cb !== cornerB));
    }
}

function tick() {
    updateInstances(frame);
    renderer.render(scene, camera);

    if (frame === 1) {
        say('render.calls', renderer.info.render.calls > 0);
        say('render.calls.low', renderer.info.render.calls <= 2);
        say('render.triangles', renderer.info.render.triangles === INSTANCE_COUNT * 12);
        checkPixels(false);
    }

    frame = frame + 1;
    if (frame === 5) {
        checkPixels(true);
        say('done', 1);
    }

    requestAnimationFrame(tick);
}

requestAnimationFrame(tick);
say('ready', 1);
