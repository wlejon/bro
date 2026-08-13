// The lit cube: the same box as main.js, drawn with MeshStandardMaterial under
// a DirectionalLight and an AmbientLight, and then read back from the
// framebuffer to prove it drew.
//
// WHY A SECOND APP RATHER THAN A FLAG IN THE FIRST. main.js draws with
// MeshBasicMaterial, which is the one built-in material that never reads a
// light: its fragment shader is the base color and nothing else, and the whole
// lighting half of the renderer — WebGLRenderStates collecting lights per
// frame, WebGLLights packing them into the uniform blocks, WebGLPrograms
// selecting a program from the light COUNTS, and the ~40 shader chunks the
// physical BRDF is assembled from — is dead code in that program. This file is
// what makes it live code. It is also the app that exercises the GL surface
// most heavily: a standard material's program has an order of magnitude more
// uniforms than a basic one, so uniform1i/3f/Matrix4fv and the texture-unit
// bookkeeping behind them are driven here in a way main.js never drives them.
//
// The import, the print rule, and the `APP ` prefix are main.js's — see its
// header. What changes here is the pixel predicate, and the reason it changes
// is stated where it is written.

import { Scene, PerspectiveCamera, Mesh, BoxGeometry, MeshStandardMaterial,
         DirectionalLight, AmbientLight, WebGLRenderer }
    from '../../../../bronze/tests/oracle/threejs/three.module.js';

const FRAMES = 5;
const WIDTH = 320;
const HEIGHT = 160;

function say(label, value) { console.log('APP ' + label + '=' + value); }

// --- The canvas and the renderer ------------------------------------------
const canvas = document.createElement('canvas');
canvas.style.display = 'block';
document.body.appendChild(canvas);

const renderer = new WebGLRenderer({ canvas: canvas, antialias: false });
renderer.setPixelRatio(1);
renderer.setSize(WIDTH, HEIGHT, false);
renderer.setClearColor(0x101010, 1);

// --- The scene ------------------------------------------------------------
const scene = new Scene();
const camera = new PerspectiveCamera(75, WIDTH / HEIGHT, 0.1, 1000);
camera.position.z = 5;

const geometry = new BoxGeometry(1, 1, 1);
// roughness 1 / metalness 0 is a pure dielectric diffuse surface: the specular
// lobe collapses to nothing, so the pixel under the camera is the diffuse term
// alone. That is a deliberate simplification of what has to be predicted below
// — a metallic or glossy surface would put a view-dependent highlight through
// the center pixel and make the predicate depend on the exact camera basis.
const material = new MeshStandardMaterial({ color: 0x00ff00, roughness: 1, metalness: 0 });
const mesh = new Mesh(geometry, material);
scene.add(mesh);

// Aimed straight down -Z from behind the camera, so the face the camera sees is
// the face the light hits: the front face's N·L is 1 at the start and stays
// near it for the small rotation this app applies. A light placed off to one
// side would leave the sampled pixel's brightness depending on the frame.
const directional = new DirectionalLight(0xffffff, 2);
directional.position.set(0, 0, 5);
scene.add(directional);
// A floor under the whole surface, which is what makes the predicate below
// hold on every face rather than only on the lit one.
const ambient = new AmbientLight(0xffffff, 0.6);
scene.add(ambient);

say('canvas.width', canvas.width);
say('canvas.height', canvas.height);
say('camera.aspect', camera.aspect);
say('material.type', material.type);
say('material.colorHex', material.color.getHex());
// The two lights are children of the scene like any other Object3D; the count
// is what WebGLRenderStates walks, and it is an integer the source states.
say('scene.children', scene.children.length);
say('light.directionalIntensity', directional.intensity);
say('light.ambientColorHex', ambient.color.getHex());
say('renderer.isWebGL2', renderer.capabilities.isWebGL2);

// --- Reading the framebuffer back -----------------------------------------
const gl = renderer.getContext();
const pixels = new Uint8Array(WIDTH * HEIGHT * 4);
const CENTER = ((HEIGHT / 2) * WIDTH + (WIDTH / 2)) * 4;
const CORNER = 0;

// THE TOLERANCE THAT SURVIVES LIGHTING, which is the whole difference between
// this predicate and main.js's. An unlit green cube is `0x00ff00` on the
// screen; a lit one is that color scaled by whatever the BRDF integrated —
// a number this file deliberately does not try to predict, because predicting
// it would mean reimplementing three.js's physical shading here and pinning
// the reimplementation rather than the render.
//
// So the predicate is about the SHAPE of the color and not its magnitude:
// green strictly dominates both other channels, and the pixel is bright enough
// that it cannot be the clear color or an unlit black face. Every way the
// render can go wrong that this app exists to catch — no light reaching the
// shader (black), the wrong uniform block bound (a different hue), the program
// selected for the wrong light counts (a compile failure and a blank frame) —
// lands outside it.
function centerIsLitGreen() {
    const r = pixels[CENTER], g = pixels[CENTER + 1], b = pixels[CENTER + 2];
    return g > 64 && g > r + 32 && g > b + 32;
}
function cornerIsClearColor() {
    const r = pixels[CORNER], g = pixels[CORNER + 1], b = pixels[CORNER + 2];
    return r > 8 && r < 24 && g > 8 && g < 24 && b > 8 && b < 24;
}

// --- The frame loop -------------------------------------------------------
let frame = 0;

function tick() {
    mesh.rotation.x += 0.01;
    mesh.rotation.y += 0.02;
    renderer.render(scene, camera);

    say('frame', frame);
    say('render.calls', renderer.info.render.calls);
    say('render.triangles', renderer.info.render.triangles);
    // A standard material compiles one program and reuses it: the count is 1
    // on every frame, and a 2 here would mean the program cache key changed
    // between frames — which is the failure mode a lit scene has and an unlit
    // one does not, because the key includes the light counts.
    say('programs', renderer.info.programs.length);

    gl.readPixels(0, 0, WIDTH, HEIGHT, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    say('pixel.centerIsLitGreen', centerIsLitGreen());
    say('pixel.cornerIsClear', cornerIsClearColor());

    frame = frame + 1;
    if (frame < FRAMES) {
        requestAnimationFrame(tick);
    } else {
        say('done', FRAMES);
    }
}

requestAnimationFrame(tick);
say('mainEnd', 1);
