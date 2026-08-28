// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro Scene Graph API Reference
 * =============================================================================
 *
 * 3D Scene Graph containing hierarchically nested nodes (MeshNode, SkinnedMeshNode,
 * InstancedMeshNode, LightNode, CameraNode, ParticleNode, HtmlNode, ShapeNode, SpriteNode).
 *
 * ── Lifetime: the graph dies with its canvas ─────────────────────────────────
 *
 * A scene belongs to the <canvas> it was asked for. The engine reclaims the
 * whole graph — every node, every TileWorld/Terrain/Clipmap it owns — the frame
 * after that canvas leaves the document, however it leaves: remove(),
 * removeChild(), or a parent's innerHTML being replaced.
 *
 * Handles you are still holding stay SAFE to hold and safe to call. Every
 * wrapper re-resolves through the graph on each call, so once the graph is gone
 * a node accessor reads null and a method that would author geometry no-ops.
 * Data that lives outside the graph survives: a TileWorld still answers
 * getTile() after its canvas is gone, it just has nothing to mesh into.
 *
 * What does NOT happen is resurrection — re-attaching the canvas does not bring
 * the graph back. Ask the re-attached canvas for getContext('scene') again and
 * rebuild. So a view that tears down and re-creates its canvas should drop its
 * old scene handles at the same time; keeping them is harmless, but they will
 * never do anything again.
 *
 * @typedef {Object} SceneNodeOptions
 * @property {string} [name]
 * @property {Array<number>} [position]
 * @property {Array<number>} [rotation]
 * @property {Array<number>} [scale]
 * @property {boolean} [visible]
 */

/**
 * @typedef {Object} SceneCameraOptions
 * @property {number} [fov]
 * @property {number} [near]
 * @property {number} [far]
 * @property {Array<number>} [eye]
 * @property {Array<number>} [target]
 * @property {Array<number>} [up]
 */

/**
 * @typedef {Object} SceneRaycastResult
 * @property {SceneNode} [node]
 * @property {Array<number>} [point]
 * @property {Array<number>} [normal]
 * @property {number} [distance]
 * @property {number} [instance] - which copy was struck, for a hit on an
 *   instanced node. Absent on plain-mesh and light hits. Use it to map a hit
 *   back to whatever you placed (a cell, an entity id): instance indices match
 *   the order they were written in setInstances().
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
 * @property {Mesh} [mesh]
 * @property {string} [material]
 * @property {string} [castShadow]
 * @property {string} [receiveShadow]
 * @property {Array<number>} [position]
 * @property {Array<number>} [rotation]
 * @property {Array<number>} [scale]
 * @property {boolean} [visible]
 */

/**
 * @typedef {Object} SkinnedMeshNodeOptions
 * @property {Mesh} [mesh]
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
 * @property {number} [width]
 * @property {number} [height]
 * @property {Array<number>} [position]
 * @property {Array<number>} [rotation]
 * @property {Array<number>} [scale]
 * @property {boolean} [visible]
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
   * @type {Array<number>}
   */
  position;

  /**
   * @type {Array<number>}
   */
  rotation;

  /**
   * @type {Array<number>}
   */
  scale;

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
   * @param {SceneNode} child
   * @returns {SceneNode}
   */
  add(child) {}

  /**
   * @param {SceneNode} child
   */
  remove(child) {}

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
   * @param {SkeletalAnimation} anim
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
   * @param {string} name
   * @returns {Object|null}
   */
  blendState(name) {}

  /**
   * @param {number} layer
   * @param {string} clipName
   * @param {number} [weight]
   * @param {number} [fadeTime]
   * @returns {SceneNode}
   */
  playLayer(layer, clipName, weight, fadeTime) {}

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
   * @param {string} name
   * @param {AnimStateMachineDef} def
   * @returns {SceneNode}
   */
  addStateMachine(name, def) {}

  /**
   * @param {string} name
   * @param {string} targetState
   * @returns {boolean}
   */
  travel(name, targetState) {}

  /**
   * @param {boolean} enabled
   * @returns {SceneNode}
   */
  setRootMotion(enabled) {}

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
   * @returns {SceneNode}
   */
  stop() {}

  /**
   * @returns {SceneNode}
   */
  pause() {}

  /**
   * @returns {SceneNode}
   */
  resume() {}

  /**
   * @param {number} index
   * @param {Array<number>} matrix
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
   * @param {string} path
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
   * @param {Object} [opts]
   * @returns {SceneNode}
   */
  createShape(opts) {}

  /**
   * @param {Object} [opts]
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
   * @param {SceneCameraOptions} [opts]
   */
  setCamera(opts) {}

  /**
   * @param {SceneCameraOptions} [opts]
   * @returns {SceneNode}
   */
  createCamera(opts) {}

  /**
   * @param {SceneNode|null} camera
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
   * @param {Array<number>} dir
   * @param {number} speed
   */
  setWind(dir, speed) {}

  /**
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
   * @param {ColorLUTConfig} [opts]
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

  syncPhysics() {}

  /**
   * Cast a world-space ray at the scene and return the nearest hit.
   *
   * Covers mesh nodes AND instanced nodes (createInstancedMesh, and every
   * TileWorld object kind — props and buildings are one InstancedMeshNode per
   * kind), plus light marker icons when showLightIcons is on. Instanced hits
   * carry an `instance` index saying which copy was struck; all node types share
   * one nearest-hit comparison, so a plain mesh in front of an instance wins.
   *
   * Note this is the geometry pick. For a TileWorld, `raycastCell` remains the
   * right call when you want the *cell* under the ray — but be aware it tests
   * the tile height field only, so it looks straight through anything standing
   * on the tiles and answers with the ground behind. Pick geometry with this;
   * pick terrain cells with raycastCell; don't use one for the other's job.
   *
   * Not covered: scatter-mode instanced nodes (setScatterSegments / the
   * `scatter` option), whose copies are generated on the GPU and have no
   * CPU-side records to intersect. Those return no hit rather than a wrong one.
   * staticBatch nodes do pick — the per-instance records survive the bake.
   *
   * @param {Array<number>} origin
   * @param {Array<number>} direction
   * @param {number} [maxDist]
   * @returns {SceneRaycastResult|null}
   */
  raycast(origin, direction, maxDist) {}

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
   * @returns {ArrayBuffer}
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

