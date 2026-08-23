// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro.ai.game — Game AI, Navigation Grids/Meshes, ORCA Avoidance, and MCTS
 * =============================================================================
 *
 * Real-time game AI simulation system providing agents, spatial worlds,
 * navigation grids, navmesh pathfinding, local collision avoidance (ORCA),
 * tactical steering behaviors, and Monte Carlo Tree Search (MCTS).
 * @example
 * const world = bro.ai.game.createWorld();
 *   const agent = bro.ai.game.createAgent(world, { radius: 0.5, maxSpeed: 5.0 });
 *   agent.setPosition(0, 0, 0);
 *   world.step(1 / 60);
 */
class AIAgent {

  /**
   * @readonly
   * @type {number}
   */
  id;

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} z
   */
  setPosition(x, y, z) {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} z
   */
  setVelocity(x, y, z) {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} z
   */
  setGoal(x, y, z) {}

  stop() {}

}

class AIWorld {

  /**
   * @param {number} dt
   */
  step(dt) {}

  /**
   * @param {Object} [opts]
   * @returns {AIAgent}
   */
  createAgent(opts) {}

  /**
   * @param {AIAgent} agent
   */
  destroyAgent(agent) {}

}

class AINavGrid {

  /**
   * @param {number} x
   * @param {number} z
   * @returns {boolean}
   */
  isWalkable(x, z) {}

  /**
   * @param {number} x
   * @param {number} z
   * @param {boolean} walkable
   */
  setWalkable(x, z, walkable) {}

  /**
   * @param {number} startX
   * @param {number} startZ
   * @param {number} endX
   * @param {number} endZ
   * @returns {Array<Object>}
   */
  findPath(startX, startZ, endX, endZ) {}

}

class AINavMesh {

  /**
   * @param {number} startX
   * @param {number} startY
   * @param {number} startZ
   * @param {number} endX
   * @param {number} endY
   * @param {number} endZ
   * @returns {Array<Object>}
   */
  findPath(startX, startY, startZ, endX, endY, endZ) {}

}

class AIMcts {

  /**
   * @param {Object} state
   * @param {Object} [opts]
   * @returns {Object}
   */
  search(state, opts) {}

}

class CombatAction {

  /**
   * @type {number}
   */
  targetX;

  /**
   * @type {number}
   */
  targetZ;

  /**
   * @type {number}
   */
  actionType;

}

class Formation {

  /**
   * @param {AIAgent} agent
   */
  setLeader(agent) {}

  /**
   * @param {AIAgent} agent
   * @param {number} offsetX
   * @param {number} offsetZ
   */
  addFollower(agent, offsetX, offsetZ) {}

  /**
   * @param {number} dt
   */
  update(dt) {}

}

class VecSim {

  /**
   * @param {number} dt
   */
  step(dt) {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * @param {AIWorld} world
 * @param {Object} [opts]
 * @returns {AIAgent}
 */
bro.ai.game.game.createAgent = function(world, opts) {};

/**
 * @param {Object} [opts]
 * @returns {AIWorld}
 */
bro.ai.game.game.createWorld = function(opts) {};

/**
 * @param {number} minX
 * @param {number} minZ
 * @param {number} maxX
 * @param {number} maxZ
 * @param {number} cellSize
 * @returns {AINavGrid}
 */
bro.ai.game.game.createNavGrid = function(minX, minZ, maxX, maxZ, cellSize) {};

/**
 * @param {Object} [opts]
 * @returns {AINavMesh}
 */
bro.ai.game.game.createNavMesh = function(opts) {};

/**
 * @param {Object} config
 * @returns {AIMcts}
 */
bro.ai.game.game.createMcts = function(config) {};

/**
 * @returns {CombatAction}
 */
bro.ai.game.game.createCombatAction = function() {};

/**
 * @param {Object} [opts]
 * @returns {Formation}
 */
bro.ai.game.game.createFormation = function(opts) {};

/**
 * @param {Object} [opts]
 * @returns {VecSim}
 */
bro.ai.game.game.createVecSim = function(opts) {};

/**
 * @param {string} name
 * @param {Object} definition
 */
bro.ai.game.game.registerCapability = function(name, definition) {};

