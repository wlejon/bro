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
// neighbours, so steps and valleys read with contact shading.
//
// Surface appearance is one of two modes:
//   • Palette   — `palette` gives an RGBA per ground-tile id (flat colour).
//   • Atlas     — `atlas` (a tileset image) maps each ground-tile id to an
//                 atlas cell on the top faces, with a `cliffCell` on the sides.
//                 The AO term rides in per-vertex shading, so steps still read.
// (Autotiling, multi-layer compositing, animation, object layers and ray→cell
// picking build on this foundation.)

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
   * @example
   *   // Tileset-atlas texturing: a 4-wide atlas; tile id N samples cell N.
   *   const world = scene.createTileWorld({
   *     width: 32, height: 32, heightStep: 0.5,
   *     atlas: 'tiles.png',          // app-relative image path
   *     atlasColumns: 4, atlasRows: 4,
   *     cliffCell: 2,                // dirt-side cell for all cliffs
   *     atlasInset: 0.5,             // half-texel inset, fights cell bleeding
   *   });
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
   *        (4 floats each); index 0 is empty. Absent → flat grey. Ignored when
   *        an atlas is set.
   *
   * Tileset atlas (optional — replaces palette colour with a texture):
   * @param {string} [opts.atlas] - app-relative path to a tileset image. The
   *        atlas is a regular grid of cells (see atlasColumns/atlasRows).
   * @param {Uint8Array} [opts.atlasPixels] - raw RGBA8 alternative to `atlas`;
   *        requires `atlasWidth` + `atlasHeight`.
   * @param {number} [opts.atlasWidth] - atlas pixel width (with atlasPixels)
   * @param {number} [opts.atlasHeight] - atlas pixel height (with atlasPixels)
   * @param {number} [opts.atlasColumns=1] - atlas cells per row
   * @param {number} [opts.atlasRows=1] - atlas cell rows
   * @param {number[]} [opts.tileAtlas] - ground tile id → atlas cell index.
   *        Absent → cell index equals the tile id (id 1 draws cell 1).
   * @param {number} [opts.cliffCell=-1] - atlas cell for vertical cliff faces;
   *        -1 reuses each column's top cell.
   * @param {number} [opts.atlasInset=0] - UV inset per cell edge in texels, to
   *        fight bilinear/mip bleeding between neighbouring cells.
   *
   * Autotiling (optional; requires an atlas). Each rule turns a field of one
   * tile id into bordered/edge art: a cell's TOP-face atlas cell is chosen from
   * a neighbour bitmask via the `cells` variant table instead of the flat per-id
   * cell. Cliff faces are unaffected. Edits restyle the 3x3 neighbourhood, so
   * borders update as you paint.
   * @param {Object[]} [opts.autotiles] - array of rules:
   * @param {number}   opts.autotiles[].id   - ground tile id the rule applies to
   * @param {string}   opts.autotiles[].mode - "edge" (4-bit edge mask → 16
   *        variants, E,N,W,S = bits 0..3), "blob47" (8-neighbour blob → 47
   *        variants), or "wang" (4-bit corner mask → 16, NE,SE,SW,NW = bits 0..3)
   * @param {number}   [opts.autotiles[].layer=0] - layer the rule + family run
   *        on (use the overlay layer index for autotiled decals)
   * @param {string}   [opts.autotiles[].family="id"] - which neighbours "join":
   *        "id" (same tile id) or "nonEmpty" (any non-empty cell on that layer)
   * @param {number[]} opts.autotiles[].cells - variant index → atlas cell index
   *        (length 16 for edge/wang, 47 for blob47)
   *
   * Multi-layer overlays (optional; require an atlas). Name more than one layer
   * (`layers`) and tiles placed on layers >= 1 render as atlas-textured DECAL
   * quads floating just above each cell's ground top face, drawn bottom-up by
   * layer index — roads, crops, decals over the base tile. `overlays` styles
   * them, aligned to `layers` (index 0 = ground, ignored):
   * @param {Object[]} [opts.overlays] - per-layer style array
   * @param {number}   [opts.overlays[].opacity=1] - <1 makes the whole layer
   *        translucent (engine "over" blend)
   * @param {number}   [opts.overlays[].alphaCutoff=0] - >0 cuts decal shapes
   *        from the atlas alpha channel
   *
   * Animated tiles (optional; require an atlas). A tile id cycles through a
   * sequence of atlas cells over time — flowing water, swaying crops, torch
   * flicker. Drive it from your frame loop with world.advance(dtMs); only
   * chunks holding animated tiles remesh, and only when the frame changes.
   * @param {Object[]} [opts.animations] - array of animations:
   * @param {number}   opts.animations[].id     - tile id to animate
   * @param {number}   [opts.animations[].fps=4] - frames per second
   * @param {number[]} opts.animations[].frames - atlas cell per frame
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

  /**
   * Set a per-cell RGB(A) tint (0..1), multiplied into the cell's ground and
   * overlay colour. Default white = no tint. Alpha is stored but only bites
   * with a layer's alphaCutoff; for translucency use an overlay layer opacity.
   */
  setTint(x, y, r, g, b, a = 1) {}

  /** Fill an inclusive rectangle with a per-cell tint. */
  fillTint(x0, y0, x1, y1, r, g, b, a = 1) {}

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

  // --- Picking / collision / navigation -------------------------------------

  /**
   * Cast a world-space ray against the tile surface — the per-cell top faces
   * AND the cliff sides between them — front-to-back with elevation occlusion
   * (a near plateau hides the ground behind it). Analytic grid traversal; no
   * BVH, so it works the instant the grid is authored, before any mesh build.
   *
   * Pair it with scene.unprojectLocal(screenX, screenY) for click picking:
   *
   *     const r = scene.unprojectLocal(mouseX, mouseY);   // { origin, dir }
   *     const hit = world.raycastCell(r.origin, r.dir);
   *     if (hit) paintAt(hit.x, hit.y);
   *
   * @param {number[]} origin   [x, y, z] world-space ray origin
   * @param {number[]} dir      [x, y, z] ray direction (need not be normalized)
   * @param {number} [maxDist]  max world distance to search (default 1e6)
   * @returns {{cell:number[], x:number, y:number, point:number[],
   *            distance:number, side:boolean} | null}
   *          `cell`/`x`/`y` is the hit cell, `point` the world hit position,
   *          `side` true when a cliff face was struck (not a flat top). null on
   *          a miss.
   */
  raycastCell(origin, dir, maxDist) {}

  /**
   * World Y of the top surface of the cell under a world XZ position — the
   * ground height an agent stands at. Returns a number, or null when that XZ is
   * off the grid or over an empty (hole) cell.
   */
  sampleHeight(worldX, worldZ) {}

  /**
   * Whether cell (x, y) is walkable: it carries ground (non-empty tile on layer
   * 0) and none of `blockMask`'s flag bits are set on it. blockMask 0 (default)
   * => every solid cell is walkable. This is the predicate the nav-grid export
   * uses per cell.
   */
  isWalkable(x, y, blockMask) {}

  /**
   * Build a fresh bro.ai.game nav grid sized to this world (bounds = the grid's
   * world extent, cellSize = the tile cellSize, so cells map 1:1) with every
   * non-walkable cell stamped as a blocked obstacle. Returns the AINavGrid, so
   * steering agents and findPath() route over the tiles immediately:
   *
   *     const nav = world.toNavGrid({ blockMask: WATER });
   *     const path = nav.findPath(ax, az, bx, bz);   // routes around water
   *
   * @param {Object} [opts]
   * @param {number} [opts.blockMask=0]  flag bits that mark a cell impassable
   * @param {number} [opts.padding=0]    extra clearance around blocked cells
   * @returns {AINavGrid}
   */
  toNavGrid(opts) {}

  /**
   * Stamp this world's non-walkable cells into an EXISTING bro.ai.game nav grid
   * (additive — leaves its other obstacles intact). Returns the number of cells
   * blocked. Use when the nav grid spans more than just the tiles.
   * @param {AINavGrid} navGrid
   * @param {Object} [opts] { blockMask=0, padding=0 }
   */
  syncNavGrid(navGrid, opts) {}

  // --- Placement / rebuild / teardown ---------------------------------------

  /** Move the whole map's root node to a world position. */
  setOrigin(x, y, z) {}

  /**
   * Advance animated tiles by `dtMs` milliseconds (call from your frame loop).
   * Remeshes only the chunks that contain animated tiles, and only when a frame
   * index actually changed. Returns true if anything was remeshed.
   */
  advance(dtMs) {}

  // --- Objects / entities ----------------------------------------------------
  // Real 3D props placed on cells, GPU-instanced: one shared mesh per "kind"
  // drawn across all its placements in a single draw call, anchored to each
  // cell's top-surface centre. Author with addObject(), then rebuild().

  /**
   * Register a prop kind from a mesh + material. Returns the kind index (-1 on
   * failure). `mesh` is a bro.mesh object (e.g. Mesh.cone(...)).
   * @param {Mesh} mesh
   * @param {Object} [style]
   * @param {number[]} [style.color=[1,1,1,1]] - base colour (RGBA)
   * @param {number} [style.roughness=0.8]
   * @param {number} [style.metallic=0]
   * @param {boolean} [style.doubleSided=false] - for leaf-card / billboard props
   * @param {number} [style.alphaCutoff=0] - >0 cuts cut-out textures
   * @param {boolean} [style.castsShadow=true]
   * @param {number} [style.atlasColumns=1] - texture atlas grid (variant picks a cell)
   * @param {number} [style.atlasRows=1]
   * @param {string} [style.texture] - app-relative baseColor texture path
   * @returns {number} kind index
   */
  addObjectKind(mesh, style) {}

  /**
   * Place an instance of `kind` on cell (x, y). Returns the instance index, or
   * -1 on a bad kind/cell. Mark for rebuild; flushed by rebuild().
   * @param {number} kind
   * @param {number} x
   * @param {number} y
   * @param {Object} [opts]
   * @param {number} [opts.yaw=0] - rotation about Y (radians)
   * @param {number} [opts.scale=1] - uniform scale
   * @param {number} [opts.yOffset=0] - lift above the cell top
   * @param {number} [opts.offsetX=0] - sub-cell offset in cell units (-0.5..0.5)
   * @param {number} [opts.offsetZ=0]
   * @param {number} [opts.variant=0] - atlas cell when the kind has an atlas
   * @param {number[]} [opts.color=[1,1,1,1]] - per-instance tint
   * @returns {number} instance index
   */
  addObject(kind, x, y, opts) {}

  /** Remove all placements of `kind` (or every kind when kind is omitted/<0). */
  clearObjects(kind) {}

  /** Number of placed instances of `kind`. */
  objectCount(kind) {}

  /** Flush object-kind instance buffers changed since the last rebuild. */
  rebuildObjects() {}

  /** Remesh only the chunks touched since the last rebuild. */
  rebuild() {}

  /** Remesh every chunk (use after mutating the grid out-of-band). */
  rebuildAll() {}

  /** Reconfigure from scratch — rebuilds the grid and all chunks. */
  configure(opts) {}

  /**
   * Serialize the grid (tiles/elevation/flags — not rendering config like
   * palette/atlas/autotiles) to a versioned binary blob. Round-trips exactly
   * with load(): save() -> load(bytes) restores the same grid contents.
   * @returns {Uint8Array}
   */
  save() {}

  /**
   * Replace the grid from bytes produced by save(). Dimensions/topology/layers
   * are taken from the saved data (may differ from the current config);
   * rendering config (cellSize, atlas, autotiles, overlays, animations, ...)
   * is preserved. Remeshes every chunk on success.
   * @param {Uint8Array} bytes
   * @returns {boolean} false if `bytes` is corrupt or an unrecognized format
   */
  load(bytes) {}

  /** Destroy all nodes and release the grid. */
  destroy() {}

  // --- Stats (read-only) -----------------------------------------------------

  get width() {}
  get height() {}
  get chunkCount() {}
  get vertexCount() {}
  get triangleCount() {}
}
