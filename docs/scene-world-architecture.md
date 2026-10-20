# Scene world architecture

The engine mechanisms for a world the size of a planet, seen from a person
standing in a forest up to orbit as one place, at high frame rates: cells,
the far field, visibility, async compute, surface layers, the world service,
lighting, shadows, the ground and its cover, placement, and the planet
itself. Each one is general: terrain, plants, grass and static meshes all
build on them, so none is shaped around a single consumer. Each has one path:
a mechanism that replaces another removes it, with no fallback kept beside
it.

The target world is a planet at Earth's size (6371 km): plates, continents
and oceans, mountain belts, climate belts and snow lines, biomes that are
whole different worlds of geometry rather than colours, clouds and weather.
Every view, eye level, aerial, orbital, space, reads the same state, so the
same place looks like the same place from any distance. They share one rule: **no stage waits for another
stage to give a resource back.** Anything written in one frame and read in a
later one is versioned, either by frame-in-flight slot or by a generation
number. Every cross-queue dependency is a timeline value one queue signals and
the other waits on at a submission boundary. The CPU never waits for the GPU
inside a frame.

Target: 2.78 ms a frame (360 fps) at 2560×1364 on an RTX 4090, for:
- a near sphere of about 10 m, drawn in full detail;
- views kilometres deep;
- worlds from a 64 km square up to a planet seen from orbit. Floats in
  camera-relative drawing hold millimetres out to 32 km; past that, content
  is drawn from cell-local coordinates (§1).

**One measure decides every level of detail: pixels.** A representation's
error in metres, projected from the current view, is its error in pixels.
Every system refines its content while that error is over its threshold, and
stops where it is not, at whatever altitude. Content smaller than a few
pixels stops being separate: it is folded into the surface of the cell it
stands on (§5). The cost of a frame then follows the pixels on screen, not
the size of the world.

Where the frame goes today (../broworkshop/demos/biomes, 32.5k plants with
grass, turf and herbs, GPU ms; capture.js `cost`):

| | glade | meadow | flyover |
|---|---|---|---|
| grass and herb rings | 1.34 | 1.71 | 0 |
| ground (mesh and turf) | 0.18 | 0.26 | 0.25 |
| plants (leaves and their shadows) | 0.60 | 0.53 | 0.55 |
| frame | 2.29 | 2.60 | 0.90 |

- **The ground's cover** is two thirds of an eye-level frame: the ground is
  shaded whole and the blades drawn over it, and the far rings draw blades
  the turf could stand in for. Fixed by the ground and its cover as one
  surface (§9).
- **Shadows redrawn every frame** for content that does not move: the plants'
  casters are a third of a millisecond on every shot. Fixed by cached shadow
  pages (§8).
- **At a million plants** (8.4 km) the plants alone take 3 ms, most of it
  plants too far to be more than a few pixels. Fixed by the far field (§2).

## 1. World cells

A quadtree over the ground that every system's content hangs off.

- **Cell keys** are integer coordinates (root, level, x, z), never floats,
  packed in 64 bits. A flat world has one root. A planet has six, the faces
  of a cube-sphere, each a quadtree of its own (about 18 levels down to
  64 m leaves at Earth's size).
- **Leaves** are a fixed size, 64 m by default.
- **A cell's origin** is part of every cell: the CPU computes each cell's
  eye-relative origin in double every frame, and a provider's content may be
  stored relative to it.
- **Cell bounds** take their y range from whatever registers content in the
  cell. Terrain gives a min/max height for each cell; the plants give their
  crowns' tops.

A system registers a **cell content provider**:
- its content's bounds for each cell;
- the geometric error, in metres, of each representation it offers, and the
  pixel threshold at which it hands from one to the next;
- a draw entry that renders that content for any view, the main camera or a
  far-field capture (§2);
- a generation number, bumped whenever the content changes.

### The per-frame cell pass

A small GPU pass walks the tree top-down. It tests each cell against:
- the frustum;
- the hierarchical depth from the visibility pass (§3);
- each shadow tile's frustum;
- on a planet, the horizon: a cell below it is hidden behind the curve of
  the ground, which from orbit is most of the planet.

Each surviving cell carries its projected size in pixels (its bounds'
radius over its distance, times the view's focal length in pixels). A cell
is refined into its children while its providers' errors, projected the
same way, are over their thresholds. The pass has no fixed bands: near, mid
and far are pixel thresholds a provider chooses, so each provider hands
over where its own representations meet, and the same thresholds hold at
any resolution and altitude.

The pass writes compact lists of visible cells for each provider's ranges
and each view. Systems consume those lists instead of their whole content:
- the plants plan walks only the plants of the cells in view;
- static chunks draw by cell;
- far cells go to the far field (§2).

### Per-system data

- **Plants:** setPlants keeps the records themselves in leaf-cell order and
  uploads them once. The plants plan (count, lay out, place) walks the
  cells in view and writes the draws' chunks and indirect commands; nothing
  on the CPU walks the plants.
- **Terrain:** the clipmap already costs the same whatever the world's size,
  so it keeps its rings. It joins the cells as the source of their heights and
  as a provider for far-field captures.
- **Grass:** already ring cells around the eye. Its tiles map onto leaf
  cells, so the cell pass's occlusion result can reject them.

### Precision and streaming

- **Precision past 32 km:** a cell's content is stored relative to the
  cell's origin, which the cell pass hands to the GPU as an eye-relative
  offset computed in double on the CPU. A provider whose content fits in
  32 km may keep it world-relative for now; the origin is there either way.
- **Streaming:** a cell has a residency state. A provider can generate or load
  its content when the cell first comes within range, under a per-frame
  budget, and drop it past a hysteresis radius. Nothing waits on a load: a
  cell that isn't resident yet draws from a coarser level, and in the far band
  from its far-field card.

## 2. Far-field layer (cell cards)

Past a reach set in pixels, a cell is drawn as a **card** captured at runtime
from the cell's real content. A card is the cell's colour, normal and depth,
rendered from the current view direction into an atlas tile.

- **Drawing:** a card draws as a few depth-correct quads. Each texel writes
  its own depth, so cards meet each other, the terrain and near content
  correctly.
- **Lighting:** cards are lit at draw time from their normals, so shadows and
  fog stay live.

### Capture

A capture calls each provider's cell draw entry with a camera fitted to the
cell. The plants draw as they would at the capture's pixel size, terrain
its patch, statics their chunks. Nothing about a card is baked, and nothing is authored per
asset.

- **Resolution** follows the cell's size on screen, so a far cell takes a
  small tile.
- **Budget:** a fixed number of captures a frame, highest error first.
- **Error** is the angle between the card's capture direction and the current
  view of the cell, plus the cell's screen growth.

### Invalidation and versioning

- A card goes stale when its provider's generation changes, the sun turns
  past a threshold, or its view error passes the threshold.
- A stale card keeps drawing until its replacement lands. The atlas tile is
  double-buffered by generation, so no capture waits for a card to stop being
  read.
- Far-cell sway is not captured. At card distances a sway is under a pixel.

### The transition

Between the near/mid band and the card band there is a distance band, about
10% of the reach, where a cell's real content dithers out as its card dithers
in. The reach is set in pixels, not metres, so the band is the same size on
screen at any resolution.

## 3. Visibility pass

Opaque content that is expensive to shade, or that discards (foliage first),
moves into two steps.

1. **Visibility.** Draw into depth plus a 32-bit visibility ID:
   - the content kind;
   - the instance;
   - a sub-element: the leaf element, the triangle.

   Content that finds its surface per pixel (a traced card) still does so
   here, but writes only depth and ID. No lighting, shadow lookups or colour.
2. **Resolve.** A full-screen pass, split into tiles by content kind,
   rebuilds each pixel's surface from its ID and shades it once:
   - traced content re-traces only the one hit;
   - cards just sample.

   Overdraw then costs only the trace, never the shading.

**Occlusion culling, two-phase:**
1. Draw last frame's visible cells and instances.
2. Build the hierarchical depth.
3. Test everything else against it.
4. Draw what is newly visible.

The hierarchical depth also feeds the cell pass (§1): a general mechanism
that rejects whole cells, plants, chunks and grass tiles before they
rasterise. Nothing builds a depth pyramid today; the plants and grass are
culled by frustum and distance alone. The retired forest node's sprite
cards had a first one, a measure of what is at stake: crowns behind
terrain and nearer trees, wilds vista −0.93 ms, treetop −0.32, walk −0.1.

**Adoption is per content kind.** Content that hasn't adopted the pass draws
forward as now, after the resolve, depth-tested against it. Nothing has to
move at once.

**First step:** measure how much of each foliage pass is trace and how much
is shading, with shading stubbed. That split is the size of the win.

## 4. Async compute

A capability of the frame graph, not of any one pass.

- **Queues:** VulkanContext gains a compute queue from a family without
  graphics, with its own VulkanQueue and timeline, beside the upload queue.
  If the device has none, the "async" work records into the graphics stream
  in the same order. One code path, two schedules.
- **Passes:** a pass can declare that it runs on the compute queue. The graph
  splits the frame into submissions at each edge between queues, and waits
  and signals on timeline values at those edges, never on a fence.
- **Overlap:** a frame's compute work, such as the plants plan and the cell
  pass, is submitted ahead of its graphics work. It runs beside the previous
  frame's tail (post-processing, translucents) and the current frame's work
  that doesn't need it (sky, probe, static and grass shadows). The graphics
  stream waits on the compute timeline only right before its first consumer.
- **Versioning:** anything compute writes for frame N lives in the slot for
  N's frame in flight. Frame N−1's draws still read their own slot, so compute
  never waits for graphics to finish with a buffer.
- **Ownership:** buffers shared between queues are created CONCURRENT.
  Images move with explicit release and acquire, as the upload queue already
  does.
- **Other users:** clipmap height-pyramid updates, cell residency generation,
  terrain streaming, and the cell pass itself.

## 5. Surface layers

What a cell's surface is made of, worked out once per cell and kept,
instead of in every pixel that shows it. Wherever content is drawn as a
stand-in for what is no longer drawn there (the turf for blades and herbs
past their rings, and, from far above, the plants and everything else on
the land), the stand-in depends on the position and the world, not on the
view. Today the ground's fragment shader recomputes it every frame, at every
pixel: its colony weights, mottling, sward colour and litter were
0.5–0.7 ms of opaque:static in wilds.

A **layer** is the surface's material, never its light: its colours,
covers, roughness, its blades' lay, its emissive. It is lit live, so the
sun, shadows and air stay current.

### Tiles

- A layer is stored in **tiles**, each a cell's square of texels (64×64 by
  default, with a border for filtering), in an atlas.
- Tiles share the cells' key space and can subdivide below the leaf size,
  so the texels near the eye are centimetres and far off are kilometres.
- **Which tiles are needed** is decided by the same projected-size rule as
  everything else (§1): a cell is tiled at the level where a texel covers
  about a pixel (`texelPixels`, 1.5 for the turf), with every ancestor of
  a needed tile needed too. The CPU picks them from the camera
  (`SurfaceLayers`, `scene/world/surface_layers.h`); nothing is read back from
  the GPU, unlike feedback-driven virtual texturing. A tile's heights, the
  slope its footprint sees and its version come from the provider's items
  (for the turf, each mesh's triangles in 32 m cells), gathered into a
  min/max pyramid so a tile reads a few cells. The atlas's slots bound the
  set, the tile most too coarse refined first: in wilds the footprint rule
  alone asks for 4–10× the 1024 slots on hilly, forested routes, since a
  frustum hides no ground behind hills or trees. Occlusion (§3's
  hierarchical depth) is what would close that gap.
- **Lookup:** for each level, a toroidal grid of entries (256 a side), a
  clipmap of page tables, maps a position to its tile; an entry carries
  its tile's slot and the high bits of its key, so a tile that wraps onto
  another's entry is told apart and the lookup falls through. A fragment
  takes its level from its footprint, blends between that level and the
  next coarser one, and falls back to a coarser level when a tile is not
  resident. The coarsest level is always resident.

### Filling

- A layer has a **provider**: it fills a tile from the world's data (the
  grass field, the floor texture, the ground's vertex data, the plants'
  cover) for a cell's rectangle and texel size. The turf's fill is a draw:
  the ground's meshes rasterised straight down under an orthographic camera
  fitted to the tile and its border, into the layer's four colour targets,
  so the vertex data a mesh carries reaches the fill as it reaches the
  ground's own pass. Its patterns are band-limited to the tile's texel, so
  a tile never aliases.
- The **budget** is a fixed number of tile fills a frame, the coarsest
  first (the coarsest level whatever the budget). Fills are recorded at the
  frame's head on the graphics queue, so the frame's own draws read them
  without a wait; they are small enough that async compute (§4) would buy
  nothing a raster fill does not already have.
- **Versioning:** a tile carries its provider's generation. A change to the
  world's data bumps it, and the stale tile keeps drawing until its
  replacement lands. A refill writes the slot in place: the queue orders it
  after the frames that read the old contents.

### The shader's part

What depends on the view stays in the fragment: how much of the blades'
tops is seen at a grazing angle, the distance the turf stands in for the
blades from, the gusts, and the detail finer than the finest tile (the
near ground's crumbs, blades and leaves). Everything else is a sample.

### What folds into layers

- **First, the turf:** sward colour, mottling, the herbs' blooms, the forest
  floor's litter. These are today's ground shader moved to fills.
- **Next, content too small to draw.** Below its last representation
  (cards, at a few pixels), a system writes its share of the cell's
  surface: a forest from orbit is a darker, rougher, taller canopy layer; a
  city at night is emissive texels. A provider declares which layers it
  writes and from what pixel size.
- **Light from far off:** lights too small to draw sum into the emissive
  layer, and shadowing past the sun's last cascade into the layer's horizon
  and occlusion terms.

## 6. The world service

Everything about the world that does not depend on the view is made once,
kept, and refreshed only when it changes: the terrain's heights and normals,
its material (§5's layers), the sun's shadow on what does not move (§8),
the horizon and the canopy's density (§7), the far field's cards (§2), the
plants placed in a cell (§10), the light the probes gather (§7), the weather.
The frame draws from what is kept; it never makes any of it. One service
makes all of it, so no producer competes with another, or with the frame,
by its own rules.

- **Producers** register kinds of tile: what a tile is keyed by (a cell key,
  a probe's place, a shadow page), the job that makes one, its size, and its
  error in pixels when it is missing, stale or too coarse.
- **One queue of jobs, ranked by that error.** What is most wrong on screen
  is made first, whoever its producer is. Ties go to the coarser tile, so the
  world fills in from the top down.
- **A budget in milliseconds from measured slack.** The GPU timer stamps every
  frame; the service gets what the frame left under its target (2.78 ms on
  the 4090, 16.7 ms on a 3060 at 60 fps), and a small floor so it never
  starves. A frame that runs over takes the service down to its floor. Each
  job's cost is measured too, so the budget is spent in jobs, not guessed.
- **Nothing waits.** The coarsest level of every producer is always resident.
  A missing tile shows its coarser parent; a stale one keeps drawing until its
  replacement lands; a replacement is written to a free slot and swapped in by
  its version, so no job waits for a frame to finish reading.
- **The queue:** the device's compute queue at low priority where it has one
  (§4: timeline values at the edges, never a fence), otherwise sliced into
  the graphics stream at the frame's head. One code path, two schedules.
  Async compute shares the GPU's cores, so the gain is less parallelism than
  amortisation: each tile is made once instead of every frame, in the gaps
  the frame leaves (its depth-only and shadow raster leave arithmetic idle).

The surface layers' fills (§5), the far field's captures (§2) and streaming
(§1) are the service's first producers; each keeps its own policy (which
tiles it wants) and gives up its own budget.

## 7. Lighting

Lighting is the lens every other system is judged through, so it is built
early and built for every altitude and time of day.

- **Atmosphere from tables** (Hillaire 2020): transmittance, multiple
  scattering and the sky's view in small LUTs, remade when the sun moves;
  the haze between the eye and what it sees in a camera-aligned froxel volume
  (about 32 × 32 × 32), not marched at every pixel. The same model from the
  ground to space, a different planet a parameter change. The sky's light on
  a surface comes from it with its direction (spherical harmonics an altitude
  and sun angle), not one colour a frame.
- **Exposure** in physical units with eye adaptation, one path from forest
  shade to snow to orbit.
- **Sky occlusion and bounced light from a probe grid** (DDGI, Majercik 2019):
  camera-centred cascades of probes, each gathering irradiance and its
  visibility by rays against a simplified world (the terrain's height tiles,
  the canopy's density, the cards), a slice of them a frame as world-service
  jobs. It is what makes a valley floor see less sky, a forest's inside dim
  and green, sunlit rock warm the shade beside it. Hardware ray tracing where
  the device has it and it is cheaper; the 4090 and a 3060 both do.
- **Horizon and canopy:** a terrain tile's horizon (the elevation it sees in
  a ring of directions) and a cell's canopy density, made once each by the
  world service: the sun's shadow and the sky's occlusion past the last
  cascade, and the light under a forest without drawing it.
- **Volumetric light** in the same froxel volume as the haze, lit by the
  shadow pages: shafts through a canopy, mist in a valley, the clouds'
  shadows on the land.
- **Night:** the moon's light, the stars, the emissive layer (§5) for what
  shines too small to draw (a city from orbit), and clustered lights past the
  forward path's 32.

The plants' crown-depth falloff and the turf's tuned darkening stand in for
the probes today; they go when the probes land.

## 8. Shadows

- **Cached shadow pages** (as Unreal's virtual shadow maps): the sun's
  shadow is a clipmap of pages at about a texel a pixel. A page holds what
  does not move and is redrawn only when that changes (a cell's generation,
  the sun past a threshold); what moves is drawn over it each frame. Plants'
  sway is under a texel past the near pages, so only the near pages redraw
  their plants.
- **Past the pages:** the horizon tiles (§7).
- **Every caster** draws into pages through its provider's draw entry, as
  for the camera; the shadow tiles of today go.

## 9. The ground and its cover

The ground and what covers it (turf, grass, herbs, litter, snow) are one
surface. The blades are only the near detail of the cover.

- **Order:** the terrain's depth first, then the blades, then the ground's
  shading only where the blades leave it showing; there it is the dark between
  their roots, not a material of its own.
- **Heights** for the blades come from the terrain's tiles, not a mesh.
- **Handover:** the turf stands in for the blades where they are too fine to
  matter, so its look (the sward's wind sheen, its ragged edge against the
  sky) is what lets the far rings go; how near they can go is judged in
  motion, at the meadow's edge.
- **The terrain's own material** (rock, soil, scree, snow) is made only
  where the cover thins, once a tile, by the world service.

## 10. Placement

A planet cannot store its plants (an Earth's is of the order of 10^12). A
cell's plants, herbs and rocks are made from its fields (biome, climate,
cover, slope, the terrain's age) when it comes within reach, deterministically
from the cell's key, as world-service jobs (as Decima places its vegetation),
and dropped past a hysteresis radius. The plants node takes the cells and the
rules (its species and what grows where), not a list of plants. The rules
are data, so a biome is a table, not code.

## 11. The planet

- **Roots:** six cube-sphere faces of world cells (§1), each a quadtree to
  64 m leaves, every origin eye-relative in double.
- **Its fields:** the terrain's shape (plates, the isostatic datum, mountain
  belts, warp, erosion), its climate (belts, snow lines, rain) and its biomes,
  each a field with a CPU reference (for physics and gameplay queries) and a
  GPU kernel the world service runs to make tiles. Every additive term
  switchable from the host, so a wrong look is ablated before it is guessed.
- **Culling** adds the horizon: a cell below it is behind the planet's curve.
- **Going up** brings things in as well as folding them away: the
  atmosphere seen as a shell, space, other bodies. They are providers like
  any other, refined by the pixels they cover: a planet a few pixels across
  is a point of light, then a lit disc, then a body with its own face roots,
  as a tree goes from a layer's texels to a card to a mesh. So there is a
  level above the face roots (a system, its bodies, each body's six faces),
  with every body's origin, like every cell's, taken eye-relative in double.

## 12. Where the code lives

All of it in bro. `src/scene` is split into sub-modules, each with its CPU
half (testable without a GPU) apart from its GPU half. They are folders of
one module, not modules of their own: one library, the namespaces
`bro::scene` and `bro::scene::vk` throughout.

```
src/scene/
  (top level)       the scene graph and node base, SceneRenderer (the
                    backend-neutral renderer: settings, lights, culling, the
                    shadow planner), animation, tweens, clips, the agent glue
  nodes/            the node types: their CPU halves
  world/            cells, surface layers' paging, the shade map, the shadow
                    plan, the wind field, the atmosphere's parameters and
                    irradiance; later the planet's roots and fields, the
                    world service's policy, placement rules
  vulkan/
    frame/          device, allocator, descriptors, pipelines, the shader
                    compiler, frame graph, targets, timers, GPU resources,
                    defaults, the view, the snapshot pass, the bridge
    draw/           the mesh drawer core and the draw list; meshes, decals,
                    particles, billboards, splats; custom mesh shaders
    world/          the cell pass, the surface layers' atlas; later the world
                    service's executor
    ground/         the ground and its cover: turf, grass, herbs
    foliage/        the plants node's plan, poses and draws; the far field
    lighting/       scene lighting, environment and sky, reflection probes;
                    later exposure, probes, volumetrics, clustered lights
    shadow/         the shadow pass; later shadow pages
    post/           post-fx, SSAO, SSR, DOF
    terrain/        (planned) planet terrain tiles, the clipmap drawn from
                    them, horizons
    sky/            (planned) clouds, weather, stars, the moon
    shaders/        the same folders (terrain/ holds the clipmap's GLSL, which
                    nodes/clipmap_terrain compiles at run time), and common/
                    for what every area includes (camera, lighting,
                    atmosphere, push blocks, vegetation sway); every
                    #include is a path from shaders/
```

A folder is made when something lands in it; the planned ones above have
nothing yet.

## Build order

Each step lands as a measured win on its own: interleaved binaries on
demos/biomes (and, once it exists, the planet), the stills' numeric diff,
the motion oracle for anything temporal. Each step removes what it replaces.

| Step | What | Win it should show | Size |
|---|---|---|---|
| 1 | Done: the old forest path retired (ForestNode, its drawers, planner, impostors, HiZ; the plants' CPU planner) | one foliage path, the plants node planned on the GPU; biomes' stills identical, GPU ms 7–11% lower | −17k |
| 2 | Done: the layout (§12), moves only | frames byte-identical, suites green | moves |
| 3 | The world service (§6), the surface layers' fills its first producer | fills follow the frame's slack; no stall when the view jumps | ~3k |
| 4 | Atmosphere tables and exposure (§7) | one sky from ground to orbit, cheaper than today's per-pixel march | ~2k |
| 5 | Cached shadow pages (§8), then horizons and canopy density (§7) | plant and terrain shadows off the per-frame cost | ~6k |
| 6 | The ground and its cover as one surface (§9) | the eye-level frame's 1.3–1.7 ms of cover cut by most of its hidden and far work | ~2k |
| 7 | The probe grid (§7) | forests, valleys and slopes lit as places; the stand-in falloffs gone | ~5k |
| 8 | Placement per cell (§10) | a world of any size placed around the eye at a flat cost | ~3k |
| 9 | The planet (§11) | ground to orbit, one place, the frame following the pixels | ~5k |
| 10 | Far-field cell cards (§2) | far plants and terrain cost by the cells on screen | ~4k |
| 11 | Volumetric light and clouds (§7) | shafts, mist, weather from every altitude | ~5k |
| 12 | Night and many lights (§7) | moon, stars, cities from orbit, fires | ~2k |

The visibility pass (§3) joins when a measurement shows foliage shading, not
its count, is what the frame is spent on.
