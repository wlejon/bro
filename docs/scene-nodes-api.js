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
 * @property {number} [innerAngle] - a spot's inner cone (radians)
 * @property {number} [outerAngle] - a spot's outer cone (radians)
 * @property {Array<number>} [direction]
 * @property {boolean} [castsShadow]
 * @property {number} [shadowBias]
 * @property {number} [shadowNormalBias]
 * @property {number} [cascadeCount=4] - a directional light's shadow
 *   cascades (1..4), each a tile of the shadow atlas
 * @property {number} [cascadeSplitLambda=0.5] - the cascades' split, 0
 *   uniform .. 1 logarithmic in depth
 * @property {number} [shadowDistance=0] - how far from the camera a
 *   directional light's shadow reaches (m): its cascades split this depth,
 *   and it fades out over the last tenth. 0: the camera's far plane. A
 *   view kilometres deep needs it, or the nearest cascade spans hundreds of
 *   metres. All of these are also properties of the node.
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
 * The grass node's herbs (createGrass / setGround `herbs`, `node.setHerbs`):
 * low plants built where they stand, nothing stored per plant. The node
 * keeps a table of up to 12 species (bro.flora.herbRecords) and a grid of
 * 0.35 m cells, each holding at most one plant: which (if any) is picked
 * per cell from each species' abundance, how well the light there suits it
 * (the `floor` canopy against its shade preference), its colonies (noise of
 * its own period, a cell holding one of its plants as often as its
 * `spacing` allows) and the floor's herb density. Near the eye (two rings,
 * to `range`) each plant is a few cards built in the vertex shader (its
 * leaves or fronds, stems, blooms) and cut per pixel to its leaf outline,
 * pinnae, petals, cups seen at their angle, spikes or bells; they sway as
 * cantilevers worked out from their build (broflora herbSway) and cast no
 * shadows. A stand shades itself: the leaves above a point (its species'
 * leaf area at this density) take the sun on its slant path and part of
 * the sky, so a fern stand is dark within and lit at its top. Past `range`
 * to `far` the same plants are drawn with fewer cards, fewer of them with
 * distance (each widened to keep the cover); seen from well above (the
 * eye's lift over a third of the distance across the ground) they are left
 * to the turf. Past them the far blades and the turf stand in for them:
 * the share of the ground they cover as the eye sees it (more seen across
 * than from above: their sides, as far as they rise over the grass), their
 * leaves mostly hidden among the blades, their blooms coloured patch by
 * patch by each species' colonies there, so a meadow's poppies and daisies
 * go on to the horizon as specks of their colour. Where herbs grow thick the grass gives
 * them room, sparser and shorter with the floor's herb density.
 * @example
 *   scene.createGrass({ ground, floor: cover.canopy, turf: true, herbs: {
 *     species: ['bracken', 'woodsorrel', { preset: 'daisy', abundance: 2 }],
 *     density: (x, z, y, canopy) => 0.8 * patchy(x, z),
 *     range: 24,
 *   } });
 * @typedef {Object} GrassHerbOptions
 * @property {Array<string|Object>|Float32Array} species - preset names,
 *   `{preset, ...HerbSpecies fields}` specs (bro.flora.herbPreset), or a
 *   table from bro.flora.herbRecords; more than 12 are dropped.
 * @property {number|Function} [density=1] - how thickly herbs grow, 0..1: a
 *   number or `(x, z, y, canopy) => value`, baked into the field with it.
 * @property {number} [range=30] - how far the whole plants are drawn (m, 4..200).
 * @property {number} [far=3*range] - where the far plants end and the turf
 *   alone stands in (m, 1.4 x range .. 800).
 */

/**
 * Grass: a meadow of procedural blades over a ground, from
 * `scene.createGrass(opts)`. No blade carries data: four rings of cells
 * around the camera, each cell an instance of several blades. Near (to 7 m at
 * the default range) a cell holds 14 fine blades (4 mm), about 1400 a square
 * metre, growing from crowns in bunches that fan out and, where `tufts` is
 * high, gather into tussocks. Each blade tapers to a fine point, arches and
 * droops by its length and age (young leaves upright, old ones bowed, a few
 * lodged flat), folds along its midrib and twists a little; some are dead
 * brown leaves low at the base (`dead`) and some are flowering culms, a bare
 * stem carrying a panicle or a spike of spikelets well above the leaves
 * (`flowering`); far off, seen across, the heads give a meadow in flower a
 * pinkish-buff haze. Sunlit blades seen against the light glow through.
 * Further out, fewer and wider
 * blades over bigger cells (the far rings' leaves fans of four strands cut
 * per pixel, lit as the sward's mean face, combed by the wind and leaning
 * with its gusts, so a far meadow shows the wind's sheen and its travelling
 * waves), so blade count x width (the cover) stays about
 * level while the work falls. Within a ring the far half keeps fewer blades,
 * widened to match the next ring, and rings cross-fade through a dithered
 * band. Only tiles of cells the view can see, inside the ring and where
 * something grows are issued. Each blade is placed, culled, bent and coloured
 * in the vertex shader from its cell. They sway in the scene's wind field
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
 * read the same noise an app colours its ground with. For a large field
 * (a few million texels, where a call a texel takes tens of seconds) any of
 * them, `floor` and `herbs.density` may instead come a grid at once: a
 * Float32Array of cols x rows, texel (i, j) at (x0 + i * cell, z0 + j *
 * cell) with cols = floor((x1 - x0) / cell) + 1 and rows likewise, or a
 * function carrying `grid(x0, z0, cell, cols, rows, ys, canopy)` that
 * returns one (`ys` the field's heights, `canopy` the clamped floor, for
 * the herbs' density). bro.flora.forestCover's functions carry one; so may
 * a `ground` function (its `grid` returns the heights). Re-bake
 * with `node.setGround(opts)` (a moved or edited ground, a new season).
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
 * @property {number|Function} [floor] - the forest floor, 0 open .. 1 under
 *   a closed canopy (e.g. `bro.flora.forestCover(...).canopy`), on the same
 *   grid. A turf ground (`turf`) lays leaf litter, twigs, damp soil and moss
 *   where it closes, and the herbs (`herbs`) choose their places by the light
 *   it leaves. Default: no floor (open everywhere).
 * @property {GrassHerbOptions} [herbs] - low plants among (and without) the
 *   grass: ferns, wildflowers, forbs, ground cover, from broflora's species
 *   (see GrassHerbOptions).
 * @property {boolean|SceneNode|Array<SceneNode>} [turf] - mesh nodes to shade
 *   as this grass's ground (`true`: the `ground` node), see `shadeGround`.
 * @property {number} [height=0.4] - mean blade height (m); clumps run
 *   from about half to one and a half times it, and lush tall patches higher.
 * @property {number} [width=0.004] - the near blades' width (m); the outer
 *   rings' are 4, 19 and 25 times it, and every blade widens with distance
 *   (never under about two thirds of a pixel).
 * @property {number} [bladeDensity=1] - blades per area, x the default
 *   (a 14-blade tuft per 10 cm square near the camera, before the clumps
 *   thin them).
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
 * @property {number} [tufts=0.35] - 0 an even sward .. 1 bunch grass: how
 *   far blades gather into tussocks with bare ground between.
 * @property {number} [flowering=0.08] - 0..1, how many flowering culms
 *   (about 150 a square metre at 1, more where the grass is lush and tall).
 * @property {number} [dead=0.08] - 0..1, the share of dead brown leaves at
 *   the base (more where it is dry).
 * @property {Array<number>|string} [color] - the node colour, multiplied in (default white).
 * @property {number} [roughness=0.7]
 * @property {boolean} [castsShadow=false] - the near ring's blades cast, a
 *   third of them widened to keep the cover (a cost: the shadow map then
 *   re-renders every frame, every caster in it).
 * @property {boolean} [receivesShadow=true]
 */

/**
 * A grass mover's push: `radius` (m, default 0.6), `strength` (default 1:
 * how hard blades lean away and along its motion; they spring back as a
 * damped spring, 0.9 Hz, swinging once a little past upright), `trample`
 * (0..1, default 0: how flat it leaves the grass it crosses, which rises
 * again over 1 to 3 s, a faint 1% of it a second or two more; under
 * its body it lays the grass along its path, at its edge out to the sides) and
 * `height` (m, default 1.8). The grass's herbs part with its blades. A
 * mover also parts the scene's swaying plants (trees, shrubs, a forest's
 * low foliage) within
 * 1.6 x `radius` of its path and up to `height` over their roots: they give
 * as it arrives and spring back once after it goes (scene_vegetation.glsl's
 * vegMoverPush; at most four movers in the scene).
 * @typedef {Object} GrassMoverOptions
 * @property {number} [radius]
 * @property {number} [strength]
 * @property {number} [trample]
 * @property {number} [height]
 */

/**
 * Forest: many trees drawn from a few SPECIMENS, from
 * `scene.createForest(opts)`. The specimens are bro.flora's
 * `world.emitForest()` (every grown plant's bark segments, crown clumps and
 * leaves, relative to where it stood); the trees are rows of `instances`
 * (`bro.flora.plantForest`, or the emit's own: every plant where it grew),
 * each a specimen stood somewhere, turned and scaled. Nothing per tree is
 * built: every leaf, clump and limb is placed and swayed in the vertex
 * shaders from the specimen's records, so ten thousand trees cost the
 * memory of two dozen.
 *
 * Each frame every tree in view picks its LEVEL OF DETAIL by its size on
 * screen (pixels, so the choice follows the resolution and field of view,
 * not metres):
 *   - LEAVES: up close, every leaf the plant grew, swaying with its limb and
 *     fluttering on its own. Once a leaf is shorter on screen than
 *     `leafPixels` the crown thins: a prefix of the tree's shuffled leaves
 *     is kept, 1 / w^2 of them for a widening w, each w times larger, so its
 *     cover holds while the work falls by w^2 (the last of the kept run
 *     shrink away, so nothing pops). The leaves' own flutter fades out by
 *     `flutterFade`; past `cardWidening` a kept leaf is a CLUSTER (`clusters:
 *     1`, the default): its widened outline drawn as the widening^2 leaves
 *     it stands for, each of its own size, placed in the fragment shader
 *     (`clusters: 0`: the one widened leaf, a 6-vertex card).
 *   - CLUMPS: from `clumpFrom` to `clumpTo` (a widening) the leaves hand
 *     over to the crown's clumps, each drawn as the three lobes its leaves
 *     fill: a camera-facing card a lobe whose pixels place the leaves of
 *     the lobe's shell, one to a jittered cell, each its own size, turn and
 *     tone, with the far half seen through the near one's gaps. Once a
 *     pixel holds several leaves it takes their mean, with the wander a
 *     pixel's few leaves have from it (count, facing, how many lie toward
 *     the light). They are lit the leaves' way (tint, crown shade and
 *     depth, translucency), shadowed by their own lobe's leaves
 *     analytically and by everything else from the shadow map, and write
 *     the depth of the leaves they show.
 *   - CROWNS: once a clump is under `crownFrom` .. `crownTo` pixels across,
 *     one such card a tree over the crown's extent. A gust's paler sheen
 *     rolls across clumps and crowns.
 *   Every tier past the leaves is drawn from the tree's own records at run
 *   time: nothing is baked or rendered ahead, so every tree may be unique.
 *   - BARK at the leaves' and sprites' tiers: the segments wider on screen
 *     than `barkPixels` times the leaves' widening, a connected run from
 *     the trunk out, so limbs go tip first as the crown thins; 20 sides
 *     while the trunk is wider than `barkFineSides` x `barkSidesPixels`,
 *     10 while wider than `barkSidesPixels`, else 5, each ring morphing
 *     between them by its own width on screen.
 *   - BLOOMS at every tier: a flowering specimen's blooms (the emit's
 *     `blooms` / `specimenBlooms`: its species' bloomForm, colour and size
 *     at its twigs' tips), a card each whose fragment shader shapes the
 *     form with nothing baked: a 'flower' is five broad petals round a
 *     yellow centre, cupped, facing its twig's way tilted up (a hibiscus);
 *     a 'head' a ball of four-petalled florets shaded as a ball, its depth
 *     traced (a hydrangea's mophead); a 'spike' a bare stalk held upright
 *     with whorls of florets tapering above it (lavender). Each is posed by
 *     its twig's sway and lit as any surface. A bloom under 1.5 px across
 *     is drawn as a dot that size, its cover cut to its share of it, so the
 *     blooms dwindle into the crown tier's distances without popping, and
 *     go once under a quarter pixel. They cast no shadow and are outside
 *     the budget.
 * A hand-over is a dither that splits each pixel between the tiers; in a
 * 4x MSAA view each sample draws its own share, so the pixel blends in
 * quarters rather than stippling. Every tier is posed by the same sway from
 * the same records (a clump as its leaves, a crown on its central limb at
 * its clumps' mean weight), so a hand-over does not swim.
 * The leaves measure their distance to the crown (a crown overhead is as
 * near as its underside), the bark to the tree.
 *
 * A vertex BUDGET (millions, the leaves' and bark's) holds the cost of any
 * view: when the trees in view would send more, the pixel thresholds scale
 * up together (`forestStats().detail`) until they fit, so every tree thins
 * alike and the furthest hand over first. Shadows choose the same way per
 * shadow tile by its texel size, against their own budget, as silhouettes
 * (leaves as cards, kept to `shadowKeep`, bark of 5 sides, crowns at twice
 * the size; clumps and crowns cast discs as dense as their leaves). Clumps
 * and crowns cost a few hundred vertices a tree and are outside both
 * budgets; clusters and sprites cost fragments the budget does not see.
 * Several forest nodes in one scene share ONE budget (the largest any of
 * them sets; none if any sets 0): the trees of every node in view thin by
 * the same detail factor, so a mixed forest drawn a node per leaf form
 * costs what one forest of its trees would.
 *
 * LEAF FORM (`leafForm`; a node draws one): 'broad' leaves are the card
 * itself. A 'needle' or 'scale' node's leaf is a whole spray's card: the
 * needles along a twig (or the overlapping scale leaves) are cut out of it
 * per pixel into the sample mask, an analytic family that fades to its
 * mean cover once a needle is under a pixel, and its shadow caster is the
 * card narrowed by the spray's fill. The form also sets how the trees move
 * through the same sway: how much the leaves flutter (`flutter`) and how
 * much limbs and twigs bounce (`limbBob`, `twigBob`): a conifer's long
 * boughs bob while its needles hardly flutter. A needle or scale node's
 * crowns take over from its clumps earlier (`lod` crownFrom 8, crownTo 5
 * unless given): a conifer's crown is many small clumps, each a few lobes
 * to draw, and the crown traces them as one card. With no leaf given, the node
 * draws its form's own card (an oval leaf, a needle spray, a scale-leaf
 * spray) at its specimens' mean leaf size (the species' leafSize: a
 * boxwood's small leaves). An emit filtered to a form
 * (`world.emitForest({ leafForm })`) names it, and the node takes it.
 *
 * A broad node's leaves grow ON TWIGS: each clump's leaves hang from the
 * nodes of current-year shoots (about 8 leaves a shoot, alternate in two
 * ranks, opposite and decussate, or a 2/5 spiral, as the specimen's
 * phyllotaxis says), the shoots branch off sprays (about five a spray: a
 * terminal one and laterals left and right), and each spray's twig leaves
 * a stem through its lobe from where the clump grows on its branch; the
 * sprays lie along the lobes' outer shell, so the foliage forms layers.
 * Each blade leaves its node on a petiole (the specimen's, a share of the
 * blade's length; cut per pixel and gone once a leaf is under 8 px),
 * turned out and up toward the light, drooping. All of it is derived per
 * clump at run time from the clump's own record and leaf count, nothing
 * stored per leaf or twig. Near trees draw the TWIGS too: camera-facing
 * ribbons at least a pixel wide (their cover their width under it),
 * posed through the same functions as the leaves' nodes and bent with
 * their spray and shoot, so a leaf never leaves its twig; the shoots go
 * once under 0.15 px, the stems and sprays likewise, and every twig fades
 * out as the leaves widen past `cardWidening`. Twigs count toward the
 * budget, draw in colour only (no shadow), and a 'needle' or 'scale' node
 * has none (its spray card is the twig).
 *
 * Each specimen carries its own LOOK (`specimenLooks`, the emit's: 8 floats
 * a specimen, leafColor rgb, form, barkColor rgb, leafSize): its leaves and
 * bark take its colours in place of the node's (a tint over the node's
 * colour, so the node's maps still apply), so a spruce is darker than a fir
 * in the same node. A zero colour keeps the node's. 16 floats a specimen
 * add its LEAF (the emit's `specimenLeaves`, which createForest appends
 * when the emit carries them): width, tip, teeth, tooth depth, gloss,
 * spray brush, petiole and phyllotaxis. A broad leaf the node's own card
 * is cut to that outline per pixel near (pointed or rounded, serrated or
 * spiny, veined; a leaf a few pixels across is drawn as its whole card),
 * glossy as its species is; a spray with a brush turns up and out along
 * its twig (a pine's).
 *
 * The node's material (colour, roughness, subsurface, maps, shadows) is the
 * leaves'; `barkColor` and `barkRoughness` the bark's; its mesh the leaf
 * (any leaf card; its length sets the leaf's size on screen). Its transform
 * places the whole forest. It is left out of raycasts.
 *
 * @example
 *   const forest = world.emitForest({ perArea: 90 });
 *   const trees = bro.flora.plantForest(forest, {
 *     area: [-200, -200, 200, 200], spacing: 6.5, ground: heightAt, scale: [1.3, 1.8],
 *   });
 *   const node = scene.createForest({
 *     forest, instances: trees, leaf: Mesh.leafCard('oval', { width: 0.07, length: 0.12 }),
 *     color: [0.2, 0.36, 0.11], subsurface: 0.5, doubleSided: true, castsShadow: true,
 *     barkColor: [0.2, 0.17, 0.14],
 *   });
 *   scene.createGrass({ ground, density: bro.flora.forestCover(forest, { instances: trees }) });
 *
 * @typedef {Object} ForestNodeOptions
 * @property {Object} forest - `world.emitForest()`'s result: the specimens.
 * @property {Float32Array|Array<number>} [instances] - the trees, 8 numbers
 *   each: x, y, z, scale, yaw (radians about +Y; local (x, z) turns to
 *   (x cos + z sin, -x sin + z cos)), specimen, 0, 0, in the node's space.
 *   Default `forest.instances` (every plant where it grew).
 * @property {Mesh|MeshGeometry} [leaf] - the leaf card (also `mesh`).
 *   Default: the leaf form's own card at the specimens' leaf size (a broad
 *   leaf's is nine vertices, its rounded, pointed outline cut per pixel up
 *   close; a spray's four, its needles or scales cut per pixel). A leaf
 *   given is drawn as it is.
 * @property {string|number} [leafForm] - 'broad' (0), 'needle' (1) or 'scale'
 *   (2). Default: the emit's `leafForm`, else the one all its specimens share,
 *   else broad.
 * @property {number} [flutter] - how much the leaves flutter, x a broadleaf's
 *   (default by form: broad 1, needle 0.15, scale 0.1).
 * @property {number} [limbBob] - how much limbs bounce (broad 0.6, needle 1.6, scale 1.3).
 * @property {number} [twigBob] - how much twigs and sprays bounce (broad 0.8, needle 1, scale 0.9).
 * @property {Float32Array|Array<number>} [specimenLooks] - 8 floats a specimen
 *   (leafColor rgb, form, barkColor rgb, leafSize), or 16 with each
 *   specimen's leaf after them all (8 a specimen: width, tip, teeth, tooth
 *   depth, gloss, spray brush, petiole (a share of the blade's length),
 *   phyllotaxis (0 alternate, 1 opposite, 2 spiral, 3 pinnate: leaflets on
 *   the wood itself, no twigs)); default the emit's.
 *   All zeros: the node's own colours.
 * @property {ForestLod} [lod] - level-of-detail settings.
 * @property {Array<number>|string} [barkColor] - default `[0.2, 0.17, 0.14]`.
 * @property {number} [barkRoughness=0.9]
 * @property {string|number} [tier] - `'auto'` (default), or draw every tree
 *   at one tier: `'leaves'`, `'clumps'`, `'crowns'` (full size, no
 *   thinning: for comparisons and tests).
 * @property {Array<number>|string} [color] - the leaves' colour; and the
 *   other material keys of createMesh (roughness, subsurface, doubleSided,
 *   castsShadow, receivesShadow, map, normalMap ...).
 */

/**
 * A forest's level of detail. Pixels are the view's (CSS px of the canvas
 * times its device scale).
 * @typedef {Object} ForestLod
 * @property {number} [leafPixels=8] - a leaf shorter than this on screen thins its crown.
 * @property {number} [flutterFade=2] - the widening by which a leaf's own flutter is gone.
 * @property {number} [cardWidening=1.6] - the widening past which leaves are clusters (or 6-vertex cards).
 * @property {number} [clumpFrom=3] - the widening at which leaves start handing over to clumps...
 * @property {number} [clumpTo=4.5] - ...and are gone.
 * @property {number} [crownFrom=7] - a clump's radius (px) at which clumps start handing over to crowns...
 * @property {number} [crownTo=4.5] - ...and are gone.
 * @property {number} [barkPixels=0.7] - bark narrower than this (times the widening) is left out.
 * @property {number} [barkSidesPixels=24] - a trunk wider than this on screen has 10 sides, else 5; each ring
 *   morphs between 5, 10 and 20 sides by its own width (5 to 10 over 1 to 2 times this, 10 to 20 over 2.5 to 5
 *   times), so no switch shows.
 * @property {number} [barkFineSides=2.5] - a trunk wider than this times `barkSidesPixels` has 20 sides; 0: never.
 * @property {number} [shadowKeep=0.35] - the share of its drawn leaves a tree's shadow keeps.
 * @property {number} [range=0] - trees further than this (m) are not drawn; 0: no limit.
 * @property {number} [forceRange=0] - a forced tier (`setForestTier`) holds within this distance (m) of the eye,
 *   further trees go by size at full detail; 0: every tree. Within it the forced trees hold nearest first until
 *   they send 16 times `budget` (`shadowBudget` for the casters) of vertices, the rest going by size: the
 *   Leaves tier forced on a whole forest to the horizon was billions of vertices, a frame past a GPU's
 *   watchdog (`forestStats().unforced` counts the trees the ceiling let go).
 * @property {number} [leavesRange=10] - the near sphere (m from the eye): inside it the trees are drawn as their
 *   leaves whatever their size on screen (their shadow casters at full detail, within `shadowBudget`); past it
 *   as clumps and crowns by size. The two meet per pixel, not per tree: a crown across the sphere draws its leaves' pixels inside and
 *   its clumps' outside, its casters likewise leaf by leaf and clump by clump, so on approach the leaves come in
 *   from the crown's near side as one front. 0: by size alone.
 * @property {number} [leavesBand=2] - the metres of dither about `leavesRange` over which a pixel turns from
 *   clumps to leaves.
 * @property {number} [budget=12.5] - millions of leaf and bark vertices the view may send; 0: no budget.
 * @property {number} [shadowBudget=6] - the same for every shadow tile together.
 * @property {number} [clusters=1] - past `cardWidening` a kept leaf is drawn as the leaves it stands for (1),
 *   or as the one widened leaf (0).
 * @property {number} [barkKeepPixels=2] - bark at least this wide on screen is drawn whatever the widening and
 *   the budget ask, so a mid tree keeps its trunk and scaffold limbs; 0: no floor.
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
  // the same drag (checked by the former broworkshop demos/grove/sway_check.js). A level bends
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
   * `tall`, `floor`, `herbs`) and replace it whole; `turf` as in createGrass.
   * @param {GrassNodeOptions} opts
   * @returns {SceneNode}
   */
  setGround(opts) {}

  /**
   * Grass: replace the herbs' species (and with `range` and `far`, how far
   * they are drawn) without re-baking the field; their density stays as
   * baked.
   * `species: []` draws none.
   * @example grass.setHerbs({ species: ['poppy', 'daisy'] });
   * @param {GrassHerbOptions} herbs
   * @returns {SceneNode}
   */
  setHerbs(herbs) {}

  /**
   * Grass: the herb plants whose roots lie within `radius` m (at most 20) of
   * world (x, z), nearest first, worked out on the CPU as the shaders pick
   * them (nothing is stored): `[{x, y, z, species}]`, `y` the ground under
   * the root, `species` its index in the herbs given. At most 256. To frame a
   * plant, to place something beside one, to answer what grows here.
   * @example
   *   const [p] = grass.herbPlants(x, z, 5);
   *   if (p) vp.reframe([p.x, p.y + 0.3, p.z], 1);
   * @param {number} x
   * @param {number} z
   * @param {number} [radius=1]
   * @returns {Array<{x: number, y: number, z: number, species: number}>}
   */
  herbPlants(x, z, radius) {}

  /**
   * Grass: shade mesh node `mesh` as this grass's ground (turf), or with
   * `on` false as itself again. Turf is shaded from the grass's field where
   * it reaches: the sward the blades stand for (their mean colour, the
   * dryness, the clumps; near the eye the dark roots and soil between the
   * blades, rising to the blade tops with distance), bare soil where nothing
   * grows, and under a closed `floor` the leaf litter, twigs, damp soil and
   * moss. Past the field it takes the same data from the mesh's vertex
   * colour: r the grass density, g the dryness, b the floor. Every pattern
   * is band-limited (finer than about two pixels fades to its mean), so it
   * reads from standing height to far above. Nothing is baked: it works on
   * any ground the field covers, streamed in or not.
   * @example
   *   const cover = bro.flora.forestCover(emit, { instances: trees });
   *   scene.createGrass({ ground, density: cover, floor: cover.canopy, turf: [ground, farGround] });
   * @param {SceneNode} mesh
   * @param {boolean} [on=true]
   * @returns {SceneNode}
   */
  shadeGround(mesh, on) {}

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
   * Grass: what the last frame drew: `{instances: [r0, r1, r2, r3], movers,
   * fieldCols, fieldRows, herbInstances: [h0, h1, h2]}`, near ring first
   * (h2 the far plants).
   * `instances` counts the cells issued in each ring (the tiles in view, in
   * the ring, where grass grows); each cell draws several blades, of which
   * the culling keeps fewer. `herbInstances` the herb cells issued in each
   * herb ring (most hold no plant).
   * @returns {{instances: Array<number>, movers: number, fieldCols: number, fieldRows: number, herbInstances: Array<number>}|undefined}
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

  // ── Forest (scene.createForest) ──

  /**
   * Forest: replace any of ForestNodeOptions (`forest`, `instances`, `lod`,
   * `barkColor`, `barkRoughness`, `tier`, `leafForm`, `flutter`, `limbBob`,
   * `twigBob`, `specimenLooks`). New specimens re-upload the
   * node's data once.
   * @param {ForestNodeOptions} opts
   * @returns {SceneNode}
   */
  setForest(opts) {}

  /**
   * Forest: the trees (8 numbers each, as ForestNodeOptions.instances).
   * @example node.setTrees(bro.flora.plantForest(forest, { area, density: (x, z) => 1 - cleared(x, z) }));
   * @param {Float32Array|Array<number>} instances
   * @returns {SceneNode}
   */
  setTrees(instances) {}

  /**
   * Forest: the level-of-detail settings as set (ForestLod's keys).
   * Undefined on any other node.
   * @returns {ForestLod|undefined}
   */
  forestLod() {}

  /**
   * Forest: change any of the level-of-detail settings.
   * @example node.setForestLod({ budget: 8, leafPixels: 10 });   // a slower GPU
   * @param {ForestLod} lod
   * @returns {SceneNode}
   */
  setForestLod(lod) {}

  /**
   * Forest: `'auto'` (by size on screen), or every tree at `'leaves'`,
   * `'clumps'` or `'crowns'` (every tree within ForestLod `forceRange`, nearest first up to a vertex ceiling).
   * @param {string|number} tier
   * @returns {SceneNode}
   */
  setForestTier(tier) {}

  /**
   * Forest: what the last frame drew: `trees` per tier (a tree handing
   * over counts in both), `culled`, `leaves` and `blooms` issued, `vertices` and
   * `shadowVertices` sent, `entries` and `draws`, `treeCount`,
   * `specimenCount`, and `detail` / `shadowDetail`, the budget's scale on
   * the pixel thresholds (1: within budget at full detail), and `unforced` /
   * `shadowUnforced`, the trees (camera's, casters') a forced tier left to
   * go by size past its vertex ceiling.
   * @returns {{trees: {leaves: number, clumps: number, crowns: number}, culled: number, leaves: number,
   *            vertices: number, shadowVertices: number, entries: number, draws: number, treeCount: number,
   *            specimenCount: number, detail: number, shadowDetail: number, blooms: number,
   *            unforced: number, shadowUnforced: number}|undefined}
   */
  forestStats() {}

  /**
   * Forest: what specimen `specimen` draws at `pxPerUnit` pixels per
   * specimen unit (the view's pixels per metre at the tree times its scale),
   * at full detail: `{widening, keep, card, leaves, clumps, crowns,
   * barkRadius, barkSides}`, the three tier weights summing to 1. By size
   * alone: ForestLod `leavesRange` needs the tree's distance
   * (forestSeenLod has it).
   * @param {number} specimen
   * @param {number} pxPerUnit
   * @returns {Object|undefined}
   */
  forestTreeLod(specimen, pxPerUnit) {}

  /**
   * Forest: what tree `instance` was drawn at in the last frame, as
   * forestTreeLod reports it but the drawer's own: its leaves measured to
   * the crown (an upright cylinder round its clumps, so nearer than the
   * tree's top or middle), its bark to the whole tree, and the budget's
   * `detail` applied. undefined when the camera did not draw it (culled,
   * out of range, or no frame yet).
   * @param {number} instance  the tree's index in the node
   * @returns {Object|undefined}
   */
  forestSeenLod(instance) {}

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
