/**
 * bro.flora, ecosystem simulation (broflora sibling)
 *
 * Procedural plant and ecosystem growth after Makowski et al. 2019,
 * "Synthetic Silviculture": plants grow as trees of branch *modules*, each an
 * instance of a module prototype (a small node/edge graph). Bud fate,
 * developmental archetypes, pipe-model thickening, tropism and competitive
 * light interception in a shared voxel shadow grid all run inside
 * `world.step(dt)`. The emit methods turn the current state into geometry:
 * bromesh `Mesh`es, branch segment lists, per-segment foliage state, bloom
 * anchors, and packed instance buffers.
 *
 * `bro.flora.available` is `true` whenever broflora is compiled in.
 *
 * ── Minimal world ──
 *
 *   const world = bro.flora.createWorld({ rngSeed: 42 });
 *   const proto = world.addPrototype(bro.flora.prototypes.monopodial(3, 0.6));
 *   world.addVoronoiSite(proto);               // new modules need a site to pick from
 *   const idx = world.addPlant({
 *     origin: [0, 0, 0],
 *     prototypeIndex: proto,
 *     species: { maxAge: 50, apicalControl: 0.8 },
 *   });
 *   for (let i = 0; i < 20; i++) world.step(1.0);
 *   const bark   = world.emitMesh(6);                          // Mesh
 *   const leaves = world.emitFoliageMesh(bro.flora.leafCluster('opposite'));
 *
 * ── Prototype selection ──
 * A plant's root module uses the prototype named by `addPlant`'s
 * `prototypeIndex`. Every module spawned after that (and every seedling a
 * mature plant drops) picks its prototype from the world's Voronoi sites:
 * the site nearest to the module's current (determinacy, apicalControl)
 * point wins. A world with no sites grows each plant's root module only.
 *
 * ── Wind and density ──
 * `bro.flora.setWind` / `setDensity` / `update` are process-global, not per
 * world. The global wind bends geometry *at emit time* in emitMesh,
 * emitPlantMesh, emitFoliageMesh, emitPlantFoliageMesh,
 * emitFoliageTransforms and emitSegmentTransforms, and `update(dt)` sways
 * the placement batches with the same model: the sway is zero at y = 0 and
 * grows with height (0.04h + 0.015h^2), a gust wave phased by ground
 * position moves neighbours out of step, and each point is offset
 * downwind and tilted rigidly toward the wind (at most 0.35 rad). A mesh
 * vertex moves by the offset at the vertex and its normal turns by the tilt
 * there. An instance matrix is swayed along its height: its translation
 * moves by the offset at its origin (so an instance rooted at y = 0 stays
 * put) and its basis turns by the tilt at its tip, origin + its +Y basis
 * column (floats 1 / 5 / 9, scale included). So a placement standing on the
 * ground still bends, a taller instance bends more, matrices stay
 * rigid (the basis is rotated, never sheared) and the tint floats are left
 * alone. `update(dt)` advances the gust phase.
 * emitSegments, emitBranchTubes, emitScatterSegments, emitBloomMesh and
 * the SDF meshes are never bent.
 * The global density (default 1) multiplies the leaf placement
 * `perUnitLength` of every foliage emit.
 * That baked bend is the old model and is off by default (strength 0). The
 * motion plants should have comes from the vegetation records below, which
 * bro.scene sways on the GPU in its wind field.
 *
 * ── Vegetation records ──
 * Every geometry emitter also writes what bro.scene needs to move a plant
 * like a plant (docs/scene-nodes-api.js "Vegetation"); emit, create the
 * nodes, and the trees sway in `scene.setWind`'s field:
 *
 *   const bark = scene.createMesh({ data: world.emitMesh(7) });              // mesh.vegetation
 *   const tubes = scene.createInstancedMesh({ tube: world.emitBranchTubes() });   // .vegetation
 *   const leaves = scene.createInstancedMesh({ mesh: card, doubleSided: true,
 *       scatter: Object.assign({}, opts, world.emitScatterSegments(opts)) });     // .vegetation
 *
 * Per vertex (emitMesh, emitPlantMesh, emitFoliageMesh, emitPlantFoliageMesh,
 * emitBloomMesh: `mesh.vegetation`) and per instance (emitFoliageVegetation,
 * emitSegmentVegetation) a record is 16 floats, in the emit's space:
 *
 *   0-3   root.xyz, height        the plant's origin and its height (tallest segment end above it)
 *   4-7   crown.xyz, crownRadius  the leaf-mass weighted centre of the plant's branches and the
 *                                 farthest leafy branch end from it; radius 0 on wood
 *   8-11  anchor.xyz, weight      a leaf's (or bloom's) attachment point on its segment, a bark
 *                                 vertex's skeleton point; the branch weight there
 *   12-15 axis, twig, limb, trunk four packed words (below): a leaf's stem direction (0 on
 *                                 wood), its limb's phase and twigs, its limb, its plant's trunk
 *
 * Per segment (emitBranchTubes, emitScatterSegments: `.vegetation`, index
 * aligned with `segments`) it is 16 floats too: root.xyz, height,
 * crown.xyz, crownRadius, weightFrom, weightTo, 0, 0, then 0, twig, limb,
 * trunk; the shader interpolates the weight along the segment and anchors
 * each scattered leaf where it hangs (the leaf's own axis comes from its
 * placement).
 *
 * The motion model. A plant is three levels of cantilever — the trunk (root
 * to height), each limb (where it leaves the trunk to its farthest tip) and
 * the limb's leafy sprays — each a beam fixed at its base, in its first
 * bending mode, with two terms from the simulation's own geometry (the
 * pipe model's diameters, the foliage):
 *
 *   freq  natural frequency (Hz), sqrt(k / m) / 2 pi: the tip stiffness
 *         k = 3 E I / L^3 at the base diameter, over the mass the mode
 *         moves (every piece of wood and leaf it carries times the mode
 *         shape squared where it sits). Euler-Bernoulli's f ~ d / L^2 for a
 *         bare beam; a stem slows for what it holds far out. The trunk is
 *         the slowest (a ~10 m tree sways at ~0.5-1 Hz), sprays the quickest.
 *   sway  static tip deflection over the length at 10 m/s: the drag on the
 *         leaves it carries (the crown's face, half its disc, shared among
 *         segments by foliage) over k. Twice as thick at one length and
 *         load deflects 16x less; twice as long, 4x more.
 *
 * (broflora/sway.h has the constants: green hardwood E 9 GPa, 800 kg/m^3;
 * Cd 0.3; gains for the trunk's taper and root plate.) bro.scene drives
 * each level with the drag at the plant, (U / 10)^1.3, as a damped
 * oscillator (scene-nodes-api.js "Vegetation").
 *
 * The packed words are integers below 2^24, exact in a float; each number
 * is log-quantised (code 0 is zero) over freq 0.05-50 Hz, sway 1e-5-1,
 * length 0.005-200:
 *
 *   axis   a leaf's stem direction, octahedral, 12 bits a component, plus one
 *   twig   phase (6 bits, top), spray freq, sway, length (6 bits each)
 *   limb   freq, sway, length (8 bits each)
 *   trunk  freq, sway (12 bits each)
 *
 * The branch weight is 0 on the trunk — the leader chain up from the
 * thickest root segment, while each child keeps at least 80 % of its
 * parent's radius and 35 % of the base's — and climbs to 1 at every twig
 * tip along each limb, continuously (a child starts at its parent's end
 * weight), so bending by weight² never tears a joint. The phase is a hash
 * of the limb (the chain leaving the trunk); all its sub-branches share it,
 * so a limb moves as one piece.
 *
 * Bloom anchors carry `segment`, the world segment index they grow on, so
 * a bloom can take its segment's record.
 *
 * ── Instance matrices ──
 * emitFoliageTransforms, emitSegmentTransforms and the placement batches
 * use 16 floats per instance laid out row by row: floats 0-2 / 4-6 / 8-10
 * are the three rows of the 3x3 basis (columns = local X, Y, Z axes, scale
 * baked in) and floats 3 / 7 / 11 are the translation. Floats 12-15 are
 * not a fourth matrix row: they are the RGBA instance tint of bro's
 * InstancedMeshNode layout, written as white (1, 1, 1, 1). Read the buffer
 * as a row-major 3x4 affine plus a tint, never as a column-major 4x4.
 *
 * ── Foliage density ──
 * Every foliage emit places leaves with bromesh's leaf placement
 * (LeafPlacementOptions below). When `opts.densityWeight` is not given, it
 * is filled per segment from the simulation:
 *   (0.12 + 0.88 * lightExposure01) * min(1, age01) * (1 - senescence01)
 *     * twigGrade01^2 * (isTerminal ? 1 : 0.1)
 * so shaded, young, dying or thick segments go bare.
 *
 * ── Crown clumps (the default) ──
 * By default a foliage emit hangs its leaves in crown clumps rather than
 * spraying them along every twig: emitClumps picks clumps greedily from the
 * tips of the leafy twigs (the ones the density above gives leaves to),
 * outermost first, each kept only if it stands `clumpSpacing` clear of those
 * already taken, so the silhouette fills first and sky shows between the
 * clumps. A clump's radius follows its plant's crown (`clumpRadiusOfCrown`
 * times the crown radius, unless `clumpRadius` is given), its leaf density
 * the twig's light and health; bromesh's placeLeavesInClumps fills three
 * overlapping lobes of each, `perArea` leaves per unit² of clump surface.
 * The GPU scatter node draws the very same leaves (scene-nodes-api.js
 * setScatterSegments, `clumps`). Every clump leaf's vegetation record is its
 * twig tip's, so a clump rides its twig. Pass `clumps: false` for the
 * per-twig spray (perUnitLength, rollJitter, densityFalloff, dedupRadius and
 * scaleByRadius apply only there). The global density multiplies perArea as
 * it does perUnitLength.
 *
 * ── Kinks ──
 * Bark is drawn with a species' `kink` (default 0.2): each node of each
 * module steps sideways by a hashed fraction of its incoming edge, the steps
 * accumulating down the module tree, so branches zig-zag like grown wood
 * while every segment still starts exactly where its parent ends. Emit only:
 * the simulation never sees them, and `kink: 0` gives the grown skeleton.
 *
 * ── Lists, counts and seeds ──
 * Every list an option reads element by element (a prototype's `nodes`,
 * `edges` and `terminalNodes`, `densityWeight`) is capped at 2^24
 * (16777216) elements: a longer `length` is a RangeError. The list is read
 * as it is, never sized by its claimed length first, so a missing element
 * (a `nodes` entry that is not an object, a `densityWeight` entry that is
 * not a number) is a TypeError at that index. Placement batches hold at
 * most 2^24 instances. Seeds (`rngSeed`, the leaf placement `seed`) are
 * integers in [0, 2^53 - 1]: a non-number is a TypeError, a negative,
 * fractional, NaN or infinite one a RangeError.
 */


// ═════════════════════════════════════════════════════════════════════════════
// Option and result shapes
// ═════════════════════════════════════════════════════════════════════════════

/**
 * @typedef {Object} FloraWorldOptions
 * @property {number} [rngSeed] - Seed for the world's RNG (seeding, variation):
 *   an integer in [0, 2^53 - 1] (TypeError for a non-number, RangeError otherwise).
 * @property {FloraClimate} [climate]
 * @property {Object} [shadow] - The voxel shadow grid plants compete in. Omit
 *   it and there is no grid: every module sees full sun and sampleShadow()
 *   returns null.
 * @property {Array<number>} [shadow.origin=[0,0,0]] - World position of cell (0,0,0)'s min corner.
 * @property {number} [shadow.cellSize=1]
 * @property {number} [shadow.width=0] - Cells along +X.
 * @property {number} [shadow.height=0] - Cells along +Y (vertical).
 * @property {number} [shadow.depth=0] - Cells along +Z.
 * @property {number} [shadow.fill=1] - Initial light value of every cell (1 = full sun).
 */

/**
 * @typedef {Object} FloraClimate
 * @property {number} [annualTempBase=15] - Temperature at elevation 0, degrees C.
 * @property {number} [annualPrecip=1000] - Annual precipitation, mm/yr.
 * @property {number} [tempLapsePerUnit=-0.0065] - Temperature change per unit of height.
 */

/**
 * A branch module prototype: a small graph that grows as one unit. The
 * `bro.flora.prototypes.*` factories return this shape, and addPrototype
 * accepts it.
 * @typedef {Object} FloraPrototypeSpec
 * @property {string} [name]
 * @property {Array<{position: Array<number>, ageAtBirth?: number, lengthMax?: number, thickening?: number}>} nodes -
 *   Node rest positions in module space. `ageAtBirth` (default 0) is the
 *   module age at which the node appears, `lengthMax` (default 1) caps the
 *   incoming segment's length, `thickening` (default 1) is its length growth
 *   rate. At least one node is required, each an object (TypeError
 *   otherwise); at most 2^24 of them, as for `edges` and `terminalNodes`.
 * @property {Array<Array<number>|{a: number, b: number}>} [edges] - Node index
 *   pairs, `[a, b]` or `{a, b}`, parent before child.
 * @property {number} [rootNode=0]
 * @property {Array<number>} [terminalNodes] - Nodes where child modules (and blooms) attach.
 */

/**
 * Species parameters. addPlant takes any subset; unnamed fields keep these
 * defaults. plantInfo returns the full set.
 * @typedef {Object} FloraSpecies
 * @property {number} [maxVigor=1] @property {number} [minVigor=0.01] @property {number} [rootVigorMax=1]
 * @property {number} [apicalControl=0.55] - Main vs lateral vigor split (juvenile).
 * @property {number} [determinacy=0.5] - Juvenile determinacy (prototype-selection axis).
 * @property {number} [shadeTolerance=0.3]
 * @property {number} [apicalControlMature=0.85] @property {number} [determinacyMature=0.2]
 * @property {Array<number>} [tropismDir=[0,-1,0]] @property {number} [tropismG1=1] @property {number} [tropismG2=1]
 * @property {number} [growthScale=1]
 * @property {number} [climateOptT=15] @property {number} [climateOptP=1000]
 * @property {number} [climateSigT=10] @property {number} [climateSigP=500]
 * @property {number} [maxAge=100] @property {number} [floweringAge=10] @property {number} [seedingRadius=5]
 * @property {number} [moduleMatureAge=1] @property {number} [pipeExp=2.5] @property {number} [leafDiameter=0.02]
 * @property {number} [kink=0.2] - Sideways step per node, a fraction of its edge (see "Kinks"); emit only.
 * @property {number} [terrainAnchorWeight=0] @property {number} [maxSeedingSlope=1.5707963]
 * @property {number} [distributionWeightCollisions=1] @property {number} [distributionWeightTropism=0.5]
 * @property {number} [orthotropy=0.3] @property {number} [individualVariation=0]
 * Bark (emit only: the simulation never reads it). The first three ride in
 * every bark record (emitBranchTubes, a forest's tubes) and are drawn by
 * bro.scene's bark shading; the last two shape the tubes.
 * @property {number} [barkFurrow=0.55] - 0 smooth (beech) .. 1 deep furrows between plates (oak).
 * @property {number} [barkScale=1] - The plates' spacing, x the default (which already widens with the girth); 0.25..4.
 * @property {number} [barkMoss=0.5] - Moss and lichen, 0 none .. 1 lush (the shaded side, a limb's top, the foot).
 * @property {number} [rootFlare=0.6] - The trunk's swell where it meets the ground, x its radius.
 * @property {number} [branchCollar=0.5] - A side branch's swell where it leaves its parent, x its radius.
 * Habit (a plant's LEADER is its root module and every main child down from
 * it; its LATERALS are every other module): what makes a conifer's whorled
 * spire, a weeping or columnar crown, a multi-stemmed shrub. The species
 * presets (`bro.flora.speciesPresets()`) set these; the defaults grow as
 * the paper's trees do.
 * @property {number} [apicalDominance=0] - 0: a main child's vigor is shared against its siblings by light; 1: it keeps that share of its parent's outright (a conifer's excurrent leader).
 * @property {number} [maxBranchOrder=0] - The deepest axis order that still branches (leader 0, boughs 1, ...); 0 no limit.
 * @property {number} [crownSlope=0] - The crown envelope: a lateral reaches no further from the axis than this x its depth below the top (0.35 a fir's cone; 0 none) ...
 * @property {number} [crownBase=0] - ... plus this at the top (a blunt spire or a rounded top) ...
 * @property {number} [crownRadiusMax=0] - ... and never past this (a column, a flat-topped pine; 0 no cap).
 * @property {number} [crownLift=0] - Self-pruning: a bough leaving the leader below this fraction of the plant's height is shed, and the short shoots the leader keeps along that bare trunk carry no foliage (0.5 a pine's crown on the top half of its trunk; 0 boughs to the ground).
 * @property {number} [heightMax=0] - No shoot buds above this height (m): the leader stops and the upper boughs fill in under it, a mature pine's flat top (0 no limit).
 * @property {number} [prototypeGroup=0] - The prototype sites the leader grows from.
 * @property {number} [lateralGroup] - The sites a lateral grows from (default: the leader's group).
 * @property {number} [lateralElevation=0] - A lateral's set-point elevation, radians from the horizon: 0 level boughs (fir), negative drooping or weeping (spruce), positive rising (pine), near pi/2 turned up (a columnar cypress).
 * @property {number} [lateralOrthotropy=NaN] - How strongly a lateral's new growth returns to that angle (0 goes on along the arm, 1 aims at it); NaN grows laterals as the leader.
 * Foliage and colour (emit only): what a renderer draws the leaves as.
 * emitForest carries them per specimen (`specimenLooks`), and bro.scene's
 * forest node draws them.
 * @property {string|number} [leafForm='broad'] - 'broad' (0), 'needle' (1) or 'scale' (2): a conifer's leaf is a whole spray, its needles or scale leaves cut out per pixel.
 * @property {Array<number>} [leafColor=[0,0,0]] - The leaves' mean albedo (linear); 0,0,0 the renderer's own.
 * @property {Array<number>} [barkColor=[0,0,0]] - The bark's; 0,0,0 the renderer's own.
 * @property {number} [clumpScale=1] - The crown clumps' size, x what the emit's options give (a conifer's small dense tufts 0.3).
 * @property {number} [leafSize=1] - A leaf's size, x the renderer's card (a boxwood 0.3): its clumps carry 1/leafSize² the leaves, so they cover as much.
 * @property {number} [leafWidth=0] - A broad leaf's width over its length (0 the renderer's card, ~0.5): its outline, cut per pixel near (emitForest's `specimenLeaves`).
 * @property {number} [leafTip=0] - 0 a pointed (acuminate) tip, 1 a rounded one (a boxwood's).
 * @property {number} [leafTeeth=0] - Teeth along each side of the margin (0 entire).
 * @property {number} [leafToothDepth=0] - Their depth as a share of the half-width: under ~0.15 a serration (hydrangea 0.06), deeper spines between concave scallops whose margin waves up and down (holly 0.32).
 * @property {number} [leafGloss=0] - 0 a matte leaf (the renderer's roughness), 1 a waxy, glossy one (holly 0.9): sharper sheen, less light through.
 * @property {string|number} [phyllotaxis='alternate'] - A broad leaf's arrangement on its shoot: 'alternate' (0, two ranks: an oak's, a birch's), 'opposite' (1, pairs turning a quarter at each node: a hydrangea's, a boxwood's) or 'spiral' (2, a 2/5 spiral: a holly's). The forest node hangs each leaf from a node of its shoot so.
 * @property {number} [petiole=0.2] - A broad leaf's stalk, as a share of its blade's length (0 .. 0.6; a birch 0.3, a boxwood 0.05): the leaf hangs this far from its twig.
 * @property {number} [sprayBrush=0] - Needle and scale forms: 0 the sprays lie round the clump facing out (a fir's), 1 each turns up and out along its twig, gathered toward the clump's end: a pine's upswept brushes at the branch tips.
 * @property {number} [clumpFlat=1] - A clump's height over its width: 1 a broadleaf's round lobes, ~0.4 the flat pads of a fir's level sprays (the clumps pack closer up and down by as much).
 * @property {number} [clumpReach=0] - A clump reaches no further across than this x its distance from the plant's axis, plus a quarter of its size (0 no limit): a conifer's short top boughs carry small clumps, so its crown narrows to a spire.
 * @property {number} [clumpFill=0] - 0 (a broadleaf tree's): foliage on the crown's outermost shoots, its interior thinly leaved; 1: every shoot tip carries foliage however its module has branched on (a conifer's clothed boughs, a shrub's mound).
 * @property {number} [clumpInner=0] - 0: a shoot that has branched on carries a tenth of a tip's foliage, thinned further by its shade; 1: as much as a tip in half light, and the clumps packed up to 40% closer (a shrub's mound leaved inside and to the ground, its stems hidden; a juniper's bushy cone).
 * @property {number} [clumpGap=1] - the clumps' spacing along a shoot, times the emit's (0.5 .. 4): above 1 they stand apart as separate tufts with sky between them (a pine's needle brushes at its boughs' ends).
 * @property {string|number} [bloomForm='none'] - What it flowers as once past floweringAge: 'none' (0), 'flower' (1, one open flower at a shoot's tip), 'head' (2, a round head of florets), 'spike' (3, florets up an upright stalk). Emitted per specimen (emitForest's `blooms` / `specimenBlooms`) and drawn by bro.scene's forest node (setForestBlooms).
 * @property {Array<number>} [bloomColor=[0,0,0]] - The blooms' albedo (linear).
 * @property {number} [bloomSize=0] - A bloom's size (m): a flower's or a head's across, a spike's length.
 */

/**
 * @typedef {Object} FloraPlantSpec
 * @property {Array<number>} [origin=[0,0,0]]
 * @property {number} [age=0] - Starting plant age.
 * @property {number} [prototypeIndex] - Prototype of the root module. Omit it
 *   and the plant starts with no modules (and emits nothing).
 * @property {number} [initialVigor] - Root module vigor; default `species.minVigor * 2`.
 * @property {FloraSpecies} [species]
 */

/**
 * One branch segment. Segments come per prototype edge per module, in
 * module-topological order; `parent` indexes into the same returned array
 * (world-level lists keep counting across plants) or is -1 for a root.
 * @typedef {Object} FloraSegment
 * @property {Array<number>} from - World-space start [x, y, z].
 * @property {Array<number>} to - World-space end.
 * @property {number} radius - Radius at the `to` end.
 * @property {number} depth - Hops from the plant's root segment (roots are 0).
 * @property {number} parent - Parent segment index, or -1.
 */

/**
 * Per-segment foliage state, index-aligned with the matching segment list.
 * @typedef {Object} FloraFoliageSample
 * @property {number} mass - Default leaf density multiplier in [0, 1].
 * @property {number} age01 - module age / moduleMatureAge, clamped to [0, 2].
 * @property {number} vigor01 - module vigor / maxVigor, [0, 1].
 * @property {number} light01 - Effective light after the shade-tolerance floor.
 * @property {number} lightExposure01 - Raw illumination, [0, 1] (true shadow gradient).
 * @property {number} senescence01 - 0 until maxAge, ramps to 1 over the next 20% of maxAge.
 * @property {boolean} isTerminal - The owning module has no child modules.
 * @property {number} twigGrade01 - 1 at leaf thickness, 0 on branches over ~6x leafDiameter.
 */

/**
 * A flower/fruit candidate at a terminal node of a terminal module. Only
 * flowering plants (age past `floweringAge`) produce anchors.
 * @typedef {Object} FloraBloomAnchor
 * @property {Array<number>} position - World-space [x, y, z] of the twig tip.
 * @property {Array<number>} normal - Unit direction along the twig, outward (+Y if degenerate).
 * @property {number} age01 @property {number} vigor01 @property {number} lightExposure01 @property {number} senescence01
 * @property {number} segment - The world segment (emitSegments index) the bloom grows on; -1 if none.
 */

/**
 * bromesh leaf placement options, read by every foliage emit. Unnamed
 * fields keep these defaults.
 * @typedef {Object} FloraLeafPlacementOptions
 * @property {number} [maxRadius=0.05] - Skip segments thicker than this (keeps leaves off the trunk).
 * @property {number} [minDepth=1] - Skip segments shallower than this.
 * @property {boolean} [terminalOnly=false] - Only segments with no child segments.
 * @property {number} [perUnitLength=20] - Average leaves per unit length (times the global density).
 * @property {number} [densityFalloff=0] - >0 biases leaves toward segment tips.
 * @property {Array<number>} [densityWeight] - Per-segment multiplier; default computed from the simulation (see top).
 *   At most 2^24 entries (RangeError), every one a number (TypeError).
 * @property {number} [upBias=0.5] - 0 = leaves point radially out, 1 = toward +Y.
 * @property {number} [tiltJitter=0.3] - Radians of random pitch.
 * @property {number} [rollJitter=0.2] - Radians of random roll.
 * @property {number} [baseScale=1] @property {number} [scaleJitter=0.2] - Fraction of baseScale.
 * @property {number} [scaleByRadius=0] - 1 = scale leaves with radius / maxRadius.
 * @property {number} [dedupRadius=0] - Minimum spacing between leaf origins; 0 = off.
 * @property {number} [seed=0] - An integer in [0, 2^53 - 1] (TypeError for a
 *   non-number, RangeError otherwise). Clump leaves use its low 32 bits.
 *
 * Crown clumps (see "Crown clumps"; maxRadius, minDepth, densityWeight,
 * upBias, tiltJitter, baseScale, scaleJitter and seed apply to them too, with
 * the clump defaults below where they differ):
 * @property {boolean} [clumps=true] - false: the per-twig spray.
 * @property {number} [clumpRadius=0] - Fixed clump radius; 0 sizes clumps from the crown.
 * @property {number} [clumpRadiusOfCrown=0.26] - Clump radius as a fraction of its plant's crown radius.
 * @property {number} [clumpJitter=0.3] - +/- fraction of the radius per clump.
 * @property {number} [clumpSpacing=1.4] - Least centre distance, in mean radii: under 2 clumps overlap.
 * @property {number} [minDensity=0.08] - The leaf density a twig needs to carry a clump.
 * @property {number} [perArea=40] - Leaves per unit² of clump surface (times the global density).
 * @property {number} [shell=0.4] - Depth of the shell the leaves sit in, as a fraction of the lobe radius.
 * (Clump defaults: upBias 0.3, tiltJitter 0.6, scaleJitter 0.25.)
 */

/**
 * A crown clump (bromesh's LeafClump).
 * @typedef {Object} FloraClump
 * @property {Array<number>} center - [x, y, z], just past its twig's tip along it.
 * @property {number} radius
 * @property {Array<number>} dir - Unit direction of its twig.
 * @property {number} density - Leaf density in (0, 1].
 * @property {number} segment - The segment it hangs on (emitSegments index).
 */

/**
 * @typedef {Object} FloraSdfMeshOptions
 * @property {number} [voxelSize=0.04] - Grid spacing; values <= 0.005 fall back
 *   to 0.04. Each axis is clamped to 8..256 voxels, so a large plant gets a
 *   coarser grid than asked for.
 * @property {number} [smoothK=0.03] - Smooth-union blend radius; <= 1e-4 is a hard union.
 * @property {boolean} [useSurfaceNets=true] - false = marching cubes.
 * @property {number} [margin=0.08] - Padding added around the bounds (on top of smoothK).
 */


// ═════════════════════════════════════════════════════════════════════════════
// FloraWorld
// ═════════════════════════════════════════════════════════════════════════════

/**
 * A simulation world. Not constructible (`new bro.flora.FloraWorld()`
 * throws); get one from `bro.flora.createWorld`. Also exposed as the global
 * `FloraWorld` for `instanceof`.
 *
 * Methods taking a `plantIdx` return null (mesh emits) or `[]` (list emits)
 * for an index out of range.
 */
class FloraWorld {

  /** Simulation time in seconds. @readonly @type {number} */
  simTime;

  /** Number of plants (seeding during step() can add more). @readonly @type {number} */
  plantCount;

  /** Number of registered prototypes. @readonly @type {number} */
  prototypeCount;

  /** Module instances across all plants. @readonly @type {number} */
  moduleCount;

  /**
   * Register a module prototype. Prototypes must be registered before any
   * plant or site refers to them.
   * @param {FloraPrototypeSpec} spec
   * @returns {number} The prototype index, or -1 if `spec` has no nodes.
   */
  addPrototype(spec) {}

  /**
   * Add a Voronoi site in the (determinacy, apicalControl) plane. Every new
   * module takes the prototype of the site nearest its own (determinacy,
   * apical control) point, so several sites let one plant switch
   * architecture as it matures. Not a spatial position.
   * @param {number} prototypeIndex
   * @param {number} [determinacy=1]
   * @param {number} [apicalControl=0.5]
   * @returns {FloraWorld} this
   */
  addVoronoiSite(prototypeIndex, determinacy, apicalControl) {}

  /**
   * Add a plant.
   * @param {FloraPlantSpec} spec
   * @returns {number} The plant index, or -1 when `spec` is not an object or
   *   `prototypeIndex` names no prototype.
   */
  addPlant(spec) {}

  /**
   * Remove a plant by swap-and-pop: the last plant takes index `plantIdx`.
   * @param {number} plantIdx
   * @returns {boolean} false for an index out of range.
   */
  removePlant(plantIdx) {}

  /**
   * Advance the simulation by `dt` seconds. `dt` must be a finite number
   * >= 0: a missing or non-number `dt` is a TypeError, and NaN, a negative
   * or an infinite one (or one past float range, ~3.4e38) a RangeError (it would poison simTime and every
   * plant's age). `step(0)` is accepted.
   * @param {number} dt
   * @returns {FloraWorld} this
   */
  step(dt) {}

  /**
   * `ticks` steps of `dt` in one call, the same world as that many `step(dt)`
   * calls. With `opts.cache` (a directory, e.g. `bro.app.cacheDir`) the grown
   * world is read from a cache of grown worlds when one is there and written
   * to it when not. The key is everything the result depends on: the world as
   * it stands (environment, prototypes, plants, rng), `ticks`, `dt`, the
   * build, and a hash of broflora's step source, so an edit to the sim (but
   * not to emit or clumps) or to the world grows afresh. A cache that cannot
   * be read or written is skipped and named in `error`.
   * @param {number} ticks integer >= 0
   * @param {number} dt
   * @param {{cache?: string}} [opts]
   * @returns {{cached: boolean, ms: number, key?: string, error?: string}} `ms` is wall time
   * @example const g = world.grow(2500, 0.02, { cache: bro.app.cacheDir });  // g.cached on the second run
   */
  grow(ticks, dt, opts) {}

  /**
   * Runtime snapshot of one plant: `{origin, age, flowering, senescing,
   * moduleCount, effectiveRootVigorMax, rootVigor?, rootLight?, species}`,
   * where `rootVigor` / `rootLight` exist only when the plant has modules and
   * `species` is the full FloraSpecies.
   * @param {number} plantIdx
   * @returns {Object|null} null for an index out of range.
   */
  plantInfo(plantIdx) {}

  /**
   * Replace climate fields; unnamed fields keep their value.
   * @param {FloraClimate} opts
   * @returns {FloraWorld} this
   */
  setClimate(opts) {}

  /**
   * Light value of the shadow-grid cell containing `pos` (1 = full sun).
   * @param {Array<number>} pos - [x, y, z]
   * @returns {number|null} null when there is no grid or `pos` is outside it.
   */
  sampleShadow(pos) {}

  /**
   * Where ground cover grows among the plants, as the `density` function
   * bro.scene's `createGrass` takes: `(x, z, y) => 0..1` in world metres.
   * Grass thins in the crowns' shade (the shadow grid's light `above` m over
   * the ground at y) and clears at each trunk (within `trunkClear` m, fading
   * out by 3.5 times that); `floor` is what is left at the darkest. `scale`
   * is the node scale the plants are drawn at. The plants are read when it
   * is called, so call it again after more growth.
   * @example
   *   scene.createGrass({ ground, density: world.groundCover({ scale: 1.5 }) });
   * @param {{scale?: number, floor?: number, shade?: number, trunkClear?: number, above?: number}} [opts]
   *   defaults: scale 1, floor 0.3, shade 0.6, trunkClear 0.7, above 0.5
   * @returns {function(number, number, number=): number}
   */
  groundCover(opts) {}

  /**
   * Check the world's structural invariants.
   * @returns {string|null} null when valid, else the first error.
   */
  validate() {}

  // ── Branch geometry ──

  /**
   * Bark mesh of every plant: one tapered open cylinder per segment,
   * `sides` facets around. Wind-bent. Carries `vegetation` and tangents.
   * @param {number} [sides=6] - An integer in [3, 256]: TypeError for a
   *   non-number, RangeError for NaN, a fraction or a value out of range.
   * @returns {Mesh}
   */
  emitMesh(sides) {}

  /**
   * Bark mesh of one plant. Wind-bent. Carries `vegetation`.
   * @param {number} plantIdx
   * @param {number} [sides=6] - An integer in [3, 256], as for emitMesh.
   * @returns {Mesh|null}
   */
  emitPlantMesh(plantIdx, sides) {}

  /**
   * Every plant's branch segments, parents indexed across the whole list.
   * @returns {Array<FloraSegment>}
   */
  emitSegments() {}

  /**
   * One plant's branch segments.
   * @param {number} plantIdx
   * @returns {Array<FloraSegment>}
   */
  emitPlantSegments(plantIdx) {}

  /**
   * One instance matrix per world segment (see "Instance matrices"), mapping
   * a unit cylinder along local +Z (0..1) with radius 1 onto the segment:
   * X/Y axes scaled by the segment radius (min 0.001), Z by its length,
   * origin at `from`. Wind-bent.
   * @returns {Float32Array} 16 floats per segment; empty with no segments.
   */
  emitSegmentTransforms() {}

  /**
   * World segments packed for instanced tube rendering: `{segments,
   * segCount, boundsMin, boundsMax}`. `segments` is a Float32Array of 8
   * floats per segment: from xyz, from radius (the parent segment's radius,
   * so tubes taper), to xyz, to radius. Zero-length segments and ones with
   * radius below `opts.minRadius` are dropped. Bounds are [x, y, z] arrays
   * padded by the largest radius ([0,0,0] when empty). `vegetation` holds
   * 16 floats per kept segment (see "Vegetation records"); pass the whole
   * object to `setTubeSegments` / createInstancedMesh's `tube` and it sways.
   * A segment continuing its parent (the parent's thickest child) starts at
   * the parent's radius, so bark runs on unbroken; a side shoot starts at
   * most 1.5x its own radius, a collar inside the parent's wood. `tangents`
   * holds 6 floats per kept segment, the joint directions at its base and
   * tip (the bisector with the parent / thickest child it continues), which
   * the tube node mitres its joints on.
   * @param {{minRadius?: number}} [opts] - minRadius defaults to 0.
   * @returns {{segments: Float32Array, segCount: number, boundsMin: Array<number>, boundsMax: Array<number>,
   *   vegetation: Float32Array, tangents: Float32Array}}
   */
  emitBranchTubes(opts) {}

  /**
   * Watertight organic mesh of one plant: every segment becomes a capsule
   * (radius = its tip radius), smooth-unioned in a bromesh SDF graph and
   * meshed. Not wind-bent. An empty plant gives an empty Mesh.
   * @param {number} plantIdx
   * @param {FloraSdfMeshOptions} [opts]
   * @returns {Mesh|null}
   */
  emitPlantSdfMesh(plantIdx, opts) {}

  /**
   * The SDF mesh of every plant in the world, as one Mesh.
   * @param {FloraSdfMeshOptions} [opts]
   * @returns {Mesh}
   */
  emitWorldSdfMesh(opts) {}

  // ── Foliage ──

  /**
   * Foliage state per world segment, index-aligned with emitSegments().
   * @returns {Array<FloraFoliageSample>}
   */
  emitFoliage() {}

  /**
   * Foliage state per segment of one plant, aligned with emitPlantSegments().
   * @param {number} plantIdx
   * @returns {Array<FloraFoliageSample>}
   */
  emitPlantFoliage(plantIdx) {}

  /**
   * Leaf instance matrices for the whole world (see "Instance matrices"),
   * placed by `opts`. Each maps a leaf in `leafCard` space (+Z tip, +Y card
   * normal, +X side) to world space. Wind-bent.
   * @param {FloraLeafPlacementOptions} [opts]
   * @returns {Float32Array} 16 floats per leaf; empty when nothing is placed.
   */
  emitFoliageTransforms(opts) {}

  /**
   * One merged Mesh of `leafMesh` stamped at every leaf placement over the
   * world's segments. `leafMesh` is a Mesh or any `{positions, normals?,
   * uvs?, colors?, indices}` typed-array object. Wind-bent.
   * @param {Mesh|Object} leafMesh
   * @param {FloraLeafPlacementOptions} [opts]
   * @returns {Mesh|null} null when `leafMesh` is missing or empty; an empty
   *   Mesh when the world has no segments.
   */
  emitFoliageMesh(leafMesh, opts) {}

  /**
   * emitFoliageMesh for one plant.
   * @param {number} plantIdx
   * @param {Mesh|Object} leafMesh
   * @param {FloraLeafPlacementOptions} [opts]
   * @returns {Mesh|null}
   */
  emitPlantFoliageMesh(plantIdx, leafMesh, opts) {}

  /**
   * World segments packed for GPU foliage scatter: `{segments, segCount,
   * instSeg, instanceCount, boundsMin, boundsMax}`. `segments` holds 8 floats
   * per kept segment: from xyz, radius, then the *direction* `to - from`
   * xyz, then 0. `instSeg` is a Float32Array with one entry per leaf
   * instance, the index (as a float) of its segment in `segments`. The leaf
   * count per segment is `floor(length * perUnitLength * weight + hash)`,
   * capped at 4096, with the filters of `opts` (maxRadius, minDepth,
   * terminalOnly, densityWeight) applied; the jitter / scale fields are left
   * to the shader. Bounds are unpadded ([0,0,0] when empty). `vegetation`
   * holds 16 floats per kept segment (see "Vegetation records").
   *
   * That is the spray (`clumps: false`). By default (crown clumps) each entry
   * of `segments` is a clump instead — center xyz, radius, dir xyz, 0 — with
   * `instSeg` the leaf's clump, `vegetation` the clump's twig record, and
   * `clumps: true` plus the placement fields (seed, upBias, tiltJitter,
   * shell, baseScale, scaleJitter) on the object, so passing it whole to
   * createInstancedMesh's `scatter` draws exactly emitFoliageTransforms'
   * leaves.
   * @param {FloraLeafPlacementOptions} [opts]
   * @returns {{segments: Float32Array, segCount: number, instSeg: Float32Array, instanceCount: number, boundsMin: Array<number>, boundsMax: Array<number>, vegetation: Float32Array, clumps?: boolean}}
   */
  emitScatterSegments(opts) {}

  /**
   * The world's crown clumps (see "Crown clumps"), in plant order; what
   * every clump-mode foliage emit fills.
   * @param {FloraLeafPlacementOptions} [opts] - The clump fields, maxRadius, minDepth, densityWeight, seed.
   * @returns {Array<FloraClump>}
   */
  emitClumps(opts) {}

  /**
   * One 16-float vegetation record per leaf, in lockstep with
   * `emitFoliageTransforms(opts)` (same options, same leaves): pass both to
   * createInstancedMesh (`instances`, `vegetation`) and the leaves sway.
   * @param {FloraLeafPlacementOptions} [opts]
   * @returns {Float32Array} 16 floats per leaf.
   */
  emitFoliageVegetation(opts) {}

  /**
   * One 16-float vegetation record per world segment, in lockstep with
   * `emitSegmentTransforms()`: wood, in the segment form (scene-nodes-api.js
   * "Vegetation"): the crown floats carry the segment's tip and its weight,
   * so each vertex of the instanced cylinder bends by the weight at its own
   * point and the tip moves exactly as its child's base.
   * @returns {Float32Array} 16 floats per segment.
   */
  emitSegmentVegetation() {}

  // ── Forests ──

  /**
   * Every plant as a SPECIMEN for bro.scene's forest node
   * (`scene.createForest`, docs/scene-nodes-api.js "Forest"): its bark
   * segments, crown clumps and leaves relative to where it stands, so a few
   * grown plants can be stood thousands of times. Plants in order; each
   * specimen's runs follow on from the last. Arrays:
   *   - `specimens`: 16 floats each: segStart, segCount, clumpStart,
   *     clumpCount, leafStart, leafCount, height, crownRadius, crown xyz
   *     (relative), plant index, origin xyz (where it grew), its clumps'
   *     height over their width (the species' clumpFlat);
   *   - `segments` (8 each: from xyz, rFrom, to xyz, rTo), `tangents` (6:
   *     the bark's direction at from and at to), `segmentVegetation` (16, the
   *     segment records of "Vegetation records") and `keepRadius` (1: the
   *     thickest tip at or beyond the segment), in an order where the keep
   *     radius never grows down a specimen's run, so "every segment at least
   *     r thick" is a prefix;
   *   - `clumps` (8 each: centre xyz, radius, twig dir xyz, leaf count) and
   *     `clumpVegetation` (16 each, the clump's twig record);
   *   - `leaves`: a Uint32Array of (clump, placement key) pairs, shuffled
   *     within each specimen (`shuffleSeed`) so any prefix is an even
   *     thinning of the crown; the scene places each leaf from its key
   *     exactly as the clump-mode scatter does;
   *   - `instances`: 8 floats a plant, x, y, z, 1, 0, specimen, 0, 0: every
   *     plant where it grew (createForest's default trees).
   *   - `specimenLooks`: 8 floats a specimen, its species' leafColor rgb,
   *     leafForm (0 broad, 1 needle, 2 scale), barkColor rgb and leafSize
   *     (createForest takes them as the node's per-specimen look).
   *   - `specimenLeaves`: 8 floats a specimen, its species' leafWidth,
   *     leafTip, leafTeeth, leafToothDepth, leafGloss, sprayBrush, petiole,
   *     phyllotaxis (0 alternate, 1 opposite, 2 spiral) (createForest's
   *     `setLook` hands them on after `specimenLooks`: each specimen's own
   *     leaf outline, gloss, spray brush, stalk and arrangement).
   *   - `blooms`: 8 floats a bloom, position xyz (relative: its twig's tip),
   *     size (m: a flower's or a head's across, a spike's length; the
   *     species' bloomSize, smaller on a young, weak or shaded shoot and as
   *     the plant senesces), its twig's outward direction xyz, and its
   *     segment (an index into its specimen's run of `segments`: the twig
   *     it sways with). Only a flowering plant whose species has a
   *     bloomForm bears them, one per terminal shoot tip, thinned so none
   *     stands nearer another than about its size (a spike a third of its
   *     length), in a shuffled order (`shuffleSeed`);
   *   - `specimenBlooms`: 8 floats a specimen, bloomStart, bloomCount (into
   *     `blooms`), bloomForm (0 none, 1 flower, 2 head, 3 spike), bloomSize,
   *     bloomColor rgb, 0 (createForest draws them: docs/scene-nodes-api.js).
   * Plus `specimenCount`, `leafCount`, `bloomCount` and the leaf placement fields (seed,
   * upBias, tiltJitter, shell, baseScale, scaleJitter), and `leafForm`
   * (the form's name) when the emit was filtered to one.
   *
   * A forest node draws one leaf form, so a mixed world is emitted a form at
   * a time: `leafForm` keeps only the plants of that form, `plants` (plant
   * indices) only those. A needle or scale emit fills in the options it is
   * not given with the conifer's (needle / scale: perArea 50 / 140, upBias
   * 0.65 / 0.6, tiltJitter 0.3, clumpSpacing 1 / 0.8: flatter spray cards
   * in small tufts that touch, the dark, dense, layered crown).
   * @example
   *   const forest = world.emitForest({ perArea: 90 });
   *   scene.createForest({ forest, instances: bro.flora.plantForest(forest, { area }), leaf });
   * @example
   *   // A mixed world, a node per form; with no leaf each draws its form's card.
   *   for (const leafForm of ['broad', 'needle', 'scale'])
   *     scene.createForest({ forest: world.emitForest({ leafForm }) });
   * @param {FloraLeafPlacementOptions & {minRadius?: number, shuffleSeed?: number,
   *   leafForm?: string|number, plants?: Array<number>}} [opts] - the clump and leaf
   *   options of the foliage emits; `minRadius` leaves out thinner bark.
   * @returns {Object}
   */
  emitForest(opts) {}

  // ── Species presets ──

  /**
   * Register a species preset (see bro.flora.speciesPresets) in this world:
   * its prototypes (the module shapes it grows from) and its Species.
   * @param {string} name
   * @returns {{prototypeStart: number, prototypeCount: number, rootPrototype: number,
   *   species: FloraSpecies}|null} null for an unknown name.
   */
  addSpecies(name) {}

  /**
   * Plant a species preset: registers it on first use (addSpecies) and
   * plants one at `spec.origin`. `spec.species` is a partial Species over the
   * preset's (a bluer spruce, a taller cypress).
   * @example
   *   world.plantSpecies('fir', { origin: [-6, 0, 0] });
   *   world.plantSpecies('boxwood', { origin: [4, 0, 2], species: { leafSize: 0.25 } });
   * @param {string} name
   * @param {FloraPlantSpec} [spec]
   * @returns {number} the plant index, -1 for an unknown name.
   */
  plantSpecies(name, spec) {}

  // ── Blooms ──

  /**
   * Bloom anchors of every flowering plant.
   * @returns {Array<FloraBloomAnchor>}
   */
  emitBloomAnchors() {}

  /**
   * Bloom anchors of one plant (empty until it flowers).
   * @param {number} plantIdx
   * @returns {Array<FloraBloomAnchor>}
   */
  emitPlantBloomAnchors(plantIdx) {}

  /**
   * Blooms as geometry: `[petals, centers]`, one merged Mesh each. The
   * anchors are first strided down to at most `opts.bloomCap`, then anchors
   * with `lightExposure01` below `opts.bloomLightMin` are skipped, so the
   * result can hold fewer than bloomCap. At each survivor `petalMesh` is
   * stamped with its +Y turned onto the anchor normal and scaled by
   * `0.8 + 0.5 * min(1, age01)`; `centerMesh`, when given, is stamped the
   * same way lifted `0.012 * scale` along the normal. Without `centerMesh`
   * the centers Mesh is empty. Not wind-bent.
   * @param {Mesh|Object} petalMesh - Required (throws when undefined).
   * @param {Mesh|Object|null} [centerMesh]
   * @param {{bloomCap?: number, bloomLightMin?: number}} [opts] - bloomCap
   *   defaults to 500 (0 means 1), bloomLightMin to 0.18.
   * @returns {Array<Mesh>|null} null when `petalMesh` is empty.
   */
  emitBloomMesh(petalMesh, centerMesh, opts) {}
}


// ═════════════════════════════════════════════════════════════════════════════
// bro.flora
// ═════════════════════════════════════════════════════════════════════════════

/**
 * Create a simulation world.
 * @example
 *  const world = bro.flora.createWorld({
 *    rngSeed: 7,
 *    climate: { annualTempBase: 12 },
 *    shadow: { origin: [-10, 0, -10], cellSize: 0.5, width: 40, height: 40, depth: 40 },
 *  });
 * @param {FloraWorldOptions} [opts]
 * @returns {FloraWorld}
 */
bro.flora.createWorld = function(opts) {};

/** @type {typeof FloraWorld} Not constructible; for `instanceof`. */
bro.flora.FloraWorld = FloraWorld;

/**
 * Trees for a forest: `forest`'s specimens (world.emitForest) stood over an
 * area, one to a cell of a jittered grid, the rows createForest's
 * `instances` takes (x, y, z, scale, yaw, specimen, 0, 0). Deterministic
 * for a seed.
 * @example
 *   const trees = bro.flora.plantForest(forest, {
 *     area: [-200, -200, 200, 200], spacing: 6.5, ground: heightAt,
 *     density: (x, z) => clearing(x, z) ? 0 : 1, scale: [1.3, 1.8],
 *   });
 * @param {Object} forest - world.emitForest()'s result.
 * @param {Object} [opts]
 * @param {Array<number>} [opts.area=[-50, -50, 50, 50]] - [x0, z0, x1, z1], metres.
 * @param {number} [opts.spacing=7] - mean distance between neighbours.
 * @param {number} [opts.jitter=0.42] - how far a tree strays from its cell's centre, in spacings.
 * @param {Function} [opts.ground] - (x, z) => y the trees stand on (default 0).
 * @param {number|Function} [opts.density=1] - the chance a cell holds a tree, or (x, z) => 0..1.
 * @param {number|Array<number>} [opts.scale=1] - the scale, or [min, max].
 * @param {number} [opts.sink=0.1] - how far a root sits below the ground.
 * @param {Array<number>} [opts.specimens] - which specimens (indices), or a weight per specimen.
 * @param {number} [opts.seed=1]
 * @param {Function} [opts.mix] - a mixed forest: with `forest` an array of
 *   emits (a node's each), (x, z, y) => a weight per emit, or the index of
 *   one, at that point (conifers by elevation, shrubs at a glade's rim).
 *   `specimens` and `scale` may then hold an entry per emit.
 * @returns {Float32Array|Array<Float32Array>} 8 floats a tree; mixed, an
 *   array a forest.
 * @example
 *   const [broad, conifer] = bro.flora.plantForest([broadEmit, needleEmit], {
 *     area, ground: heightAt, mix: (x, z, y) => [1 - high(y), high(y)],
 *     scale: [[1.3, 1.8], [0.7, 0.95]],
 *   });
 */
bro.flora.plantForest = function(forest, opts) {};

/**
 * The species presets plantSpecies / addSpecies know, each a habit with
 * its leaf form, colours and bark:
 *   - broadleaf: oak (spreading, round), elm (vase), beech (tall, smooth grey
 *     bark), birch (slender and narrow, a light open crown of small bright
 *     leaves, white bark);
 *   - needle conifers: pine (an open crown widest near its flat top, on a
 *     bare red-brown trunk), spruce (a narrow spire, drooping boughs and
 *     hanging sprays), fir (a regular cone of level boughs), cedar (flat
 *     shelves), redwood (a tall narrow column of near-level short boughs on
 *     a long clear massive trunk, fibrous red bark);
 *   - scale-leaf conifers: cypress (a dark column, boughs turned up against
 *     the leader), juniper (a bushy blue-grey cone filled to the core);
 *   - shrubs, multi-stemmed, leaved inside and to the ground (clumpInner):
 *     boxwood (a dense mound of tiny dark leaves), holly (upright, glossy),
 *     hydrangea (a broad mound of arching stems, big pale leaves, round
 *     flower heads), hibiscus (upright, open, large flowers), lavender (a
 *     low grey-green cushion under purple spikes).
 * @returns {Array<string>}
 */
bro.flora.speciesPresets = function() {};

/**
 * Herbs (broflora/herbs.h): ferns, wildflowers, forbs and ground cover,
 * nothing grown — a species table bro.scene's grass node builds every plant
 * from where it stands (scene.createGrass({herbs}), scene-nodes-api.js
 * GrassHerbOptions). The presets:
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

/**
 * Where ground cover grows under a forest, for scene.createGrass's
 * `density`: a function (x, z) => 0..1, clear at each trunk, thinner the
 * more crowns overlap overhead, nothing where they close, full in the open.
 * The function also carries `canopy`, (x, z) => 0..1: how closed the crowns
 * are overhead (0 in the open, 1 under a crown's middle or where crowns
 * overlap), for createGrass's `floor` (the litter and low leafy plants
 * under trees).
 * @example const cover = bro.flora.forestCover(forest, { instances: trees });
 * scene.createGrass({ ground, density: cover, floor: cover.canopy, turf: true });
 * @param {Object} forest - world.emitForest()'s result.
 * @param {Object} [opts]
 * @param {Float32Array} [opts.instances] - the trees (default `forest.instances`).
 * @param {number} [opts.floor=0.35] - the cover left in a crown's shade before the canopy closes.
 * @param {number} [opts.closed=1.6] - the crown overlap at which nothing grows.
 * @param {number} [opts.trunkClear=0.7] - the bare radius round a trunk (m).
 * @returns {function(number, number): number}
 */
bro.flora.forestCover = function(forest, opts) {};

/**
 * Leaf arrangement enum. Also exposed as `bro.flora.phyllotaxy`, and each
 * value has a lowercase alias (`alternate` ... `compoundPinnate`).
 * @enum {number}
 */
bro.flora.Phyllotaxy = {
  /** Two-ranked, alternating sides (oak, elm, birch). */
  Alternate: 0,
  /** Decussate pairs with a 90 degree twist per node (maple, ash). */
  Opposite: 1,
  /** Golden-angle rosette (magnolia, apple). */
  Spiral: 2,
  /** Needle bundle from a basal sheath (pine). */
  Fascicle: 3,
  /** Leaflet pairs along a rachis plus a terminal leaflet (walnut, rowan). */
  CompoundPinnate: 4,
};

/**
 * Module prototype factories. Each returns a FloraPrototypeSpec to pass to
 * `world.addPrototype` (or to edit first).
 */
bro.flora.prototypes = {
  /** One straight segment, one terminal: the unbranched "I" pole. @returns {FloraPrototypeSpec} */
  straight() {},
  /** A symmetric planar two-terminal "Y" fork. @returns {FloraPrototypeSpec} */
  fork() {},
  /** A short trunk topped by `arms` (clamped 2-8) terminals spread around +Y; `spread` (0..1) leans them out. The workhorse for rounded crowns. @param {number} [arms=3] @param {number} [spread=0.55] @returns {FloraPrototypeSpec} */
  whorl(arms, spread) {},
  /** A dominant upward leader (terminal 0) with `lateralBranches` (1-4) side arms: excurrent / conifer growth. @param {number} [lateralBranches=2] @param {number} [lateralSpread=0.7] @returns {FloraPrototypeSpec} */
  monopodial(lateralBranches, lateralSpread) {},
  /** An asymmetric fork, dominant primary arm (terminal 0) plus a secondary: decurrent spreading crowns (oak, maple). @param {number} [primarySpread=0.3] @param {number} [lateralSpread=0.7] @returns {FloraPrototypeSpec} */
  sympodial(primarySpread, lateralSpread) {},
  /** A shelf of `arms` (clamped 2-8) near-horizontal arms (pine, cedar, dogwood). @param {number} [arms=3] @param {number} [spread=0.85] @returns {FloraPrototypeSpec} */
  horizontalTier(arms, spread) {},
  /** Alias of horizontalTier. @param {number} [arms=3] @param {number} [spread=0.85] @returns {FloraPrototypeSpec} */
  tier(arms, spread) {},
  /** Pendulous shoots that arch out and droop (weeping willow, birch). @param {number} [spread=0.6] @param {number} [droop=0.4] @returns {FloraPrototypeSpec} */
  weeping(spread, droop) {},
};

/**
 * Build a leaf cluster / twig spray Mesh in local space: twig root at the
 * origin running along +Z to `twigLength`, +Y the light-facing side, +X the
 * spread axis. Vertex color R holds the wind-bend weight (0 at the twig
 * base, 1 at leaf tips). Also callable as `leafCluster(opts)` with
 * `opts.phyllotaxy`.
 * @example
 *  const spray = bro.flora.leafCluster('spiral', { count: 8, leafShape: 'pointed' });
 *  const leaves = world.emitFoliageMesh(spray, { perUnitLength: 6 });
 * @param {number|string} [phyllotaxy='alternate'] - A Phyllotaxy value or its
 *   name (either case; `'pinnate'` / `'compound_pinnate'` also work). Unknown
 *   values mean Alternate.
 * @param {Object} [opts]
 * @param {number} [opts.count=6] - Leaves / leaflets, an integer in [0, 4096]
 *   (RangeError otherwise).
 * @param {number} [opts.twigLength=0.25] @param {number} [opts.twigRadius=0.005]
 * @param {number} [opts.petioleLength=0.04]
 * @param {number} [opts.leafWidth=0.12] @param {number} [opts.leafLength=0.2]
 * @param {string|number} [opts.leafShape='oval'] - 'oval' | 'pointed' | 'lobed' |
 *   'needle' | 'frond' | 'petal' or 0-5; `shape` is an alias.
 * @param {number} [opts.leafBend=0.3] - Lengthwise bend, radians.
 * @param {number} [opts.leafCurl=0.1] - Axial twist, radians.
 * @param {number} [opts.leafCup=0.2] @param {number} [opts.droop=0.2]
 * @param {number} [opts.upBias=0.6] @param {number} [opts.spread=0.7] - Fan angle, radians.
 * @param {boolean} [opts.includeTwigMesh=true] - Emit the twig cylinder.
 * @param {boolean} [opts.shapedSilhouette=true] - Shape card width by leafShape.
 * @param {boolean} [opts.fullUV=false] - UVs span 0..1 instead of a 4x4 atlas cell.
 * @returns {Mesh}
 */
bro.flora.leafCluster = function(phyllotaxy, opts) {};

// ── Global wind, density and placement batches ───────────────────────────────
//
// Process-wide state shared by every world (see "Wind and density" at the
// top) plus a small JS-side registry of instanced placement batches that
// update() sways.

/**
 * Set the global wind. Throws without `strength`. The direction is a 2D
 * vector on the ground plane, `dirX` along world X and `dirY` along world Z;
 * a zero vector means +X.
 * @param {number} strength
 * @param {number} [dirX=0]
 * @param {number} [dirY=0]
 */
bro.flora.setWind = function(strength, dirX, dirY) {};

/**
 * With no arguments, returns `{strength, dirX, dirY}`; with arguments, the
 * same as setWind (returns undefined).
 * @param {number} [strength]
 * @param {number} [dirX=0]
 * @param {number} [dirY=0]
 * @returns {{strength: number, dirX: number, dirY: number}|undefined}
 */
bro.flora.wind = function(strength, dirX, dirY) {};

/**
 * Set the global foliage density multiplier (default 1). Throws without an
 * argument.
 * @param {number} density
 */
bro.flora.setDensity = function(density) {};

/**
 * With no argument, returns the density; with one, sets it.
 * @param {number} [density]
 * @returns {number|undefined}
 */
bro.flora.density = function(density) {};

/**
 * Advance the wind clock by `dt` seconds (throws without it) and re-sway
 * every placement batch's `transforms` from its `baseTransforms`.
 * @param {number} dt
 */
bro.flora.update = function(dt) {};

/** Reset wind, wind clock and density to defaults and drop every placement batch. */
bro.flora.clear = function() {};

/**
 * An instanced placement batch, as addPlacement returns it. `transforms`
 * (swayed by update) and `baseTransforms` (rest pose) are Float32Arrays of
 * 16 floats per instance with translation at [3], [7], [11].
 * @typedef {Object} FloraBatch
 * @property {string|*} id @property {*} mesh @property {*} material
 * @property {Float32Array|null} transforms @property {Float32Array|null} baseTransforms
 * @property {number} count @property {number} instanceCount
 * @property {number} windFactor @property {boolean} castShadow @property {boolean} receiveShadow
 * @property {*} aabb
 */

/**
 * Register a placement batch (or an array of them, returning an array).
 * Instances come from `config.transforms` (alias `instances`): a flat
 * Float32Array / number array of 16-float matrices, an array of 16-element
 * arrays, or an array of [x, y, z] positions (identity rotation). Failing
 * those, `config.count` identity instances: an integer in [0, 16777216],
 * TypeError for a non-number and RangeError for anything else. A
 * `transforms` list of more than 16777216 instances is a RangeError too.
 * `update(dt)` sways each instance along its height (see "Wind and
 * density" at the top), so instances standing at y = 0 bend without
 * leaving their root; floats 12-15 (the tint) are copied through as given.
 * @param {Object} config
 * @param {*} [config.id] - Default `'flora_batch_<n>'`.
 * @param {*} [config.mesh] @param {*} [config.material] - Stored as given.
 * @param {Float32Array|Array} [config.transforms]
 * @param {number} [config.count]
 * @param {number} [config.windFactor=1] - Scales the global wind for this batch.
 * @param {boolean} [config.castShadow=true] @param {boolean} [config.receiveShadow=true]
 * @param {*} [config.aabb]
 * @returns {FloraBatch|Array<FloraBatch>|null} null for a missing config.
 */
bro.flora.addPlacement = function(config) {};

/**
 * No argument: the registered placement configs. An array: replace every
 * batch with these and return the batches. An object: addPlacement.
 * @param {Object|Array<Object>} [config]
 * @returns {Array<Object>|Array<FloraBatch>|FloraBatch|null}
 */
bro.flora.placement = function(config) {};

/** @returns {Array<FloraBatch>} The live batch list. */
bro.flora.batches = function() {};

/** Alias of batches(). @returns {Array<FloraBatch>} */
bro.flora.getBatches = function() {};
