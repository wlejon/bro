// The textured cube: the same box again, drawn with a MeshBasicMaterial whose
// map is a DataTexture built here in a typed array, and then read back from the
// framebuffer to prove the texels reached the screen.
//
// WHY A DataTexture AND NOT A LOADER. The thing this app exists to exercise is
// the texture UPLOAD path — WebGLTextures allocating a GL texture, setting its
// parameters from the three.js Texture object, and calling texImage2D with a
// typed array — and a loader would put a network, a decoder and an async
// settlement in front of it. `../README.md` records that three.js r160's own
// FileLoader is fetch-based and that the host has no fetch, so a loader here
// would not merely be noise, it would not run. A texture whose pixels the app
// wrote itself has no such dependency: no image file, no network, no decode,
// and the bytes checked on the screen are bytes this file put in an array.
//
// The import, the print rule, and the `APP ` prefix are main.js's — see its
// header.

import { Scene, PerspectiveCamera, Mesh, BoxGeometry, MeshBasicMaterial,
         DataTexture, RGBAFormat, UnsignedByteType, NearestFilter, SRGBColorSpace,
         WebGLRenderer }
    from '../../../../bronze/tests/oracle/threejs/three.module.js';

const FRAMES = 5;
const WIDTH = 320;
const HEIGHT = 160;

function say(label, value) { console.log('APP ' + label + '=' + value); }

// --- The texture ----------------------------------------------------------
// An 8x8 checkerboard of two saturated, opposite colors. Saturated and opposite
// because the check at the bottom is "which channel won", and two colors that
// each own a different channel outright are decidable through the sRGB round
// trip, the rasterizer and the driver's rounding without a tolerance that has
// to be tuned.
const TEX = 8;
const texels = new Uint8Array(TEX * TEX * 4);
for (let y = 0; y < TEX; y++) {
    for (let x = 0; x < TEX; x++) {
        const i = (y * TEX + x) * 4;
        // The parity of x+y is the checker. Integer arithmetic all the way
        // down: no float ever enters the texture, so the bytes below are
        // exactly the bytes the GL is handed.
        const odd = (x + y) % 2 === 1;
        texels[i] = odd ? 255 : 0;        // R
        texels[i + 1] = 0;                // G
        texels[i + 2] = odd ? 0 : 255;    // B
        texels[i + 3] = 255;              // A
    }
}

const texture = new DataTexture(texels, TEX, TEX, RGBAFormat, UnsignedByteType);
// NEAREST both ways and no mipmaps, which is what keeps a texel a texel:
// a filtered checkerboard is a gray, and a gray tells the check below nothing.
// It also means WebGLTextures performs exactly one texImage2D and no
// generateMipmap, so what is being proven is the upload and not the mip chain.
texture.magFilter = NearestFilter;
texture.minFilter = NearestFilter;
texture.generateMipmaps = false;
// The bytes above are sRGB, and saying so is what makes the renderer's output
// transform a round trip rather than a second darkening.
texture.colorSpace = SRGBColorSpace;
// A DataTexture starts dirty, but saying it is the one thing that makes the
// upload happen on the first render rather than never, and it costs nothing to
// be explicit about the flag the whole app depends on.
texture.needsUpdate = true;

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
camera.position.z = 3;   // closer than the other two apps: a bigger cube on the
                         // screen is more texels across, and the row scanned
                         // below has to cross several of them

const geometry = new BoxGeometry(1, 1, 1);
// White base color, so the pixel is the texel: MeshBasicMaterial multiplies
// its color by the map, and any other color would fold a second factor into
// the thing being measured.
const material = new MeshBasicMaterial({ color: 0xffffff, map: texture });
const mesh = new Mesh(geometry, material);
scene.add(mesh);

say('canvas.width', canvas.width);
say('canvas.height', canvas.height);
say('texture.width', texture.image.width);
say('texture.height', texture.image.height);
// The array is the app's own, handed to the texture rather than copied by it —
// an identity a wrapper that quietly reallocated would break.
say('texture.dataIsOurs', texture.image.data === texels);
say('texture.byteLength', texels.length);
say('texture.isDataTexture', texture.isDataTexture === true);
say('material.hasMap', material.map === texture);
say('renderer.isWebGL2', renderer.capabilities.isWebGL2);

// --- Reading the framebuffer back -----------------------------------------
const gl = renderer.getContext();
const pixels = new Uint8Array(WIDTH * HEIGHT * 4);
const CORNER = 0;

// A ROW rather than a point, which is the difference between this predicate and
// the other two apps'. The center of the screen is the center of the cube's
// front face, which is a CORNER of four texels on an even-sized checkerboard —
// exactly the pixel whose color depends on how the rasterizer rounded. Scanning
// the middle row instead asks a question that does not depend on any single
// pixel: does the row cross both colors? A texture that never uploaded gives
// one color (the map read as white, or black), a texture sampled with the wrong
// filter gives one color (the mip average), and a cube that did not draw gives
// neither — three failures, one predicate.
const ROW = HEIGHT / 2;

function scanRow() {
    let sawRed = false;
    let sawBlue = false;
    for (let x = 0; x < WIDTH; x++) {
        const i = (ROW * WIDTH + x) * 4;
        const r = pixels[i], g = pixels[i + 1], b = pixels[i + 2];
        if (r > 128 && b < 64) sawRed = true;
        if (b > 128 && r < 64) sawBlue = true;
    }
    say('pixel.rowSawRed', sawRed);
    say('pixel.rowSawBlue', sawBlue);
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
    // One texture allocated on the first frame and reused on every later one.
    // A number that climbed would mean the renderer was re-uploading a texture
    // it should have cached, which is the leak this app is the one able to see.
    say('memory.textures', renderer.info.memory.textures);

    gl.readPixels(0, 0, WIDTH, HEIGHT, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    scanRow();
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
