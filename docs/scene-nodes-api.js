// ── bro.scene: node types ────────────────────────────────────────────────────
//
// The per-type half of the scene graph: the option dictionaries the
// `SceneGraph.create*` factories read (meshes, skinned and instanced meshes,
// Gaussian splats, HTML / shape / sprite billboards, lights, 2D and 3D
// particles, decals, reflection probes, grass), octahedral impostors
// (`bro.impostor`), and the `SceneNode` members that apply only to some node
// types: custom shaders, LOD and distance gating, instances, textures from
// pixels, splat clouds, spatial-audio emitters and skeletal playback.
//
// The graph itself — `SceneGraph`, the node hierarchy, transforms, cameras,
// render settings, raycasts and capture — is docs/scene-api.js; the
// `SceneNode` below continues the class declared there. Lighting configs are
// docs/lighting-api.js, clips / blend spaces / state machines
// docs/animation-api.js, skeletons and skins docs/rigging-api.js.

// ── Dictionaries ─────────────────────────────────────────────────────────────

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
 * @property {boolean} [vertexColorTint] - Unset: a mesh's `colors` replace the albedo. `true`: the albedo is
 *   `color` × the vertex colour, so baked shading survives a runtime `node.color` change. `false`: ignore them.
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
 * @property {Float32Array} [vegetation] - wind-sway records, 16 floats per instance (see "Vegetation")
 * @property {Array<number>} [position]
 * @property {Array<number>} [rotation]
 * @property {Array<number>} [scale]
 * @property {boolean} [visible]
 */

/**
 * @typedef {Object} GaussianSplatNodeOptions
 * @property {string} [path]
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
 * A 2D particle emitter (drawn with the 2D scene layer, like shapes and
 * sprites). Units are canvas pixels and seconds; angles are degrees, 0 = +x,
 * -90 = up. The node reads back `liveCount` (alias `particleCount`),
 * `isPlaying` and `rate` (settable), and takes `burst(n)`, `clear()`,
 * `play()`, `stop()`.
 *
 * @typedef {Object} ParticleNodeOptions
 * @property {number} [maxParticles=256] - hard cap on live particles; extra emits drop
 * @property {string} [texture] - app-relative image; absent draws filled circles
 * @property {'normal'|'additive'} [blend='normal']
 * @property {number} [rate=0] - particles emitted per second while playing
 * @property {number} [burst] - particles emitted once, at creation
 * @property {number|{min:number,max:number}} [lifetime={min:0.5,max:1}] - seconds
 * @property {{angle?:number, angleSpread?:number, speed?:number, speedSpread?:number}} [velocity]
 *   - defaults -90 / 360 / 100 / 0 (px per second)
 * @property {Array<number>|{x:number,y:number}} [gravity] - px/s² ([x, y] or {x, y})
 * @property {number|{start:number,end:number}} [size] - diameter in px over the lifetime
 * @property {string|Array<number>|{start:(string|Array<number>), end:(string|Array<number>)}} [color]
 *   - CSS colour or [r,g,b,a] 0..1; `end` defaults to `start` at alpha 0
 * @property {{start?:number, spinSpeed?:number, spinSpread?:number}} [rotation] - degrees, degrees/s
 * @property {number} [drag] - per-second velocity multiplier (1 = none)
 * @property {boolean} [autoplay=true] - false: emit only on burst() until play()
 * @property {Array<number>} [position] - [x, y(, z)]; or `x` / `y`
 * @property {string} [name]
 * @property {boolean} [visible]
 */

/**
 * A world-space 3D particle system: camera-facing quads, soft round points
 * unless `texture` is set, drawn into the HDR pass so additive systems bloom.
 * Beyond `burst(n)`, `clear()`, `play()`, `stop()`, `rate` and `liveCount`,
 * the node takes:
 *
 *   emit({ count, position, direction, spread, speed, speedSpread,
 *          sizeScale, lifeScale, tint })
 *     spawns `count` particles with per-call overrides, so one pooled system
 *     serves every one-shot effect of its kind. `position` is world space and
 *     replaces the node transform; `direction`/`spread` (degrees, full width)
 *     replace the launch cone; `sizeScale` and `lifeScale` multiply the size
 *     curve and lifetime of these particles only; `tint` [r, g, b, a]
 *     multiplies their colour over life, and rgb above 1 pushes additive
 *     systems past the bloom threshold. A packed Float64Array of 16 numbers
 *     (count, position xyz, direction xyz, spread, speed, speedSpread,
 *     sizeScale, lifeScale, tint rgba) is accepted to skip the object.
 *   paused (boolean): freezes the simulation, ages included; emit() and
 *     burst() still add. `pause()` / `resume()` set it.
 *
 * @typedef {Object} Particles3DNodeOptions
 * @property {number} [maxParticles]
 * @property {number} [seed]
 * @property {number} [rate] - particles per second while playing
 * @property {number} [burst] - emitted once at creation
 * @property {number|{min:number,max:number}} [lifetime]
 * @property {number|{start:number,end:number}} [size] - world-space diameter over life
 * @property {{start:(string|Array<number>), end:(string|Array<number>)}} [color]
 * @property {'normal'|'additive'} [blend]
 * @property {'world'|'local'} [space]
 * @property {{direction?:Array<number>, spread?:number, speed?:number, speedSpread?:number}} [velocity]
 * @property {Array<number>} [gravity]
 * @property {number} [drag] - per-second velocity multiplier (1 = none)
 * @property {{start?:number, spinSpeed?:number, spinSpread?:number}} [rotation]
 * @property {{type?:'point'|'sphere'|'hemisphere'|'box'|'cone', radius?:number}} [shape]
 * @property {number} [softness] - soft-particle depth fade in world units
 * @property {number} [duration]
 * @property {boolean} [loop]
 * @property {boolean} [autoplay]
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
 * A local reflection probe: captures the sky and opaque meshes around its
 * position, lit and shadowed, into a GGX-prefiltered cube that replaces the
 * global environment's specular for meshes inside its box (`size`).
 * `updateMode` 'once' (default) captures on the first visible frame,
 * 'manual' on `capture()`; `boxProjection` (default true) parallax-corrects
 * against the box.
 * @typedef {Object} ReflectionProbeNodeOptions
 * @property {Array<number>} [size]
 * @property {number} [resolution]
 * @property {Array<number>} [position]
 */

/**
 * Grass: a meadow of procedural blades over a ground, from
 * `scene.createGrass(opts)`. No blade carries data: rings of cells around the
 * camera are drawn as instanced blades (fine and close near, coarser and
 * wider further out, cross-faded), each placed, culled, bent and coloured in
 * the vertex shader from its cell. They sway in the scene's wind field
 * (`setWind`: the steady lean, each blade's flutter and the travelling gust
 * bands, which lift the sheen of the blades they bend), are lit mostly as the
 * ground they cover, and are parted and flattened by movers (`addMover`,
 * `push`). The node is left out of raycasts and bounds; its transform is
 * ignored.
 *
 * What the node draws over is a FIELD baked once from these options: a grid
 * over `area` every `cell` metres of (ground height, density, dryness, height
 * multiplier). `density`, `dry` and `tall` are numbers or functions
 * `(x, z, y) => value`, called once per field texel at bake time, so they can
 * read the same noise an app colours its ground with. Re-bake with
 * `node.setGround(opts)` (a moved or edited ground, a new season).
 *
 * @example
 *   const ground = scene.createMesh({ data: Mesh.heightmapGrid(h, N, N, cell) });
 *   const grass = scene.createGrass({
 *     ground,                                  // the blades stand on it
 *     dry: (x, z) => patches(x, z),            // 0 lush .. 1 straw
 *     density: (x, z) => nearTrunk(x, z) ? 0.3 : 1,
 *     tall: (x, z) => 1.2 - 0.4 * patches(x, z),
 *   });
 *   grass.addMover(player, { radius: 0.5, trample: 1 });   // parts it, leaves a lane
 *
 * @typedef {Object} GrassNodeOptions
 * @property {SceneNode|Object|Function} [ground] - what the blades stand on:
 *   a mesh node (its top surface on the field's grid, `sampleHeights`;
 *   texels off the mesh grow nothing), a terrain
 *   (`heightAt`), a clipmap terrain (`renderedElevationAt`), or a function
 *   `(x, z) => y`. Default: flat ground at y = 0.
 * @property {Array<number>} [area] - `[x0, z0, x1, z1]` the field covers;
 *   default the mesh's world bounds, else `[-50, -50, 50, 50]`.
 * @property {number} [cell=0.5] - field texel spacing (m): the detail of
 *   the ground's shape and of the masks.
 * @property {number|Function} [density=1] - where grass grows, 0..1.
 * @property {number|Function} [dry=0] - 0 lush .. 1 sun-dried.
 * @property {number|Function} [tall=1] - height multiplier.
 * @property {number} [height=0.4] - mean blade height (m); clumps run
 *   from about half to one and a half times it, and lush tall patches higher.
 * @property {number} [width=0.005] - blade width (m) on top of each ring's
 *   own (near blades are about 2 cm wide).
 * @property {number} [bladeDensity=1] - blades per area, x the default
 *   (one per 6 cm square near the camera, before the clumps thin them).
 * @property {number} [range=150] - the far edge of the outermost ring (m).
 * @property {number} [minNormalY=0.62] - no grass on ground steeper than
 *   this (the ground normal's y); or `maxSlope` in degrees.
 * @property {Array<number>|string} [lush] - lush blade colour, linear
 *   `[r, g, b]` or `'#rrggbb'` (default `[0.12, 0.17, 0.03]`).
 * @property {Array<number>|string} [dryColor] - sun-dried colour (default
 *   `[0.42, 0.31, 0.09]`).
 * @property {number} [bulk=0.72] - how much of a blade's shading follows
 *   the ground's normal (1 = lit exactly as the ground).
 * @property {number} [clump=0.21] - clump noise frequency (1/m).
 * @property {number} [rootShade=0.45] - the root's colour, x the blade's.
 * @property {number} [sheen=0.35] - how much a gust lightens the blades it bends.
 * @property {number} [variety=1] - large-scale height variation, 0..1.
 * @property {number} [wind=1] - response to the wind, x.
 * @property {Array<number>|string} [color] - the node colour, multiplied in (default white).
 * @property {number} [roughness=0.7]
 * @property {boolean} [castsShadow=false] - the near ring's blades cast (a
 *   cost: the shadow map then re-renders every frame).
 * @property {boolean} [receivesShadow=true]
 */

/**
 * A grass mover's push: `radius` (m, default 0.6), `strength` (default 1:
 * how hard blades lean away and along its motion; they spring back in about
 * half a second) and `trample` (0..1, default 0: how flat it leaves the grass
 * it crosses, which rises again over about 1 / 0.05 = 20 s).
 * @typedef {Object} GrassMoverOptions
 * @property {number} [radius]
 * @property {number} [strength]
 * @property {number} [trample]
 */

// ── SceneNode: per-type members ──────────────────────────────────────────────
//
// Continues `class SceneNode` from docs/scene-api.js (identity, hierarchy,
// transforms and camera projection live there). Each member below applies to
// the node types its comment names.

class SceneNode {

  /** @type {number} */ rx;
  /** @type {number} */ ry;
  /** @type {number} */ rz;
  /** @readonly @type {string} */ type;
  /** @readonly @type {string} */ kind;
  /** @readonly @type {number} */ childCount;
  /** @readonly @type {number} */ splatCount;
  /** @type {Array<number>|null} */ worldAnchor;
  /** @type {number} */ cullMargin;
  /** @type {number|Array<number>} */ size;
  /** @type {Function|null} */ onFinished;
  /** @type {Function|null} */ onAnimationEnd;
  /** @type {Function|null} */ onAnimationFinished;
  /** @type {Function|null} */ onStateChanged;
  /** @type {number} */ alphaCutoff;
  /** @type {boolean} */ doubleSided;
  /** @type {number} */ nearClipDist;
  /** @type {Array<number>|string} */ emissiveColor;
  /** @type {boolean|string} */ billboard;
  /** @type {boolean} */ autoSync;
  /** @type {number} */ pixelsPerUnit;
  /** @type {number} */ radius;
  /** @type {string} */ fillColor;
  /** @type {string} */ strokeColor;
  /** @type {number} */ strokeWidth;
  /** @type {number} */ frameIndex;

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
   * (a number or an array of up to 4 numbers). A uniform declared in both
   * chunks is one value, a line may declare several, and a node takes at
   * most 8 `sampler2D`s (a ninth fails to compile). Chunks may read the
   * engine's `vUV`, `vWorldPos` (camera-relative), `vCamDist` and the
   * vertex inputs (`aTangent` among them), and pass their own varyings
   * (`out` in the vertex chunk, `in` in the fragment chunk); the shadow
   * pass runs the vertex chunk too. Throws TypeError on bad
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

  // ── Vegetation (MeshNode, InstancedMeshNode) ────────────────────────────
  //
  // Plants that move like plants in the scene's wind field (scene-api.js
  // setWind / windAt), all on the GPU, from records bro.flora's emitters
  // write (flora-api.js "Vegetation records"): emit, create the nodes, and
  // the trees sway with no further code.
  //
  // A record is 16 floats in node space — per vertex in a Mesh
  // (`mesh.vegetation`, vertexCount * 16), per instance on an instanced mesh
  // (the `vegetation` option or setVegetation):
  //
  //   0-3   root.xyz, height        the plant's base and height
  //   4-7   crown.xyz, crownRadius  its leafy volume; radius 0: no bulk normal
  //   8-11  anchor.xyz, weight      where a leaf hangs; branch weight 0 at the trunk .. 1 at a twig tip
  //   12-15 axis, twig, limb, trunk packed words: a leaf's stem direction (0 on wood); its limb's
  //                                 phase and leafy sprays; its limb; its plant's trunk
  //                                 (flora-api.js "Vegetation records" has the bits)
  //
  // A wood record for a whole segment (one instanced cylinder per segment,
  // flora's emitSegmentVegetation) is the same 16 floats with the crown
  // floats holding the segment's tip instead: 4-7 = tip.xyz, -(1 + weightTo)
  // (a negative w marks the form), 8-11 = base.xyz, weightFrom. Each vertex
  // takes the weight at its own point along the segment, so the tip bends
  // exactly as its child's base and the joint holds.
  //
  // Branch tubes and foliage scatter (setTubeSegments / setScatterSegments)
  // take 16 floats per segment — root.xyz, height, crown.xyz, crownRadius,
  // weightFrom, weightTo, 0, 0, 0, twig, limb, trunk — and the shader
  // interpolates the weight along the segment and anchors each leaf where it
  // hangs. Tube rings sit in the plane between a joint's two segments, framed
  // by a minimal rotation from +Y, so a bent branch has no seam or twist.
  //
  // The motion. A plant is three cantilevers — its trunk, each limb, each
  // limb's leafy sprays — with a natural frequency and a deflection at 10
  // m/s each, from the plant's geometry (flora-api.js "Vegetation records").
  // Each is driven by the drag at the plant, (U / 10 m/s)^1.3, U the wind
  // there (the gust band over its root, so a gust crosses a grove tree by
  // tree), and answers as a forced, damped oscillator: its static bend under
  // the drag a quarter period back (a heavy trunk lags the gust), plus
  // ringing at its own frequency from the turbulence too fine for the gust
  // field (narrow-band resonance: mean drag x f^-1/3 / sqrt(damping)) and
  // from the gusts' own change over its last half period, with damping
  // ratios 0.15 (trunk: its limbs take its energy), 0.08 (limb), 0.05
  // (spray). Over a minute of the grove's wind the deflection's RMS and
  // speed at each level are within ~15% of an integrated oscillator under
  // the same drag (broworkshop's demos/grove/sway_check.js). A level bends
  // in a loaded cantilever's shape, s² (3 - s) / 2 along it, the levels add
  // down the hierarchy, and the wood turns with its slope, so bark normals
  // follow the bend. A leaf turns with the wood it hangs from (its shading
  // follows its limb), flutters about its petiole (a twist about its stem
  // and a smaller pitch at 3-9 Hz on its own phase, growing with the drag,
  // in bursts) and in a gust turns part way downwind and folds along its
  // midrib. A calm is still. The shadow pass runs the same pose.
  //
  // Two layers, so a level-of-detail chain shares one pose: vegPose (the
  // wood's offset and rotation at a rest point, from the plant's root,
  // height and trunk word, the limb's twig and limb words, and the point's
  // branch weight) and, on top, vegLeaf (the near tier's own leaf motion).
  // A far tier calling vegPose with the same records moves exactly as the
  // near one (scene_vegetation.glsl).
  //
  // The look: a leaf's normal leans toward its crown's centre-out direction
  // so the crown lights as one form, leaves deep inside the crown take less
  // sky, bark darkens toward the ground, and the sun glows through leaves
  // seen against it (stronger with `subsurface`). A leaf's light wraps past
  // its terminator (a thin blade scatters) and its shadow lookup is one
  // wider, softer filter shared by its direct and through-leaf light, never
  // fully black, so a leaf does not flip between lit and dark. A cupped card
  // (bromesh leafCard `cup`) varies its normal across the blade. Leaves are
  // cut-out geometry, so the scene's MSAA (setMSAA) is what smooths their
  // edges. The records also vary the
  // colour, multiplying the node's (and an instance's) colour: each plant
  // leans warm or cool (hashed from its root), each limb (its phase) and each
  // leaf is a little lighter or darker, leaves deep in the crown go darker and
  // cooler and the rim lighter, and bark varies by plant. Set the node's
  // colour to the species' mean and the crowns come out varied around it.
  //
  // A node with a custom shader, a static batch or line drawing ignores its
  // records; scatter leaves with records cast shadows when castsShadow is on.
  // Records replace the legacy `wind` vertex-colour sway on that node.

  /**
   * InstancedMeshNode: the vegetation records — 16 floats per instance, or
   * 12 per segment after setTubeSegments / setScatterSegments (which also
   * take them as `vegetation`). A length that does not match the instances
   * or segments leaves the node still. An empty array clears them.
   *
   * @example
   *   const tubes = world.emitBranchTubes();          // carries .vegetation
   *   const bark = scene.createInstancedMesh({ color: '#5a4636' });
   *   bark.setTubeSegments(tubes);                    // sways in the wind field
   * @param {Float32Array} data
   * @returns {SceneNode}
   */
  setVegetation(data) {}

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
   *  The live mix: `{ state, clips: [{ name, weight }], phase, pos?,
   *  layers: [{ slot, name, weight, phase }] }`. `pos` ([x] or [x, y]) is
   *  present only while a blend space is the base source; a plain clip has
   *  no `pos` at all.
   * @returns {Object|null}
   */
  blendState() {}

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
   * @param {AnimStateMachineDef} def
   * @returns {SceneNode}
   */
  addStateMachine(def) {}

  /**
   * @param {string} targetState
   * @returns {SceneNode}
   */
  travel(targetState) {}

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
   * InstancedMesh: GPU foliage scatter. The node's mesh (a leaf card: +Z tip,
   * +Y card normal, +X side) is drawn once per leaf, each leaf placed along
   * its branch segment in the vertex shader — up bias, tilt / roll / scale
   * jitter as bromesh's placeLeavesOnBranches, from a hash of the leaf index,
   * so placement is deterministic. Replaces the instance rows. Leaves cast no
   * shadow unless they carry `vegetation` records (16 floats per segment,
   * "Vegetation"), which also sway them. The object
   * `world.emitScatterSegments()` (flora-api.js) returns.
   *
   * With `clumps: true` (what emitScatterSegments returns by default) each
   * "segment" is a crown clump instead — centre.xyz, radius, dir.xyz, 0 — and
   * its leaves fill three overlapping lobes of it (bromesh's
   * placeLeavesInClumps, bit for bit: the same hash draws), packed toward
   * the surface: in a shell `shell` of the lobe radius deep (default 0.4; 0
   * puts every leaf on the skin, 1 spreads them to the centre), facing outward with `upBias`, tilt and scale jitter. A clump's
   * vegetation record is its twig's, so a clump rides the twig it hangs on.
   *
   * @example
   *   const leaves = scene.createInstancedMesh({ mesh: Mesh.plane(0.08, 0.15), color: '#3a7', doubleSided: true });
   *   leaves.setScatterSegments({
   *     segments: new Float32Array([0, 0, 0, 0.05,  0, 2, 0, 40]),   // from.xyz, radius, dir.xyz, leaf count
   *     instSeg: new Float32Array(40),                               // each leaf's segment index
   *     seed: 1, upBias: 0.5, boundsMin: [-1, -0.5, -1], boundsMax: [1, 2.5, 1],
   *   });
   * @param {{segments: Float32Array, instSeg: Float32Array, seed?: number, upBias?: number,
   *   tiltJitter?: number, rollJitter?: number, baseScale?: number, scaleJitter?: number,
   *   scaleByRadius?: number, maxRadius?: number, densityFalloff?: number,
   *   clumps?: boolean, shell?: number,
   *   boundsMin?: Array<number>, boundsMax?: Array<number>, vegetation?: Float32Array}} opts
   */
  setScatterSegments(opts) {}

  /**
   * InstancedMesh: GPU branch tubes. Each segment (8 floats: from.xyz,
   * radiusFrom, to.xyz, radiusTo) draws a tapered `sides`-sided tube wall
   * with no end caps, built in the vertex shader — no mesh, no instance rows.
   * Lit with the node's material; casts and receives shadows; sways with
   * `vegetation` records (16 floats per segment, "Vegetation"). The object
   * `world.emitBranchTubes()` (flora-api.js) returns.
   *
   * `tangents` (6 floats per segment: the unit tube direction at its base and
   * at its tip) mitres the joints: each end ring is cut on the plane with
   * that normal instead of square to the segment, so a branch continuing
   * into its next segment at an angle bends without a crack on the outside
   * or an overlap on the inside (the ring slides at most 0.45 of the
   * segment's length). emitBranchTubes writes the bisector of each segment
   * and its thickest child. Without them every joint is square.
   *
   * @param {{segments: Float32Array, sides?: number, radiusScale?: number, tangents?: Float32Array,
   *   boundsMin?: Array<number>, boundsMax?: Array<number>, vegetation?: Float32Array}} opts
   */
  setTubeSegments(opts) {}

  /** InstancedMesh: true after setScatterSegments. @readonly @type {boolean} */
  isScatter;

  /** InstancedMesh: true after setTubeSegments. @readonly @type {boolean} */
  isTube;

  /**
   *  ReflectionProbe: where probe boxes overlap, a mesh takes the probe with
   *  the highest priority whose box holds its bounds centre, ties going to
   *  the smaller box. Default 0; undefined on other nodes.
   * @type {number}
   */
  priority;

  /**
   *  ReflectionProbe: the margin (world units) over which the probe fades to
   *  the global environment near its box faces; undefined on other nodes.
   * @type {number}
   */
  interior;

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

  /** @returns {SceneNode} */ capture() {}
  /** @param {number} cutoff @returns {SceneNode} */ setAlphaCutoff(cutoff) {}
  /** @param {boolean} enabled @returns {SceneNode} */ setDoubleSided(enabled) {}
  /** @returns {SceneNode} */ syncToPhysics() {}
  /** @param {number} index @param {Object|Array<number>|Float32Array} data @returns {SceneNode} */ updateInstance(index, data) {}
  /** @param {Array<Object>|Float32Array} instances @returns {SceneNode} */ updateInstances(instances) {}
  /** @param {(MeshGeometry|Mesh)} mesh @returns {SceneNode} */ setInstancedMesh(mesh) {}
  /** @param {number} cols @param {number} [rows] @returns {SceneNode} */ setAtlasGrid(cols, rows) {}

  // ── Grass (scene.createGrass) ──

  /**
   * Grass: change the look (any of GrassNodeOptions' blade and colour
   * keys; what it leaves out keeps its value). The field is not re-baked.
   * @example grass.setGrass({ height: 0.6, dryColor: '#b89a52', wind: 1.4 });
   * @param {GrassNodeOptions} opts
   * @returns {SceneNode}
   */
  setGrass(opts) {}

  /**
   * Grass: re-bake the field (`ground`, `area`, `cell`, `density`, `dry`,
   * `tall`) and replace it whole.
   * @param {GrassNodeOptions} opts
   * @returns {SceneNode}
   */
  setGround(opts) {}

  /**
   * Grass: the look as set: `{height, width, bladeDensity, range, minNormalY,
   * lush, dryColor, bulk, clump, rootShade, sheen, variety, wind}`.
   * Undefined on any other node.
   * @returns {Object|undefined}
   */
  grassParams() {}

  /**
   * Grass: track `node` as a mover. Every frame its path through the grass
   * since the last frame (sampled along the way, so a fast mover leaves no
   * gaps) pushes the blades within `radius` away from it and along its
   * motion, and with `trample` flattens them the way it pushed. Removed
   * nodes are dropped by themselves.
   * @example grass.addMover(walker, { radius: 0.45, strength: 1, trample: 0.6 });
   * @param {SceneNode} node
   * @param {GrassMoverOptions} [opts]
   * @returns {SceneNode}
   */
  addMover(node, opts) {}

  /**
   * Grass: stop tracking `node` (the grass it bent recovers as usual).
   * @param {SceneNode} node
   * @returns {SceneNode}
   */
  removeMover(node) {}

  /**
   * Grass: a one-off push this frame at world (x, z): a landing, an
   * explosion, a gust from a rotor. `vx`, `vz` (m/s) bend the blades along
   * a motion as well as away from the point.
   * @example grass.push(p[0], p[2], { radius: 3, strength: 1.5, trample: 0.4 });
   * @param {number} x
   * @param {number} z
   * @param {{radius?: number, vx?: number, vz?: number, strength?: number, trample?: number}} [opts]
   * @returns {SceneNode}
   */
  push(x, z, opts) {}

  /**
   * Grass: what the last frame drew: `{instances: [near, mid, far], movers,
   * fieldCols, fieldRows}`. `instances` counts the cells of each ring's
   * visible rectangle; the blades that survive culling are fewer.
   * @returns {{instances: Array<number>, movers: number, fieldCols: number, fieldRows: number}|undefined}
   */
  grassStats() {}

  /**
   * Grass: the baked ground height at world (x, z), bilinear as the GPU
   * samples it (so a mover set on it sits where the blades root).
   * @param {number} x
   * @param {number} z
   * @returns {number|undefined}
   */
  grassHeightAt(x, z) {}

  /**
   * Mesh: the node's top surface (world y) on a grid, NaN where it has none:
   * texel (i, j) at world (x0 + i * cell, z0 + j * cell), row-major
   * (`cols` per row). What createGrass bakes a mesh ground with.
   * @param {number} x0
   * @param {number} z0
   * @param {number} cell
   * @param {number} cols
   * @param {number} rows
   * @returns {Float32Array}
   */
  sampleHeights(x0, z0, cell, cols, rows) {}

  /**
   *  Writes a Gaussian-splat node's cloud as a PLY file; false when the write fails.
   *
   * @param {string} path
   * @returns {boolean}
   */
  savePly(path) {}

}
