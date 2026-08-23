// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro Tile World API Reference
 * =============================================================================
 *
 * Infinite chunked 2D/2.5D/3D tile maps, voxel extraction, pathfinding, autotiling, and navigation grid integration.
 * @typedef {Object} TilePickResult
 * @property {number} [tileX]
 * @property {number} [tileY]
 * @property {number} [layer]
 * @property {number} [tileId]
 * @property {Array<number>} [worldPosition]
 */

/**
 * @typedef {Object} TilePathResult
 * @property {boolean} [reachable]
 * @property {Array<Array<number>>} [path]
 * @property {number} [totalCost]
 */

/**
 * @typedef {Object} TileRegionResult
 * @property {number} [regionId]
 * @property {Array<Array<number>>} [tiles]
 * @property {number} [area]
 */

/**
 * @typedef {Object} TileAutotileRule
 * @property {number} [matchTile]
 * @property {number} [outputTile]
 * @property {number} [mask]
 */

/**
 * @typedef {Object} TileLayerConfig
 * @property {string} [name]
 * @property {number} [zIndex]
 * @property {boolean} [collision]
 * @property {number} [opacity]
 * @property {boolean} [visible]
 */

/**
 * @typedef {Object} TileChunkInfo
 * @property {number} [chunkX]
 * @property {number} [chunkY]
 * @property {number} [tileCount]
 * @property {boolean} [dirty]
 */

/**
 * @typedef {Object} TileVoxelExtractOptions
 * @property {number} [minX]
 * @property {number} [minY]
 * @property {number} [maxX]
 * @property {number} [maxY]
 * @property {number} [heightScale]
 */

/**
 * @typedef {Object} TileAgentNavOptions
 * @property {number} [agentRadius]
 * @property {boolean} [allowDiagonal]
 * @property {number} [maxSlope]
 */

/**
 * @typedef {Object} TileWorldConfig
 * @property {number} [chunkSize]
 * @property {number} [tileSize]
 * @property {Array<TileLayerConfig>} [layers]
 * @property {string} [tileAtlas]
 * @property {number} [atlasTileWidth]
 * @property {number} [atlasTileHeight]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

class TileWorld {

  /**
   * @readonly
   * @type {SceneNode|null}
   */
  node;

  /**
   * @readonly
   * @type {number}
   */
  width;

  /**
   * @readonly
   * @type {number}
   */
  height;

  /**
   * @readonly
   * @type {number}
   */
  chunkCount;

  /**
   * @readonly
   * @type {number}
   */
  vertexCount;

  /**
   * @readonly
   * @type {number}
   */
  triangleCount;

  /**
   * @param {number} layer
   * @param {number} x
   * @param {number} y
   * @param {number} tileId
   */
  setTile(layer, x, y, tileId) {}

  /**
   * @param {number} layer
   * @param {number} x
   * @param {number} y
   * @returns {number}
   */
  getTile(layer, x, y) {}

  /**
   * @param {number} layer
   * @param {number} x
   * @param {number} y
   * @param {number} w
   * @param {number} h
   * @param {number} tileId
   */
  fillRect(layer, x, y, w, h, tileId) {}

  /**
   * @param {number} layer
   */
  clearLayer(layer) {}

  /**
   * @param {number} worldX
   * @param {number} worldZ
   * @returns {TilePickResult|null}
   */
  pickTile(worldX, worldZ) {}

  /**
   * @param {number} startX
   * @param {number} startY
   * @param {number} endX
   * @param {number} endY
   * @param {TileAgentNavOptions} [opts]
   * @returns {TilePathResult}
   */
  findPath(startX, startY, endX, endY, opts) {}

  /**
   * @param {number} layer
   * @returns {Array<TileRegionResult>}
   */
  computeRegions(layer) {}

  /**
   * @param {number} layer
   * @param {Array<TileAutotileRule>} rules
   */
  applyAutotile(layer, rules) {}

  /**
   * @param {TileVoxelExtractOptions} [opts]
   * @returns {Mesh}
   */
  extractVoxelMesh(opts) {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} z
   */
  setOrigin(x, y, z) {}

  /**
   * @param {number} dtMs
   * @returns {boolean}
   */
  advance(dtMs) {}

  /**
   * @param {number} kindId
   * @param {Object} spec
   */
  addObjectKind(kindId, spec) {}

  /**
   * @param {number} kindId
   * @param {number} x
   * @param {number} y
   * @param {number} z
   */
  addObject(kindId, x, y, z) {}

  /**
   * @param {number} [kindId]
   */
  clearObjects(kindId) {}

  /**
   * @param {number} kind
   * @returns {number}
   */
  objectCount(kind) {}

  rebuildObjects() {}

  rebuild() {}

  rebuildAll() {}

  /**
   * @param {Object} cfg
   */
  configure(cfg) {}

  /**
   * @returns {ArrayBuffer}
   */
  save() {}

  /**
   * @param {ArrayBuffer} data
   * @returns {boolean}
   */
  load(data) {}

  destroy() {}

}

