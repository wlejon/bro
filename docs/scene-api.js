// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro Scene Graph API Reference
 * =============================================================================
 *
 * 3D Scene Graph containing hierarchically nested nodes (MeshNode, SkinnedMeshNode,
 * InstancedMeshNode, LightNode, CameraNode, ParticleNode, HtmlNode, ShapeNode, SpriteNode).
 * @typedef {Object} SceneNodeOptions
 * @property {string} [name]
 * @property {Array<number>} [position]
 * @property {Array<number>} [rotation]
 * @property {Array<number>} [scale]
 * @property {boolean} [visible]
 */

/**
 * Camera view for setCamera() (the imperative view) and createCamera() (a
 * camera node). Three shapes:
 *
 *   perspective (default): { fov=60, near=0.1, far=1000, aspect, eye, target, up }
 *   quaternion:            { fov, near, far, aspect, eye, quaternion: [x,y,z,w] }
 *     camera local -Z is forward; `quaternion` overrides target/up/mode
 *   orthographic:          { mode: "orthographic", size=10, near, far, aspect, eye, target, up }
 *
 * `position` is accepted as an alias of `eye` and `lookAt` of `target`.
 * @typedef {Object} SceneCameraOptions
 * @property {number} [fov] -  Vertical field of view in degrees (perspective only, default 60).
 * @property {number} [near] -  Near clipping plane (default 0.1).
 * @property {number} [far] -  Far clipping plane (default 1000).
 * @property {Array<number>} [eye] -  Camera position [x,y,z] (default [0,5,-10]). `position` is an alias.
 * @property {Array<number>} [target] -  Look-at point [x,y,z] (default [0,0,0]). `lookAt` is an alias.
 * @property {Array<number>} [up] -  Up vector [x,y,z] (default [0,1,0]).
 * @property {number} [aspect] - width/height. Omit it (or pass <= 0) and the projection is built from
the current canvas size AND flagged to auto-follow future canvas
resizes, which is normally what you want; an explicit aspect pins the
projection. With no canvas size yet the omitted case falls back to 4/3.
 * @property {Array<number>} [quaternion] - [x,y,z,w] camera orientation, local -Z forward. Set, it overrides
target/up/mode: the 6DOF / FPS path that avoids target+up precision loss.
 * @property {string} [mode] -  "perspective" (default) or "orthographic" / "ortho".
 * @property {number} [size] -  Orthographic view height in world units (default 10).
 */

/**
 * @typedef {Object} WindConfig
 * @property {Array<number>} [direction] -  Sway direction [x,y,z] (default [1,0,0]).
 * @property {number} [strength] -  Displacement amplitude in world units (default 0: no sway).
 * @property {number} [frequency] -  Oscillation frequency in rad/s (default 1.5).
 */

/**
 * @typedef {Object} SceneRaycastResult
 * @property {SceneNode} [node]
 * @property {Array<number>} [point]
 * @property {Array<number>} [normal]
 * @property {number} [distance]
 */

/**
 * @typedef {Object} SceneCullStats
 * @property {number} [totalNodes]
 * @property {number} [renderedNodes]
 * @property {number} [culledNodes]
 */

/**
 * @typedef {Object} ImpostorAtlasInfo
 * @property {number} [width]
 * @property {number} [height]
 * @property {number} [cols]
 * @property {number} [rows]
 * @property {number} [boundsRadius]
 * @property {Array<number>} [boundsCenter]
 * @property {Uint8Array} [atlasRGBA]
 */

/**
 * @typedef {Object} ImpostorOptions
 * @property {number} [margin]
 * @property {number} [cullNear]
 * @property {number} [cullFar]
 */

/**
 * @typedef {Object} ImpostorResult
 * @property {SceneNode} [node]
 * @property {number} [quadCount]
 */

/**
 * @typedef {Object} MeshNodeOptions
 * @property {Mesh|string} [mesh] - A Mesh object, or a primitive name ('box', 'sphere', ...).
 * @property {Mesh} [data] - Alias of `mesh` for a Mesh object.
 * @property {string} [material]
 * @property {string} [castShadow]
 * @property {string} [receiveShadow]
 * @property {Array<number>} [position]
 * @property {Array<number>} [rotation]
 * @property {Array<number>} [scale]
 * @property {boolean} [visible]
 */

/**
 * @typedef {Object} MeshGeometry
 * @property {Float32Array} [positions]
 * @property {Uint32Array} [indices]
 * @property {Float32Array} [normals]
 * @property {Float32Array} [uvs]
 * @property {Float32Array} [colors]
 * @property {Float32Array} [tangents]
 */

/**
 * @typedef {Object} MeshUpdateOptions
 * @property {boolean} [recomputeNormals]
 */

/**
 * @typedef {Object} SkinnedMeshNodeOptions
 * @property {Mesh} [mesh] - A Mesh object (or raw `positions`/`indices`/... streams as in MeshGeometry).
 * @property {Mesh} [data] - Alias of `mesh` for a Mesh object.
 * @property {SkinData} [skin]
 * @property {Skeleton} [skeleton]
 * @property {string} [material]
 * @property {Array<number>} [position]
 * @property {Array<number>} [rotation]
 * @property {Array<number>} [scale]
 * @property {boolean} [visible]
 */

/**
 * @typedef {Object} InstancedMeshNodeOptions
 * @property {Mesh} [mesh]
 * @property {number} [capacity]
 * @property {string} [material]
 * @property {Float32Array} [instances] - 16 floats per instance, stored as
 *   given: a row-major 3x4 affine in floats 0-11 (basis rows at 0-2 / 4-6 /
 *   8-10, translation at 3 / 7 / 11) and an RGBA tint in floats 12-15 (write
 *   1, 1, 1, 1 for none). Not a column-major 4x4; `setInstanceTransform`
 *   is the call that takes one of those.
 * @property {Float32Array} [instancesFromTransforms] - 9 floats per instance
 * @property {string|Array<number>} [color]
 * @property {number} [metallic]
 * @property {number} [roughness]
 * @property {number} [emissive]
 * @property {string|Array<number>} [emissiveColor]
 * @property {boolean} [unlit]
 * @property {number} [alphaCutoff]
 * @property {boolean} [vertexColorTint]
 * @property {boolean} [doubleSided]
 * @property {boolean} [castsShadow]
 * @property {boolean} [receivesShadow]
 * @property {{width: number, height: number, data: Uint8Array}} [texture] - also normalTexture, metallicRoughnessTexture, occlusionTexture, emissiveTexture
 * @property {number} [atlasCols] - atlas grid columns (the other defaults to 1)
 * @property {number} [atlasRows] - atlas grid rows
 * @property {boolean} [staticBatch] - merge all instances into one draw
 * @property {Array<number>} [position]
 * @property {Array<number>} [rotation]
 * @property {Array<number>} [scale]
 * @property {boolean} [visible]
 */

/**
 * @typedef {Object} GaussianSplatNodeOptions
 * @property {string} [file]
 * @property {ArrayBuffer} [data]
 * @property {Object} [cloud]
 * @property {Array<number>} [position]
 * @property {Array<number>} [rotation]
 * @property {Array<number>} [scale]
 * @property {boolean} [visible]
 */

/**
 * @typedef {Object} HtmlNodeOptions
 * @property {string} [html]
 * @property {string} [name]
 * @property {number} [width=200] - layout width in DOM pixels
 * @property {number} [height=50]
 * @property {number} [pxPerUnit=100] - DOM pixels per world unit
 * @property {Array<number>} [worldAnchor] - [x, y, z] world point the billboard follows
 * @property {string} [billboard] - 'full' | 'ylock'
 * @property {Array<number>} [position]
 * @property {Array<number>|number} [rotation] - quaternion [x, y, z, w], or a number = Z rotation
 * @property {Array<number>|number} [scale] - [x, y, z], or a number = uniform
 * @property {boolean} [visible]
 */

/**
 * @typedef {Object} ShapeNodeOptions
 * @property {string} [shape='rect'] - 'rect' | 'roundrect' | 'circle' | 'ellipse' | 'polygon' | 'line'
 * @property {string} [name]
 * @property {number} [width]
 * @property {number} [height]
 * @property {number} [radius] - circle
 * @property {number} [cornerRadius] - roundrect
 * @property {number} [radiusX] - ellipse
 * @property {number} [radiusY]
 * @property {Array<number>} [points] - polygon, flat [x0, y0, x1, y1, ...]
 * @property {string} [fill] - CSS colour
 * @property {string} [stroke] - CSS colour
 * @property {number} [strokeWidth]
 * @property {number} [anchorX=0.5]
 * @property {number} [anchorY=0.5]
 * @property {number} [x]
 * @property {number} [y]
 * @property {Array<number>} [worldAnchor]
 * @property {string} [billboard] - 'full' | 'ylock'
 */

/**
 * @typedef {Object} SpriteNodeOptions
 * @property {string} [name]
 * @property {string} [src] - image path (app-relative or a mount path)
 * @property {number} [width]
 * @property {number} [height]
 * @property {number} [opacity]
 * @property {number} [anchorX=0.5]
 * @property {number} [anchorY=0.5]
 * @property {{frameWidth?: number, frameHeight?: number, columns?: number, rows?: number, frames?: Array<{x: number, y: number, w: number, h: number}>}} [sheet]
 * @property {Object<string, {frames: Array<number>, fps?: number, loop?: boolean, next?: string}>} [animations]
 * @property {string} [play] - animation to start
 * @property {number} [frameIndex]
 * @property {number} [x]
 * @property {number} [y]
 * @property {Array<number>} [worldAnchor]
 * @property {string} [billboard] - 'full' | 'ylock'
 */

/**
 * @typedef {Object} LightNodeOptions
 * @property {string} [type]
 * @property {Array<number>} [color]
 * @property {number} [intensity]
 * @property {number} [range]
 * @property {number} [innerCone]
 * @property {number} [outerCone]
 * @property {boolean} [castShadow]
 * @property {Array<number>} [position]
 * @property {Array<number>} [rotation]
 */

/**
 * @typedef {Object} ParticleNodeOptions
 * @property {number} [maxParticles]
 * @property {string} [texture]
 * @property {Array<number>} [position]
 * @property {boolean} [visible]
 */

/**
 * @typedef {Object} Particles3DNodeOptions
 * @property {number} [maxParticles]
 * @property {string} [mode]
 * @property {Mesh} [mesh]
 * @property {Array<number>} [position]
 * @property {boolean} [visible]
 */

/**
 * @typedef {Object} DecalNodeOptions
 * @property {string} [texture]
 * @property {Array<number>} [size]
 * @property {Array<number>} [position]
 * @property {Array<number>} [rotation]
 */

/**
 * @typedef {Object} ReflectionProbeNodeOptions
 * @property {Array<number>} [size]
 * @property {number} [resolution]
 * @property {Array<number>} [position]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

class SceneNode {

  /**
   * @readonly
   * @type {number}
   */
  id;

  /**
   * @type {string}
   */
  name;

  /**
   * @type {boolean}
   */
  visible;

  /**
   *  Shape / Sprite: the anchor point in [0,1] (0.5, 0.5 = centred). Undefined on other nodes.
   * @type {number}
   */
  anchorX;

  /** @type {number} */
  anchorY;

  /**
   *  Shape: rounded-rect corner radius. Undefined on other nodes.
   * @type {number}
   */
  cornerRadius;

  /**
   *  HtmlNode: DOM pixels per world unit (default 100). Undefined on other nodes.
   * @type {number}
   */
  pxPerUnit;

  /**
   *  InstancedMesh: collapse every instance into one merged draw (kills the
   *  per-instance GPU cost of many tiny meshes; base-colour-only materials).
   *  Undefined on other nodes.
   * @type {boolean}
   */
  staticBatch;

  /**
   *  InstancedMesh: the atlas grid set by `setAtlasGrid` / the `atlasCols`,
   *  `atlasRows` or `atlas` options (1x1 by default); 0 on other nodes.
   * @readonly
   * @type {number}
   */
  atlasCols;

  /** @readonly @type {number} */
  atlasRows;

  /**
   *  PhysicsNode: the Jolt body id it follows (the `bodyId` create option),
   *  null with no body; undefined on other nodes.
   * @readonly
   * @type {number|null}
   */
  bodyId;

  /**
   * @type {number}
   */
  x;

  /**
   * @type {number}
   */
  y;

  /**
   * @type {number}
   */
  z;

  /**
   * @type {Array<number>}
   */
  position;

  /**
   * @type {Array<number>}
   */
  rotation;

  /**
   * @type {number}
   */
  rotationX;

  /**
   * @type {number}
   */
  rotationY;

  /**
   * @type {number}
   */
  rotationZ;

  /**
   * @type {Array<number>}
   */
  scale;

  /**
   * @type {number}
   */
  scaleX;

  /**
   * @type {number}
   */
  scaleY;

  /**
   * @type {number}
   */
  scaleZ;

  /**
   * @type {Array<number>}
   */
  quaternion;

  /**
   * @type {number}
   */
  fov;

  /**
   * @type {number}
   */
  near;

  /**
   * @type {number}
   */
  far;

  /**
   * @type {number}
   */
  aspect;

  /**
   * @type {number}
   */
  orthoHeight;

  /**
   * @type {string}
   */
  projection;

  /**
   * @readonly
   * @type {Array<number>}
   */
  worldPosition;

  /**
   * @readonly
   * @type {Array<number>}
   */
  worldMatrix;

  /**
   * @readonly
   * @type {SceneNode|null}
   */
  parent;

  /**
   * @readonly
   * @type {Array<SceneNode>}
   */
  children;

  /**
   * @readonly
   * @type {number}
   */
  boneCount;

  /**
   * @readonly
   * @type {boolean}
   */
  skinReady;

  /**
   * @readonly
   * @type {boolean}
   */
  isPlaying;

  /**
   * @readonly
   * @type {string}
   */
  currentAnimation;

  /**
   * @readonly
   * @type {number}
   */
  animationDuration;

  /**
   * @type {number}
   */
  animationTime;

  /**
   * @type {number}
   */
  animationSpeed;

  /**
   * @readonly
   * @type {string}
   */
  state;

  /**
   * @param {SceneNode} child
   * @returns {SceneNode}
   */
  add(child) {}

  /**
   * @param {SceneNode} child
   */
  remove(child) {}

  /**
   * @param {SceneNode} child
   * @returns {SceneNode}
   */
  addChild(child) {}

  /**
   * @param {SceneNode} child
   */
  removeChild(child) {}

  destroy() {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @returns {SceneNode}
   */
  setPosition(x, y, z) {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @param {number} [w]
   * @returns {SceneNode}
   */
  setRotation(x, y, z, w) {}

  /**
   * @param {number} x
   * @param {number} [y]
   * @param {number} [z]
   * @returns {SceneNode}
   */
  setScale(x, y, z) {}

  /**
   * @param {Array<number>} target
   * @param {Array<number>} [up]
   * @returns {SceneNode}
   */
  lookAt(target, up) {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} [z]
   * @returns {Object}
   */
  localToWorld(x, y, z) {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} [z]
   * @returns {Object}
   */
  worldToLocal(x, y, z) {}

  /**
   * Replace a MeshNode's geometry in place: positions and indices (both
   * required), with normals, uvs, colors and tangents when given. A Mesh
   * object works too. The node, its material, transform and children are
   * untouched, so this is the per-frame path for geometry that deforms —
   * a soft body's vertices(), a procedural surface, a streamed chunk.
   * Normals are recomputed when the geometry carries none or when
   * `opts.recomputeNormals` is true. Throws on a node that is not a
   * MeshNode. Returns the node.
   *
   * @param {(MeshGeometry|Mesh)} mesh
   * @param {MeshUpdateOptions} [opts]
   * @returns {SceneNode}
   * @example
   * const topo = cloth.topology();
   * const node = scene.createMesh({ positions: cloth.vertices(), indices: topo.indices });
   * // each frame, after Physics.step():
   * node.updateMesh({ positions: cloth.vertices(), indices: topo.indices },
   *                 { recomputeNormals: true });
   */
  updateMesh(mesh, opts) {}

  /**
   * @param {Object} mat
   * @returns {SceneNode}
   */
  setMaterial(mat) {}

  /**
   * @param {Skeleton} skeleton
   * @returns {SceneNode}
   */
  setSkeleton(skeleton) {}

  /**
   * @param {string} name
   * @param {*} anim
   * @returns {SceneNode}
   */
  addClip(name, anim) {}

  /**
   * @param {*} nameOrIndex
   * @returns {Float32Array|null}
   */
  getBoneWorldMatrix(nameOrIndex) {}

  /**
   * @param {string} name
   * @param {Array<BlendSpace1DClip>} clips
   * @returns {SceneNode}
   */
  addBlendSpace1D(name, clips) {}

  /**
   * @param {string} name
   * @param {Array<BlendSpace2DClip>} clips
   * @returns {SceneNode}
   */
  addBlendSpace2D(name, clips) {}

  /**
   * @param {string} name
   * @param {number} x
   * @param {number} [y]
   * @returns {SceneNode}
   */
  setBlendPos(name, x, y) {}

  /**
   * @param {string} [name]
   * @returns {Object|null}
   */
  blendState(name) {}

  /**
   * @param {number} layer
   * @param {string} clipName
   * @param {Object} [opts]
   * @returns {SceneNode}
   */
  playLayer(layer, clipName, opts) {}

  /**
   * @param {number} layer
   * @param {number} [fadeTime]
   * @returns {SceneNode}
   */
  stopLayer(layer, fadeTime) {}

  /**
   * @param {number} layer
   * @param {number} weight
   * @returns {SceneNode}
   */
  setLayerWeight(layer, weight) {}

  /**
   * @param {*} nameOrDef
   * @param {AnimStateMachineDef} [def]
   * @returns {SceneNode}
   */
  addStateMachine(nameOrDef, def) {}

  /**
   * @param {string} name
   * @param {string} [targetState]
   * @returns {boolean}
   */
  travel(name, targetState) {}

  /**
   * @param {*} enabledOrOpts
   * @returns {SceneNode}
   */
  setRootMotion(enabledOrOpts) {}

  /**
   * @returns {Object}
   */
  consumeRootMotion() {}

  /**
   * @param {string} [clipName]
   * @param {Object} [opts]
   * @returns {SceneNode}
   */
  play(clipName, opts) {}

  /**
   * @param {Object} [opts]
   * @returns {SceneNode}
   */
  stop(opts) {}

  /**
   * @returns {SceneNode}
   */
  pause() {}

  /**
   * @returns {SceneNode}
   */
  resume() {}

  /**
   * @param {Float32Array} matrices
   * @returns {number}
   */
  setSkinningMatrices(matrices) {}

  /**
   * Replace one instance's transform, keeping the node's row-major storage
   * (see `InstancedMeshNodeOptions.instances`). The instance's tint (floats
   * 12-15, as `setInstanceColor` or `instances` set it) is kept.
   * @param {number} index
   * @param {Array<number>} matrix - a column-major 4x4 (16 numbers).
   */
  setInstanceTransform(index, matrix) {}

  /**
   * @param {number} index
   * @param {Array<number>} color
   */
  setInstanceColor(index, color) {}

  /**
   * @param {number} count
   */
  setInstanceCount(count) {}

  /**
   * @param {string} html
   * @returns {SceneNode}
   */
  setHtml(html) {}

  markHtmlDirty() {}

  /**
   * @param {number} count
   */
  burst(count) {}

  clear() {}

  probeCapture() {}

  /**
   *  Writes a Gaussian-splat node's cloud as a PLY file; false when the write fails.
   *
   * @param {string} path
   * @returns {boolean}
   */
  savePly(path) {}

}

class SceneGraph {

  /**
   * @readonly
   * @type {SceneNode}
   */
  root;

  /**
   * @type {number}
   */
  cameraX;

  /**
   * @type {number}
   */
  cameraY;

  /**
   * @type {number}
   */
  cameraZoom;

  /**
   * @type {boolean}
   */
  showLightIcons;

  /**
   * @type {boolean}
   */
  frustumCulling;

  /**
   * @type {boolean}
   */
  shadowCache;

  /**
   * @type {number}
   */
  renderScale;

  /**
   * @type {number}
   */
  msaa;

  /**
   * @type {SceneNode|null}
   */
  activeCamera;

  /**
   * @readonly
   * @type {Array<number>}
   */
  viewMatrix;

  /**
   * @readonly
   * @type {Array<number>}
   */
  projectionMatrix;

  /**
   * @readonly
   * @type {Array<number>}
   */
  cameraEye;

  /**
   * @param {SceneNodeOptions} [opts]
   * @returns {SceneNode}
   */
  createNode(opts) {}

  /**
   * @param {ShapeNodeOptions} [opts]
   * @returns {SceneNode}
   */
  createShape(opts) {}

  /**
   * @param {SpriteNodeOptions} [opts]
   * @returns {SceneNode}
   */
  createSprite(opts) {}

  /**
   * @param {Object} [opts]
   * @returns {SceneNode}
   */
  createPhysicsNode(opts) {}

  /**
   * @param {MeshNodeOptions} [opts]
   * @returns {SceneNode}
   */
  createMesh(opts) {}

  /**
   * @param {SkinnedMeshNodeOptions} [opts]
   * @returns {SceneNode}
   */
  createSkinnedMesh(opts) {}

  /**
   * @param {InstancedMeshNodeOptions} [opts]
   * @returns {SceneNode}
   */
  createInstancedMesh(opts) {}

  /**
   * @param {GaussianSplatNodeOptions} [opts]
   * @returns {SceneNode}
   */
  createGaussianSplat(opts) {}

  /**
   * @param {HtmlNodeOptions} [opts]
   * @returns {SceneNode}
   */
  createHtmlNode(opts) {}

  /**
   * @param {LightNodeOptions} [opts]
   * @returns {SceneNode}
   */
  createLight(opts) {}

  /**
   * @param {ParticleNodeOptions} [opts]
   * @returns {SceneNode}
   */
  createParticles(opts) {}

  /**
   * @param {Particles3DNodeOptions} [opts]
   * @returns {SceneNode}
   */
  createParticles3D(opts) {}

  /**
   * @param {DecalNodeOptions} [opts]
   * @returns {SceneNode}
   */
  createDecal(opts) {}

  /**
   * @param {ReflectionProbeNodeOptions} [opts]
   * @returns {SceneNode}
   */
  createReflectionProbe(opts) {}

  /**
   * @returns {Tween}
   */
  createTween() {}

  /**
   * @returns {AnimationPlayer}
   */
  createAnimationPlayer() {}

  /**
   * @param {TerrainConfig} [opts]
   * @returns {Terrain}
   */
  createTerrain(opts) {}

  /**
   * @param {ClipmapTerrainConfig} [opts]
   * @returns {ClipmapTerrain}
   */
  createClipmapTerrain(opts) {}

  /**
   * @param {TileWorldConfig} [opts]
   * @returns {TileWorld}
   */
  createTileWorld(opts) {}

  /**
   * @param {number} id
   * @returns {SceneNode|null}
   */
  findById(id) {}

  /**
   * @param {string} name
   * @returns {SceneNode|null}
   */
  findByName(name) {}

  /**
   * @param {SceneNode} node
   */
  destroyNode(node) {}

  /**
   * Install the imperative view (see SceneCameraOptions). The LAST camera
   * call wins: setCamera deactivates the active camera node, and
   * setActiveCamera overrides an imperative view. `activeCamera` is null
   * while the imperative view is in effect.
   *
   * @param {SceneCameraOptions} [opts]
   */
  setCamera(opts) {}

  /**
   * Create a camera NODE and add it to the root. The node's WORLD transform
   * is the view (local -Z forward, +Y up); only projection parameters live
   * on the node. `name` and `active` (activate it now) are also accepted.
   *
   * @param {SceneCameraOptions} [opts]
   * @returns {SceneNode}
   */
  createCamera(opts) {}

  /**
   * @param {SceneNode} camera
   */
  setActiveCamera(camera) {}

  /**
   * @param {ToneMapConfig} [opts]
   */
  setToneMap(opts) {}

  /**
   * @param {AmbientConfig} [opts]
   */
  setAmbient(opts) {}

  /**
   * Global wind sway for meshes created with `wind` set:
   * pos += direction * sin(time*frequency + dot(pos.xz, k)) * strength * bend.
   * The engine advances the wind clock from the per-frame virtual delta so
   * offline captures stay deterministic.
   *
   * @param {WindConfig} [opts]
   */
  setWind(opts) {}

  /**
   * Shadow atlas size and filter. `setShadowQuality(atlasSize, pcfTaps)`
   * positional is accepted too.
   *
   * @param {ShadowQualityConfig} [opts]
   */
  setShadowQuality(opts) {}

  /**
   * @param {ShadowCacheConfig} [opts]
   */
  setShadowCache(opts) {}

  /**
   * @param {FogConfig} [opts]
   */
  setFog(opts) {}

  /**
   * @param {AtmosphereConfig} [opts]
   */
  setAtmosphere(opts) {}

  /**
   * @param {StarfieldConfig} [opts]
   */
  setStarfield(opts) {}

  /**
   * @param {TiltShiftConfig} [opts]
   */
  setTiltShift(opts) {}

  /**
   * @param {BloomConfig} [opts]
   */
  setBloom(opts) {}

  /**
   * @param {SSAOConfig} [opts]
   */
  setSSAO(opts) {}

  /**
   * @param {SSRConfig} [opts]
   */
  setSSR(opts) {}

  /**
   * @param {DepthOfFieldConfig} [opts]
   */
  setDepthOfField(opts) {}

  /**
   * Loads a colour-grading LUT strip (`path`, `size` inferred from the strip
   * when 0, `amount` 0..1) and applies it as the last tonemap stage; false
   * when the strip fails to decode. Call with no options (or null) to clear.
   *
   * @param {ColorLUTConfig} [opts]
   * @returns {boolean}
   */
  setColorLUT(opts) {}

  /**
   * @param {boolean} enabled
   */
  setFXAA(enabled) {}

  /**
   * @param {number} scale
   */
  setRenderScale(scale) {}

  /**
   * @param {number} samples
   */
  setMSAA(samples) {}

  /**
   * @param {EnvironmentConfig} [opts]
   */
  setEnvironment(opts) {}

  /**
   * @param {boolean} enabled
   */
  setFrustumCulling(enabled) {}

  /**
   * @returns {SceneCullStats}
   */
  cullStats() {}

  clear() {}

  syncPhysics() {}

  /**
   * @param {Array<number>} origin
   * @param {Array<number>} direction
   * @returns {SceneRaycastResult|null}
   */
  raycast(origin, direction) {}

  /**
   * @param {SceneNode} node
   * @param {Array<number>} screenPoint
   * @returns {Array<number>}
   */
  unprojectLocal(node, screenPoint) {}

  /**
   * @returns {ImageData}
   */
  toImageData() {}

  /**
   * @param {string} [format]
   * @param {number} [quality]
   * @returns {ImageData}
   */
  captureFrame(format, quality) {}

  /**
   * @returns {Object}
   */
  asTexture() {}

  /**
   * @param {boolean} bind
   */
  bindAudioListenerToCamera(bind) {}

  /**
   * @param {Object} aiWorld
   * @param {Object} [opts]
   */
  attachAIWorld(aiWorld, opts) {}

  detachAIWorld() {}

}

