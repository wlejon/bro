// =============================================================================
// bro Worldgen API Reference
// =============================================================================
//
// `bro.worldgen`: learned terrain generation. A port of xandergos/terrain-
// diffusion (MIT) running on brodiffusion's WorldPipeline.
//
// This is a REPLACEMENT for noise, not a filter on top of it. Three magnitude-
// preserving UNets (a 2.8M coarse net, a 253.7M latent net, a 27.9M decoder)
// produce an infinite, deterministic, randomly accessible world with coherent
// drainage networks, coastlines, continental shelves and mountain ranges,
// structure that FBm cannot produce at any parameter setting, because FBm has
// no notion of water flowing downhill.
//
// Output is ELEVATION IN METRES, one cell per 30 m, on a realistic scale:
// roughly -1550 m ocean floor to +2850 m peaks.
//
// The world is a pure function of (seed, position). Any region can be read in
// any order, and reads agree exactly where they overlap (see "Tiles compose"
// below), so there is no generation order to preserve and nothing to persist.
//
//   bro.worldgen.available            // false in builds without BRO_WITH_DIFFUSION
//   bro.worldgen.init()               // initialise the tensor runtime (once)
//   bro.worldgen.loadWorld(dir, opts) // -> AsyncHandle
//
// Requires a converted checkpoint directory (config.json + coarse/base/decoder
// safetensors + synthetic_map_stats.json), the output of brodiffusion's
// scripts/convert-terrain-diffusion.py. Paths resolve relative to the app
// directory, as with the other model loaders.
// =============================================================================


// -----------------------------------------------------------------------------
// bro.worldgen.loadWorld(dir, opts) -> AsyncHandle
// -----------------------------------------------------------------------------
//
// Loads the three networks on a background thread. ~2 s, paid once.
//
//   bro.worldgen.init();
//   bro.worldgen.loadWorld('weights/terrain-diffusion-30m-bro', {
//       seed: 42,                       // uint64; the world's identity
//       onReady: (world) => { ... },
//       onError: (msg)   => { ... },
//   });
//
// The returned AsyncHandle has .cancel(). Loading is monolithic, so cancelling
// stops the result being delivered rather than stopping the work.


class World {

    /**
     * Elevation in metres over the half-open cell region [i1, i2) x [j1, j2),
     * generated on a background thread so the frame loop keeps running.
     *
     *   world.elevation(i1, j1, i2, j2, {
     *       margin: 8,                       // see "Tiles compose" below
     *       onDone: ({ width, height, cellSize, data }) => { ... },
     *       onError: (msg) => { ... },
     *   });
     *
     * `data` is a row-major Float32Array of height*width metres; index (z, x)
     * lives at `data[z * width + x]`. `cellSize` is metres per cell (30).
     *
     * Bounds are in CELLS and may be negative: the origin is just the seed's
     * reference point, not a corner of anything. i is the north-south axis
     * (rows), j is west-east (columns).
     *
     * SIZE THE REQUEST GENEROUSLY. Cost is dominated by fixed overhead, not by
     * area: measured on one machine: 256x256 takes ~2.3 s, 1024x1024 ~4.6 s,
     * 2048x2048 ~14.6 s. That makes the largest request about 25x cheaper per
     * cell than the smallest. Ask for one big tile and sample it; do NOT issue
     * one request per consumer chunk.
     *
     * For scale: a 1024x1024 tile is 30.7 km across. Flying it at 100 m/s takes
     * five minutes, so generation runs far ahead of any camera.
     *
     * ONE REQUEST AT A TIME per world. The pipeline memoises tiles and that
     * cache is not thread-safe, so calling this while `world.generating` is
     * true throws rather than racing. Queue in JS, or load a second world.
     *
     * @returns {AsyncHandle} with .cancel(): monolithic, so cancelling drops
     *                        the result rather than stopping the work.
     */
    elevation(i1, j1, i2, j2, opts) {}

    /**
     * The same, but blocking on the calling thread. For headless tests and
     * Workers. Returns the result object directly; throws on failure.
     *
     * Do not call this on the main thread of a windowed app, a multi-second
     * request will freeze the frame.
     */
    elevationSync(i1, j1, i2, j2, opts) {}

    /** Drop every cached tile. Purely an optimisation, the world is a pure
     *  function of (seed, position), so this changes timing and nothing else. */
    clearCache() {}

    get seed()       {}   // number (lossy above 2^53; echoed, not round-tripped)
    get cellSize()   {}   // metres per cell, 30 for the 30m checkpoint
    get generating() {}   // true while an async request is in flight
}


// -----------------------------------------------------------------------------
// Tiles compose exactly, why `margin` exists
// -----------------------------------------------------------------------------
//
// Reconstruction pads outward and crops, because the Gaussian blur and the
// resampling both reach beyond the requested edge. That pad is finite, so the
// outermost cells of any request are still built from truncated support.
//
// Measured on the 30 m checkpoint: a region read alone differs from the same
// region read inside a larger request by 0.18 m at the very edge, decaying
// through 0.13 / 0.08 / 0.03 m and reaching BIT-EXACTLY ZERO four cells in.
//
// So both elevation forms over-request by `margin` cells (default 8, double the
// measured depth) and crop the result. Independently generated neighbouring
// tiles then agree exactly where they meet, and a chunked consumer shows no
// seam. This is on by default because the failure mode is otherwise silent:
// terrain that looks entirely correct with a small lip at every tile boundary.
//
// Pass `margin: 0` for the pipeline's raw output. That is what the C++ parity
// gates compare against, and it is the only reason to.


// -----------------------------------------------------------------------------
// Feeding terrain, the intended shape
// -----------------------------------------------------------------------------
//
// Generate large tiles asynchronously, cache them, and sample per chunk. The
// per-chunk step is then a cheap array read, which is what keeps chunk streaming
// synchronous and simple (see terrain-api.js `heightSource`).
//
//   const TILE = 1024;                      // 30.7 km per tile
//   const tiles = new Map();
//
//   function tileKey(ti, tj) { return ti + ',' + tj; }
//
//   function ensureTile(ti, tj) {
//       const k = tileKey(ti, tj);
//       if (tiles.has(k) || world.generating) return;
//       tiles.set(k, 'pending');
//       world.elevation(ti * TILE, tj * TILE, (ti + 1) * TILE, (tj + 1) * TILE, {
//           onDone: (r) => tiles.set(k, r),
//           onError: () => tiles.delete(k),      // retry on a later frame
//       });
//   }
//
// At 30 m/cell the model resolves geology, not footsteps: on a valley floor
// adjacent cells differ by ~0.4 m (walks smooth), but on a mountainside the
// median step is ~6.7 m and can reach 26 m, which reads as faceting up close.
// Layering FBm detail on top of the diffusion macro-relief is the intended fix,
// returning null from a heightSource falls back to the built-in noise, and the
// two can be summed.
