// ── bro.scene: vegetation ────────────────────────────────────────────────────
//
// What grows in a scene: the plants node (`scene.createPlants`: trees and
// shrubs built on the GPU from a species table), the grass node
// (`scene.createGrass`: its blades (a world's sward module), its herbs (a
// world's herb module) and its turf; bro.flora's herb species, which
// broworkshop's lib/herbs module grows), and the vegetation
// records that make any mesh sway in the scene's wind field
// (`setVegetation`, flora-api.js "Vegetation records").
//
// The `SceneNode` below continues the class declared in docs/scene-api.js
// (and continued in docs/scene-nodes-api.js). The wind field itself
// (`setWind` / `windAt`) is docs/scene-api.js.

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * The grass node's herbs (createGrass `herbs`, `node.setHerbs`):
 * the low plants among its blades (ferns and wildflowers, pebbles, coral,
 * whatever a world grows), built where they stand, nothing stored per
 * plant. What they are is the world's HERB MODULE, GLSL compiled at run
 * time into the node's cover shaders (docs/scene-world-architecture.md §13,
 * "worlds as code"); the engine owns where and how they are drawn.
 *
 * The engine: a grid of 0.35 m cells, each holding at most one plant; near
 * the eye (two rings, to `range`) each plant is a few cards, an instance a
 * plant the cover's plan lists, dithered through the rings' hand-overs;
 * past `range` to `far` the same plants with fewer cards, fewer of them
 * with distance (each widened to keep the cover), and seen from well above
 * left to the turf; past them the far tier's elements (one a cell, the
 * plant's variety as a mound of its leaves and blooms) and the turf, which
 * take the module's far look. Each card fragment's cover is kept as that
 * share of the pixel's samples and marked for the sub-pixel resolve; the
 * lighting is the scene's. How thickly they grow is the land's (its
 * surface's `herbs`, docs/land-api.js); where they grow thick the grass
 * gives them room, sparser and shorter.
 *
 * The module defines, against what the engine declares
 * (src/scene/vulkan/shaders/ground/scene_herb_layer.glsl: HerbPlant,
 * HerbVertex, HerbFragment, HerbShade, HerbMix, HerbFarLook, herbData(i)):
 *   - `int herbStand(ivec2 cell, vec2 xz, vec2 floorAt)`: the variety
 *     (0..14) standing in a cell, or -1; floorAt the land's canopy and herb
 *     density there;
 *   - `HerbMix herbFarMean(float canopy, float density, float tanView)`:
 *     the layer from afar, by the canopy, the density and the view's
 *     elevation;
 *   - `float herbFarLocal(vec2 xz, float canopy, float u, float fp, out
 *     vec3 bloom, out vec3 pick)`: its blooms patch by patch over that mean;
 *   - `HerbFarLook herbFarLook(int variety, vec4 h)`: a variety as one far
 *     element;
 *   - under `HERB_BUILD` (the vertex stage, with scene_vegetation.glsl's
 *     wind): `bool herbBuild(HerbPlant p, int card, int cards, int verts,
 *     vec3 v, inout HerbVertex o)`, a vertex of one of the plant's cards
 *     (false leaves the card out);
 *   - under `HERB_SHADE` (mesh.frag, with scene_lighting.glsl): `HerbShade
 *     herbShade(HerbFragment f)`, what a fragment of a card is.
 * It may use anything of scene_camera.glsl and scene_grass.glsl (the land
 * under it, grassGround / landHeightAt, grassHash, the wind's noise). A
 * module compiles once a
 * process, its SPIR-V cached on disk (docs/code-cache.md); one that does
 * not compile is logged and the node draws no herbs.
 *
 * broworkshop's lib/herbs is such a module: broflora's herb species
 * (bracken, daisy, poppy, ...) grown from their records, cut per pixel to
 * their leaf outlines, pinnae, petals, cups, spikes and bells, swaying as
 * cantilevers, the stand shading itself.
 * @example
 *   import { herbModule } from "/lib/herbs/index.js";
 *   scene.createGrass({ ground: land, turf: true, herbs: {
 *     ...herbModule({ species: ['bracken', 'woodsorrel', { preset: 'daisy', abundance: 2 }] }),
 *     range: 24,
 *   } });
 * @typedef {Object} GrassHerbOptions
 * @property {string} source - the module's GLSL ('' draws no herbs).
 * @property {Float32Array|Array<number>} [data] - what its herbData(i)
 *   reads, vec4 i at floats 4i..4i+3 (at most 1024 floats).
 * @property {number} [height=0.4] - its tallest plant (m): how high the
 *   grass's bounds and the far tier reach for them.
 * @property {number} [range=30] - how far the whole plants are drawn (m, 4..200).
 * @property {number} [far=3*range] - where the far plants end and the turf
 *   alone stands in (m, 1.4 x range .. 800).
 */

/**
 * The grass node's sward (createGrass `sward`, `node.setSward`): its blades
 * (a meadow's leaves and culms, moss, kelp, crystal spines, whatever a world
 * grows), built where they stand, nothing stored per blade. What they are
 * is the world's SWARD MODULE, GLSL compiled at run time into the node's
 * cover shaders (docs/scene-world-architecture.md §13, "worlds as code");
 * the engine owns where and how they are drawn. A node with no sward module
 * grows no blades (its herbs and turf still draw).
 *
 * The engine: four rings of cells around the camera, each cell an instance
 * of several candidate blades (14 near, to 7 m at the default range); further
 * out fewer candidates over bigger cells, the module asked for wider blades
 * so the cover holds while the work falls. Within a ring the far half keeps
 * fewer, widened to match the next ring, and rings cross-fade through a
 * dithered band. Only tiles of cells the view can see, inside the ring and
 * where something grows, are issued. Past the rings the far tier's tufts
 * and then the turf take the module's far look. Each fragment's cover is
 * kept as that share of the pixel's samples and marked for the sub-pixel
 * resolve; the lighting is the scene's (a blade's own face near, the
 * sward's mean facet as far as the module says).
 *
 * The module defines, against what the engine declares
 * (src/scene/vulkan/shaders/ground/scene_sward_layer.glsl: SwardCell,
 * SwardRing, SwardVertex, SwardFragment, SwardShade, SwardFarLook,
 * SwardTurfIn, SwardTurf, swardData(i)):
 *   - `void swardRoot(inout SwardCell s)`: where a cell's blades rise (it may
 *     gather them toward a tussock), and its own share of the cell (s.own);
 *   - `float swardDensity(inout SwardCell s)`: the chance a candidate (a
 *     cell's blade, a far tuft) stands there: clumps, slope, the land's
 *     density (s.f: the ground's height, the land's cover density, dryness
 *     and height multiplier);
 *   - `SwardFarLook swardFarLook(SwardCell s, vec4 h)`: a far tuft's height,
 *     width and colour;
 *   - `vec3 swardBake(vec2 xz, float fp)`: the turf's surface layer's three
 *     channels of the sward's patterns, band-limited to a footprint;
 *   - `SwardTurf swardTurf(SwardTurfIn t)`: the sward as one surface where
 *     the turf stands in for it (its tops, its roots, its cover);
 *   - `vec4 swardLay(vec2 xz, vec3 gn)`: the way its blades stand there, and
 *     a gust's lift of its sheen;
 *   - under `SWARD_BUILD` (the vertex stages, the shadow casters' too):
 *     `bool swardBuild(SwardCell s, int j, SwardRing r, vec3 v, inout
 *     SwardVertex o)`, a vertex of the cell's blade j at the ring's width
 *     (false leaves the blade out);
 *   - under `SWARD_SHADE` (mesh.frag): `SwardShade swardShade(SwardFragment
 *     f)`, what a fragment of a blade is.
 * It may use anything of scene_camera.glsl and scene_grass.glsl (the land
 * under it, grassGround / landHeightAt, the movers' push `grassInfluence`, grassHash, the wind, the band-limited
 * ground noise). It compiles and caches as a herb module does; one that
 * does not compile is logged and the node draws neither blades nor herbs.
 *
 * broworkshop's lib/sward is such a module: a meadow of fine leaves from
 * crowns in bunches (tussocks with `tufts`), arching, drooping, folded and
 * twisting, dead leaves at the base and flowering culms above them, fans
 * of strands far off, swaying in the wind's lean, flutter and gusts.
 * @example
 *   import { swardModule } from "/lib/sward/index.js";
 *   scene.createGrass({ ground, turf: true, sward: swardModule({ height: 0.5, tufts: 0.6 }) });
 * @typedef {Object} GrassSwardOptions
 * @property {string} source - the module's GLSL ('' grows no blades).
 * @property {Float32Array|Array<number>} [data] - what its swardData(i)
 *   reads, vec4 i at floats 4i..4i+3 (at most 256 floats).
 * @property {number} [height=0.4] - the blades' mean height (m): the rings'
 *   bounds and the turf's depth are sized by it (grassSwardHeight()).
 * @property {number} [tallest=height] - the tallest blade (m): how high a
 *   cell's bounds reach.
 * @property {number} [reach=0] - how far a blade's tip may stand from its
 *   cell's spot (m), past the ring's spread and its lean.
 */

/**
 * Grass: a sward of blades over a land, its herbs and its turf, from
 * `scene.createGrass(opts)`. What the blades are is its sward module
 * (GrassSwardOptions), what the herbs are its herb module
 * (GrassHerbOptions); the node owns the rings, the far tier and the turf,
 * the wind (`setWind`) and the movers that part and flatten them
 * (`addMover`, `push`). The node is left out of raycasts and bounds; its
 * transform is ignored.
 *
 * The grass stands on a LAND (docs/land-api.js) and reads it on the GPU
 * from the land's tiles: its heights, and what its surface module says grows
 * there (`LandSurface.cover`: the sward's density, dryness, height
 * multiplier and the canopy over it; `LandSurface.herbs`). Nothing is baked
 * on the CPU and nothing is stored per place: the grass reaches as far as
 * the land does, the land's data (`land.setLand({ data })`) or module
 * changes it everywhere at once, and a world's cover is the same GLSL that
 * colours its ground. It fades out over the land's last 12 m.
 *
 * @example
 *   const land = scene.createLand({ height: hills, surface: MEADOW });   // MEADOW sets s.cover, s.herbs
 *   const grass = scene.createGrass({
 *     ground: land,                            // the blades stand on it, where it grows them
 *     sward: swardModule(),                    // broworkshop's lib/sward
 *     herbs: herbModule({ species: ['daisy', 'poppy'] }),
 *     turf: true,                              // shade the land as the grass's ground
 *   });
 *   grass.addMover(player, { radius: 0.5, trample: 1 });   // parts it, leaves a lane
 *
 * @typedef {Object} GrassNodeOptions
 * @property {SceneNode} ground - the land the blades stand on
 *   (scene.createLand; anything else throws a TypeError).
 * @property {GrassSwardOptions} [sward] - the blades (see
 *   GrassSwardOptions). Default: none.
 * @property {GrassHerbOptions} [herbs] - low plants among (and without) the
 *   grass: ferns, wildflowers, forbs, ground cover, from broflora's species
 *   (see GrassHerbOptions).
 * @property {boolean|SceneNode|Array<SceneNode>} [turf] - nodes to shade
 *   as this grass's ground (`true`: the land), see `shadeGround`.
 * @property {number} [width=0.004] - the width the near ring asks of a
 *   blade (m); the outer rings' are 4, 19 and 25 times it, and it grows with
 *   distance.
 * @property {number} [bladeDensity=1] - candidate blades per area, x the
 *   default (14 a 10 cm cell near the camera, before the module's density).
 * @property {number} [range=150] - the far edge of the outermost ring (m).
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
 * mover also parts the scene's swaying meshes (vegetation records) within
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
 * Plants: trees and shrubs (or coral, crystal spires, giant fungi: whatever
 * a world stands up) from `scene.createPlants(opts)`, every one built where
 * it stands on the GPU from its record, its species and its seed (nothing
 * is stored per element). What a plant is is the world's PLANT MODULE
 * (`module`), GLSL compiled at run time into the plants' shaders
 * (docs/scene-world-architecture.md §13, "worlds as code"); the engine owns
 * where plants stand, how many elements each is drawn with, and the light.
 * A node with no module draws nothing.
 *
 * Its foliage is ELEMENTS, opaque polygons (the depth test rejects what is
 * hidden before it shades), its wood TUBES. One representation at every
 * distance: a plant drawn with N elements draws the first N of its module's
 * one sequence, so any N should cover the whole crown, each element growing
 * as N falls (the engine hands the module sqrt(full / N)); a near tree is
 * thousands of leaves and a far one a few dozen daubs. N is the species'
 * `elements` while an element (`leafLength` long) is at least `pixels`
 * across on screen, then falls with the square of the distance (each stays
 * about `pixels` across), never under `minElements` (nor, for a crown only
 * a few elements across on screen, under what covers it: a speck at the
 * horizon is one); its fraction grows the last element in, so nothing pops.
 * The trunk and `limbs` limbs are drawn to 0.4 x `woodRange`, the trunk
 * alone to `woodRange`.
 *
 * The light is the engine's: the crown is an envelope (an ellipsoid over
 * the plant from `base` of its `height` up, `radius` across, by `shape` a
 * cone) of a medium of `density`, so the sun reaches an element through
 * the crown in front of it, the shadow map is read where that path leaves
 * the crown, the sky reaches a crown's core less, and the probe grid takes
 * the crown as that medium of the species' mean colour (`albedo`). What a
 * fragment is (its colour, the normal it is lit by, the light the sun
 * shows through it from behind) is the module's.
 *
 * The module defines, against what the engine declares
 * (src/scene/vulkan/shaders/foliage/scene_plant_layer.glsl: PlantElementIn,
 * PlantElement, PlantLimb, PlantFragment, PlantSurface; and of
 * scene_plant_records.glsl Plant, PlantSpecies, PlantCrown, plantData(s, i)):
 *   - under `PLANT_BUILD` (a compute pass, once a frame an element):
 *     `bool plantElement(Plant p, PlantSpecies s, PlantCrown c,
 *     PlantElementIn k, inout PlantElement e)`, element k.k of a plant drawn
 *     k.n strong (k.caster for a shadow view's): its polygon, normals,
 *     colour, crown centre, how far (a leaf .. a daub) and how deep in the
 *     crown it is (false leaves it out);
 *   - `vec2 plantLeafPoint(int species, int c)`: the element polygon's
 *     point c (base, two pairs up its sides, tip) as (across, along);
 *   - `bool plantLimb(Plant p, PlantSpecies s, PlantCrown c, int limb, out
 *     PlantLimb l)`: tube `limb` (0 the trunk, then 1..`limbs`): its ends,
 *     radii, sway lag and colour;
 *   - `vec3 plantSway(Plant p, PlantSpecies s, float up, float lag)`: the
 *     wind's push on a point of a plant (a far crown's shadow disc too);
 *   - under `PLANT_SHADE` (plants.frag): `PlantSurface
 *     plantSurface(PlantFragment f)`, what a fragment of an element or a
 *     tube is.
 * It may use anything of scene_camera.glsl (the wind, sceneWind) and
 * scene_plant_records.glsl (plantRand, plantToWorld, plantCrownWidth, ...),
 * and plantSphere under PLANT_BUILD. Its data is its 20 floats of each
 * species' record (`data`, read as plantData(species, 0..4)). It compiles
 * and caches as a sward module does (SPIR-V under the user cache dir); one
 * that does not compile is logged and the node draws nothing.
 *
 * broworkshop's lib/trees is such a module: a crown of lobes on a
 * golden-angle spiral round a trunk with a limb to each, leaves near and
 * broad daubs far, needle sprays in tiers, a leaf's midrib and the sun
 * through it, the trunk bending in the wind and each lobe lagging.
 *
 * Casting (`castsShadow`): each shadow view drawn this frame (the shadow
 * pages being drawn, lighting-api.js) gets its own casters, the plants in
 * its frustum (in view or not) within `shadowRange`, each kept to
 * `shadowTexels` of the page's texels an element; a crown only a few of
 * those across casts its outline, a disc square to the light. So a coarse
 * level's thousands of trees cost a disc each. The pages are cached: a
 * still forest draws no casters, and swaying plants redraw only the sway
 * level near the eye each frame (finer levels read it; coarser ones keep
 * the plants still, where the sway is under a texel). A crown whose
 * shadow the sun's penumbra blurs wider than half a metre (a tall tree
 * under a low sun) hides its sway too: it is cached at every level, cast
 * as coarse as its penumbra.
 *
 * The land (`land`, a scene.createLand node they stand on): past `range`
 * the plants are its canopy. From the rules (each layer's chance, scale and
 * species weights, the species' height, radius and albedo) the land's tiles
 * take the cover the plants would make there (1 - e^-(crowns' area a square
 * metre)), their leaves' albedo and their top; past `range` the land rises
 * by that top as far as they cover it and is drawn as their leaves (lit as
 * a sward is: a layer of leaves too fine to resolve), and past
 * `shadowRange` it casts so. Both come in over the range's last tenth as
 * the plants there go, a growing share of them picked by where they stand.
 * So a forest reaches the horizon at no cost past the land's own. A plant
 * past `range` casts nothing (`shadowRange` is at most `range`).
 *
 * Where they stand: given as records (`plants`), or placed by `rules` over
 * a world with no edge. Rules are layers of candidates on a grid fixed in
 * the world (`spacing` apart, `jitter`ed), each kept by a `chance` field,
 * its species drawn by weight fields, sized by a `scale` field and
 * coloured by a `tint` field, and stood on the `ground` field. A field is a
 * FastNoise node tree (noise-api.js), its encoding, a number (a constant)
 * or `{ tree, seed }`: the engine decodes its own copy and places the
 * world's cells (leaves of 64 m, scene/world/world_cells.h) on its worker threads
 * about the eye, the nearest first, out to `range`, dropping those past
 * `range + hysteresis`. Every draw is the candidate's own hash, so a cell
 * places the same plants whenever and however often it is placed, and a
 * script sampling the same trees (`genSingle2D`) reads the fields the
 * engine placed by. Hand-placed `plants` stay beside the placed ones.
 *
 * What each view draws is planned on the GPU (scene_plants_gpu_plan.cpp):
 * the plants live in a store of runs a cell each (scene/world/cell_store.h),
 * uploaded where they change (a cell placed or dropped copies its run and
 * nothing else; only shadow pages and probe cascades over a changed cell
 * are redone); each frame the cell
 * pass culls the cells against the camera and every shadow view, and the
 * plan picks each plant of the cells in view, writing the draws' chunks and
 * indirect commands. So a frame's CPU cost does not grow with the plants (a
 * million cost what seventy thousand do), and the camera's plants go in
 * distance bands, nearest first. Each leaf is built once a frame by a
 * compute pass (plants_pose.comp, the module's plantElement) and its
 * points read by the colour and shadow passes. The node is left out of
 * raycasts and bounds; its transform is ignored (plants stand in the world).
 *
 * @example
 *   import { treeModule, treeSpecies } from "/lib/trees/index.js";
 *   const plants = scene.createPlants({
 *     module: treeModule(),
 *     species: [
 *       treeSpecies({ height: 15, radius: 5, lobes: 16, lobeSize: 0.52, leafLength: 0.34, elements: 8000,
 *                     light: [0.19, 0.32, 0.07], dark: [0.02, 0.05, 0.025], density: 0.9 }),
 *       treeSpecies({ height: 18, radius: 4.4, shape: 0.95, lobes: 34, needle: 1, droop: 0.5,
 *                     light: [0.10, 0.20, 0.08], dark: [0.015, 0.04, 0.025] }),
 *     ],
 *     plants: trees.map((t, i) => ({ x: t.x, y: t.y, z: t.z, yaw: t.yaw, scale: t.s, species: t.kind, seed: i })),
 *     castsShadow: true, receivesShadow: true,
 *   });
 *
 * @typedef {Object} PlantSpecies
 * The engine's fields (its share of the record: the plant's size, detail
 * and crown as the light sees it), then the module's `data`.
 * @property {number} [height=12] - m, at scale 1.
 * @property {number} [base=0.3] - where the crown starts, a fraction of the height.
 * @property {number} [radius=4] - the crown's radius (m).
 * @property {number} [elements=6000] - elements at full detail.
 * @property {number} [leafLength=0.22] - an element's length (m) at full detail.
 * @property {number} [limbs=18] - tubes besides the trunk (at most 48).
 * @property {number} [shape=0] - the crown's envelope: 0 round .. 1 a cone.
 * @property {number} [density=0.6] - the crown's density to the sun's light (1/m).
 * @property {Array<number>|string} [albedo] - the foliage's mean colour (linear, or '#rrggbb' sRGB): the probes' canopy.
 * @property {Array<number>|Float32Array} [data] - the module's: at most 20 numbers, plantData(species, 0..4).
 *
 * @typedef {Object} PlantRecord
 * @property {number} x
 * @property {number} y - its foot.
 * @property {number} z
 * @property {number} [yaw=0]
 * @property {number} [scale=1]
 * @property {number} [species=0] - index into the species table.
 * @property {number} [seed] - an integer under 2^24 (default its index): its shape.
 * @property {number} [tint=0] - -1..1, its colours toward the dark or the light.
 *
 * @example
 *   // A forest over the whole world: conifers where the pine noise is up.
 *   const N = (type, props) => { const n = FastNoise.create(type); for (const k in props) n.set(k, props[k]); return n; };
 *   const fbm = N('FractalFBm', { Source: N('Value', { 'Feature Scale': 160 }), Octaves: 3 });
 *   const pine = N('Max', { LHS: N('Simplex', { 'Feature Scale': 250 }), RHS: 0 });
 *   plants.setPlants({ rules: {
 *     ground: heightTree, range: 2000,
 *     layers: [{ spacing: 5.5, chance: N('Max', { LHS: fbm, RHS: 0 }), scale: 1, scaleVariety: 0.2,
 *                species: [{ species: 2, weight: pine }, { species: 0, weight: N('Subtract', { LHS: 1, RHS: pine }) }] }],
 *   } });
 *
 * @typedef {FastNoise|string|number|{tree: FastNoise|string|number, seed: number}} PlantField
 * A field over the ground (x, z): a FastNoise node, its `encode()`, a
 * constant, or either with its seed (default the rules' `seed`, else 1337).
 *
 * @typedef {Object} PlantLayer
 * @property {number} [spacing=5] - m between candidates.
 * @property {number} [jitter=0.9] - how far each is moved off its grid point: up to half this of the spacing each way.
 * @property {PlantField} [chance] - 0..1, the chance a candidate grows (default 1).
 * @property {Array<number|{species: number, weight: PlantField}>} species - a candidate's species,
 *   drawn by the weights where it stands (a bare index weighs 1); where every weight is 0, none grows.
 * @property {PlantField} [scale] - its size (default 1); under 0, none grows.
 * @property {number} [scaleVariety=0] - each plant's own: the size times 1 +- this.
 * @property {PlantField} [tint] - -1..1 (default 0).
 * @property {number} [tintVariety=0] - each plant's own, +- half of this.
 * @property {number} [sink=0] - m its foot is set below the ground.
 * @property {number} [seed] - the layer's draws (default its index + 1).
 *
 * @typedef {Object} PlantRules
 * @property {Array<PlantLayer>} layers
 * @property {PlantField} [ground] - the height a plant stands on (default 0).
 * @property {number} [range=1000] - m: cells nearer the eye than this are placed.
 * @property {number} [hysteresis=128] - m past `range` a placed cell is kept.
 * @property {number} [seed] - the fields' seed where a field names none.
 *
 * @typedef {Object} PlantsNodeOptions
 * @property {string|{source: string}} [module] - the plant module's GLSL (see above); '' none, nothing drawn.
 * @property {Array<PlantSpecies>|Float32Array} [species] - at most 64.
 * @property {Array<PlantRecord>|Float32Array} [plants] - hand-placed.
 * @property {PlantRules|null} [rules] - plants placed about the eye; null removes them.
 * @property {number} [pixels=14] - the size on screen (px) an element is kept to past full detail.
 * @property {number} [minElements=12] - the fewest a plant is drawn with.
 * @property {number} [woodRange=220] - past this (m) no trunks or limbs.
 * @property {number} [range=6000] - past this (m) no plant is drawn.
 * @property {number} [shadowTexels=12] - as `pixels`, in a shadow page's texels, for its casters.
 * @property {number} [shadowRange=1500] - past this (m), or `range`, no plant casts.
 * @property {SceneNode|null} [land] - the land they stand on, which is their canopy past `range` (above); null none.
 * @property {boolean} [castsShadow=false]
 * @property {boolean} [receivesShadow=false]
 */

// ── Herbs (bro.flora) ────────────────────────────────────────────────────────

/**
 * Herbs (broflora/herbs.h): ferns, wildflowers, forbs and ground cover,
 * nothing grown — a species table a herb module builds every plant from
 * where it stands (broworkshop's lib/herbs; GrassHerbOptions). The
 * presets:
 *   - ferns: bracken (tall fronds, deltoid, deeply lobed pinnae), ladyfern
 *     (a shuttlecock of lanceolate fronds; shade);
 *   - wildflowers: daisy (rays over a low rosette), poppy (red cups, lobed
 *     leaves), foxglove (a tall raceme of pink bells; part shade), bluebell
 *     (strap leaves, a nodding raceme of blue bells; shade);
 *   - forbs and ground cover: yarrow (white umbels, feathery leaves), nettle
 *     (leafy stems, toothed leaves in pairs), plantain (ribbed rosette, brown
 *     spikes), clover (a carpet of trifoliate leaves, white heads), woodsorrel
 *     (a carpet of notched leaflets, small white cups; deep shade).
 * @returns {Array<string>}
 */
bro.flora.herbPresets = function() {};

/**
 * A herb species as a plain object: a preset's name, or `{preset, ...fields}`
 * overriding any HerbSpecies field (herbs.h: habit 'rosette' | 'leafy' |
 * 'carpet'; height, spread, leaves, stems; stemLeaves and stemLeafSize (a
 * rosette's leaves up each flowering stem, the lowest that share of
 * leafLength: a foxglove's); leafShape 'frond' | 'simple' |
 * 'strap' | 'trifoliate' | 'lobed' | 'ribbed', leafLength, leafWidth,
 * leafTip, teeth, toothDepth, pinnae, lobes, stalk, rise, arch, leafColor,
 * gloss; stemColor, stemDiameter, ribDiameter, hairs (0 bare .. 1 a
 * poppy's bristling stems); bloomForm 'none' | 'ray' |
 * 'cup' | 'head' | 'spike' | 'umbel' | 'bell', bloomSize, bloomCount,
 * petals, bloomColor, centreColor, nod; where it grows: shade, shadeRange,
 * abundance, patch, patchCover, spacing — the distance between its plants
 * inside a colony, 0 for 1.6 x spread; a bracken's fronds stand 0.45 m
 * apart). Comes back with its sway worked out from
 * its build (`stemSway`, `leafSway`: {freq, sway, length}) and what it shows
 * from above (`meanColor`, `bloomShare`, `cover`). Null for an unknown name.
 * @example
 *   const tall = bro.flora.herbPreset({ preset: 'foxglove', height: 1.6, bloomColor: [0.8, 0.8, 0.75] });
 * @param {string|Object} spec
 * @returns {Object|null}
 */
bro.flora.herbPreset = function(spec) {};

/**
 * The records a herb module reads (56 floats a species: the build, then
 * its spacing, its side cover and its blooms' share of it, its leaves' top,
 * its stem leaves), from names or specs as herbPreset takes them (unknown
 * ones are skipped). broworkshop's lib/herbs packs them as its module's
 * data.
 * @param {Array<string|Object>} list
 * @returns {Float32Array}
 */
bro.flora.herbRecords = function(list) {};

// ── SceneNode: vegetation members ────────────────────────────────────────────

class SceneNode {

  // ── Vegetation records (MeshNode, InstancedMeshNode) ────────────────────
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
  // (spray). Over a minute of gusty wind the deflection's RMS and speed at
  // each level are within ~15% of an integrated oscillator under the same
  // drag. A level bends
  // in a loaded cantilever's shape, s² (3 - s) / 2 along it, the levels add
  // down the hierarchy, and the wood turns with its slope, so bark normals
  // follow the bend. A leaf turns with the wood it hangs from (its shading
  // follows its limb), flutters about its petiole (a twist about its stem
  // and a smaller pitch at 3-9 Hz on its own phase, growing with the drag,
  // in bursts) and in a gust turns part way downwind and folds along its
  // midrib. A calm is still. The shadow pass runs the same pose.
  //
  // Two layers: vegPose (the wood's offset and rotation at a rest point,
  // from the plant's root, height and trunk word, the limb's twig and limb
  // words, and the point's branch weight) and, on top, vegLeaf (a leaf's own
  // motion). Whatever calls vegPose with the same records moves exactly as
  // the rest of the plant (scene_vegetation.glsl).
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

  // ── Grass (scene.createGrass) ──

  /**
   * Grass: change the rings (`width`, `bladeDensity`, `range`), the sward
   * or herb module (`sward`, `herbs`) or the material (`color`,
   * `roughness`, shadows); what it leaves out keeps its value.
   * @example grass.setGrass({ range: 220, bladeDensity: 0.8 });
   * @param {GrassNodeOptions} opts
   * @returns {SceneNode}
   */
  setGrass(opts) {}

  /**
   * Grass: stand it on another land (what grows where is then that land's).
   * @param {SceneNode} land
   * @returns {SceneNode}
   * @throws {TypeError} when `land` is not a land node.
   */
  setGround(land) {}

  /**
   * Grass: replace the herb module and its data (and with `range` and
   * `far`, how far the herbs are drawn); how thickly they grow stays the
   * land's. `source: ''` draws none.
   * @example grass.setHerbs(herbModule({ species: ['poppy', 'daisy'] }));
   * @param {GrassHerbOptions} herbs
   * @returns {SceneNode}
   */
  setHerbs(herbs) {}

  /**
   * Grass: replace the sward module and its data (its blades, their far
   * look and the turf's look of them). `source: ''` grows no blades.
   * @example grass.setSward(swardModule({ tufts: 0.8, dead: 0.3 }));
   * @param {GrassSwardOptions} sward
   * @returns {SceneNode}
   */
  setSward(sward) {}

  /**
   * Grass: shade node `mesh` (the land, or a mesh over it) as this grass's
   * ground (turf), or with `on` false as itself again. Turf is shaded from
   * the land's cover where the land reaches: the sward the blades stand for
   * (as its module's swardTurf says: near the eye the roots and soil
   * between the blades, rising to the blade tops with distance), bare soil
   * where nothing grows, and under a closed canopy (`cover.w`) the leaf
   * litter, twigs, damp soil and moss. Past the land it takes the same data
   * from a mesh's vertex colour: r the grass density, g the dryness, b the
   * canopy. Every pattern is band-limited (finer than about two pixels
   * fades to its mean), so it reads from standing height to far above.
   * @example
   *   scene.createGrass({ ground: land, sward, turf: true });
   * @param {SceneNode} mesh
   * @param {boolean} [on=true]
   * @returns {SceneNode}
   */
  shadeGround(mesh, on) {}

  /**
   * Grass: the rings as set: `{width, bladeDensity, range}`.
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
   * herbInstances: [h0, h1, h2]}`, near ring first (h2 the far plants).
   * `instances` counts the cells issued in each ring (the tiles in view and
   * in the ring); each cell draws several blades, of which the land's cover
   * and the culling keep fewer. `herbInstances` the herb cells issued in
   * each herb ring (most hold no plant). (The ground's height under a mover
   * is the land's: `land.landHeight(x, z)`.)
   * @returns {{instances: Array<number>, movers: number, herbInstances: Array<number>}|undefined}
   */
  grassStats() {}

  // ── Plants (scene.createPlants) ──

  /**
   * Plants: replace the plants (an array of records) or set any of
   * PlantsNodeOptions (`module`, `species`, `plants`, the detail params).
   * @example plants.setPlants({ pixels: 20, shadowTexels: 16 });
   * @param {Array<PlantRecord>|PlantsNodeOptions} opts
   */
  setPlants(opts) {}

  /**
   * Plants: what a recent frame drew: `plants` (all of them), `visible`
   * (in the camera's view), `elements` and `limbs` (the camera's), and
   * `shadowElements` (every shadow view's casters summed, a disc one). The
   * plan's counts are read back from the GPU a frame or two late: after a
   * change, draw a few frames (`advanceTime`) before reading them.
   * @returns {{plants: number, visible: number, elements: number, limbs: number, shadowElements: number}|undefined}
   */
  plantStats() {}

  /**
   * Plants: the rules' placement now: `cells` placed and `pending` (wanted
   * and not yet placed; 1 before the first frame has looked about the
   * eye). A script waiting for the world in reach draws frames until
   * `pending` is 0.
   * @example for (let i = 0; i < 600 && plants.plantPlacement().pending > 0; i++) advanceTime(16);
   * @returns {{cells: number, pending: number}|undefined}
   */
  plantPlacement() {}

}
