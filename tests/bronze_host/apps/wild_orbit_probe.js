// The wild orbit probe: an unmodified three.js scene with Mesh, Materials,
// Lights, WebGLRenderer, OrbitControls, and fetch-backed TextureLoader.
//
// Proves that compiled JS can:
//  1. Construct and render a full 3D scene with lighting and textures
//  2. Load textures and asset configs via host fetch and ImageLoader
//  3. Interactively orbit and dolly the camera via OrbitControls and DOM events
//  4. Maintain valid camera transforms and render non-trivial lit pixels

import {
    Scene,
    PerspectiveCamera,
    WebGLRenderer,
    BoxGeometry,
    MeshStandardMaterial,
    Mesh,
    DirectionalLight,
    AmbientLight,
    TextureLoader
} from '../../../../bronze/tests/oracle/threejs/three.module.js';
import { OrbitControls } from '../../../../bronze/tests/oracle/threejs/OrbitControls.js';

const WIDTH = 320;
const HEIGHT = 240;

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
camera.position.set(0, 0, 5);
camera.lookAt(0, 0, 0);

const ambientLight = new AmbientLight(0x404040, 1.0);
scene.add(ambientLight);

const dirLight = new DirectionalLight(0xffffff, 1.5);
dirLight.position.set(5, 5, 5);
scene.add(dirLight);

// --- OrbitControls --------------------------------------------------------
const controls = new OrbitControls(camera, canvas);
controls.enableDamping = false;

// --- Asset loading via fetch & TextureLoader ------------------------------
fetch('scene_config.json').then(function (res) {
    say('fetch.ok', res.ok);
    say('fetch.status', res.status);
    return res.json();
}).then(function (cfg) {
    say('fetch.name', cfg.name);
    say('fetch.lightIntensity', cfg.lightIntensity > 0);
});

const textureLoader = new TextureLoader();
const texture = textureLoader.load('test_texture.png', function (tex) {
    say('texture.loaded', true);
});

const geometry = new BoxGeometry(1.5, 1.5, 1.5);
const material = new MeshStandardMaterial({
    color: 0xffffff,
    map: texture,
    roughness: 0.4,
    metalness: 0.2
});
const mesh = new Mesh(geometry, material);
scene.add(mesh);

// --- Initial Camera Predicates --------------------------------------------
camera.updateMatrixWorld();
say('camera.init.x', Math.abs(camera.position.x) < 1e-4);
say('camera.init.y', Math.abs(camera.position.y) < 1e-4);
say('camera.init.z', Math.abs(camera.position.z - 5.0) < 1e-4);
say('camera.init.det', Math.abs(camera.matrixWorld.determinant() - 1.0) < 1e-4);

// --- Frame Loop & State Verification --------------------------------------
const gl = renderer.getContext();
const pixels = new Uint8Array(WIDTH * HEIGHT * 4);

let frame = 0;
let reportedOrbit = false;
let reportedZoom = false;

function checkPixels(isFinal) {
    gl.readPixels(0, 0, WIDTH, HEIGHT, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    const centerIdx = (120 * WIDTH + 160) * 4;
    const cr = pixels[centerIdx], cg = pixels[centerIdx + 1], cb = pixels[centerIdx + 2];
    const cornerR = pixels[0], cornerG = pixels[1], cornerB = pixels[2];

    if (!isFinal) {
        // Front-lit cube at center has bright lit pixels
        say('pixel.frontLit', (cr > 30 || cg > 30 || cb > 30));
    } else {
        const cornerClear = (cornerR > 8 && cornerR < 24 && cornerG > 8 && cornerG < 24 && cornerB > 8 && cornerB < 24);
        say('pixel.cornerClear', cornerClear);
        say('pixel.rendered', (cr !== cornerR || cg !== cornerG || cb !== cornerB));
    }
}

function tick() {
    controls.update();
    renderer.render(scene, camera);

    if (frame === 1) {
        say('render.calls', renderer.info.render.calls > 0);
        say('render.triangles', renderer.info.render.triangles === 12);
        checkPixels(false);
    }

    if (!reportedOrbit && (Math.abs(camera.position.x) > 0.05 || Math.abs(camera.position.y) > 0.05)) {
        reportedOrbit = true;
        say('camera.orbit.moved', true);
        say('camera.orbit.det', Math.abs(camera.matrixWorld.determinant() - 1.0) < 1e-4);
    }

    const dist = camera.position.distanceTo(controls.target);
    if (!reportedZoom && Math.abs(dist - 5.0) > 0.05) {
        reportedZoom = true;
        say('camera.zoom.dolly', true);
        say('camera.zoom.distChanged', dist !== 5.0);
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
