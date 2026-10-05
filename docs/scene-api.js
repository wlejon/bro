// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro Scene Graph API Reference
 * =============================================================================
 *
 * 3D Scene Graph containing hierarchically nested nodes (MeshNode, SkinnedMeshNode,
 * InstancedMeshNode, LightNode, CameraNode, ParticleNode, HtmlNode, ShapeNode, SpriteNode).
 *
 * This file is the graph itself: `SceneGraph` (factories, cameras, render
 * settings, raycasts, capture, moving a scene between canvases) and the
 * `SceneNode` core every node shares (identity, hierarchy, transforms,
 * camera projection). The per-type half — the create* option dictionaries,
 * `bro.impostor`, and the SceneNode members for meshes, shaders, LOD,
 * instances, splats, audio emitters and skeletal playback — is
 * docs/scene-nodes-api.js. Lighting configs are docs/lighting-api.js.
 *
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

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * The members every node has. Node-type members (meshes, shaders, LOD,
 * instances, splats, audio emitters, skeletal playback, billboards) continue
 * this class in docs/scene-nodes-api.js.
 */
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

}

/**
 * A scene canvas clears to transparent: where nothing draws (and no sky or
 * environment background is set), the canvas element's CSS background and
 * the page behind it show through. For a solid backdrop, style the canvas
 * (`canvas.style.background = '#1a1a1a'`) or set an environment.
 */
class SceneGraph {

  /**
   * @readonly
   * @type {SceneNode}
   */
  root;

  // ── Moving a scene to another canvas ──────────────────────────────────────
  //
  // A scene belongs to the canvas that created it, and by default dies with
  // it: when that canvas leaves the DOM the engine reclaims the graph and
  // everything it uploaded. To rebuild a page around a live world (swap the
  // canvas element, re-render a panel) without re-authoring and re-uploading
  // it, move the scene instead:
  //
  //     scene.keepAlive = true;          // survive the old canvas leaving the DOM
  //     oldCanvas.remove();              // ... the page rebuilds ...
  //     scene.attachTo(newCanvas);       // same nodes, same GPU buffers/textures
  //
  // or, with both canvases live, just `scene.attachTo(newCanvas)`. Meshes,
  // textures, instance buffers, materials, lights, camera and animation state
  // all stay; nothing is re-uploaded (tests/scene/test_scene_attach.js checks
  // the upload counters). Only the canvas binding changes: the new canvas
  // composites the scene from its next frame, its size drives the aspect as
  // usual, and `newCanvas.getContext('scene')` answers this scene.

  /**
   * Show this scene on `canvas` (a <canvas> of the main document with no
   * other context, or none yet). The canvas it was on stops showing it and
   * may take a new scene of its own. Returns the scene. TypeError for a
   * non-canvas, a canvas with a 2d/webgl context, or one already showing
   * another scene.
   *
   * @param {HTMLCanvasElement} canvas
   * @returns {SceneGraph}
   */
  attachTo(canvas) {}

  /**
   * Park the scene on no canvas: it is not rendered (and costs no GPU time)
   * until attachTo, and it is not reclaimed with a canvas. A parked scene
   * lives until it is attached again or the page unloads — drop the last
   * reference to one and its GPU memory stays held until then.
   *
   * @returns {SceneGraph}
   */
  detach() {}

  /**
   * Whether the scene is on a canvas (false while parked).
   * @readonly
   * @type {boolean}
   */
  attached;

  /**
   * When true, the scene is parked (see detach) instead of destroyed when
   * its canvas leaves the DOM or is collected, ready for attachTo. Default
   * false: the scene is reclaimed with its canvas.
   * @type {boolean}
   */
  keepAlive;

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
   * Multisample the scene's colour and depth. 0 or 1 is off. The frame uses
   * the highest count at or below `samples` that the GPU supports (Apple GPUs
   * stop at 4); `msaa` reads back the request.
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
   * World-space pick ray through a canvas-local pixel. `x`/`y` are CSS
   * pixels relative to the canvas content box (top-left origin), e.g.
   * `ev.clientX - canvas.getBoundingClientRect().left`. Works for every
   * camera: `setCamera` (perspective or orthographic), `setCameraQuat`, and
   * an active camera node. Orthographic rays are parallel: `dir` is the
   * camera forward and `origin` slides across the view plane.
   *
   *     const r = canvas.getBoundingClientRect();
   *     const ray = scene.unprojectLocal(ev.clientX - r.left, ev.clientY - r.top);
   *     const hit = ray && scene.raycast(ray.origin, ray.dir, 100);
   *
   * @param {number} x
   * @param {number} y
   * @returns {?{ origin: number[], dir: number[] }} `dir` is unit length;
   *   null before the canvas has a size
   */
  unprojectLocal(x, y) {}

  /**
   * The inverse of unprojectLocal: the canvas-local CSS pixel a world point
   * renders at under the current camera. Add the canvas rect's left/top for
   * a page position (to click a 3D object from a test, or to place a DOM
   * label over it). Also takes one `[x, y, z]` array.
   *
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @returns {?{ x: number, y: number, depth: number, behind: boolean }}
   *   `depth` is the distance along the camera forward axis; `behind` is true
   *   at or behind the camera plane (x/y are then meaningless). null before
   *   the canvas has a size.
   */
  projectLocal(x, y, z) {}

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

