// ── Classes & Interfaces ─────────────────────────────────────────────────────

class World {

  /**
   * World generation random seed.
   * @readonly
   * @type {number}
   */
  seed;

  /**
   * Model weights filesystem directory.
   * @readonly
   * @type {string}
   */
  directory;

  /**
   * Fine resolution cell size in metres.
   * @readonly
   * @type {number}
   */
  cellSize;

  /**
   * Intermediate latent stage cell size in metres.
   * @readonly
   * @type {number}
   */
  latentCellSize;

  /**
   * Coarse overview stage cell size in metres.
   * @readonly
   * @type {number}
   */
  coarseCellSize;

  /**
   * Asynchronously compute fine elevation in metres for cell region [i1, j1, i2, j2).
   *
   * @param {number} i1
   * @param {number} j1
   * @param {number} i2
   * @param {number} j2
   * @param {Object} [opts]
   * @returns {AsyncHandle}
   */
  elevation(i1, j1, i2, j2, opts) {}

  /**
   * Synchronously compute fine elevation grid in metres.
   *
   * @param {number} i1
   * @param {number} j1
   * @param {number} i2
   * @param {number} j2
   * @param {Object} [opts]
   * @returns {Object}
   */
  elevationSync(i1, j1, i2, j2, opts) {}

  /**
   * Synchronously compute coarse terrain elevation channel at 7.68 km resolution.
   *
   * @param {number} i1
   * @param {number} j1
   * @param {number} i2
   * @param {number} j2
   * @param {Object} [opts]
   * @returns {Object}
   */
  coarse(i1, j1, i2, j2, opts) {}

  /**
   * Asynchronously compute intermediate DAG stage buffer.
   *
   * @param {string} name
   * @param {number} i1
   * @param {number} j1
   * @param {number} i2
   * @param {number} j2
   * @param {Object} [opts]
   * @returns {AsyncHandle}
   */
  stage(name, i1, j1, i2, j2, opts) {}

  /**
   * Synchronously compute intermediate DAG stage buffer.
   *
   * @param {string} name
   * @param {number} i1
   * @param {number} j1
   * @param {number} i2
   * @param {number} j2
   * @returns {Object}
   */
  stageSync(name, i1, j1, i2, j2) {}

  /**
   * Invalidate cached memoized tiles.
   */
  clearCache() {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro.worldgen — learned neural terrain diffusion pipeline
 * =============================================================================
 *
 * Deterministic, infinite neural world generation using brodiffusion WorldPipeline.
 * Generates continuous elevation grids in metres from multi-scale UNets with coherent
 * drainage networks, hydrological ridges, and coarse/latent stage intermediate diagnostics.
 * @example
 * bro.worldgen.loadWorld("weights/terrain", {
 *     seed: 42,
 *     onReady: (world) => {
 *       const tile = world.elevationSync(0, 0, 256, 256);
 *       console.log(`Generated ${tile.width}x${tile.height} tile with cell size ${tile.cellSize}m`);
 *     }
 *   });
 */
/**
 * Initialize brotensor acceleration runtime for neural terrain generation.
 */
bro.worldgen.init = function() {};

/**
 * Load a neural terrain world pipeline asynchronously from a model directory.
 *
 * @param {string} dir - Checkpoint model directory path.
 * @param {Object} [opts] - Seed and callback hooks (onReady, onError).
 * @returns {AsyncHandle} Async job handle.
 */
bro.worldgen.loadWorld = function(dir, opts) {};

