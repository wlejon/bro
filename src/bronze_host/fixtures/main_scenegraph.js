// The runnable half of the cube app: everything three.js's WebGLRenderer would
// do EXCEPT the drawing, built from the modules that are actually vendored.
//
// Why this file exists at all is in MISSING_MODULES.md beside it: r160's
// WebGLRenderer is not in the vendored tree and its import closure is ~200
// files, so `main.js` (the renderer version) cannot be compiled today. This one
// can, and it is the real integration proof for the host layer — it drives
// every seam the renderer would drive except the GL calls themselves:
//
//   * document.createElement('canvas') and body.appendChild
//   * canvas.width/height, canvas.style, canvas.getContext('webgl2')
//   * window.innerWidth/innerHeight/devicePixelRatio
//   * requestAnimationFrame, once per engine frame, with a timestamp
//   * performance.now() on the same clock as the rAF timestamp
//   * setTimeout on that clock
//   * the microtask checkpoint the host performs after each frame's rAF
//   * the whole three.js scene graph: Scene, PerspectiveCamera, BoxGeometry,
//     MeshBasicMaterial, Mesh, and Object3D.updateMatrixWorld
//
// WHAT IT PRINTS, and the rule behind it: every line is a boolean, an integer,
// or a decimal IEEE-754 represents exactly. Nothing accumulated is pinned. The
// rotation loop below accumulates rounding in every matrix element, so what is
// printed for it is what stays TRUE of any rotation matrix — orthonormal
// columns, determinant 1 — inside a tolerance far wider than double rounding
// over ~20 flops and far tighter than any miscompilation or wrong host value.
// That is the same rule bronze's own three.js milestone case is written under
// (bronze/tests/oracle/threejs/README.md), and it is what lets the expectation
// beside this file be derived by hand rather than recorded from a run.
//
// Every line is prefixed `APP ` so the checker can separate it from the
// engine's own log, which shares stdout and is not deterministic.
//
// THE IMPORT PATHS are relative to this file and reach into the bronze
// checkout's vendored three.js: bro and bronze are siblings, which is the same
// assumption the CMake makes (bro_default_sibling_dir). A -DBRONZE_DIR override
// moves the C++ but NOT these specifiers — module resolution is the bronze
// CLI's, done at compile time against the filesystem.

import { Scene } from '../../../../bronze/tests/oracle/threejs/three/scenes/Scene.js';
import { PerspectiveCamera } from '../../../../bronze/tests/oracle/threejs/three/cameras/PerspectiveCamera.js';
import { Mesh } from '../../../../bronze/tests/oracle/threejs/three/objects/Mesh.js';
import { BoxGeometry } from '../../../../bronze/tests/oracle/threejs/three/geometries/BoxGeometry.js';
import { MeshBasicMaterial } from '../../../../bronze/tests/oracle/threejs/three/materials/MeshBasicMaterial.js';
import { Vector3 } from '../../../../bronze/tests/oracle/threejs/three/math/Vector3.js';

const FRAMES = 5;
// 2:1 on purpose: the camera's aspect is then exactly 2, an integer this file
// can print instead of the 1.3333333333333333 a 4:3 buffer would make it.
const WIDTH = 320;
const HEIGHT = 160;

// A deviation this wide is not reachable by double rounding over the tens of
// flops each check covers (2^-52 ~ 2.2e-16), and no miscompilation lands inside
// it either: a wrong operand, a wrong order or a lost store moves a trig result
// by ~1e-2, not by ~1e-13.
const EPS = 1e-12;
function near(a, b) { return a - b < EPS && b - a < EPS; }
function say(label, value) { console.log('APP ' + label + '=' + value); }

// --- 1. The host DOM ------------------------------------------------------
const canvas = document.createElement('canvas');
canvas.style.display = 'block';
document.body.appendChild(canvas);

// getContext caches, per spec: the second call must be the same object.
const gl = canvas.getContext('webgl2');
const gl2 = canvas.getContext('webgl2');
say('gl.notNull', gl !== null);
say('gl.cached', gl === gl2);

// SIZE AFTER getContext, deliberately, and this is the order three.js's
// WebGLRenderer.setSize uses too. The engine sizes a brand-new context from
// the element's LAYOUT BOX (Engine::createWebGL2Context), and a canvas appended
// in the same turn has no box yet — so it starts at the viewport size. The
// width/height setters are what resize the drawing buffer to what the app
// asked for, and writing the HTML attributes is also what tells the engine's
// per-frame syncWebGLCanvasSizes to stop auto-fitting this canvas.
canvas.width = WIDTH;
canvas.height = HEIGHT;

say('canvas.width', canvas.width);
say('canvas.height', canvas.height);
say('gl.drawingBufferWidth', gl.drawingBufferWidth);
say('gl.drawingBufferHeight', gl.drawingBufferHeight);
// The sniff three.js performs to decide it has a WebGL2 context.
say('gl.isWebGL2', typeof WebGL2RenderingContext !== 'undefined' &&
                   gl.constructor.name === 'WebGL2RenderingContext');
// One GL constant and one live query, to prove the binding is wired to a real
// context and not to a table of names. The query's VALUE is the driver's, so
// what is pinned is that it answered at all.
say('gl.TEXTURE_2D', gl.TEXTURE_2D);
say('gl.maxTextureSizePositive', gl.getParameter(gl.MAX_TEXTURE_SIZE) > 0);

say('window.hasSizes', window.innerWidth > 0 && window.innerHeight > 0);
say('window.self', window.self === window);

// --- 2. The scene graph ---------------------------------------------------
const scene = new Scene();
const camera = new PerspectiveCamera(75, WIDTH / HEIGHT, 0.1, 1000);
const geometry = new BoxGeometry(1, 1, 1);
const material = new MeshBasicMaterial({ color: 0x00ff00 });
const mesh = new Mesh(geometry, material);
scene.add(mesh);
camera.position.z = 5;

say('scene.type', scene.type);
say('camera.type', camera.type);
say('material.type', material.type);
// MeshBasicMaterial({color}) runs setValues, so a green cube is 0x00ff00 read
// back through Color.getHex() — an exact integer round trip, not a float.
say('material.colorHex', material.color.getHex());
// BoxGeometry.js:44-49 builds six planes, each 1x1 segments: 4 vertices and 6
// indices per plane. 24 and 36, derived from the source, not measured.
say('position.count', geometry.attributes.position.count);
say('index.count', geometry.index.count);
say('scene.children', scene.children.length);
say('camera.aspect', camera.aspect);

// --- 3. Timers and the microtask checkpoint -------------------------------
// A zero-delay timeout scheduled while the top level is still running is due at
// clock 0, and the host fires timers before rAF — so this lands on the first
// frame, ahead of that frame's rAF line.
setTimeout(function () { say('timeout', 'fired'); }, 0);

// --- 4. The frame loop ----------------------------------------------------
let frame = 0;

function tick(timestampMs) {
    // The rAF timestamp and performance.now() are the same clock, which is the
    // invariant three.js's Clock leans on. Printed as a boolean because the
    // value depends on the step size the host was driven with.
    say('clockAgrees', performance.now() === timestampMs);

    mesh.rotation.x += 0.01;
    mesh.rotation.y += 0.02;
    scene.updateMatrixWorld();

    const e = mesh.matrixWorld.elements;
    // The mesh sits at the origin under an identity parent, so the translation
    // column is a sum of products of exact zeros: exactly 0, not nearly 0.
    say('frame', frame);
    say('translation', e[12] + ',' + e[13] + ',' + e[14] + ',' + e[15]);

    const c0 = new Vector3(e[0], e[1], e[2]);
    const c1 = new Vector3(e[4], e[5], e[6]);
    const c2 = new Vector3(e[8], e[9], e[10]);
    say('orthonormal',
        near(c0.dot(c0), 1) && near(c1.dot(c1), 1) && near(c2.dot(c2), 1) &&
        near(c0.dot(c1), 0) && near(c0.dot(c2), 0) && near(c1.dot(c2), 0));
    // det = 1 distinguishes a rotation from a reflection; |det| = 1 would not.
    say('determinant1', near(mesh.matrixWorld.determinant(), 1));

    // A promise resolved inside a rAF callback proves the host performs the
    // microtask checkpoint AFTER rAF and within the same frame: this line lands
    // between this frame's last rAF line and the next frame's first one. Drain
    // it before rAF instead and it would print one whole frame late.
    Promise.resolve(frame).then(function (f) { say('microtask', f); });

    frame = frame + 1;
    if (frame < FRAMES) {
        requestAnimationFrame(tick);
    } else {
        say('done', FRAMES);
    }
}

requestAnimationFrame(tick);
say('mainEnd', 1);
