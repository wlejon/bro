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
 * camera projection). The per-type half — the create* option dictionaries
 * and the SceneNode members for meshes, shaders, LOD,
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
 *
 * The wind field (what vegetation bends with; see vegetation-api.js
 * "Vegetation records"). It blows along `direction`. Every field is optional and a
 * field left out keeps its value; strength / frequency above do not touch it.
 * @property {number} [speed] -  Steady wind in m/s (default 2.5, a breeze; 0 with gust 0 is a calm). It breathes ±28 % over a few minutes.
 * @property {number} [gust] -  Gust amount: 0 none, 1 a gusty day (default), 2 a gale's.
 * @property {number} [gustSpeed] -  How fast the gust bands travel downwind, m/s (default 9).
 * @property {number} [gustScale] -  A gust band's length along the wind in m (default 45; 1.85× that across it).
 * @property {number} [drift] -  Radians the direction wanders either side of `direction`, slowly (default 0.5).
 */

/**
 * @typedef {Object} WindSample
 * @property {Array<number>} velocity -  The wind [x, 0, z] in m/s at the point now: the steady speed plus 4 m/s per unit of gust, along the drifting direction with a little cross-wind wobble.
 * @property {number} gust -  The gust band over the point now (0 between bands, up to ~1.7 × the gust amount inside one).
 * @property {number} speed -  |velocity|.
 */

/**
 * @typedef {Object} SceneRaycastResult
 * @property {boolean} [hit]
 * @property {SceneNode} [node]
 * @property {number} [instance]
 * @property {Array<number>} [point]
 * @property {Array<number>} [normal]
 * @property {number} [distance]
 */

/**
 * The last frame's counters (`cullStats()`), and the world service's state.
 * The world service (scene/world/world_service.h) makes what does
 * not depend on the view (shadow pages, the atmosphere's tables) in the time
 * the frame leaves under its target (`setFrameTarget`), most wrong on screen
 * first.
 * @typedef {Object} SceneCullStats
 * @property {number} [totalNodes]
 * @property {number} [renderedNodes]
 * @property {number} [culledNodes]
 * @property {number} [shadowDrawn]   shadow casters drawn this frame (per caster × page view)
 * @property {number} [shadowCulled]  shadow casters culled
 * @property {number} [shadowPagesNeeded]    shadow pages the newest marking of the depth buffer named
 * @property {number} [shadowPagesResident]  of those, the pages drawn and sampled this frame
 * @property {number} [shadowPagesDrawn]     pages drawn into the cache this frame (0 in a still scene)
 * @property {number} [shadowPagesDynamic]   overlays drawn over the cached depth this frame, for what
 *   moves every frame (skinned or moving nodes, swaying plants)
 * @property {number} [shadowPagesWanted]    needed pages left for a later frame; a coarser (or finer)
 *   resident level shows in their place meanwhile
 * @property {number} [shadowPagesEvicted]   pages taken from others this frame (least recently used)
 * @property {number} [shadowPagesBuilding]  while the sun's next clipmap is built (after a turn past
 *   about 0.1 degree), its needed pages not drawn yet; it is shown, every shadow moving at once,
 *   when this reaches 0
 * @property {number} [shadowViews]          the views (page rects) the shadow pass drew
 * @property {number} [worldBudgetMs]   the world service's budget this frame: GPU ms the last frames left
 *   under the target (their heaviest of the last four, less the service's own share), never under a
 *   floor of 0.1 ms, and the floor after a frame over the target
 * @property {number} [worldSpentMs]    the jobs granted this frame, at each kind's cost measured from GPU
 *   timestamps
 * @property {number} [worldJobs]       jobs granted this frame
 * @property {number} [worldQueued]     jobs left for a later frame
 * @property {number} [worldWorstError] the largest error left queued, in pixels on screen (what is shown
 *   in its place); 0 when nothing waits
 * @property {Object<string, number>} [worldKindMs] each kind of job's GPU ms a unit by its name, as last
 *   measured (its guess until it is): `shadow pages`, ...
 * @property {number} [exposureEv]      the exposure applied (EV100, `setExposure`), read back from the GPU a
 *   few frames late
 * @property {number} [meteredEv]       auto exposure: the EV100 that frame's histogram metered (fixed: the setting)
 * @property {number} [exposureScale]   the multiplier the HDR frame got before the tone curve
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * The members every node has. Node-type members (meshes, shaders, LOD,
 * instances, splats, audio emitters, skeletal playback, billboards) continue
 * this class in docs/scene-nodes-api.js, the vegetation records' in
 * docs/vegetation-api.js.
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
   * Getter returns `number` (`rotationZ` in radians); setter accepts `number` (sets `rotationZ`) or `Array<number>` (`[rx, ry, rz]` Euler radians or `[qx, qy, qz, qw]` quaternion).
   * @type {number|Array<number>}
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
   * A medium over the whole view: clouds, fog banks, smoke, light shafts.
   * Not a surface: each frame it runs one triangle over the view whose
   * fragment chunk defines
   *
   *   void userVolume(vec3 dir, float sceneDist, inout vec3 radiance,
   *                   inout float transmittance, inout float depth)
   *
   * and marches the pixel's ray: `dir` unit and camera-relative (the eye at
   * the origin; `camera.eyeWorld` is its absolute position), up to
   * `sceneDist`, the opaque surface's distance along the ray (1e30 over the
   * sky). It sets `radiance`, the light the medium sends the eye (as it
   * leaves the medium), `transmittance` (from 1), the share of what lies
   * behind that comes through, and `depth`, the distance its light comes
   * from. The pixel becomes scene * transmittance + airT * radiance +
   * (1 - transmittance) * airL, airT and airL the air between the eye and
   * `depth` (aerial perspective, else fog), whose own light the scene behind
   * already carries. The chunk may call `sceneToSun()` (unit, to the sun),
   * `volumeSunLight(pos)` (the sun's illuminance at camera-relative pos,
   * through the air) and `volumeSkyLight(pos, n)` (the sky's irradiance
   * over pi on a surface facing n). The volumes draw, in tree order, into a
   * target of their own (each over the ones before it), which is
   * reconstructed over renders before it lands on the scene, after the
   * opaque surfaces, their sky and the decals, before SSR and translucent
   * surfaces: last render's result is found again where `depth`
   * reprojects the pixel to (the sky's direction where nothing set it),
   * kept within the range of the pixel's neighbours this render, and
   * blended in at 90%. So a march may offset its steps per pixel and per
   * render (`camera.frame.x`, the render's index mod 65536) and its grain
   * averages out, where a fixed offset is a fixed grain. A render that
   * does not follow the last (the first, a resize, another set of
   * volumes) is its own alone.
   * It casts no shadow and is never culled. The incident exposure meter
   * (setExposure) reads it: once a frame userVolume also runs from the eye
   * along four rays in a cone of 3 degrees about the sun (the share they
   * pass dims the sun the meter reads) and four cosine-weighted over the
   * sky's hemisphere (the share they pass dims the sky's light, and their
   * light down counts as an overcast sky's), so under an overcast deck the
   * meter reads the deck's light, not the sun it hides (the first 8
   * volumes in tree order). The global `volumeMeterRay` is true during
   * those calls, so a medium whose surfaces light themselves by a model of
   * it (an average over a footprint) can answer the meter by that model. Its uniforms, `buffer`
   * declarations and textures are a custom shader's (setShader):
   * setShaderUniform, setShaderBuffer and setShaderTexture reach them.
   * Throws TypeError without a fragment chunk and Error with the GLSL log
   * when it fails to compile (or defines no userVolume).
   *
   * @param {{fragment: string, uniforms?: Object<string, number|number[]>, name?: string}} opts
   * @returns {SceneNode}
   * @example
   * // Uniform haze, 1 km visibility, lit by the sun and the sky.
   * scene.createVolume({
   *   fragment: `uniform float u_sigma;
   *     void userVolume(vec3 dir, float sceneDist, inout vec3 radiance,
   *                     inout float transmittance, inout float depth) {
   *       float d = min(sceneDist, 5000.0);
   *       transmittance = exp(-u_sigma * d);
   *       vec3 p = dir * d * 0.5;
   *       radiance = (volumeSunLight(p) / 12.57 + volumeSkyLight(p, vec3(0, 1, 0))) * (1.0 - transmittance);
   *       depth = d * 0.5;
   *     }`,
   *   uniforms: { u_sigma: 0.003 },
   * });
   */
  createVolume(opts) {}

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
   * The tone curve (lighting-api.js, ToneMapConfig).
   * @param {ToneMapConfig} [opts]
   */
  setToneMap(opts) {}

  /**
   * Exposure in EV: fixed (the default, a scale of 1) or metered from the
   * frame on the GPU with eye adaptation (lighting-api.js, ExposureConfig,
   * which also lists physical light values).
   *
   * @example
   *   scene.setExposure({ compensation: 0.5 });           // half a stop brighter, fixed
   *   scene.setExposure({ mode: 'auto' });                // metered, adapting
   *   scene.setExposure({ mode: 'fixed', ev: 15 });       // a sunny-16 still
   * @param {ExposureConfig} [opts]
   */
  setExposure(opts) {}

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
   * The same call sets the wind field plants carrying vegetation records
   * sway in (`speed`, `gust`, `gustSpeed`, `gustScale`, `drift`): a steady
   * wind whose direction drifts slowly, with gusts that travel downwind as
   * bands, so a gust visibly crosses a grove tree by tree. Until set it is a
   * light breeze (2.5 m/s, gusty) along +x; `setWind({speed: 0, gust: 0})`
   * stills it.
   *
   * @example
   *   scene.setWind({ direction: [1, 0, 0.4], speed: 4, gust: 1.5 });   // a gusty afternoon
   *   scene.setWind({ speed: 0, gust: 0 });                             // dead calm
   * @param {WindConfig} [opts]
   */
  setWind(opts) {}

  /**
   * The wind field at a world point now, exactly as the vegetation shaders
   * evaluate it (the same noise and formulas in float), so a script can make
   * particles, flags or sounds follow the gust that is bending the trees.
   * `windAt(x, y, z)`, `windAt([x, y, z])` or `windAt(x, z)`; the height is
   * ignored.
   *
   * @example
   *   const w = scene.windAt(tree.position);
   *   if (w.gust > 0.5) rustle.play();
   * @param {number|Array<number>} x
   * @param {number} [y]
   * @param {number} [z]
   * @returns {WindSample}
   */
  windAt(x, y, z) {}

  /**
   * Everything setWind has set, plus the field now: `{direction, strength,
   * frequency, speed, gust, gustSpeed, gustScale, drift, time, now:
   * {direction, speed}}`. `now.direction` is the drifted direction and
   * `now.speed` the breathing steady speed at the wind clock's `time`.
   * @returns {Object}
   */
  wind() {}

  /**
   * What the scene still prepares for its view, as of its newest render: one
   * line each (auto exposure still adapting, pipelines still being built off
   * the frame thread, ...). [] once what the
   * view shows is ready. The heavy first-time work runs off the frame, so a
   * page shows its own progress from this and holds back views (or warms
   * them) until it empties; headless's perf.settle waits on the same list.
   *
   * @example
   *   const status = document.querySelector('#status');
   *   const show = () => { const p = scene.pendingWork(); status.textContent = p[0] ?? ''; };
   *   scene.addEventListener('ready', () => { status.hidden = true; });
   * @returns {string[]}
   */
  pendingWork() {}

  /**
   * `'ready'`: pendingWork() emptied (read each animation frame while a
   * listener waits), `{type: 'ready', pending: []}`; fired again each time
   * new work (a new view's) comes and goes.
   * @param {'ready'} type
   * @param {function(Object)} listener
   */
  addEventListener(type, listener) {}
  removeEventListener(type, listener) {}

  /**
   * The receivers' shadow filter (see ShadowQualityConfig in
   * docs/lighting-api.js for how shadows are cached).
   *
   * @param {ShadowQualityConfig} [opts]
   */
  setShadowQuality(opts) {}

  /**
   * @param {FogConfig} [opts]
   */
  setFog(opts) {}

  /**
   * The atmosphere: the sky, the haze, the sun's and the sky's light, one
   * model from the ground to orbit (lighting-api.js, AtmosphereConfig).
   *
   * @example
   *   sun.intensity = 128000;                          // lux above the air
   *   scene.setAtmosphere({ groundAlbedo: [0.1, 0.13, 0.05] });
   *   scene.setExposure({ mode: 'auto' });
   *   scene.setAtmosphere(null);                       // off
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
   * The light the ground throws back up: `color` is the albedo of a level
   * ground (linear rgb, e.g. a meadow's `[0.12, 0.16, 0.07]`), lit by the
   * dominant directional light and the environment's sky, and it lights
   * every surface's lower hemisphere on top of the environment. A panorama's
   * ground is rarely the scene's (a sky-only one has none), and under a high
   * sun this bounce is most of the light in the shade on anything upright:
   * a trunk under its crown, a wall, a leaf's underside. Null or `[0, 0, 0]`
   * (the default): none. Needs an environment (setEnvironment).
   *
   * @param {number[]|null} color
   */
  setGroundBounce(color) {}

  /**
   * Dither the tonemapped image as it is written at 8 bits a channel. On by
   * default: a triangular noise of one level, fixed per pixel, so slow
   * gradients (a sky, haze over far hills) do not band into plateaus one
   * level apart. Off gives the exact quantised values, for a test that reads
   * single pixels (`captureFrame`).
   *
   * @param {boolean} enabled
   */
  setDither(enabled) {}

  /**
   * The frame's GPU target in milliseconds: 2.78 by default (360 fps on an
   * RTX 4090), 16.7 for 60 fps. The world service spends what each frame
   * leaves under it on world work (shadow pages, the atmosphere's tables),
   * and only its floor while frames run over it; `cullStats()`'s `world*`
   * counters show its state. A value of 0 or less restores the default.
   *
   * @param {number} ms
   */
  setFrameTarget(ms) {}

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
   * Image-based lighting and sky from an HDR panorama (`panorama` or `hdr`),
   * plus its `intensity` and `rotation`; null clears it. The panorama is
   * decoded now and baked on the next frame.
   * @param {EnvironmentConfig} [opts]
   * @returns {boolean} false when the panorama does not decode or is larger
   *   than the GPU's image limit (the previous environment stays)
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
   * @param {number} [maxDist]
   * @returns {SceneRaycastResult|null}
   */
  raycast(origin, direction, maxDist) {}

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
   * @param {number} [width]
   * @param {number} [height]
   * @returns {ImageData}
   */
  captureFrame(width, height) {}

  /**
   * Inspection: from the next render on, each render's depth and result are
   * read back with it (in its own submission, no extra one) along with its
   * camera on the body, for inspectFrame(). A tool for finding what changes
   * on the world between frames that should not: pops, morphs, surfaces
   * that vanish. Costs a readback a frame; off by default. Turning it on or
   * off forgets the render compared against.
   * @param {boolean} on
   */
  setInspect(on) {}

  /**
   * The last render compared with the one inspected before it. Every pixel's
   * point on the body (from its depth and the render's camera) is carried to
   * where it lands in the earlier render and compared there:
   * - `same`: the earlier render saw that surface there (within ~2.5 pixel
   *   footprints); its normal (from the depth) and luma are compared
   *   (`normalDeg`, `normalMoved` past 15°, `luma`, `lumaMoved` past 0.08).
   * - `rose`: something now stands in front of what was seen there;
   *   `sank`: what was seen there is gone from in front (disocclusion too).
   * - `appeared`: a surface where sky was; `vanished`: sky where a surface
   *   was; `newView`: outside the earlier view; `sky`: sky in both
   *   (`skyLuma`, its luma change: clouds against the sky).
   * Fractions are of all the pixels. The camera's `turnDeg`, signed
   * `rollDeg` and `travel` (m) between the two renders; `medianDist` (m) of
   * the surfaces, and `nearDist` (m) their tenth nearest (a tenth of them
   * nearer: how cluttered the view is close by); `lumaSpread` the frame's
   * luma's standard deviation (0 .. 0.5: near 0 a blank view, the eye inside
   * a cloud or a fog). `bands`: four bands top
   * to bottom, each `moved`
   * (rose + sank + appeared + vanished), `normalMoved` and `lumaMoved` as
   * fractions of the band's pixels.
   *
   * With `image`, `.image` is that picture of the render (top-down RGBA,
   * `width` x `height`):
   * - `'change'`: over a dim copy of the frame, red = normal change (full
   *   at 20°), green = luma change (full at 0.15), blue = rose, magenta =
   *   sank, cyan = appeared, orange = vanished.
   * - `'normals'`: the depth's normals in the camera's axes (x right, y up,
   *   z toward the eye) as rgb = n * 0.5 + 0.5; sky black.
   *
   * Draws a frame a flush left pending first, like every read. null when
   * inspection is off or nothing was rendered since the last call; the
   * first call after setInspect has `compared` false.
   * @param {{ image?: 'change' | 'normals' }} [opts]
   * @returns {?{ width: number, height: number, compared: boolean, same: number,
   *   rose: number, sank: number, appeared: number, vanished: number,
   *   newView: number, sky: number, normalDeg: number, normalMoved: number,
   *   luma: number, lumaMoved: number, skyLuma: number, turnDeg: number,
   *   rollDeg: number, travel: number, medianDist: number, nearDist: number,
   *   lumaSpread: number, bands: { moved: number, normalMoved: number, lumaMoved: number }[],
   *   image?: Uint8Array }}
   * @example
   * scene.setInspect(true);
   * // ... per frame, after advancing time:
   * const s = scene.inspectFrame({ image: 'change' });
   * if (s && s.compared && s.rose + s.sank > 0.02) console.log('a pop at', t);
   */
  inspectFrame(opts) {}

  /**
   * Inspection as a recording: the renders `keepInspect` names are written
   * to `path` (replaced), each its depth, luma and camera, cut `scale`
   * times each way (default 2), by a thread of the engine's own while the
   * scene goes on; nothing is compared until `inspectRecording` asks. A
   * frame kept costs a few ms of the CPU, so a flight records at its own
   * pace, and the recording can be asked again without flying. Each render
   * is copied out while a recording goes on, as with setInspect. Throws when
   * the file cannot be made.
   * @param {string} path a real filesystem path (bro.resolvePath)
   * @param {{ scale?: number }} [opts]
   * @example
   * scene.recordInspect(bro.resolvePath('/app/captures/run.inspect'));
   * // ... per frame, after advancing time:
   * scene.keepInspect();
   * // ... then
   * scene.stopInspectRecording();
   * const { frames } = scene.inspectRecording(bro.resolvePath('/app/captures/run.inspect'));
   * frames.forEach((s, i) => { if (s.compared && s.rose + s.sank > 0.02) console.log('a pop at frame', i); });
   */
  recordInspect(path, opts) {}

  /**
   * Keep the last render in the recording (recordInspect). False when none
   * is going on, or nothing was rendered since the last one kept.
   * @returns {boolean}
   */
  keepInspect() {}

  /**
   * Finish the recording: what was kept written, the file closed.
   * @returns {number} the frames kept
   */
  stopInspectRecording() {}

  /**
   * Every frame of a recording (recordInspect) compared with the one kept
   * before it, as inspectFrame compares two renders (the first frame's
   * `compared` false), the frames shared out over the CPU's threads. With
   * `image`, `images` holds that picture of each of `frames` (indices into
   * the recording), at the recording's size. Throws when the file is not a
   * recording.
   * @param {string} path
   * @param {{ image?: 'change' | 'normals', frames?: number[] }} [opts]
   * @returns {{ frames: Object[], images?: (Uint8Array|null)[] }}
   */
  inspectRecording(path, opts) {}

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

