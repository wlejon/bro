// The cube app: a real three.js WebGLRenderer drawing a rotating
// MeshBasicMaterial box into a bro-hosted WebGL2 canvas, and then READING THE
// PIXELS BACK to prove it drew.
//
// THE IMPORT is the single-file r160 ESM bundle vendored in the bronze
// checkout — `bronze/tests/oracle/threejs/three.module.js`, byte-for-byte as
// released. That is option 2 in MISSING_MODULES.md beside this file, taken:
// `WebGLRenderer`'s import closure is ~200 files and the bundle is one, and
// nothing about the 28-file tree the milestone compiles changes. The path is
// relative to this file, on the same sibling-checkout assumption
// main_scenegraph.js documents at length — module resolution is the bronze
// CLI's, done at compile time against the filesystem, so a -DBRONZE_DIR
// override moves the C++ and not this specifier.
//
// WHAT IT PRINTS, and the rule behind it: every line is a boolean or an
// integer. Nothing accumulated is pinned, and no pixel VALUE is printed —
// what is printed is a predicate over a pixel, which is what survives the
// several places a color legitimately loses low bits between `0x00ff00` and
// the framebuffer: three.js converts a hex color sRGB->linear on input and
// linear->sRGB on output, the rasterizer interpolates, and the driver picks
// its own rounding. A predicate written far outside that band still fails
// loudly for the things worth catching — a frame that drew nothing, a cube
// drawn in the wrong color, a clear that never happened.
//
// Every line is prefixed `APP ` so the checker can separate it from the
// engine's own log, which shares stdout and is not deterministic.

import { Scene, PerspectiveCamera, Mesh, BoxGeometry, MeshBasicMaterial, WebGLRenderer }
    from '../../../../bronze/tests/oracle/threejs/three.module.js';

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
// and asking for one the context cannot give would be a silent downgrade. It is
// also what makes the pixel checks below meaningful — a resolve would blend the
// cube's edge into the clear color, and an interior pixel is only reliably
// interior when nothing is filtering it.
const renderer = new WebGLRenderer({ canvas: canvas, antialias: false });
renderer.setPixelRatio(1);            // pinned, not window.devicePixelRatio:
                                      // a DPR-scaled buffer would make the
                                      // printed sizes depend on the display
renderer.setSize(WIDTH, HEIGHT, false);  // false: do not write canvas.style
// A dark, unambiguous background. Every channel is far from the cube's, so
// "this pixel is the clear color" and "this pixel is the cube" are decidable
// from the bytes alone rather than from a threshold that has to be tuned.
renderer.setClearColor(0x101010, 1);

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

// --- Reading the framebuffer back -----------------------------------------
// The whole default framebuffer, once per frame, into one buffer allocated
// here rather than per frame: a fresh 200 KB typed array inside the render
// loop would make every frame a collection and say nothing extra.
const gl = renderer.getContext();
const pixels = new Uint8Array(WIDTH * HEIGHT * 4);

// readPixels' origin is the BOTTOM-left, which does not matter for the two
// points sampled here — the center is the center either way, and the corner is
// a corner either way — but it is why these are computed and not guessed.
const CENTER = ((HEIGHT / 2) * WIDTH + (WIDTH / 2)) * 4;
const CORNER = 0;

// The cube is 1x1x1 seen from z=5 through a 75-degree lens, so it covers rather
// less than a fifth of a 320x160 buffer's width and the corner is nowhere near
// it. Both predicates are therefore about the geometry as much as the color:
// a cube drawn at the wrong scale fails the corner check, and a cube not drawn
// at all fails the center one.
function centerIsGreen() {
    const r = pixels[CENTER], g = pixels[CENTER + 1], b = pixels[CENTER + 2];
    return g > 128 && r < 64 && b < 64;
}
function cornerIsClearColor() {
    // 0x101010 is 16 per channel; +-8 covers the sRGB round trip and any
    // driver rounding, and excludes both black and the cube.
    const r = pixels[CORNER], g = pixels[CORNER + 1], b = pixels[CORNER + 2];
    return r > 8 && r < 24 && g > 8 && g < 24 && b > 8 && b < 24;
}

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

    // Immediately after render() and before the host's swap, which is the only
    // window in which the default framebuffer holds this frame's result.
    // WebGLInfo counts what the renderer ASKED for; this is what the driver
    // actually produced, and the two disagreeing is the bug worth catching.
    gl.readPixels(0, 0, WIDTH, HEIGHT, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    say('pixel.centerIsGreen', centerIsGreen());
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
