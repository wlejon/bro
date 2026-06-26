// =============================================================================
// bro TileWorld API Reference  —  scene.createTileWorld
// =============================================================================
//
// A TileWorld renders a `bro::tile` grid as chunked, flat-topped tile geometry
// inside the 3D scene. It is the scene-side bridge over the pure tile core:
// authoring/query happen on a square (hex planned) grid of cells, each carrying
// a tile id per named layer, a signed elevation level, and a 32-bit flag mask;
// the world meshes that grid into chunk MeshNodes parented under one root node.
//
// Because tiles are real 3D geometry, "2D top-down", "isometric", and "3D" are
// just camera choices (see scene.setCamera — orthographic + tilt gives iso) over
// the same data. Flat maps are simply elevation-0 fields.
//
// Each solid cell (non-empty tile on the ground layer, index 0) emits a flat top
// quad at its elevation height, plus vertical CLIFF quads on any edge where the
// neighbour sits lower — or is empty / off the map, dropping to `baseLevel`.
// Top-face corners are darkened by an ambient-occlusion term from taller
// neighbours, so steps and valleys read with contact shading. Per-cell colour
// comes from `palette` indexed by the ground-layer tile id. (Tileset-atlas
// texturing, autotiling, multi-layer compositing, animation, object layers and
// ray→cell picking build on this foundation.)

class SceneGraph {
  /**
   * Create a TileWorld and add its root node to the scene.
   *
   * @example
   *   const palette = new Float32Array([
   *     0,0,0,1,            // 0 = empty (unused)
   *     0.42,0.70,0.27,1,   // 1 = grass
   *     0.55,0.38,0.20,1,   // 2 = dirt
   *     0.56,0.56,0.62,1,   // 3 = stone
   *   ]);
   *   const world = scene.createTileWorld({
   *     width: 32, height: 32,
   *     cellSize: 1.0, heightStep: 0.5, chunkSize: 16,
   *     palette,
   *   });
   *   world.fillTile(0, 0, 31, 31, 1);          // grass everywhere
   *   world.fillTile(4, 4, 11, 11, 2);          // a dirt patch
   *   world.fillElevation(4, 4, 11, 11, 3);     // raised 3 levels
   *   world.rebuild();                          // remesh dirty chunks
   *
   * @param {Object} opts
   * @param {number} [opts.width=16]  - grid size in cells (X)
   * @param {number} [opts.height=16] - grid size in cells (Z)
   * @param {string} [opts.topology="square"] - "square" or "hex" (hex WIP)
   * @param {string[]} [opts.layers=["ground"]] - named tile layers; layer 0 is
   *                                               the ground/solidity layer
   * @param {number} [opts.cellSize=1.0]   - world units per cell along X/Z
   * @param {number} [opts.heightStep=0.5] - world units per elevation level (Y)
   * @param {number} [opts.chunkSize=16]   - cells per chunk edge (remesh granularity)
   * @param {number} [opts.baseLevel=0]    - elevation the map-edge / hole skirt drops to
   * @param {number} [opts.aoStrength=0.45]- corner ambient occlusion, 0..1
   * @param {number[]} [opts.origin=[0,0,0]] - world position of the grid origin
   * @param {Float32Array|number[]} [opts.palette] - RGBA per ground-tile id
   *        (4 floats each); index 0 is empty. Absent → flat grey.
   * @returns {TileWorld}
   */
  createTileWorld(opts) {}
}

class TileWorld {
  // --- Authoring (marks affected chunks dirty; call rebuild() to remesh) -----

  /** Set the tile id at (x, y) on `layer` (default 0). 0 = empty. */
  setTile(x, y, id, layer) {}

  /** Set the signed elevation level at (x, y). */
  setElevation(x, y, level) {}

  /** Set or clear flag bits at (x, y). `bit` is a bitmask; geometry unaffected. */
  setFlag(x, y, bit, on) {}

  /** Fill an inclusive rectangle of tiles on `layer` (default 0). */
  fillTile(x0, y0, x1, y1, id, layer) {}

  /** Fill an inclusive rectangle of elevation levels. */
  fillElevation(x0, y0, x1, y1, level) {}

  // --- Query -----------------------------------------------------------------

  /** Tile id at (x, y) on `layer` (default 0). 0 if empty / out of bounds. */
  getTile(x, y, layer) {}

  /** Elevation level at (x, y). 0 if out of bounds. */
  getElevation(x, y) {}

  /** Whether all bits of `bit` are set in the flags at (x, y). */
  hasFlag(x, y, bit) {}

  /**
   * Map a world XZ position to a grid cell (flat top-down projection; ignores
   * elevation). Returns { x, y } or null if outside the grid.
   */
  worldToCell(worldX, worldZ) {}

  // --- Placement / rebuild / teardown ---------------------------------------

  /** Move the whole map's root node to a world position. */
  setOrigin(x, y, z) {}

  /** Remesh only the chunks touched since the last rebuild. */
  rebuild() {}

  /** Remesh every chunk (use after mutating the grid out-of-band). */
  rebuildAll() {}

  /** Reconfigure from scratch — rebuilds the grid and all chunks. */
  configure(opts) {}

  /** Destroy all nodes and release the grid. */
  destroy() {}

  // --- Stats (read-only) -----------------------------------------------------

  get width() {}
  get height() {}
  get chunkCount() {}
  get vertexCount() {}
  get triangleCount() {}
}
