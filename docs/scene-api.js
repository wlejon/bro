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

