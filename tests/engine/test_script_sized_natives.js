// Natives that size something from a script's number — src/bronze_host's
// physics, tile-world, math, video and headless bindings, with their engine
// halves (physics_world.cpp, tile_world.cpp, webm/gif_encoder.cpp,
// engine_lifecycle.cpp).
//
// Each case used to hand the number to a plain C++ cast or an unbounded loop:
// maxBodies 1e12 wrapped negative and sized Jolt's tables from the wrap, a
// cloth of 1e5 x 1e5 was a 10^10-vertex allocation, fillTile to INT_MAX spun
// forever (`x <= INT_MAX; ++x` never ends), a tile config with `length: -1`
// resized a vector to ~2^64, gridIndex2D overflowed int, a stride shorter than
// a row read past the frame, and resize(1e9, 1e9) sized every surface to it.

function throwsNamed(fn, name) {
    try { fn(); } catch (e) { return e && e.name === name; }
    return false;
}

// ---- physics ------------------------------------------------------------------
if (typeof Physics === 'object' && Physics.available !== false) {
    const t0 = Date.now();
    const w = Physics.createWorldHandle({ maxBodies: 1e12, contactBufferSize: 1e12 });
    assert(w, 'a world asked for 1e12 bodies is created (clamped), not wrapped');
    const w2 = Physics.createWorldHandle({ maxBodies: -5 });
    assert(w2, 'a negative maxBodies is clamped to 1, not wrapped to 4e9');
    assert(Date.now() - t0 < 20000, 'the clamped worlds were cheap to make');

    Physics.destroyAll();
    Physics.createWorld({ gravity: { x: 0, y: -9.8, z: 0 } });
    assert(throwsNamed(() => Physics.createSoftBody({ cloth: { gridX: 100000, gridZ: 100000 } }),
                       'RangeError'),
           'a cloth past 2^20 vertices is a RangeError');
    const cloth = Physics.createSoftBody({ cloth: { gridX: 4, gridZ: 4 } });
    assert(cloth, 'a small cloth is still made');
    Physics.destroyAll();
}

// ---- bro.math grid ----------------------------------------------------------------
{
    const grid = { origin: { x: 0, y: 0 }, cellSize: 1, width: 2000000000, depth: 10 };
    const idx = bro.math.gridIndex2D(grid, 5, 2000000000);
    assert(idx === 2000000000 * 2000000000 + 5,
           'gridIndex2D is the 64-bit answer past int32 (no overflow): ' + idx);
    assert(bro.math.gridIndex2D({ width: 3 }, NaN, Infinity) === 2147483647 * 3,
           'NaN and Infinity saturate instead of casting undefined behaviour');
}

// ---- tile world ---------------------------------------------------------------
{
    const canvas = document.createElement('canvas');
    canvas.width = 64; canvas.height = 64;
    document.body.appendChild(canvas);
    flush();
    const scene = canvas.getContext('scene');
    if (scene) {
        const bad = scene.createTileWorld({
            width: 8, height: 8, chunkSize: 4,
            palette: { length: -1 },
            tileAtlas: { length: 1e12 },
            atlasPixels: [1, 2, 3, 4], atlasWidth: 4096, atlasHeight: 4096,
        });
        assert(bad && bad.width === 8, 'malformed list lengths are skipped, the world still builds');

        const t0 = Date.now();
        bad.fillTile(0, 0, 2147483647, 2147483647, 1, 0);
        bad.fillElevation(-2147483648, -2147483648, 2147483647, 2147483647, 1);
        bad.fillTint(2147483647, 2147483647, -2147483648, -2147483648, NaN, 1e300, -1, 0.5);
        assert(Date.now() - t0 < 5000, 'fills to INT_MAX are clipped to the grid, took ' + (Date.now() - t0) + 'ms');
        assert(bad.getTile(7, 7, 0) === 1, 'the clipped fill still covered the grid');
        assert(bad.getElevation(3, 3) === 1, 'the clipped elevation fill landed');
        flush();
        advanceTime(16);
        flush();
    }
}

// ---- video encoders -----------------------------------------------------------
if (typeof VideoEncoder === 'function') {
    const os = require('os');
    const path = require('path');
    const base = path.join(os.tmpdir(), 'bro_sized_' + Date.now());
    assert(throwsNamed(() => new GifEncoder({ path: base + '.gif', width: 70000, height: 2 }), 'Error'),
           'a GIF wider than the format can store is refused');
    assert(throwsNamed(() => new VideoEncoder({ path: base + '.webm', width: 20000, height: 20000 }), 'Error'),
           'a WebM past 16384 a side is refused');
    const enc = new GifEncoder({ path: base + '_ok.gif', width: 4, height: 4 });
    assert(throwsNamed(() => enc.addFrameRGBA(new Uint8Array(4 * 4 * 4), 1), 'RangeError'),
           'a stride shorter than one row is a RangeError');
    assert(enc.addFrameRGBA(new Uint8Array(4 * 4 * 4)) === true, 'the default stride still encodes');
    enc.finish();
}

// ---- headless resize ----------------------------------------------------------
{
    resize(1e9, -5);
    flush();
    assert(window.innerWidth === 16384 && window.innerHeight === 1,
           'resize clamps each side to [1, 16384]: ' + window.innerWidth + 'x' + window.innerHeight);
    resize(NaN, Infinity);
    flush();
    assert(window.innerWidth === 1 && window.innerHeight === 16384,
           'NaN is 1 and Infinity is 16384: ' + window.innerWidth + 'x' + window.innerHeight);
    resize(640, 480);
    flush();
    assert(window.innerWidth === 640 && window.innerHeight === 480, 'an ordinary resize is unchanged');
}

console.log('script-sized natives: all checks passed');
