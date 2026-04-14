// =============================================================================
// bro Scene Graph API Reference
// =============================================================================
//
// The scene graph API provides 2D and 3D rendering on <canvas> elements.
// Obtain a SceneGraph by calling getContext("scene") on a canvas:
//
//   const canvas = document.querySelector('canvas');
//   const scene = canvas.getContext('scene');
//
// The scene graph supports 2D shapes, sprites, 3D meshes (via bromesh), and
// physics bodies (via Jolt). Nodes form a parent-child hierarchy rooted at
// scene.root. Rendering happens automatically each frame for visible nodes,
// or can be triggered manually via scene.render().
//
// =============================================================================


// -----------------------------------------------------------------------------
// SceneGraph
// -----------------------------------------------------------------------------
// Obtained via `canvas.getContext("scene")`.

class SceneGraph {

  // --- Properties -----------------------------------------------------------

  /** Root node of the scene hierarchy (read-only). All created nodes are added here by default. */
  get root() {}

  /** 2D camera X offset. */
  get cameraX() {}
  set cameraX(value) {}

  /** 2D camera Y offset. */
  get cameraY() {}
  set cameraY(value) {}

  /** 2D camera zoom factor. */
  get cameraZoom() {}
  set cameraZoom(value) {}


  // --- Node Creation --------------------------------------------------------

  /**
   * Create a generic SceneNode and add it to the root.
   * @param {string} [name] - optional node name
   * @returns {SceneNode}
   */
  createNode(name) {}

  /**
   * Create a 2D shape node and add it to the root.
   * @param {Object} [opts]
   * @param {string} [opts.shape="rect"] - "rect"|"roundrect"|"circle"|"ellipse"|"polygon"|"line"
   * @param {string} [opts.name]
   * @param {number} [opts.width]
   * @param {number} [opts.height]
   * @param {number} [opts.radius] - circle radius
   * @param {number} [opts.radiusX] - ellipse X radius
   * @param {number} [opts.radiusY] - ellipse Y radius
   * @param {number} [opts.cornerRadius] - roundrect corner radius
   * @param {string} [opts.fill] - CSS color string (default: white)
   * @param {string} [opts.stroke] - CSS color string
   * @param {number} [opts.strokeWidth]
   * @param {number} [opts.anchorX=0.5] - anchor point X (0-1)
   * @param {number} [opts.anchorY=0.5] - anchor point Y (0-1)
   * @param {number} [opts.x=0]
   * @param {number} [opts.y=0]
   * @param {number[]} [opts.points] - flat array of [x,y,...] pairs for polygon shape
   * @returns {SceneNode}
   */
  createShape(opts) {}

  /**
   * Create a 2D sprite node and add it to the root.
   * @param {Object} [opts]
   * @param {string} [opts.name]
   * @param {string} [opts.src] - image file path
   * @param {number} [opts.width]
   * @param {number} [opts.height]
   * @param {number} [opts.x=0]
   * @param {number} [opts.y=0]
   * @param {number} [opts.opacity=1.0]
   * @param {number} [opts.anchorX=0.5]
   * @param {number} [opts.anchorY=0.5]
   * @returns {SceneNode}
   */
  createSprite(opts) {}

  /**
   * Create a 3D mesh node and add it to the root.
   * Accepts a Mesh object, named primitive, or raw vertex data.
   *
   * From a Mesh object (see mesh-api.js):
   *   scene.createMesh({ data: Mesh.sphere(1).simplify(0.5), color: 'red' })
   *
   * Named primitives (set opts.mesh to one of these):
   *   "box"      - { halfW=0.5, halfH=0.5, halfD=0.5 }
   *   "sphere"   - { radius=0.5, segments=16, rings=12 }
   *   "cylinder" - { radius=0.5, halfHeight=0.5, segments=16 }
   *   "capsule"  - { radius=0.5, halfHeight=0.5, segments=16, rings=8 }
   *   "plane"    - { halfW=5, halfD=5, subdivX=1, subdivZ=1 }
   *   "torus"    - { majorRadius=1, minorRadius=0.3, majorSegments=24, minorSegments=12 }
   *
   * Raw vertex data (overrides mesh primitive if present):
   *   { positions: Float32Array, indices: Uint32Array, normals?: Float32Array }
   *
   * @param {Object} [opts]
   * @param {Mesh} [opts.data] - a Mesh instance (takes priority over mesh/positions)
   * @param {string} [opts.mesh="box"] - primitive type (ignored if data or positions provided)
   * @param {string} [opts.name]
   * @param {number} [opts.x=0] - position X
   * @param {number} [opts.y=0] - position Y
   * @param {number} [opts.z=0] - position Z
   * @param {number} [opts.scale=1] - uniform scale
   * @param {number} [opts.rx=0] - rotation X in degrees
   * @param {number} [opts.ry=0] - rotation Y in degrees
   * @param {number} [opts.rz=0] - rotation Z in degrees
   * @param {string|number[]} [opts.color] - CSS color string or [r,g,b] / [r,g,b,a] (0-1 range)
   * @param {number} [opts.emissive=0] - emissive intensity (0 = none, >0 = self-lit)
   * @param {Float32Array} [opts.positions] - raw vertex positions (xyz, stride 3)
   * @param {Float32Array} [opts.normals] - raw vertex normals (xyz, stride 3)
   * @param {Uint32Array} [opts.indices] - raw triangle indices
   * @returns {SceneNode}
   */
  createMesh(opts) {}

  /**
   * Create a physics body node and add it to the root.
   * @param {Object} [opts]
   * @param {string} [opts.name]
   * @param {number} [opts.body] - Jolt BodyID (raw index+sequence number)
   * @param {number} [opts.pixelsPerUnit=100] - scale factor for physics-to-pixel conversion
   * @param {boolean} [opts.autoSync=true] - automatically sync transform from physics each frame
   * @returns {SceneNode}
   */
  createPhysicsNode(opts) {}


  // --- 3D Camera ------------------------------------------------------------

  /**
   * Configure the 3D camera (perspective or orthographic).
   *
   * Perspective mode (default):
   *   { fov=60, near=0.1, far=1000, aspect, position: [x,y,z], target: [x,y,z], up?: [x,y,z] }
   *
   * Orthographic mode:
   *   { mode: "orthographic", size=10, near=0.1, far=1000, aspect, position, target, up }
   *
   * @param {Object} opts
   * @param {string} [opts.mode="perspective"] - "perspective" or "orthographic"/"ortho"
   * @param {number} [opts.fov=60] - vertical field of view in degrees (perspective only)
   * @param {number} [opts.size=10] - view height in world units (orthographic only)
   * @param {number} [opts.near=0.1] - near clipping plane
   * @param {number} [opts.far=1000] - far clipping plane
   * @param {number} [opts.aspect] - width/height ratio (defaults to 4/3)
   * @param {number[]} [opts.position=[0,5,-10]] - camera position [x, y, z]
   * @param {number[]} [opts.target=[0,0,0]] - look-at target [x, y, z]
   * @param {number[]} [opts.up=[0,1,0]] - up vector [x, y, z]
   */
  setCamera(opts) {}


  // --- Queries --------------------------------------------------------------

  /**
   * Find a node by its unique integer ID.
   * @param {number} id
   * @returns {SceneNode|null}
   */
  findById(id) {}

  /**
   * Find a node by name (first match).
   * @param {string} name
   * @returns {SceneNode|null}
   */
  findByName(name) {}


  // --- Lifecycle ------------------------------------------------------------

  /**
   * Destroy a node and remove it from the scene.
   * @param {SceneNode} node
   */
  destroyNode(node) {}

  /** Sync all physics node transforms from the physics world. */
  syncPhysics() {}


  // --- AI integration (see docs/ai-game-api.js for the full surface) -------

  /**
   * Bind a brogameagent::World and drive it at a fixed step from the engine
   * main loop. After this, each frame: world.tick(1/stepHz) is invoked up to
   * maxStepsPerFrame times based on the real frame delta, then every
   * AgentBinding on this graph steps (think + advance + transform sync).
   *
   * @param {AIWorld} world  - from bro.ai.game.createWorld()
   * @param {Object}  [opts]
   * @param {number}  [opts.stepHz=60]
   * @param {number}  [opts.maxStepsPerFrame=8]
   */
  attachAIWorld(world, opts) {}

  /** Stop auto-ticking the AI world. */
  detachAIWorld() {}
}


// -----------------------------------------------------------------------------
// SceneNode AI extensions
// -----------------------------------------------------------------------------
//
// Any scene node can own an AI binding. See docs/ai-game-api.js for the full
// capability / think API; summary:
//
//   node.attachAgent(world, agent, {
//     capabilities: ["move_to","basic_attack","hold"],
//     thinkHz: 15,
//     think(self, w) { ... },
//   });
//
//   node.detachAgent();
//
// Once attached, the node's x/y/z/rotationY are driven by the agent each
// frame (with yOffset applied on Y for ground clearance).


// -----------------------------------------------------------------------------
// SceneNode
// -----------------------------------------------------------------------------
// Base wrapper for all node types (generic, shape, sprite, mesh, physics).
// Properties specific to a node type (e.g. fillColor on shapes) are silently
// ignored when accessed on other types.

class SceneNode {

  // --- Common Properties ----------------------------------------------------

  /** Unique integer ID (read-only). */
  get id() {}

  /** Node name. */
  get name() {}
  set name(value) {}

  /** Whether this node is visible. */
  get visible() {}
  set visible(value) {}


  // --- Transform (all node types) -------------------------------------------

  /** Position X. */
  get x() {}
  set x(value) {}

  /** Position Y. */
  get y() {}
  set y(value) {}

  /** Position Z (3D depth). */
  get z() {}
  set z(value) {}

  /** Rotation around Z axis in radians (legacy 2D shorthand). */
  get rotation() {}
  set rotation(radians) {}

  /** Rotation around X axis in radians. */
  get rotationX() {}
  set rotationX(radians) {}

  /** Rotation around Y axis in radians. */
  get rotationY() {}
  set rotationY(radians) {}

  /** Rotation around Z axis in radians. */
  get rotationZ() {}
  set rotationZ(radians) {}

  /** Scale along X axis (default 1). */
  get scaleX() {}
  set scaleX(value) {}

  /** Scale along Y axis (default 1). */
  get scaleY() {}
  set scaleY(value) {}

  /** Scale along Z axis (default 1). */
  get scaleZ() {}
  set scaleZ(value) {}


  // --- Shape Properties (ShapeNode only) ------------------------------------

  /** Shape width in pixels. */
  get width() {}
  set width(value) {}

  /** Shape height in pixels. */
  get height() {}
  set height(value) {}

  /** Circle radius. */
  get radius() {}
  set radius(value) {}

  /** Fill color as CSS rgba() string. Set with any CSS color string. */
  get fillColor() {}
  set fillColor(cssColor) {}

  /** Stroke color as CSS rgba() string. Set with any CSS color string. */
  get strokeColor() {}
  set strokeColor(cssColor) {}

  /** Stroke width in pixels. Setting this also enables the stroke. */
  get strokeWidth() {}
  set strokeWidth(value) {}


  // --- Physics Properties (PhysicsNode only) --------------------------------

  /** Whether transform auto-syncs from physics each frame. */
  get autoSync() {}
  set autoSync(value) {}

  /** Pixels-per-unit scale for physics conversion. */
  get pixelsPerUnit() {}
  set pixelsPerUnit(value) {}

  /** Raw Jolt BodyID (read-only, null if no body). */
  get bodyId() {}


  // --- Hierarchy Methods ----------------------------------------------------

  /**
   * Add a child node.
   * @param {SceneNode} child
   * @returns {SceneNode} this (for chaining)
   */
  add(child) {}

  /**
   * Remove a child node.
   * @param {SceneNode} child
   */
  remove(child) {}

  /** Destroy this node (removes from parent and scene). */
  destroy() {}


  // --- Coordinate Conversion ------------------------------------------------

  /**
   * Transform a local-space point to world space.
   * @param {number} x
   * @param {number} y
   * @param {number} [z=0]
   * @returns {{ x: number, y: number, z: number }}
   */
  localToWorld(x, y, z) {}


  // --- Physics Methods (PhysicsNode only) -----------------------------------

  /** Manually sync this node's transform to its physics body. */
  syncToPhysics() {}
}
