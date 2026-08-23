// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro Clipmap Terrain API Reference
 * =============================================================================
 *
 * A camera-centred GEOMETRY CLIPMAP: concentric square rings of fixed topology,
 * built once, parked on the camera, displaced on the GPU from a streamed height
 * pyramid. This is the "continuous world from underfoot to the horizon" case.
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

