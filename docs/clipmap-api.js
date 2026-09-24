// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro Clipmap Terrain API Reference
 * =============================================================================
 *
 * A camera-centred GEOMETRY CLIPMAP: concentric square rings of fixed topology,
 * built once, parked on the camera, displaced on the GPU from a streamed height
 * pyramid. This is the "continuous world from underfoot to the horizon" case.
 *
 * ── The layer stacks in GLSL (for an app composing its own chunk) ──────────
 *
 * `shaderSource('vertex' | 'fragment')` returns the GLSL the terrain hands to
 * its mesh. An app replacing a chunk (a material of its own) reads the layer
 * stacks through these names:
 *
 *   uniform sampler2DArray u_heights;       // slice i = height layer i (R, mipmapped)
 *   uniform vec2           u_heightsSize;   // every slice's (width, height), texels
 *   uniform vec3 u_l<i>a;  uniform vec2 u_l<i>b;   // layer i: (originX, originZ,
 *                                           //   metresPerCell), (width, height)
 *   uniform sampler2DArray u_surfaces;      // slice i = surface layer i (RGBA, level 0)
 *   uniform vec2           u_surfacesSize;
 *   uniform vec3 u_surfA / u_surf<i>A;  uniform vec2 u_surfB / u_surf<i>B;
 *
 * Both stacks are TEXTURE ARRAYS — two sampler units for the whole terrain,
 * however many layers it holds, which is what keeps it inside a fragment
 * stage's 16-sampler floor (macOS's GL 4.1 core; six sampler2Ds per stack did
 * not link there). They replaced the per-layer `sampler2D u_h0..u_h5` and
 * `u_surface, u_surface1..u_surface5`, which no longer exist; the per-layer
 * `u_l<i>a/b`, `u_surf*A/B`, `u_layerCount`, `u_surfaceCount` and
 * `u_surfPresent` are unchanged.
 *
 * A slice is the size of the stack's largest layer, and each layer sits in its
 * slice's low corner at its own size, so a layer's uv maps into the array as
 * `uv * size / u_heightsSize`. Prefer the terrain's own readers, which also
 * carry each layer's edge and wrap handling:
 *   cmLayer(slice, a, b, worldXZ, cDesired, wrapX, out w)   one height layer
 *   cmHeight(worldXZ, cDesired)                             the blended stack
 *   cmSurface(worldXZ, cDesired, out present)               blended control channels
 *   cmCubicTap(u_surfaces, slice, uv, size, u_surfacesSize) C2 read, level 0
 *   cmCubicTapLevel(u_heights, slice, uv, size, u_heightsSize, level, wrapX)
 * (the last two exist only with `cubicSurface` / `cubicHeight` on).
 *
 * Memory: every slice is the largest layer's size (plus, for a periodic layer
 * narrower than that, an eighth of its width as wrap padding). A stack of
 * equally-sized layers — the usual pyramid — costs exactly what it holds; a
 * small window beside a large chart pays for a large slice.
 *
 * @typedef {Object} ClipmapMaterialComponent
 * @property {Array<number>} [albedo]
 * @property {number} [roughness]
 */

/**
 * @typedef {Object} ClipmapMaterialsOptions
 * @property {ClipmapMaterialComponent} [rock]
 * @property {ClipmapMaterialComponent} [snow]
 * @property {ClipmapMaterialComponent} [sand]
 * @property {ClipmapMaterialComponent} [grass]
 */

/**
 * @typedef {Object} ClipmapForestOptions
 * @property {Array<number>} [albedo]
 * @property {number} [strength]
 */

/**
 * @typedef {Object} ClipmapDetailOptions
 * @property {number} [wavelength]
 * @property {number} [relief]
 * @property {number} [gain]
 * @property {number} [octaves]
 */

/**
 * @typedef {Object} ClipmapHeightLayerOptions
 * @property {Float32Array} [data]
 * @property {number} [width]
 * @property {number} [height]
 * @property {number} [originX]
 * @property {number} [originZ]
 * @property {number} [metresPerCell]
 * @property {boolean} [wrapX]
 * @property {boolean} [bandLimited]
 */

/**
 * @typedef {Object} ClipmapSurfaceLayerOptions
 * @property {Float32Array} [data]
 * @property {number} [width]
 * @property {number} [height]
 * @property {number} [originX]
 * @property {number} [originZ]
 * @property {number} [metresPerCell]
 * @property {number} [components]
 */

/**
 * `levels` (default 10) is clamped to [1, 20]. `resolution` (default 128) is
 * quads per level per axis, rounded down to a multiple of 4 and clamped to
 * [4, 2048]. A `cellSize` that is not positive is 1. `detailOctaves`
 * (default 7, also `setDetail({octaves})`) is clamped to [0, 8].
 *
 * Detail synthesis below the data floor: `detailWavelength` (metres, default
 * 48) is the coarsest synthesised octave. `detailRelief` (default 0.35) is a
 * unitless SLOPE, not a height: each octave's amplitude is detailRelief x
 * that octave's wavelength x the ground's own slope, so it needs no retuning
 * when heightScale changes and adds nothing to flat ground. Keep it around
 * 0.1-1; a value like 18 read as "metres" makes km-high walls.
 * `detailGain` (default 1) tilts the octaves: below 1 smooths the fine end,
 * above 1 sharpens it.
 * @typedef {Object} ClipmapTerrainConfig
 * @property {number} [levels]
 * @property {number} [resolution]
 * @property {number} [cellSize]
 * @property {number} [heightScale]
 * @property {number} [seaLevel]
 * @property {number} [snowLine]
 * @property {number} [maxCellScale]
 * @property {number} [planetRadius]
 * @property {boolean} [layerFade]
 * @property {boolean} [coverageFloor]
 * @property {boolean} [cubicSurface]
 * @property {boolean} [cubicHeight]
 * @property {number} [detailWavelength]
 * @property {number} [detailRelief]
 * @property {number} [detailGain]
 * @property {number} [detailOctaves]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

class ClipmapTerrain {

  /**
   * @readonly
   * @type {SceneNode|null}
   */
  node;

  /**
   * @readonly
   * @type {number}
   */
  levels;

  /**
   * @readonly
   * @type {number}
   */
  resolution;

  /**
   * @readonly
   * @type {number}
   */
  cellSize;

  /**
   * @readonly
   * @type {number}
   */
  layerCount;

  /**
   * @readonly
   * @type {number}
   */
  triangleCount;

  /**
   * @readonly
   * @type {number}
   */
  vertexCount;

  /**
   * @readonly
   * @type {number}
   */
  farDistance;

  /**
   * @readonly
   * @type {number}
   */
  cellScale;

  /**
   * @readonly
   * @type {number}
   */
  planetRadius;

  /**
   * @param {number} index
   * @param {ClipmapHeightLayerOptions|null} desc
   * @returns {ClipmapTerrain}
   */
  setHeightLayer(index, desc) {}

  /**
   * @param {number} m
   * @returns {ClipmapTerrain}
   */
  setSnowLine(m) {}

  /**
   * @param {number|null} x
   * @param {number} [z]
   * @returns {ClipmapTerrain}
   */
  setChartCenter(x, z) {}

  /**
   * @param {ClipmapDetailOptions} desc
   * @returns {ClipmapTerrain}
   */
  setDetail(desc) {}

  /**
   * @param {ClipmapMaterialsOptions} desc
   * @returns {ClipmapTerrain}
   */
  setMaterials(desc) {}

  /**
   * @param {ClipmapForestOptions} desc
   * @returns {ClipmapTerrain}
   */
  setForest(desc) {}

  /**
   * @param {*} indexOrDesc
   * @param {ClipmapSurfaceLayerOptions|null} [desc]
   * @returns {ClipmapTerrain}
   */
  setSurfaceLayer(indexOrDesc, desc) {}

  /**
   * @param {number} camX
   * @param {number} camY
   * @param {number} camZ
   * @returns {ClipmapTerrain}
   */
  update(camX, camY, camZ) {}

  /**
   * @param {string} stage
   * @returns {string}
   */
  shaderSource(stage) {}

  /**
   * @param {number} x
   * @param {number} z
   * @returns {number}
   */
  elevationAt(x, z) {}

  /**
   * @param {number} x
   * @param {number} z
   * @returns {number}
   */
  renderedElevationAt(x, z) {}

  /**
   * @param {number} eyeAboveSeaLevel
   * @returns {number}
   */
  coverageDistance(eyeAboveSeaLevel) {}

  /**
   * @param {number} eyeAboveSeaLevel
   * @returns {number}
   */
  horizonDistance(eyeAboveSeaLevel) {}

  destroy() {}

}

