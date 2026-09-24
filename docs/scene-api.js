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
 * An octahedral impostor atlas: a cols x rows grid of views of one object,
 * each cell seen from the direction its octahedral coordinate encodes, in a
 * width x height RGBA8 image (alpha < 0.5 is cut out). `bounds` is the
 * object's bounding sphere in its local frame.
 * @typedef {Object} ImpostorAtlasInfo
 * @property {number} width
 * @property {number} height
 * @property {number} cols
 * @property {number} rows
 * @property {{center: Array<number>, radius: number}} [bounds]  Default a unit sphere at the origin.
 * @property {Uint8Array|Uint8ClampedArray} atlasRGBA  width * height * 4 bytes.
 */

/**
 * @typedef {Object} ImpostorOptions
 * @property {number} [margin]  Billboard half-extent as a multiple of the bounds radius (default 1.03).
 * @property {number} [cullNear]  Distance where the dithered fade-out starts (default 450).
 * @property {number} [cullFar]  Distance past which billboards are gone (default 950).
 */

/**
 * @typedef {Object} ImpostorResult
 * @property {SceneNode} node  The one MeshNode carrying every billboard.
 * @property {number} quadCount  Billboards built.
 * @property {function(number, number): void} setCull  Change (cullNear, cullFar) live.
 */

/**
 * Draw many copies of an object as camera-facing octahedral impostors in a
 * single MeshNode: one quad per transform, each picking the atlas cell for
 * the view direction and fading out by dither between cullNear and cullFar.
 * `transforms` is 9 floats per copy (px py pz, qx qy qz qw, scale, unused;
 * the setInstancesFromTransforms layout), of which position and scale are
 * used. The atlas color is drawn as emission; casts no shadow. Needs GPU rendering (it
 * installs a custom shader).
 *
 * @param {SceneGraph} scene
 * @param {ImpostorAtlasInfo} atlas
 * @param {Float32Array|number[]} transforms
 * @param {ImpostorOptions} [opts]
 * @returns {ImpostorResult}
 */
bro.impostor.createLayer = function(scene, atlas, transforms, opts) {};

/**
 * Geometry comes from, in order: raw `positions` + `indices` (with optional
 * `normals`, `colors`, `uvs`, `tangents`; see MeshGeometry), a Mesh object in
 * `mesh` or `data`, or a primitive named by `mesh`:
 *
 *   'box'      halfW, halfH, halfD (0.5 each)
 *   'sphere'   radius (0.5), segments (16), rings (12)
 *   'cylinder' radius (0.5), halfHeight (0.5; or `height`, the full extent), segments (16)
 *   'capsule'  radius (0.5), halfHeight (0.5; or `height`), segments (16), rings (8)
 *   'plane'    halfW (5), halfD (5), subdivX (1), subdivZ (1)
 *   'torus'    majorRadius (1), minorRadius (0.3), majorSegments (24), minorSegments (12)
 *
 * Each count is capped at 4096 (the bound bromesh's Mesh.sphere etc. enforce);
 * one below the primitive's minimum (3 segments, 2 sphere rings) gives an
 * empty mesh. Any other name is a box.
 *
 * @typedef {Object} MeshNodeOptions
 * @property {Mesh|string} [mesh] - A Mesh object, or a primitive name (above).
 * @property {Mesh} [data] - Alias of `mesh` for a Mesh object.
 * @property {string|Array<number>} [color] - CSS colour or [r, g, b(, a)] in 0..1.
 * @property {{metallic?: number, roughness?: number}} [material] - PBR params; the flat keys below win.
 * @property {number} [metallic]
 * @property {number} [roughness]
 * @property {number} [emissive] - Emissive intensity; tinted by `emissiveColor`, else the base colour.
 * @property {string|Array<number>} [emissiveColor]
 * @property {boolean} [unlit]
 * @property {boolean} [twoSided] - `doubleSided` is accepted as the glTF spelling.
 * @property {number} [subsurface]
 * @property {number} [alphaCutoff]
 * @property {boolean} [vertexColorTint]
 * @property {'triangles'|'lines'} [drawMode]
 * @property {number} [lineWidth]
 * @property {boolean|number} [wind] - Wind sway: true is 1, or a [0, 1] multiplier.
 * @property {boolean} [castsShadow]
 * @property {boolean} [receivesShadow]
 * @property {number|Array<number>} [depthBias] - Units, or [factor, units].
 * @property {{width: number, height: number, data: Uint8Array}} [texture] - RGBA8 base colour map;
 *   also normalTexture, metallicRoughnessTexture, occlusionTexture, emissiveTexture. A map whose
 *   data is shorter than width*height*4 bytes is ignored.
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
 * @property {Mesh|string} [mesh] - A Mesh object (or raw `positions`/`indices`/... streams as in
 *   MeshGeometry), or a primitive name read exactly as MeshNodeOptions reads it.
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

  // ── Custom shaders (MeshNode, SkinnedMeshNode, InstancedMeshNode) ────────

  /**
   *  True while a custom shader from setShader() is installed.
   * @readonly
   * @type {boolean}
   */
  hasShader;

  /**
   * Splice GLSL into the node's PBR program. `vertex` must define
   * `void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv)`,
   * run on the object-space position (after skinning and wind; for an
   * InstancedMesh, in mesh-local space before the per-instance transform)
   * so lighting, fog and shadows follow the displacement. `fragment` must
   * define `void userFragment(inout vec3 baseColor, inout vec3 normal,
   * inout float metallic, inout float roughness, inout vec3 emissive,
   * inout float alpha)`, run after every material input is gathered and
   * before lighting (`normal` is world space and renormalised after). At
   * least one is required. User uniforms and samplers are declared in the
   * chunk and must be named `u_*`; `uniforms` gives their initial values
   * (a number or an array of up to 4 numbers). Throws TypeError on bad
   * options and Error with the GLSL log when the program fails to compile.
   * Needs GPU rendering.
   *
   * @param {{vertex?: string, fragment?: string, uniforms?: Object<string, number|number[]>}} opts
   * @returns {SceneNode}
   * @example
   * box.setShader({
   *   fragment: `uniform vec3 u_tint;
   *     void userFragment(inout vec3 baseColor, inout vec3 normal,
   *                       inout float metallic, inout float roughness,
   *                       inout vec3 emissive, inout float alpha) {
   *       emissive = u_tint;
   *     }`,
   *   uniforms: { u_tint: [1, 0, 0] },
   * });
   */
  setShader(opts) {}

  /**
   * Set one `u_*` uniform of the custom shader: a number, or 1 to 4 numbers
   * (float, vec2, vec3, vec4). Extra components are ignored.
   *
   * @param {string} name
   * @param {number|number[]|Float32Array} value
   * @returns {SceneNode}
   */
  setShaderUniform(name, value) {}

  /**
   * Upload a single-channel float (R32F) texture for a `uniform sampler2D
   * u_*` in the custom shader (MeshNode only). `data` holds width * height
   * floats, row-major. With `x`/`y` given it writes a sub-rectangle of an
   * existing slot instead (width * height floats for the rect alone; a rect
   * outside the texture is ignored). A zero width or height releases the
   * slot. `mipmap` builds a mip chain for textureLod(). A data array
   * shorter than the extent is a RangeError. Returns the node.
   *
   * @param {string} name
   * @param {{data: Float32Array|number[], width: number, height: number, x?: number, y?: number, mipmap?: boolean}} opts
   * @returns {SceneNode}
   */
  setShaderTexture(name, opts) {}

  /**
   * Remove the custom shader; the node draws with the stock program again.
   * @returns {SceneNode}
   */
  clearShader() {}

  // ── Level of detail and distance gating ─────────────────────────────────

  /**
   * Install a discrete LOD chain on a MeshNode (not skinned): each frame
   * the level whose `maxDist` first exceeds the camera distance to the
   * node's origin draws, in every pass; beyond the last the coarsest keeps
   * drawing (pair with setVisibilityRange to cull). The base mesh stays the
   * raycast source. An empty array clears the chain.
   *
   * @param {Array<{maxDist: number, mesh: {positions: ArrayLike<number>, normals?: ArrayLike<number>, indices: ArrayLike<number>}}>} lods
   * @returns {SceneNode}
   */
  setLodMeshes(lods) {}

  /**
   *  Levels in the LOD chain (0 without one).
   * @readonly
   * @type {number}
   */
  lodCount;

  /**
   *  The level selected this frame.
   * @readonly
   * @type {number}
   */
  lodLevel;

  /**
   * Render the node (and its subtree) only while begin <= d < end, d being
   * the camera's distance to the node's world origin. `margin` adds
   * hysteresis on both edges so a camera on a boundary does not flicker.
   * Independent of `visible`; raycasts are not gated.
   *
   * @param {number} begin
   * @param {number} end
   * @param {number} [margin]
   * @returns {SceneNode}
   */
  setVisibilityRange(begin, end, margin) {}

  /**
   * Remove the distance gate.
   * @returns {SceneNode}
   */
  clearVisibilityRange() {}

  /**
   *  The distance gate, or null without one. Assigning an object sets it,
   *  assigning null clears it.
   * @type {{begin: number, end: number, margin: number}|null}
   */
  visibilityRange;

  // ── Instances (InstancedMeshNode) ───────────────────────────────────────

  /**
   * Replace every instance. With a length that is a multiple of 16, each
   * instance is 16 floats: a row-major 4x3 affine [m00 m01 m02 tx, m10 m11
   * m12 ty, m20 m21 m22 tz] then an RGBA tint that multiplies the material
   * color. Otherwise, with a multiple of 9, it is the setInstancesFromTransforms
   * layout. (A length divisible by both is read as 16-float records.)
   *
   * @param {Float32Array|number[]} data
   * @returns {SceneNode}
   */
  setInstances(data) {}

  /**
   * Replace every instance from 9 floats each: px py pz, qx qy qz qw,
   * uniform scale, variant index (packed into the tint's alpha, picking the
   * atlas cell when an atlas grid is set; RGB is white).
   *
   * @param {Float32Array|number[]} data
   * @returns {SceneNode}
   */
  setInstancesFromTransforms(data) {}

  /**
   *  Number of instances.
   * @readonly
   * @type {number}
   */
  instanceCount;

  // ── Textures from pixels ────────────────────────────────────────────────

  /**
   * Base-color texture for a MeshNode or DecalNode. `src` is an RGBA8
   * image ({data, width, height}, e.g. an ImageData; data holds width *
   * height * 4 bytes, shorter is a RangeError), another SceneGraph (or its
   * asTexture()) whose live output is sampled each frame, or null to clear.
   *
   * @param {{data: Uint8Array|Uint8ClampedArray, width: number, height: number}|SceneGraph|Object|null} src
   * @returns {SceneNode}
   */
  setBaseColorTexture(src) {}

  /**
   * Emission texture for a MeshNode or DecalNode from an RGBA8 image
   * ({data, width, height}; shorter data is a RangeError), or null to clear.
   *
   * @param {{data: Uint8Array|Uint8ClampedArray, width: number, height: number}|null} src
   * @returns {SceneNode}
   */
  setEmissionTexture(src) {}

  /**
   * Replace a Gaussian-splat node's cloud from structure-of-arrays data
   * (the shape bro.triposplat returns): for N splats, positions N*3, scales
   * N*3, rotations N*4 (quaternions), opacities N, and sh N*(shDegree+1)^2*3
   * spherical-harmonic coefficients; shDegree is clamped to [0, 3]. Streams
   * that do not all describe the same N are a TypeError.
   *
   * @param {{positions: Float32Array, scales: Float32Array, rotations: Float32Array, opacities: Float32Array, sh: Float32Array, shDegree?: number}} cloud
   * @returns {SceneNode}
   */
  setCloud(cloud) {}

  // ── Spatial audio ───────────────────────────────────────────────────────

  /**
   * Make an engine voice follow this node: spatialization is turned on for
   * it, and every frame its position and velocity are set from the node's
   * world origin. While a scene's listener is bound to its camera
   * (SceneGraph.bindAudioListenerToCamera) the voice is also muffled when
   * scene geometry lies between camera and node. `handle` is a voice id
   * (OscillatorNode.voiceId) with isVoice true; with isVoice false it is an
   * engine playback handle. One emitter per node (attaching again replaces
   * it); the link ends when the node is destroyed. A negative handle is
   * ignored.
   *
   * @param {number} handle
   * @param {boolean} [isVoice]
   */
  attachAudioEmitter(handle, isVoice) {}

  /** Stop driving the attached voice from this node. */
  detachAudioEmitter() {}

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

