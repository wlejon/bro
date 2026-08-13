// The cube app: a real three.js WebGLRenderer drawing a rotating
// MeshBasicMaterial box into a bro-hosted WebGL2 canvas.
//
// THIS FILE DOES NOT COMPILE TODAY, and that is a fact about the tree, not
// about the file. `WebGLRenderer` is not vendored — MISSING_MODULES.md beside
// this file has the exact list and the ~200-module transitive closure behind
// it. Nothing here works around that: the imports name the modules the program
// genuinely needs, so the day the renderer closure lands (or the day the
// single-file `three.module.js` bundle does), this becomes a one-line change to
// the specifiers and nothing else.
//
// `main_scenegraph.js` beside it is the version that runs against what IS
// vendored, and it is the integration proof for the host layer today.
//
// THE IMPORT PATHS assume the renderer has been vendored INTO the existing
// tree, at the paths the library's own relative specifiers name — option 1 in
// MISSING_MODULES.md. For the bundle instead (option 2), every import below
// collapses to one line:
//
//     import * as THREE from '<path>/three.module.js';
//
// and the five names come off `THREE.`.

import { Scene } from '../../../../bronze/tests/oracle/threejs/three/scenes/Scene.js';
import { PerspectiveCamera } from '../../../../bronze/tests/oracle/threejs/three/cameras/PerspectiveCamera.js';
import { Mesh } from '../../../../bronze/tests/oracle/threejs/three/objects/Mesh.js';
import { BoxGeometry } from '../../../../bronze/tests/oracle/threejs/three/geometries/BoxGeometry.js';
import { MeshBasicMaterial } from '../../../../bronze/tests/oracle/threejs/three/materials/MeshBasicMaterial.js';
import { WebGLRenderer } from '../../../../bronze/tests/oracle/threejs/three/renderers/WebGLRenderer.js';

const FRAMES = 5;
// 2:1, so the camera's aspect is exactly 2 rather than a repeating fraction —
// the same reason main_scenegraph.js picks these numbers.
const WIDTH = 320;
const HEIGHT = 160;

function say(label, value) { console.log('APP ' + label + '=' + value); }

// --- The canvas -----------------------------------------------------------
// Built and appended by the app, exactly as a browser page's script would: the
// host's document.createElement answers a canvas object whose getContext
// reaches Engine::createWebGL2Context, and body.appendChild puts the element in
// the engine's document so it gets a layout box and is composited.
const canvas = document.createElement('canvas');
canvas.style.display = 'block';
document.body.appendChild(canvas);
// Note what is NOT here: canvas.width/height. setSize below writes them, and it
// has to happen AFTER the renderer has taken the context — the engine sizes a
// brand-new context from the element's layout box, which a canvas appended in
// this same turn does not have yet. main_scenegraph.js spells the same
// constraint out at the point it hits it.

// --- The renderer ---------------------------------------------------------
// Passing the canvas rather than letting the renderer create one is what keeps
// the element the engine composites and the element three.js draws into the
// same object. `antialias` is off: the host canvas FBO has no multisample path,
// and asking for one the context cannot give would be a silent downgrade.
const renderer = new WebGLRenderer({ canvas: canvas, antialias: false });
renderer.setPixelRatio(1);            // pinned, not window.devicePixelRatio:
                                      // a DPR-scaled buffer would make the
                                      // printed sizes depend on the display
renderer.setSize(WIDTH, HEIGHT, false);  // false: do not write canvas.style

// --- The scene ------------------------------------------------------------
const scene = new Scene();
const camera = new PerspectiveCamera(75, WIDTH / HEIGHT, 0.1, 1000);
camera.position.z = 5;

const geometry = new BoxGeometry(1, 1, 1);
const material = new MeshBasicMaterial({ color: 0x00ff00 });
const mesh = new Mesh(geometry, material);
scene.add(mesh);

// Facts about the setup, each an integer or a boolean — nothing accumulated,
// for the reason main_scenegraph.js states at length.
say('canvas.width', canvas.width);
say('canvas.height', canvas.height);
say('camera.aspect', camera.aspect);
say('material.colorHex', material.color.getHex());
say('position.count', geometry.attributes.position.count);
say('index.count', geometry.index.count);
say('renderer.isWebGL2', renderer.capabilities.isWebGL2);
say('renderer.drawingBufferWidth', renderer.getContext().drawingBufferWidth);

// --- The frame loop -------------------------------------------------------
// Plain requestAnimationFrame rather than renderer.setAnimationLoop: the two
// are the same mechanism here (setAnimationLoop drives WebGLAnimation, which
// calls requestAnimationFrame), and spelling it out keeps what is being tested
// — the host's rAF — visible in this file.
let frame = 0;

function tick() {
    mesh.rotation.x += 0.01;
    mesh.rotation.y += 0.02;
    renderer.render(scene, camera);

    // What the renderer actually did this frame, as integers: one draw call for
    // one mesh, and 36 indices for the box's twelve triangles. These come off
    // WebGLInfo, which counts what reached the GL, so a frame that silently
    // drew nothing reads 0 here rather than looking identical to a good one.
    say('frame', frame);
    say('render.calls', renderer.info.render.calls);
    say('render.triangles', renderer.info.render.triangles);
    say('memory.geometries', renderer.info.memory.geometries);

    frame = frame + 1;
    if (frame < FRAMES) {
        requestAnimationFrame(tick);
    } else {
        say('done', FRAMES);
    }
}

requestAnimationFrame(tick);
say('mainEnd', 1);
