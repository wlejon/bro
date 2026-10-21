// ── bro.scene: vegetation ────────────────────────────────────────────────────
//
// What grows in a scene: the plants node (`scene.createPlants`: trees and
// shrubs built on the GPU from a species table), the grass node
// (`scene.createGrass`: a meadow of procedural blades, its herbs and its
// turf, and bro.flora's herb species it draws them from), and the vegetation
// records that make any mesh sway in the scene's wind field
// (`setVegetation`, flora-api.js "Vegetation records").
//
// The `SceneNode` below continues the class declared in docs/scene-api.js
// (and continued in docs/scene-nodes-api.js). The wind field itself
// (`setWind` / `windAt`) is docs/scene-api.js.

// ── Dictionaries ─────────────────────────────────────────────────────────────

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
 *   scene.createGrass({ ground, floor: canopy, turf: true, herbs: {
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
 * per pixel, lit as the sward's mean face, laid over as far as the near
 * blades bend in the wind and its gusts, so a far meadow shows the wind's
 * sheen and its travelling waves), so blade count x width (the cover) stays about
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
 * the herbs' density); so may a `ground` function (its `grid` returns the
 * heights). Re-bake with `node.setGround(opts)` (a moved or edited ground,
 * a new season).
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
 * @property {number|Function} [floor] - the woodland floor, 0 open .. 1
 *   under a closed canopy (the crowns' shade over a point, an app's
 *   function of its plants), on the same grid. A turf ground (`turf`) lays
 *   leaf litter, twigs, damp soil and moss where it closes, and the herbs
 *   (`herbs`) choose their places by the light it leaves. Default: no floor
 *   (open everywhere).
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
 * Plants: trees and shrubs from `scene.createPlants(opts)`, every one built
 * where it stands on the GPU from its species and its seed (nothing is
 * stored per leaf). A plant is a crown of LOBES (clumps of foliage, on a
 * golden-angle spiral over an ellipsoid or, by `shape`, a cone) round a
 * trunk with a limb to each, and its foliage is ELEMENTS: opaque leaf
 * polygons, so the depth test rejects what is hidden before it shades.
 *
 * One representation at every distance: a plant drawn with N elements draws
 * the first N of one fixed sequence, which goes round its lobes, so any N
 * covers the whole crown; as N falls each element grows (by sqrt(full / N),
 * at most a lobe across) and turns toward the eye, so a near tree is
 * thousands of leaves and a far one a few dozen daubs of its colours. N is
 * the species' `elements` while a leaf is at least `pixels` across on
 * screen, then falls with the square of the distance (each element stays
 * about `pixels` across), never under `minElements` (nor, for a crown only
 * a few elements across on screen, under what covers it: a speck at the
 * horizon is one); its fraction grows the last element in, so nothing pops.
 * A daub standing for many leaves takes their mean colour and depth, so a
 * far crown shows its form's light and shade rather than a speckle. Limbs are drawn to 0.4 x `woodRange`,
 * the trunk to `woodRange`.
 *
 * A leaf is lit as its lobe's round more than its own face (more so as it
 * stands for more leaves): the sun reaches it through the crown's density
 * (`density`), the shadow map is read where that path leaves the crown, the
 * sun shows through the leaves at the rim from behind (`glow`,
 * `glowColor`), and the sky reaches a crown's core less. A near leaf shows
 * its midrib and is darker toward its stalk. Plants sway in the scene's wind
 * field (`setWind`): the trunk bends, each lobe lags, near leaves flutter.
 *
 * Casting (`castsShadow`): each shadow view drawn this frame (the shadow
 * pages being drawn, lighting-api.js) gets its own casters, the plants in
 * its frustum (in view or not) within `shadowRange`, each kept to
 * `shadowTexels` of the page's texels an element; a crown only a few of
 * those across casts its outline, a disc square to the light. So a coarse
 * level's thousands of trees cost a disc each. The pages are cached: a
 * still forest draws no casters, and swaying plants redraw only the sway
 * level near the eye each frame (finer levels read it; coarser ones keep
 * the plants still, where the sway is under a texel).
 *
 * What each view draws is planned on the GPU (scene_plants_gpu_plan.cpp):
 * the plants are sorted into world cells once, at setPlants, and uploaded
 * once (again only when they or the species change); each frame the cell
 * pass culls the cells against the camera and every shadow view, and the
 * plan picks each plant of the cells in view, writing the draws' chunks and
 * indirect commands. So a frame's CPU cost does not grow with the plants (a
 * million cost what seventy thousand do), and the camera's plants go in
 * distance bands, nearest first. Each leaf is built once a frame by a
 * compute pass (plants_pose.comp) and its corners read by the colour and
 * shadow passes. The node is left out of raycasts and bounds; its
 * transform is ignored (plants stand in the world).
 *
 * @example
 *   const plants = scene.createPlants({
 *     species: [
 *       { height: 15, radius: 5, lobes: 16, lobeSize: 0.52, leafLength: 0.34, elements: 8000,
 *         light: '#5d8c2a', dark: '#0c1c0a', density: 0.9 },
 *       { height: 18, radius: 4.4, shape: 0.95, lobes: 34, needle: 1, droop: 0.5,
 *         light: [0.10, 0.20, 0.08], dark: [0.015, 0.04, 0.025] },
 *     ],
 *     plants: trees.map((t, i) => ({ x: t.x, y: t.y, z: t.z, yaw: t.yaw, scale: t.s, species: t.kind, seed: i })),
 *     castsShadow: true, receivesShadow: true,
 *   });
 *
 * @typedef {Object} PlantSpecies
 * @property {number} [height=12] - m, at scale 1.
 * @property {number} [base=0.3] - where the crown starts, a fraction of the height.
 * @property {number} [radius=4] - the crown's radius (m).
 * @property {number} [shape=0] - 0 a round crown .. 1 a cone.
 * @property {number} [lobes=18] - clumps in the crown (at most 48).
 * @property {number} [lobeSize=0.42] - a lobe's radius, a fraction of the crown's.
 * @property {number} [leafLength=0.22] - a leaf's length (m) at full detail.
 * @property {number} [leafWidth=0.5] - its width over its length.
 * @property {number} [elements=6000] - leaves at full detail.
 * @property {number} [trunk=0.25] - the trunk's radius at its foot (m).
 * @property {number} [spread=0.8] - how far the lobes sit out from the trunk, 0..1.
 * @property {number} [droop=0.2] - how far the leaves (a needle tree's tiers) hang, 0..1.
 * @property {Array<number>|string} [light] - sunlit foliage colour (linear, or '#rrggbb' sRGB).
 * @property {number} [hueVariety=0.25] - plant to plant and lobe to lobe colour variety.
 * @property {Array<number>|string} [dark] - its colour deep in the crown.
 * @property {number} [glow=0.8] - how much the sun shows through from behind.
 * @property {Array<number>|string} [bark] - the trunk's colour.
 * @property {number} [density=0.6] - the crown's density to the sun's light (1/m).
 * @property {Array<number>|string} [glowColor] - the colour of the light through it.
 * @property {number} [needle=0] - 0 broad leaves .. 1 needle sprays: flat drooping tiers, narrow stiff elements.
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
 * @typedef {Object} PlantsNodeOptions
 * @property {Array<PlantSpecies>|Float32Array} [species] - at most 64.
 * @property {Array<PlantRecord>|Float32Array} [plants]
 * @property {number} [pixels=14] - the size on screen (px) an element is kept to past full detail.
 * @property {number} [minElements=12] - the fewest a plant is drawn with.
 * @property {number} [woodRange=220] - past this (m) no trunks or limbs.
 * @property {number} [range=6000] - past this (m) no plant is drawn.
 * @property {number} [shadowTexels=12] - as `pixels`, in a shadow page's texels, for its casters.
 * @property {number} [shadowRange=1500] - past this (m) no plant casts.
 * @property {boolean} [castsShadow=false]
 * @property {boolean} [receivesShadow=false]
 */

// ── Herbs (bro.flora) ────────────────────────────────────────────────────────

/**
 * Herbs (broflora/herbs.h): ferns, wildflowers, forbs and ground cover,
 * nothing grown — a species table the grass node builds every plant from
 * where it stands (scene.createGrass({herbs}), GrassHerbOptions). The
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
 * The table the grass node reads (56 floats a species: the build, then its
 * spacing, its side cover and its blooms' share of it, its leaves' top, its
 * stem leaves),
 * from names or
 * specs as herbPreset takes them (unknown ones are skipped). createGrass's
 * `herbs.species` takes the names and specs directly; this is for building
 * the table once and sharing it.
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
   *   scene.createGrass({ ground, density: cover, floor: canopy, turf: [ground, farGround] });
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

  // ── Plants (scene.createPlants) ──

  /**
   * Plants: replace the plants (an array of records) or set any of
   * PlantsNodeOptions (`species`, `plants`, the detail params).
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

}
