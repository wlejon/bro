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

Where the frame went when this plan began, and where it goes now
(../broworkshop/demos/biomes, 32.5k plants with grass, turf and herbs, GPU
ms; capture.js `cost`; now is after steps 1–6 of the build order, on the
atmosphere):

| | glade then / now | meadow then / now | flyover then / now |
|---|---|---|---|
| grass and herb rings | 1.34 / 1.10 | 1.71 / 1.43 | 0 / 0 |
| ground (mesh and turf) | 0.18 / 0.14 | 0.26 / 0.18 | 0.25 / 0.26 |
| plants (leaves and their shadows) | 0.60 / 0.43 | 0.53 / 0.18 | 0.55 / 0.17 |
| frame | 2.29 / 1.89 | 2.60 / 2.04 | 0.90 / 0.67 |

- **The ground's cover** was two thirds of an eye-level frame: the ground
  shaded whole and the blades drawn over it, and every vertex of a ring
  testing its blade. The cover's plan and its order (§9) took its hidden
  work; what is left is the far blades' lighting and overdraw.
- **Shadows redrawn every frame** for content that does not move: the plants'
  casters were a third of a millisecond on every shot. Cached shadow pages
  (§8) keep only the sway near the eye.
- **At a million plants** (8.4 km) the plants alone take 3 ms, most of it
  plants too far to be more than a few pixels. Fixed by the far field (§2).

The low end is about a 3060 at 60 fps (16.7 ms, `target=16.7`). Measured
on halo's Radeon 8060S (Strix Halo, RADV), a GPU of that class, at golden
hour (the dearest sun, 2026-10-22): at 1920×1080 the shots' means are
2.9–5.0 ms (max 6.5), at 2560×1364 4.4–7.3 ms (max 8.8), and the golden
walk at 1080p 5.46 ms, no frame of 150 over 16.7 ms. About half the frame
is spare there.

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
- the land's horizon about the eye (done: `world/scene_world_horizon.h`),
  for the camera only: for 1,024 directions and 128 distances, the steepest
  slope of the land out to there, made from the land's tiles on the graphics
  queue when the eye moves half a metre, read by the cell pass a frame or two
  later on the compute queue (the land lowered 2.5 m for that drift). A cell
  whose top is under the land's slope in every direction it spans is not
  planned for the camera; the shadow views keep it. At eye level in a 16 km
  biomes most of the forest past a kilometre is behind hills: the glade's
  plants in view fell from 526k to 60k, its frame from 3.03 to 2.76 ms;
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

- **Plants:** the records live in a store of runs, a leaf cell each (placed
  by rules about the eye, or hand-placed by setPlants), uploaded where they
  change (§10). The plants plan (count, lay out, place) walks the
  cells in view and writes the draws' chunks and indirect commands; nothing
  on the CPU walks the plants.
- **Land:** its rings already cost the same whatever the world's size, and
  its tiles are a surface layer set the world service makes (§13). It joins
  the cells as the source of their heights and as a provider for far-field
  captures.
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

### As built: the cover's depth prepass

Measured in a 16 km biomes, the meadow's grass rings were 1.4 ms with
lighting and 0.3 ms with it stubbed (ring 2, 20-55 m, 0.78 against 0.14):
nearly all of it the lighting of blade fragments a nearer blade then hid,
since a stage that cuts (the dithered cover) is depth-tested after it
shades. Thinning ring 2's blades changed nothing, so vertices were not the
cost. The grass needs no ID buffer for that: its vertex work is cheap, so
it is drawn twice (`SceneMeshDrawer::CoverPass`). First its blades' and
herb plants' depth alone, their cut deciding it (mesh.frag's
`MESH_COVER_DEPTH`: nothing lit, no colour written), in a pass of its own
(`PassCoverDepth`) that reads nothing of the frame's compute, so it runs
in the scope the sky opens, before the join, beside the plants' plan
(§4). Then, in the opaque pass, the
same draws lit with the depth test EQUAL and no cut (`MESH_COVER_SHADE`),
so each visible pixel is lit once. Both use one vertex stage
(`invariant gl_Position`), and under MSAA the prepass's per-sample depth
carries the dither. The prepass is 0.21-0.26 ms. Frames: meadow 2.69 →
2.05 ms, glade 2.77 → 2.42, edge 2.55 → 1.85, with the stills the same and
the glade's shadow pages now converging (none left wanted, 286 before).
The far tier, the probes' and the turf's fills draw the cover in one pass
as before. The plants keep their single pass: their leaves' vertex work is
the heavy part, which an ID buffer and resolve (above) would not repeat.

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
  that doesn't need it (sky, static and grass shadows, the cover's depth
  prepass). The graphics stream waits on the compute timeline only right
  before its first consumer. What runs ahead of the join has to outlast the
  compute or the wait shows: in biomes' golden-hour glade walk the plants'
  compute is about 0.4 ms and the graphics ahead of it was 0.2, so the
  first planned pass waited 0.1-0.2 ms a frame; with the cover's depth
  (0.25 ms) moved ahead of the join it waits none. Open: on Windows (the
  4090), in about one walk of two, one frame's first planned pass reads
  1.3-1.8 ms with its usual casters and the compute done 0.3 ms into the
  frame: a gap between the head's submission and the rest's, not work.
  Submitting the head (and the compute) at the join instead of at the
  frame's end made it every walk, the GPU idling there until the rest was
  recorded and submitted, so the submissions stay together at the end.
  It falls on the same frame of the walk (the 147th of 150) in four walks
  of six, and none of the frame's own causes shows there: its compute
  waits only for an old, finished horizon's ticket, as the frames about it
  do; no device memory is allocated near it; no plants' records or cells
  are uploaded.
- **Versioning:** anything compute writes for frame N lives in the slot for
  N's frame in flight. Frame N−1's draws still read their own slot, so compute
  never waits for graphics to finish with a buffer.
- **Ownership:** buffers shared between queues are created CONCURRENT.
  Images move with explicit release and acquire, as the upload queue already
  does.
- **Other users:** the land's tiles, cell residency generation,
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
  land's tiles, the ground's vertex data, the plants' cover) for a cell's
  rectangle and texel size. The turf's fill is a draw:
  the ground's meshes rasterised straight down under an orthographic camera
  fitted to the tile and its border, into the layer's four colour targets,
  so the vertex data a mesh carries reaches the fill as it reaches the
  ground's own pass. Its patterns are band-limited to the tile's texel, so
  a tile never aliases.
- The **budget** is the world service's (§6): each tile a view needs and
  has not got is a job, its error the pixels of what shows in its place (a
  missing tile's nearest resident ancestor's texels, 2^levels its own; a
  stale tile's own), never above its parent's while that is a job too, so
  with ties to the larger tile a layer fills in from the top down. The
  coarsest level's missing tiles are required, made whatever the budget.
  Fills are recorded at the frame's head on the graphics queue (a raster
  job), so the frame's own draws read them without a wait; they are small
  enough that async compute (§4) would buy nothing a raster fill does not
  already have.
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

### Design: the canopy layer

The plants are drawn to their `range` (biomes: 6 km) and cast into the
shadow pages to their `shadowRange` (1.5 km). Past these, a forest is
nothing: from 1.5 km up the far hills are bare, and the plants past the
cast range shade nothing. Drawing them further does not scale: a coarse
page over a square kilometre of forest is tens of thousands of crowns,
and with the cast range at 6 km biomes' golden walk left 750 pages
wanted, against 200–400. The planet cannot place plants to the horizon
at all.

- **What the canopy is comes from the plants' rules.** The rules are
  fields, so the plants' expected canopy at any point is a function, not a
  sum over placed plants. For each layer, its candidates' density (its
  chance over its spacing squared), each species' share (its weight over
  the sum), and the size field give the crowns' projected area per square
  metre τ. The cover is 1 − e^(−τ), a Poisson overlap. The mean top height
  and the leaves' albedo (the species' `albedo`, the same one the probes'
  medium uses) are weighted by crown area. This is written as GLSL from
  the rules and species (`plantCanopyModule`, scene/world/plant_canopy.h,
  its fields through brokit's noise_glsl), so it is the placement's own
  maths and reaches as far as the land does.
- **The land keeps it.** A land on which plants stand
  (`createPlants({ land })`) splices that module into its fill: a fourth
  tile layer (the canopy's albedo and cover), and the top height beside
  the ground's in the heights layer. It is made with the land's tiles, by
  the same jobs.
- **Past the plants' range, the land is the canopy.** In the plants'
  last tenth of range they dither out as the canopy comes in. The land's
  vertices rise by the top height times the cover, and the cover blends
  the canopy over the ground's own surface (mesh.frag's canopySurface).
  It is lit as a sward is, a layer of leaves too fine to resolve, facing
  up whatever the slope. Its albedo is the leaves' in their own shade,
  more of it seen the lower the eye's line (crown sides and the shade
  between them). Both shades are measured against the plants drawn
  there: biomes' high view (1.5 km up) with the canopy past 3 km is
  within 2–10 levels of the plants at 3–4 km, forests and meadows, and
  past 6 km within 2–9 of the plants drawn to 12 km.
- **Past the plants' cast range, the canopy shades through the land's
  sun** (below). A plant past its range casts nothing; the canopy stands
  in its place in the land's sun's march, so the plants' own crowns past
  the cast range are shaded by the canopy toward the sun of them, their
  tops lit.
- **What it costs: nothing measured.** Biomes' shots, interleaved, the
  canopy on and off: the land's pass 0.30 ms either way, every shot
  within noise. The fill evaluates the module once a texel when a tile is
  made.
- **Read a fragment at a time.** The rings rise by the canopy per vertex;
  its colour and cover are read per fragment from the tiles
  (mesh.frag's landCanopyAt: set 3 in the land's own build, SCENE_LAND,
  and the turf's grass node's set 4, LG_CANOPY), at the tiles' texel,
  half a ring cell.
- **Where it falls short.** The far tiles' texels are coarse (60–90 m at
  8–12 km, where biomes' high view has pixels of 3–20 m), so a field as
  crisp as a forest's edge is smeared across them: with the canopy past
  6 km, the forest's pixels there (as the plants drawn to 12 km mark them)
  were 5 levels of red darker than the meadow's, against 25 with the
  plants, its mean the same, so the stands faded into a tone at a line.
  Far plants cost next to nothing (to 12 km, 0.01–0.02 ms more on the high
  views), so biomes draws them past its edge; the canopy is for worlds
  larger than the plants' range, where a finer far tier for it is still
  to do.

### As built: the land's sun

The shadow pages cannot hold a shadow cast along the light. Their depth
offsets (a constant and a slope term, §8) are sized for casters that face
the sun; at a low sun a raised canopy top or a long hillside lies nearly
along the light, and a 20 m rise is two metres of light-space depth at 6°.
Raising the land's caster by the canopy changed nothing at golden hour,
and the probe grid's land shade ends at its last cascade, 2 km out. The
land's sun is the shade the land and its canopy throw, made from the land's
tiles in world space:

- **Levels about the eye** (`scene/world/land_sun.h`,
  `ground/land_sun_layout.h`): six square levels of 512² texels, level
  l's texel 2·2^l m (±512 m to ±16 km), each texel two floats: the world
  height a point there must clear to see the sun, and how far off what
  shades it stands. A level is centred on the eye, snapped to 32 of its
  texels.
- **A march toward the sun** (`ground/land_sun.comp`, a texel an
  invocation): from the texel's ground the ray climbs toward the sun in
  steps of the larger of a texel and 0.12 of the way come (at most 64),
  reading the tiles a step's length fine. At each step what stands there
  (the ground, and past the plants' cast range the canopy's top times its
  cover) less the ray's climb is a height to clear; the texel keeps the
  highest and stops once the ray is over the land's top. It starts a texel
  below its own ground, so open ground clears it by more than a receiver's
  soft band however low the sun.
- **Every lit shader reads it** (`scene_lighting.glsl`'s sceneLandSun, the
  lowest of it and the probe grid's land shade): the finest level holding
  the point, blended into the next over its last 24 texels, the sun seen
  over a band a quarter texel and 2% of the shader's distance to the
  shading wide. A leaf or a far crown the land shades skips its page
  lookup.
- **A world service job a level** (the `land sun` kind, Raster, after the
  land's fills). A level is made when it was never made (required), when
  the eye moves it a snap, the land's module, canopy or cast range
  changes, the sun turns a quarter degree, or (30 frames on) the tiles
  under it refined, at an error of 8, 8, 4 or 1 pixels. A want's error
  doubles every 30 frames it waits, up to 64: a level made over a view's
  first coarse tiles, outranked by the probes' traces frame after frame,
  stood the coarse heights over the glade's slope and shaded it for good.
  A level has two copies; it is made into the one not shown and shown
  from then on, since the frame's lighting block is written before the
  world service records. The image (12 layers of rg32f, 25 MB) is made
  with the first land and stays in GENERAL.
- **What it costs.** A level is 0.04–0.06 ms; six in one frame were
  0.21 ms in the afternoon and 0.35 ms at golden hour. Biomes' golden
  shots, interleaved, on and off, two rounds: no shot dearer (edge 1.70
  against 1.82 ms, meadow and above cheaper too, the rest within noise),
  the walk 5 and 5 frames over 2.78 ms against 4 and 7, the flyover the
  same to 0.01 ms.
- **What it shows.** From the flyover and the high view at golden hour
  the forests' edges throw long shade across the meadows past the plants'
  cast range and the hills shade the valleys behind them, where before the
  far land was lit flat; the afternoon's glade slope stays lit (a CPU
  march over the land's heights agrees). test_land: a ridge 40 m high at a
  4° sun shades flat ground 350 m behind it (red 6.1 against 18.2 before
  it; with the land's sun not read, 19.4 and 19.4).
- **Where it falls short.** The canopy's cover is a mean; it shades as a
  soft slab, not as crowns. Its penumbra is a texel at least, so a level's
  coarse texels (32 m at level 4) blur a hill's shade line far off.

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
  Less a headroom: a frame's own work and a job's cost each vary from what was
  expected of them, so a budget spent to the target runs about half its
  frames over. The headroom is learned as the quantile of that surprise that
  leaves 0.5% of frames over (a step up on each frame over, a two-hundredth
  of one down on each under). At 2% it took biomes' golden-hour walk from 16
  of 150 frames over to 3-8, the mean unchanged; at 0.5%, with the next
  frame's work charged to it (below), from 7 to 3-4 (four rounds
  interleaved), the pages drawn a frame and the backlog the same.
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

### As built

- **The policy** (`world/world_service.h`, CPU only, unit-tested by
  `bro_world_service_test`) holds the kinds (a name, raster or compute, a
  first guess at a unit's cost), takes each frame's offers (key, producer,
  error in pixels, size, required, units) and grants them: required jobs
  first, then by error, ties to the larger tile, then by kind, producer and
  key, so the order never depends on who offered first. A job's units are
  its work in its kind's measure: 1 where a kind's jobs cost alike, a probe
  box's probe-rays where they do not (a fresh strip of the probe grid traces
  twice the rays of a refresh slice, and the boxes differ in columns). A job
  is granted while the frame's spending plus its cost (its kind's a unit
  times its units) fits the budget; one that does not fit is left, and a
  cheaper one may still fit.
- **The budget** is the target less the most the last four timed frames
  needed outside the service (each frame's GPU ms less its service spans on
  the graphics queue; spans on the async queue are not subtracted), less
  the headroom (above: the surprise's 99.5th percentile, learned), floored
  at 0.1 ms. A frame over the target, or no timed frame yet, gives the floor.
  A kind drawn the next frame (`WorldJobKind::nextFrame`: the shadow pages,
  planned before the service runs) spends that frame's budget: its grants
  are carried, the next frame's jobs drawn at once fit what the carry leaves,
  its own next-frame grants fit the budget less its jobs drawn at once, and a
  job drawn at once that the carry keeps out holds its cost back from them,
  and from the jobs drawn at once ranked under it, so it goes the frame
  after. (Charged to the frame that granted them, a frame drew last frame's
  pages and spent its whole budget again: on the walk a frame drew 0.86 ms
  of pages beside 0.39 ms of its own probe traces. Held back from the next
  frame's grants only, a dear job was starved by the cheap ones under it:
  the probe grid's world for an unseen strip, 0.26 ms, waited ten frames of
  the walk behind refresh traces taking the slack it needed, until the
  strip was seen and it was required, with that strip's fresh traces, in
  one frame: 0.86-0.88 ms of world on a 0.45 ms budget, frame 80 over in
  every walk.) The probe grid's world is charged in plants splatted into
  the coarsest cascade: each box's stages (16,000), its sun's march out
  through its cascade and each coarser one (16,000 a cascade: the march is
  latency-bound, so a strip costs about what a cascade does), and the
  plants of the cells over it, each by the share of the cell the box
  covers and dearer in a finer cascade (at most 64 times). The splat
  samples only the slice of a crown that falls in its box, at the whole
  crown's points a voxel: drawn through whole crowns, a strip of the
  finest cascade cost 0.21 ms, almost all of it points outside the strip,
  and its world waited unseen behind the shadow pages' carry until it was
  required (biomes' golden walk, frame 80: 0.37 ms granted on a 0.43 ms
  budget). Sliced, the strip costs 0.09 ms, a coarsest cascade's 0.025 ms,
  the four remade at once 1.0 ms; the walk's frame 80 ran 2.75-2.83 ms
  against 2.82-3.08 over six walks each, and the stills are as alike as two
  runs of either.
  The target is a scene setting, `scene.setFrameTarget(ms)`, 2.78 ms by
  default. A kind's cost a unit is learned from its span's ms over its
  units: the first measurement replaces the guess, and each later one pulls
  it toward itself by a power of their ratio, 0.25 x 0.9 when it is dearer
  and 0.25 x 0.1 when cheaper. A steady cost is converged on; one that
  varies is priced near its dear end, so a frame's grants of it fit when
  they come in dear. (Followed at the mean, biomes' plant pages, 0.02 to
  0.1 ms apiece at a low sun, were granted fourteen to a frame at 0.018: on
  the golden-hour walk 8-9 of 150 frames ran past 2.78 ms, to 3.5-4.1 ms,
  with up to 28 pages drawn in a frame; priced near the dear end, 6-7, to
  3.0-3.25, at most 7-11 pages, and the afternoon walk's 4-5 became 2.) (Charged
  by the job, the probe traces' kind learned the average box: a frame
  granted three wide cascade-0 slices on a budget of 1.5 ms spent 2.1 ms on
  them, and a 16 km biomes flyover at 40 m/s ran 8 of 150 frames past
  2.78 ms, to 4.3; charged by probe-rays, none, its worst 2.66 ms.) Units
  hold only while a job's time grows with its work: a trace that marched a
  probe's rays a workgroup, each invocation two to four rays in turn, ran
  as long as its slowest probe's marches, 0.6-0.75 ms whether 6k or 49k
  probe-rays. A ray an invocation (a probe's rays over PG_TRACE_PARTS
  workgroups, the blend summing their SH) took it to 0.2-0.37 ms. Costs by
  a job's class within a kind (a shadow page's level: near ones draw every
  leaf, far ones discs) were tried and dropped: on the golden-hour walk the
  frames over and the mean were the same with them as without.
- **A cut.** The slack a view left says nothing of the next view after a
  jump, and a frame or two of timings is read late. So when the eye moves
  more than 10 m or the view turns more than half its vertical field of view
  between two renders, the service forgets its slack, and the frames
  rendered before the cut count only for the kinds' costs. The new view
  then settles: its budget is at least 8 ms until nothing worse than a
  pixel is left queued four frames running (what the frames in flight read
  back), or 30 frames have passed. A cut wants a whole place at once (the
  glade from the meadow: 1,655 jobs of turf, still and plants shadow pages
  and land's sun levels), and its first frame is a hitch whatever is spent.
  On the running budget (0.5-0.7 ms) the glade took 64 frames to settle,
  the land's sun levels made about the meadow shading it for 20 of them,
  and a still a second in caught a straight-edged lit field where, settled,
  the trees shade it; settling, it is five frames.
- **The executor** (`vulkan/world/scene_world_service.h`, run by the
  `world-service` pass at the frame's head) feeds the policy the GPU
  timer's frames, collects the producers' offers, and runs each kind's
  granted jobs inside one span of the timer (`SceneGpuTimer::beginSpan`,
  stamped always, with the units it held), and marks each graphics kind as
  a timed pass of its own (`world:<kind>` in `perf.gpuPasses()`). A raster
  kind records into the
  frame's graphics stream; a compute kind into the frame's compute stream,
  the async compute queue where there is one, else the graphics stream at
  the same point. The compute path has no shipping producer yet; it is
  exercised by `bro_vulkan_scene_test`'s "compute jobs on the compute
  stream". The compute queue at low priority is not made: a world job on
  the async queue shares the plants' plan's priority until a compute
  producer's jobs are long enough to need one.
- **The turf** (the surface layers' one kind, `turf`) offers every tile its
  view wants and has not got; its fills are about 0.01 ms each. A refill
  still writes its slot in place (§5) rather than a free slot swapped by
  version: the queue orders it after the frames that read the old contents,
  and nothing waits.
- **Stats:** the scene's cull stats carry `worldBudgetMs`, `worldSpentMs`,
  `worldJobs`, `worldQueued` and `worldWorstError` (docs/scene-api.js).

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

The probes (below) light every lit surface's ambient; the plants' own crown
occlusion is left only below their resolution.

### Design: the probe grid

What it fixes, in biomes: a leaf's sky light is the open sky's, scaled only
by how deep it sits in its own crown, so a tree in the middle of a forest
is lit like one alone in the meadow, and the floor under the trees is
darkened by a tuned map the app hands in.

- **The probes follow the ground.** A cascade is a square of columns
  around the eye, each column a few probes at fixed heights above the
  ground under it (a forest's light changes with height above the ground,
  not with height above the sea). Each cascade's spacing is a few times the
  last's (2 m near the eye, out to kilometres), and a cascade moves with the
  eye in whole columns, its storage wrapping (a clipmap), so a step forward
  only refreshes the new edge. A point finds its cascade by distance and
  blends to the next near the edge; inside one, its column by x, z and its
  layer by its height above the ground, so hardware trilinear filtering
  across columns blends probes at the same height above their own ground.
  Past the last cascade the open sky is the far field (§7's horizon and
  canopy tiles replace it).
- **Each probe holds irradiance** as l ≤ 1 spherical harmonics, RGB, in
  three RGBA16F 3D textures (all cascades in one, stacked along the layer
  axis), so a lit pixel pays a few filtered taps. Their sum is the light
  from every direction: the sky as it gets through, the ground's bounce
  and the canopy's green.
- **A probe traces a world made for it**, never the drawn geometry:
  - the ground's height under each cascade, captured from above (the
    depth of what is opaque and stands still, the shadow pass's casters
    drawn orthographically, so any mesh, terrain or building the scene has
    is in it; the planet's height tiles take its place in step 9);
  - the canopy's density: the plants' crowns as a medium, their leaf area
    a volume around the eye splatted from the plants' records (each crown
    its species' ellipsoid and extinction, the same the plants' shader
    shades a crown by), with the leaves' mean albedo;
  - the sun's light through that medium, a volume of its transmittance to
    the sun swept along the sun's way, remade when the sun or the canopy
    moves.

  A ray marches the medium (its transmittance falling, picking up the
  light the leaves it passes scatter: the sun's through the transmittance
  volume, the sky's from the probes themselves, a bounce later) and ends on
  the ground (its albedo, the sun on it, the probes' light) or in the sky
  (the sky-view table). A probe's rays go into its harmonics, blended into
  what it held (hysteresis), so light changes over a few updates rather than
  flickering.
- **World-service jobs.** A job is a slice of a cascade's probes, ranked by
  the error of their staleness; the density and transmittance volumes are
  jobs when their cells change or the sun turns. A probe is never waited on:
  the frame reads what the textures hold.
- **What it replaces.** The ambient light of every lit surface (mesh,
  plants, grass, herbs, ground) is the probes' irradiance along its normal,
  its sky reflection scaled by how much of the open sky's irradiance got
  through. The plants keep only the occlusion of their own crown finer than
  the probes resolve (a crown much smaller than the spacing); the turf's
  and the herbs' canopy darkening go. The floor map stays what it is for
  (where litter and herbs grow), not a light.

Built in this order, each verified before the next: the density volume
(checked against the records on the CPU), the ground's height (against the
mesh it was drawn from), the probes (a probe in the open within a few
percent of the open sky's irradiance, a known crown's shade against a CPU
integral), then the shaders' switch, judged in stills and in motion.

### As built: the probe grid

On when `SceneGraph.setProbeGrid(true)` (biomes turns it on); off, the
ambient is the open sky's as before.

- **The CPU half** (`world/probe_grid`, testable without a GPU): 4 cascades
  of 32 × 32 columns, 8 layers, 2 m apart in the finest and each the last's
  spacing times 4 (2, 8, 32, 128 m). The corner snaps so the eye stays in
  the middle column; storage wraps (column i at `(corner + i) mod 32`). A
  column's layers stand 0.25 m over its ground up to the canopy's top
  (`max(8, 1.15 × the tallest crown)`), spread by the power 1.6. The
  columns a move brings in are fresh boxes (traced afresh), at most 8
  columns each; the rest refresh a slice (2 rows) at a time.
- **Unseen columns.** A lit surface gives a cascade's outermost column no
  weight (the blend into the next coarser ramps in over columns 1 to 4), so
  the strip a one-column move brings in is not seen until the next move
  takes it inside. Its world and its fresh traces are offered at 1 px
  (ahead of a refresh's aged error), not required, the traces once its world
  is made; the next move makes them required. A walk's moves are made in
  the slack of the frames between them, not all in the frame of the move:
  biomes' glade walked at 1.4 m/s, the frame of a move had cost about
  0.85 ms of required world and fresh traces (6 of 150 frames over
  2.78 ms, to 3.2 ms); now 0–4 over, by at most 0.15 ms (the frames' noise
  past the budget's window), the walk's mean 2.43 → 2.23 ms. Until made,
  the column holds what its storage held: the light of the column that
  left across the square. The last cascade, blended into nothing, is seen
  to its edge, as is a strip two columns wide.
- **The world it traces** (`lighting/scene_probe_grid`): the ground's
  heights (128² a cascade, the still opaque casters drawn straight down
  through the shared caster drawer), the canopy (64 × 64 × 32 RGBA16F of
  extinction and albedo, every plants node's crowns splatted from their
  records) and the sun's light through it (RG16F: the transmittance past
  the canopy and the ground, and the land's shade alone, the least the ray
  clears the ground by as a share of a penumbra 0.02 a metre wide, the ray
  going on through a canopy that took all the light). It is stored
  wrapped like the probes (`pgHeightStored`, `pgVoxelStored`), so it scrolls:
  what is stale is boxes of a cascade's columns (`ProbeGrid::staleBoxes`),
  the strips a move brought in, the columns over a change of the canopy
  widened away from the sun as far as its shade reaches under the layers'
  top, every column when the sun turns a quarter degree or the top moves.
  A cascade's boxes never overlap (one that would takes in those it
  overlaps; past 8 they merge into one), so the splat deposits only into
  its box's voxels and the resolve zeroes the sums it reads. All of it is
  one required world-service job, a dispatch a box and stage, recorded
  ahead of the traces in the same stream: a cascade's traces are offered
  the frame its world is remade (a cascade moving every frame, the finest
  past 720 m/s at 360 fps, would otherwise trace nothing until it stood
  still, and then its whole square at once). A trace's ray under a hole's
  base going down ends there (no ground, no canopy under it).
- **The probes** (`lighting/probe_trace.comp`, `probe_blend.comp`): l ≤ 1
  SH of radiance in three RGBA16F 3D images (32 × 32 × 32, a colour channel
  each). A workgroup traces a probe's rays (256 fresh, 128 a refresh, a
  Fibonacci set turned at random) through the finest cascade's world at each
  step: the canopy scatters the sun through its transmittance and the
  probes' mean light, the ground sends back its albedo (the atmosphere's
  ground, else the renderer's ground bounce) of the sun and of the probes
  over it, an escaping ray takes the sky. The traces go to a buffer and a
  second dispatch blends them in (fresh replaces, a refresh blends a half),
  so a trace reads only what the probes held before it.
- **The refresh cadence.** A slice of the finest cascade is offered a second
  after its last refresh, doubling each coarser one; the slices start spread
  over that period, so they come due one or two at a time. A slice just due
  is a quarter pixel wrong, growing with its age (halved each cascade): under
  a shadow page still building (half a pixel), so light that has probably
  not changed waits for what visibly has. At a pixel a second the refreshes
  outranked the shadow pages and left the meadow's tree shadows undrawn.
- **The lit shaders** (`common/scene_lighting.glsl`, lighting set bindings
  17–22: a repeat sampler, the SH, the heights, the sun's volume): `sceneAmbientIrradiance` is
  the probes' irradiance along the normal (each cascade blended into the next
  over its outer 1–4 columns), else the open sky's; the ambient's specular is
  scaled by how much of the open sky the probes see along the reflection. A
  plant's crown keeps its own occlusion only where the crown is smaller than
  the probes' spacing there (`SceneProbe::spacing`), a quarter for the
  leaves' crowding where they resolve it. A lit shader reads the grid once a
  pixel (`sceneProbe`: one walk over the cascades, the ground under the
  point read once a cascade, giving the SH, the land's shade and the spacing)
  and hands it to `sceneDirectLight` and `sceneAmbient`; walked once for
  each, the ground's four texels a cascade read up to three times a pixel.
- **The land's shade** (binding 22, `sceneLandSun`, blended over the
  cascades as the probes are) multiplies the sun's shadow wherever it is
  read: `sceneDirectLight` (leaves past their shadow's floor, so a hill's
  shade passes on nothing) and the plants' own sun. The shadow pages miss
  it at a low sun: the slope-scaled bias that keeps a lee face off its own
  depth swallows the shade a hill casts over it. At golden hour (6°) a
  third of biomes' ground should be in a hill's shade and almost none of it
  was; the sward's light, off on a slope facing away from the sun and
  tipped by the gusts, showed as dark islands drifting over a lit hillside.

Measured: `bro_probe_grid_test` (49 checks) and `bro_probe_grid_gpu_test`
(the world against its scene, every cascade's again after a move scrolls
it; the land's shade under a low sun up the slope within 3% of a march over
the ground, a voxel's error 0.006 on average, and nothing under a high sun
through the crowns; under a black crown 32 probes within 0.04 of a
CPU march and its shade within 2%, the open sky within 1%, sunlit ground's
bounce within 1.5%). On biomes, interleaved: the forest bands 5–11% darker,
the sky bands 2–6% brighter by the exposure, the meadow unchanged; +0.25–0.4
ms a frame under capture.js's 30 Hz clock (a 512-probe slice, about 0.3 ms,
nearly every frame; at 360 fps one frame in twelve), no frame over 2.78 ms;
the motion oracle (pan, a turning sun) shows no refresh step, the frames
between the shadow pages' steps changing 0.027 a point either way. Flown
over a placed forest (2 km range) at 360 fps steps, interleaved against
remaking whole cascades: at 300 m/s the world's remake 0.20 → 0.11 ms a
frame, the frame 1.28 → 1.23 ms mean (p99 1.9 both); at 1,000 m/s the
remake 0.53 → 0.27, the frame's p99 9.4 → 2.2 ms (the finest cascade's
traces were piling up, then its whole square traced in one frame), its mean
1.55 → 1.73 ms, the finest cascade now traced as it moves (0.76 ms a frame
of fresh probes at that speed). Stills: the scrolled world alone is the
remade one's to within the runs' noise; with refreshes always granted (a
20 ms target) the glade's light is the same either way. Not yet: a frame
near its target (the glade and the meadow, 2.54 ms) leaves the world
service less than a refresh slice's cost (about 0.3 ms against the 0.1 ms
floor), so their probes keep their first fresh trace however old it grows;
which frame that trace fell in moves their stills 2–3% in some bands.
The land's shade, interleaved: the afternoon's stills unchanged (a mean
difference of 0.5–0.7 of 255); at golden hour the glade and the meadow
+0.05–0.15 ms (3.1 → 3.2 ms, both already over 2.78), the forest's edge
0.3 ms less (its shaded ground lit by less).

### As built: the atmosphere and exposure

The first two bullets. `world/atmosphere.h` holds the parameters (Earth's
air by default: Rayleigh, Mie, ozone, the planet's radius and albedo, flat
or spherical) and the tables' parametrisations, which
`shaders/common/scene_atmosphere.glsl` mirrors; `vulkan/lighting/
scene_atmosphere` makes the tables, and every table holds light for a sun
of 1 (the sun's illuminance is a uniform), scaled by 1024 in its fp16 so a
twilight sky does not underflow.

- **View-independent tables, world-service compute jobs** (required, so the
  frame the air changes is the frame they are remade): transmittance
  (256 × 64, Bruneton's (μ, r)); multiple scattering (64 × 32 over
  (μs, r), μs mapped by its square root so the twilight degrees get half
  the table, Hillaire's L2 / (1 − f_ms) with the ground's bounce in f_ms);
  the sky's light (64 × 32 × 5: the radiance arriving from every
  direction at an altitude and sun angle as l ≤ 2 spherical harmonics in
  the sun's frame, the lit ground below included).
- **View tables, per frame slot:** the sky view (256 × 128, azimuth from the
  sun by zenith angle, the horizon at the middle; from above the air it
  covers the shell and the planet's disc), remade when the eye's height or
  the sun moved enough, in the graphics stream: it is small and seldom
  remade, and the sky then draws before the frame waits for its compute,
  beside the plants' plans; and the aerial perspective (32 × 32 × 32
  froxels over the camera's frustum, slices squared in depth out to the far
  plane or the air's horizon, eight steps a slice, a slice ending at the
  ground, an invocation a froxel and a scan summing each column front to
  back), in the graphics stream beside the sky view, ahead of the frame's
  join: on the async compute queue the join waited for it behind the
  plants' plans, where the graphics idled anyway.
- **The sky** reads the sky view (its lookup dithered by a texel), the sun's
  disc from the transmittance with its limb darkened, the planet's surface
  where the view meets it, and nothing where a view from above misses the
  air. Every lit shader takes its haze from the froxels (`sceneAir`), the
  sun at a surface dimmed by the transmittance, and its ambient from the
  sky's light (`sceneSkyIrradiance`): one per-pixel march, the old
  isotropic multiple-scattering fill and the one sky colour a frame are
  gone.
- **Exposure** (`scene/exposure.h`, `vulkan/post/scene_exposure`): EV100 with
  a compensation, scale = 1 / (1.2 · 2^(EV − compensation)). Fixed (the
  default, EV −log2 1.2: a scale of exactly 1) or auto: a 128-bin log2
  histogram of the HDR frame on the GPU (a pixel of every 4 × 4), its
  log-average over the 10–90% share keyed to middle grey (0.18, so EV100 =
  log2(L / (1.2 · 0.18)); pixels darker than its range, such as space,
  left out), adapted exponentially at separate up and down speeds, all in
  one dispatch whose last workgroup meters, read by the tone pass on the
  GPU so no frame waits on it. The old exposure multiplier is the fixed
  mode's compensation.
- **Free of the atmosphere when off:** the lit pipelines specialise on it
  (`SCENE_ATMOSPHERE`, the mesh drawer's kAtmosphere raster bit), so a
  scene without it has none of its code; and pooled buffers of 64 KB or
  more start on a 64 KB page, since where a big buffer began in its page
  moved the plants' shadow draws by 0.06 ms.

Measured. Against a CPU reference (`src/scene/tests/atmosphere_reference`:
brute-force single scattering and multiple scattering iterated order by
order to 0.1%) at 2 m, 2 km, 30 km and 400 km and suns at 68°, 10° and −4°,
`bro_atmosphere_test`: transmittance within 0.7%; day sky radiance worst
7.3%, mean 4.8%; twilight worst 25%, mean 10% (the sun's last degrees,
where the sky's light falls by orders of magnitude); aerial in-scatter
worst 7.5% by day, 18% at twilight; the air's transmittance 0.6%; the sky's
light on the ground 1.4–5.8%. In a clear day's haze (turbidity 10, the
workshop kit's air) at 2 m and 2 km: sky radiance worst 14%, mean 5%;
aerial in-scatter worst 12%, mean 8% (the haze's forward lobe scatters
more of the light more than once, which the isotropic series fits worse);
the bright aureole such a sky has about the sun is the reference's too.
The sky-view table is checked over a dense grid of directions (the zenith
cap finely, azimuths both sides of the sun) and as the sky pass draws it.
The sky and haze cost, interleaved at 2560 × 1364 on a 4090: 0.61 ms for
the per-pixel march, 0.13 ms for the tables (sky 0.04, view tables
0.007). biomes on the atmosphere, its sun at 128,000 lux, turbidity 10 and
exposure metered: 0.10–0.24 ms more a frame than on its HDR panorama with
no haze (GPU ms, HDRI / atmosphere: glade 1.92 / 2.10, edge 1.99 / 2.23,
meadow 2.13 / 2.30, canopy 0.89 / 0.99, above 0.67 / 0.79, flyover 0.74 /
0.86); per pass, the view tables 0.009, the sky 0.04–0.06 against 0.025,
the exposure 0.016, and in the lit shaders the aerial lookup 0.04–0.08 and
the sky's SH 0.03–0.05. The sky's gradient bands are gone (the longest run
of one colour down a sky column 6–9 rows, was 9–13). A sunlit field meters
where a camera puts it (biomes' ground band, sRGB luminance: meadow 77,
canopy 75, above 80; on the HDR panorama 105, 89, 83). A cut canopy →
flyover → canopy adapts with no overshoot or oscillation and holds still
once the view does. Those stills were taken in turbidity 10. Facing the
afternoon sun at eye level, that haze turned the whole sky white under the
ACES curve, so the workshop's kit now uses turbidity 3 (about 145 km of
visibility). Its sky stays blue around the sun's glow. From above, the ground
past the terrain fades the same either way: what that view lacks is the
planet's relief (step 9), not haze.

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

### As built: cached shadow pages

`world/shadow_pages.*` is the CPU half (tested by `bro_shadow_pages_test`),
`vulkan/shadow/` the GPU half.

- **The sun** is a clipmap of 16 levels fixed in light space. The finest
  level has a texel of 1/1024 m and each level doubles it. Each level is a
  64 × 64 window of 128-texel pages that moves in whole pages, so a pan
  changes only which pages are needed and no edge swims.
- **Spots and points** have their own maps on the same pool: a four-mip
  chain per spot and six per point.
- **The pool** is 60 × 60 physical pages of 128 texels plus a 4-texel
  border, reused least recently used. Each frame's views (rects of pages)
  are drawn into a 4096² scratch target and copied into the pool.
- **Requests.** `shadow_mark.comp` marks the page each 2 × 2 block of the
  depth buffer needs, at the level its footprint wants. The marks are read
  back a frame or two late.
- **Lookup.** A lit pixel walks the page table from the level it wants to
  the finest resident one. It filters inside that page's border, the filter
  widening across a level with the footprint (up to twice the texel, the
  next level's own), so no band blends two levels.
- **Penumbra.** A shadow cast from far up the light is as soft as the
  sun's disc makes it there (2 × its angular radius a metre along the
  light). Where that's wider than the still penumbra (0.5 m), the lookup
  reads it at the level whose filter spans it. The search level is the one
  whose texel is about the still penumbra (level 9), past the sway level,
  every plant in its pages. A gather of compares there asks whether a
  caster lies as far up the light as the still penumbra's distance (53 m),
  twice it, four times it. The share of its texels that do lifts the
  wanted level from the footprint's toward the penumbra's (two levels
  finer than the search level, plus one for each doubling), so the
  softening is continuous. A point whose footprint already reaches the
  search level doesn't search. The marking pass asks for the search level
  and the two finer ones. Under a 6° sun a tree 8 m up casts its shadow
  76 m along the light, 0.7 m soft: the glade's long tree shadows are
  soft bands, the ferns' beside them sharp. It costs about 0.08 ms on
  eye-level frames, in the near rings' lit passes.
- **Drawing pages.** A needed page that isn't drawn, or is stale, is a job
  for the world service (§6). Its error is what shows meanwhile: 2^k px for
  a drawn ancestor k levels up, 1 px when stale. The top of a needed chain
  is required, and the base pages covering the view are drawn at once.
  There are two kinds of job: pages the plants draw into (a sun page past
  the sway level) and pages only what stands still draws into. A still
  page is one unit; in biomes it costs about 0.007 ms, mostly its copy. A
  plants page's cost follows the leaf elements drawn into it, not the page
  (about 0.4 ms a million elements on the 4090; one page 0.007 to 0.2 ms),
  so its units are its texels plus the elements its plants cast into it.
  Each plants cell within the plants' shadow range gives `ShadowPages::addLoad`
  its elements at every level past the sway level (`PlantsNode::castElements`,
  the plan's count for its species at their scales, kept with the cell
  until it changes), spread evenly over the pages its plane rect touches
  there. (Priced by the page, three interleaved golden-hour walks ran 1, 0
  and 2 of 150 frames past 2.78 ms, to 3.8 ms, with up to 61 pages drawn in
  a frame; priced by the elements, 1, 0 and 0, the one at frame 147 the
  submission gap (§4), with at most 20-43. The flyover is unchanged.) The
  load is worked out on the CPU each frame, and per-frame CPU work shows up
  in the GPU's frame time: worked out per level per cell rather than kept,
  it cost the flyover 0.2 ms a frame, with the GPU idle between the
  frame's submissions. A kind's GPU timer span runs in segments, its draw
  and its copy, so its price is what it costs.
- **A plan out of room.** The plants' GPU plan sizes its records and leaves
  from what the frames before asked, so a frame that asks for more (a jump
  into denser plants) draws short, and a page keeps what it was drawn with.
  Each page holds the shadow plan's serial its plants were drawn at; when a
  frame's read-back says it asked past its room, the node is told
  (`PlantsNode::notePlanShort`) and the pages that frame drew are missing
  again (`ShadowPages::redrawPlantsOf`), their ancestors read until they are
  redrawn. Biomes' glade from the meadow ran out of room in two frames, whose
  175 and 123 pages would otherwise have kept their missing plants until
  something else redrew them.
- **What moves** is drawn each frame into overlays of the needed pages it
  touches, sampled in the scratch target. Plants and grass sway in the sway
  level's overlays (8 mm texels), which the finer levels read; past it their
  sway is under a texel and they sit in the cached depth. A crown whose
  penumbra at its middle (the clipmap's sun, fixed while it's shown, so
  its cached pages and its overlays part the crowns alike) is wider than
  the still penumbra hides its sway too. The overlays leave it out and the
  cached pages of every level take it (`ShadowPlants`: Swaying, Still,
  All), cast no finer than its penumbra: an element about as wide. The
  lookup reads its shadow at the penumbra's level. The fine pages' coarse
  copy serves only where a receiver lies nearer its caster than the crown's
  height (up a slope from it). Under biomes' golden-hour sun (6°) every
  tree within about 140 m sunward fell in the near overlays: the glade cast
  2.72M elements into 207 overlay pages each frame, 1.08 ms, and no frame
  had slack to draw pages in. Now:

  | golden hour | before | after |
  |---|---|---|
  | glade | 3.07 | 2.20 |
  | edge | 2.14 | 1.80 |
  | walk (1.4 m/s) | 3.00, 150 of 150 over | 2.45, 15 of 150 over |

  The afternoon is the same within 0.1 ms (the search's cost). Holding
  still, the ground's frame-to-frame change is the build before's. A sun
  turning 0.02° a frame rebuilds the clipmap faster than its near pages,
  now with the still crowns in them, can be drawn. The ground's change
  doubles (0.18 a point a frame against 0.08). At a real sun's speed a
  build is about 60 frames every 24 s.
- **The sun turning.** A turn past 0.1° (or casters outgrowing the depth
  range) begins the next clipmap in the sun as it is now, while the shown
  one is still sampled. Its pages are jobs under every shown page (0.5 px).
  It's shown once requests have named it and every page it needs is drawn,
  or after 90 frames. A turn during a build waits for it.

Measured on biomes, 2560 × 1364 on the 4090, interleaved against the atlas
it replaced:

| | Atlas | Pages |
|---|---|---|
| glade | 2.10 | 2.29 |
| edge | 2.25 | 2.33 |
| meadow | 2.30 | 2.39 |
| canopy | 0.99 | 0.82 |
| above | 0.78 | 0.60 |
| flyover | 0.86 | 0.65 |

- **Look:** at a texel a pixel the canopy's dappled leaf shadows are sharp
  where the atlas gave blobs.
- **Motion:**
  - Walking (1.4 m/s) costs 2.36 ms and draws about 10 pages a frame.
  - Flying (40 m/s) costs 0.65.
  - A sun turning 0.02° a frame (about 25 times a day cycle's speed) costs
    2.52 against the atlas' 1.98, with 19 of 96 frames over 2.78 ms.

Open:

- **Eye-level lookup cost.** At eye level the page lookup costs 0.1–0.2 ms
  more than the atlas did. It's the texel density mid-field, not the finest
  levels: a coarser texel saves it but loses the dapple.
- **Unpriced plan wait.** The plants' GPU plan covers the camera and every
  shadow view in one dispatch chain. Its wait at the join (up to about
  0.2 ms more on frames that draw many page views) isn't in any kind's
  price.
- **The show is a step.** Every shadow moves at once by the 0.1° turn: the
  ground's frame-to-frame change spikes to about 3 (of 255) a point on a
  show frame, against 0.03 between shows. At a day cycle's speed that's a
  small jump every half second or so.
- **The copy** runs about 1.8 µs a page as image-to-image regions.
- **Flyover streaks:** faint streaks in tree shadows, along the light.

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

### As built: the cover's plan

Measured first (pipeline statistics per ring, biomes' meadow): the rings
ran about 5 million vertices a frame for some 600 thousand triangles on
screen. Every vertex re-derived its blade or plant and tested it, and most
tests failed: grass ring 0 ran 1.4M vertices for 9% of its triangles on
screen, the far herbs 0.72M for 14%. Batching more cells into an instance
bought nothing (the cost is the vertices' work, not the instances).

- **The plan** (`vulkan/ground/scene_cover_plan`, `cover_plan.comp`) runs
  the test (`grassBladeLive`, `herbPlantLive`) once a candidate over each
  ring's cells in view, and lists the blades and plants drawn. Each ring's
  camera draw takes them as its instances through an indirect command. It
  costs 0.02 ms in the graphics stream (it samples the field, which lives
  on the graphics queue). Grass ring 0 now runs 0.23M vertices, the far
  herbs 0.12M, with the same triangles on screen.
- **Order:** plants, then the cover, then the ground, so the ground under
  the blades fails the early depth test (its shading 0.26–0.33 ms was
  0.13–0.19 drawn after them).
- **Casters** stay a draw a run of the near ring's cells, each vertex
  running its blade's test.

Biomes, GPU ms interleaved: glade 2.29 → 1.87, edge 2.33 → 1.93, meadow
2.37 → 1.98, canopy 0.82 → 0.68, walking 2.35 → 1.88.

Grass ring 2 (20–55 m) is then the dearest ring (0.4–0.5 ms at the edge
and the meadow): about 3.5M fragments a frame, four blades deep over its
band. Taken apart (throwaway builds, interleaved): drawn unlit it is 0.22
ms, so its fans' lighting is half of it, the sun's shadow lookup 0.07 ms
of that and the rest (the sward's light, the sky's, the air) 0.15. Its
cut (the fans' leaves, per pixel) is 0.07.

- **Lighting a far fan at its vertices lost.** Everything but the shadow
  is the same along a fan, so it was worked out at the vertices and handed
  to the pixels (7 more outputs a vertex), the sun's shadow still per
  pixel. Grass triangles are about 17 pixels, and the outputs cost more to
  set up than the lighting saved: ring 2 0.48 → 0.54, and rings 0 and 1,
  with no fans, 50% slower from the declared outputs alone. Per-blade
  values belong in a buffer read by the instance, not interpolated.
- **What is left is overdraw.** A fan can discard, so its pixels are
  depth-tested after they shade, nearer blades or not. Cutting it means a
  second pass over the ring's geometry (its depth, then its colour against
  it) or splitting the ring nearest-first and dropping quads behind the
  depth drawn so far (the retired forest's farDepth batches); either buys
  some 0.1–0.15 ms for that machinery, not worth it while every eye-level
  shot is under 2.0 ms.

The far rings and herbs stay as they are: they cost 0.2–0.25 ms
together.

### As built: the cover's far tier

Past the rings the turf alone was a hard line at eye level (a lush stand
of flowers, then flat ground a similar colour) and plastic from a
flyover. Extending the rings fixed the look (the ground's grain at
90–130 m 3.5 → 20) but cost 1 ms more, their cards and fans being dear a
pixel. So the cover goes on past them as a tier of its own
(`ground/scene_cover_far.glsl`, `mesh_cover_far.vert`, `cover_far.frag`):

- **An element a quad.** A cell of the herbs' grid (0.35 m) is the herb
  the world's module stands there (`herbStand`, §13), or a tuft of the
  sward where the grass grows; it
  is drawn as one quad at its root facing the eye, upright seen across
  the ground and a blob seen from above, cut to a tuft's blade tips or a
  herb's lobes with its blooms, lit as the sward it stands in.
- **As many as the screen holds, not the ground.** Each element stands
  for about 5×5 pixels of ground: at a ring distance where a cell is
  `cellPx` pixels and the eye meets the ground at sine `sinE`, the share
  kept is `cellPx² sinE / 25` and λ = −log4 of it. The thinning is nested
  (stochastic simplification): a level-k cell is 2^k herb cells a side
  and stands for its four children by one of them, by a hash, so the
  elements kept are spread a cell of the level apart, never clumped, and
  as the eye recedes one either stays (widened 2^λ, at most 8×, so the
  cover holds) or shrinks away over the last share of its level. The far
  herb ring keeps its plants by the same rule, so a plant there is the
  one the tier draws past it.
- **A draw a level** (10 levels): its cells are the tiles whose λ can
  fall in it (at least the ground's at the tile's nearest seen as face on
  as its height range allows, at most its farthest's seen edge on), run
  through the cover's plan like the rings (`coverFarLive`, the entry's
  slot its element's kind). It hands over from the far fans and the far
  herb ring by the complements of their dithers, and to the turf alone
  over 2.4–3 km.
- **Its detail no finer than a pixel or two.** An element is about 5
  pixels, so a tuft's tips, a herb's lobes' own shades and its bloom dots
  are a pixel or less: drawn as they are, they crawl as it moves over the
  pixels. Each fades to its mean as it nears that size (the tips to their
  mean height, the lobes to their mean shade, the blooms to their share
  of the pixel, lit in proportion), and its shade up its height, dark at
  the root, to its mean as it nears two pixels tall.
- **A patch, not a bar.** Widened to keep the cover, a tuft far off was
  ten pixels wide and one or two tall, its root dark: a slope seen across
  (the glade's far hillside, 150-250 m) was rows of dark dashes with the
  ground in slivers between. Its length up the screen is never under 0.4
  of its width, so the elements overlap into a mottled sward. The far
  ground's change a point a frame on the far pan is the same (9.83 against
  9.93), and so are the passes' costs.

Biomes (4090, 2560×1364): meadow 2.50 → 2.61 ms, edge 2.41 → 2.48,
above 0.85 → 1.17, flyover 1.03 → 1.26; the meadow's grain at 90–130 m
3.5 → 11.3. Walking on at 3 m/s (`capture.js -- motion advance base=meadow
far=1`, 60 frames at 30 fps), the ground's change a point a frame by
distance ahead, without the tier / with it: 60–100 m 7.3 / 8.3, 100–160 m
1.0 / 2.3, 160–300 m 2.2 / 2.2 (its detail drawn whole it was 9.7 and 3.1
nearer in); with nothing moving it is 0.002. Much of the 60–100 m change
is the far fans' own (6.3 without them). Still to come: where the far
fans thin, the turf between their clumps shows flat and about twice as
bright as they are; and the biomes' other covers (pebbles, litter, snow)
as kinds of element.

### As built: the sub-pixel resolve

A cut card keeps a part-covered pixel all or nothing by its dither
(`coverSampleMask`), so detail finer than a pixel (a far fan's leaves, a
herb's leaf or bloom a pixel or two across) averages to the right cover
in a still but flips frame to frame as it moves. MSAA settles it (4×:
60–100 m 8.3 → 5.2) at a full millisecond. Instead
(`post/pass_subpixel.h`):

- **The opaque scope marks it.** A second attachment (`SceneTargets::mark`,
  R8; the indirect light moved to the third) holds what each pixel is
  (`common/scene_subpixel_mark.glsl`): the ground the cards stand on, or a
  surface of its own with the share of it that was the dither's decision
  on detail too fine to show. That share is the card's own: a whole card
  under 2–4 pixels (a herb's by its length, so a stem is a line, not a
  speck; a far fan's by its leaves' width), or a part-covered pixel (an
  edge, a fine cut) of one up to a dozen across; the far tier's elements by
  their edges. A solid leaf, blade, bloom or trunk is 0 and stays crisp.
- **A 3×3 tent blends it.** After AO and before decals, SSR and the
  translucents, a marked pixel takes the tent of its neighbours as far as
  its share, a ground pixel as far as its most marked neighbour (what the
  dither left showing). Neighbours count only at about its own depth (within
  25–40%: the ground a few metres behind a far fan does, a crest over the
  next hill, a trunk before the meadow beyond, or the sky does not). It
  reads a copy of the colour (`PassSubpixelSnapshot` into `colorSnapshot`,
  which SSR's snapshot also uses) and the depth snapshot.

Walking on as above, 60–100 m 8.3 → 6.8, 100–160 m 2.3 → 2.0, 160–300 m
2.2 → 2.2; the near meadow's grain unchanged (a resolve on everything a
card marks, whole cards up to 16 pixels, went to 4.9 but blurred the
flowers within 30 m and the flyover's ground). The rest of the 60–100 m
change is real detail of a few pixels (the herbs' clumps) crawling. Cost
0.07–0.09 ms (copy 0.03, blend 0.04): meadow 2.67 → 2.75 ms.

## 10. Placement

A planet cannot store its plants (an Earth's is of the order of 10^12). A
cell's plants, herbs and rocks are made from its fields (biome, climate,
cover, slope, the terrain's age) when it comes within reach, deterministically
from the cell's key, as world-service jobs (as Decima places its vegetation),
and dropped past a hysteresis radius. The plants node takes the cells and the
rules (its species and what grows where), not a list of plants. The rules
are data, so a biome is a table, not code.

### As built: the plants' placement

- **Fields** are FastNoise2 node trees (`world/fields.h`): a script builds
  one with `FastNoise.create` and `set`, and `encode()` (brokit's binding
  mirrors every `set` into FastNoise2's NodeData) hands it to the engine,
  which decodes its own copy, safe to sample from any thread. The same tree
  sampled in script (`genSingle2D`) is the field the engine placed by, so
  the ground, the grass and the views read what the plants stand on. They
  run on the CPU (FastNoise2 is SIMD, not GPU), so placement is not a
  world-service job but a job for `world/world_workers.h`'s pool (half the
  hardware's threads, 1 to 8); that also leaves every plant's place known
  on the CPU, for physics and gameplay.
- **Rules** (`world/plant_placement.h`, JSON from `scene_plants.js`): layers
  of candidates on a grid fixed in the world, each draw its candidate's own
  hash (layer seed, i, j, draw), so a leaf's plants are the same however the
  ground is cut into leaves or how often it is placed. A `chance` field keeps
  a candidate; species are drawn over weight fields' running total; `scale`
  and `tint` fields with each plant's own variety; it stands on the `ground`
  field. (vegetation-api.js has the JS shape.)
- **Streaming** (`PlantPlacement`): the 64 m leaves within `range` of the
  eye are queued nearest first and kept posted to the pool as `queueMs` of
  each thread's time at the jobs' learned cost; a leaf past `range +
  hysteresis` is dropped. The queue is re-ranked when the eye crosses a
  leaf, looking only at the band where new leaves can enter.
- **The store** (`world/cell_store.h`): the node's GPU records are a prefix
  (header, species) and a pool of fixed-size records, each leaf's run
  anywhere in it (first fit, coalescing free runs, a run released held 4
  frames before reuse), with a log of the ranges written. The upload copies
  only those ranges; a full copy only when the pool grows. Hand-placed
  plants keep their own runs beside the placed ones.
- **Invalidation by cell:** the node keeps a log of changed boxes by
  generation (`changesSince`). The shadow pages redraw only the pages over
  a changed box within `shadowRange`; the probe grid remakes only the
  columns of each cascade over it (and those its shade falls on), its
  splat reading only the plants of the cells over them.
- **What was measured** (4090, 2560×1364): biomes' 1.4 km world, now its
  rules (its own fields as FastNoise trees), places 478 leaves (3,433 jobs
  over the 2 km range, 0.11 ms each) in 9 frames, 300 ms; its shots cost
  2.54 / 2.53 / 0.98 ms (glade, meadow, flyover). Over an unbounded world at
  the same density (2,692 leaves, 259k plants resident at a 2 km range),
  the main thread's own time is 0.25 ms a frame flown at 0, 300 or
  1,000 m/s for up to 10 km, the resident set flat; GPU 1.9 ms mean in
  flight against 2.2 for the same density hand-placed over the whole flight.
  The spikes that flight showed were the probe grid's whole-cascade remakes,
  since scrolled (§7's "As built"). tests/scene/test_plants.js flies 20 km and
  comes back; bro_plant_placement_test checks the draws, the streaming and
  the store.
- **Not yet:** herbs and rocks are still the grass node's own density
  functions, and the ground is biomes' mesh: both come to fields with the
  planet's terrain (step 9). FastNoise2's DistanceToPoint left at its
  default measures squared distance (the default its metadata now names,
  pinned at upstream's fix).

## 11. The planet (design; decisions taken, 9a to 9d's global grid built)

A body of Earth's radius (6371 km) seen from a person in its grass to
geostationary orbit as one place: the land's rings, its tiles, the plants
and their canopy, the cover, the air, the probe grid, the shadow pages and
the land's sun, all reading one state. ../planet-bro had the shape and
scale right and the look wrong; ../PLANET-LESSONS.md holds what it taught
(fields not objects, shape from process, the fine scale conditioned on the
coarse, lighting as the lens, ablation and reference renders from day
one, regeneration under a second). This section is what the engine adds
for it; the planet itself (its plates, belts, climate and biomes) is a
world's code (§13).

### What already holds, and what does not

Holds as it is: detail from pixels; the world service and its cut; the
air (`world/atmosphere.h` is one spherical model from the ground to space,
and draws the shell and the planet's disc from above); the canopy as the
land past the plants' range; placement per cell about the eye at a flat
cost; every clipmap about the eye (the cover's rings, the probe grid, the
shadow pages, the land's sun); reversed depth.

Does not: the land is a square plane `extent` each way, its rings reach
32 km, its tiles are keyed on the plane, and every position the engine
keeps (the plants' records, a node's transform, a field's input) is a
float in one world frame, good to millimetres out to 32 km and to half a
metre at 6371 km. Of those, the last is the one that shapes the design:
every other gap is a map from the plane to the sphere.

### The body and its place frame

- **A body** is the land's shape: a sphere of radius R about its centre,
  its spin axis and its day (the sun's direction at every point follows
  from them, so the terminator is where the planet's night begins, not a
  setting). A flat world is the body of infinite radius: one root, the
  identity map, the atmosphere's flat mode. It is the same path with the
  degenerate map, not a second one: everything below takes the body's map
  and nothing else.
- **The place frame.** Everything the GPU draws and the scene graph holds
  stays in float, in a frame anchored on the body near the eye: its origin
  a point on the datum, y its radial, x and z its tangents. The anchors lie
  on a lattice of the face cells 16 km on a side; when the eye's nadir
  leaves its anchor's cell by a margin, the frame re-anchors to the cell
  under it. The eye's position on the body, every anchor and every cell's
  origin are double on the CPU; the GPU never sees a number at 6371 km
  except as a direction, which a float carries to 0.6 m at the surface.
- **A re-anchor is a rigid move,** a translation and a tilt of at most
  0.2° (a diagonal step of the lattice, 23 km, over R). The engine moves what it owns: the scene graph's
  root nodes, Jolt's bodies, the plants' and placement's cell origins
  (their records are cell-relative already, §1), the cover's and the
  land's rings (re-centred every frame anyway). The eye-centred caches
  scroll: the probe grid's cascades take the move as a scroll and the
  tilt as error (0.45 m over a 128 m column, under its finest voxel),
  refreshed as they age. What lives in the body's frame does not move at
  all: the shadow pages and the land's sun are laid out in the body's
  light space, their page origins double, so a re-anchor redraws nothing.
  The app gets a `reanchor` event with the move, for positions it keeps
  in its own variables.
- **Gravity** is the world's rule (§13, 5): radial on a body, any strength,
  handed to Jolt, the sway and the plants' `build` in the place frame. At
  the plants' range a tree's radial is up to 0.3° off the frame's y, so
  each stands on its own radial, a direction, which a float carries
  exactly enough.

### As built: the body and its place frame (9a)

- **The CPU half** is `scene/world/place.h`: a `Body`, the lattice
  (`Place::kLattice` 16384 m, re-anchoring 2048 m past a cell's half), the
  cube's tan-warped faces (611 cells a side at Earth's radius), the place's
  origin and basis in double, `toBody` / `toPlace`, and the `PlaceMove` a
  re-anchor makes. `bro_place_test` walks 100 km on a flat body and over a
  face's edge and a cube's corner on Earth's (every frame orthonormal and on
  the datum, every tilt under 0.2°).
- **Two frames in the renderer.** `SceneView` carries the place's origin and
  `world()` gives the same camera in the body's frame: the same
  camera-relative matrices, its eye, view and frustum shifted. A frame holds
  both (`SceneFrame::view`, `::world`). Draws' bounds and lights are the
  place's; what is keyed on the world takes `frame.world`: the land's tiles,
  rings, horizon and sun, the cover's and the turf's tiles and the grass's
  push map, the plants' placement and GPU plan, the probe grid, the shadow
  pages, the atmosphere, the world service and the wind. The GPU sees one
  space, positions less the eye, and the camera block's `eyeWorld` is the
  body's eye, so a field or a hash a shader takes from it is the body's.
- **The re-anchor** runs first in `SceneGraph::tickAnimations`, before the
  frame's scripts, while physics is idle: the root nodes, the imperative
  camera, world-space particles and the physics world (`PhysicsWorld::move`:
  bodies, characters, area and world gravity, the interpolation snapshots)
  take the move, then the `reanchor` event fires. A flat body's origin is a
  whole number of cells, exact in float, so the frame after a re-anchor is
  bit for bit the frame before (`tests/scene/test_place_reanchor.js`, its
  shadow pages not redrawn), and biomes, with no body, is unchanged.
- **Not yet on the body:** on a sphere the keyed systems lie on the anchor's
  tangent plane (`placeOrigin` is zero there) until the land is on the body
  (9c); the legacy `createTerrain` and the tile world's shade maps are keyed
  on the place; navmesh worlds and tweens holding absolute positions are not
  moved (a script moves them with the event); the body's eye is a float on
  the GPU, good to centimetres a few hundred kilometres out, until 9c hands
  the shaders the split origin (9b). The sphere's walk and flight at speed are 9c's oracle.

### Fields at the body's scale

A field evaluated at a position 6371 km from the centre in float has half
a metre of resolution, and a lattice noise's octaves finer than a few
metres break on it (on the CPU too: FastNoise2 is float). So a position a
field is evaluated at is **split**: an integer part and a float fraction.

- The CPU hands each tile, placement cell and region its origin in double.
  For each frequency a tree uses, the origin times the frequency is split
  in double into a lattice integer (taken mod 2^32) and a remainder; the
  integer goes into the noise's hash (whose arithmetic is mod 2^32, so the
  result is exact), and only the remainder and the local offset are float.
- This is a change inside the noise on both sides: brokit's `noise_glsl`
  (its emitter knows every node's frequency chain) and FastNoise2's
  generators, which take the lattice offset (the wlejon fork, offered
  upstream). `bro_noise_glsl_test` checks the two agree at Earth-radius
  coordinates as they agree at the origin now.
- Domain warps add a small float displacement to the remainder, so they
  keep working.

### As built: split positions (9b)

- **FastNoise2** (wlejon's fork, now tracked like any sibling: bro and
  brokit resolve `../FastNoise2`, else its default branch's head) carries a
  position as `PosV`: the lanes' float offsets and an origin, a double
  shared by every lane. Scaling scales both, a per-lane displacement goes to
  the lanes, a rotation turns the origin in double; each lattice noise
  splits the origin at its own frequency into the cell it lies in (mod 2^32,
  added to its cells before the primes) and the fraction (added to the
  lanes), and the simplex family does so in its skewed coordinates. The
  grid and position-array entry points take double offsets as that origin,
  `GenSingle*` takes a double point as one. An origin of zero is the old
  arithmetic, bit for bit; a nonzero `GenSingle` point now takes the split
  path, so a placement decision on a threshold may fall the other way (biomes'
  meadow: the same look, a few flowers moved).
- **noise_glsl** threads the same origin through every node function as a
  double-float an axis (`FnO2` / `FnO3`, each axis `vec2(hi, lo)`), scaled
  and turned with the positions (Dekker's products and Knuth's sums, every
  step `precise`), split at each lattice noise as the CPU splits it. The
  host turns it on by defining `FN_ORIGIN2` / `FN_ORIGIN3` before the
  source; without them every helper passes the position through and costs
  nothing. Functions of the position itself (White, Gradient,
  DistanceToPoint) take it flattened to a float, exact only near zero.
- **The oracle:** `bro_noise_glsl_test` runs every node again about an
  origin on Earth's surface (4.1, -3.7, 2.9 thousand km): FastNoise2's
  batch with double offsets, its single value at the absolute point and
  the GLSL agree as at the origin (the land tree's worst 1.6e-4 in a range
  of 130 m; before the split the GPU's simplex was off by 3% everywhere).
- **Not yet in bro:** the scene's shaders still evaluate fields at a float
  `eyeWorld`-relative position about zero; 9c gives them the field origin
  (the place's, in double, as a uniform) and defines the macros.

### The land on the body

- **Its module's domain** is the point on the body's datum, `vec3 p` in
  the body's frame (the plane's (x, 0, z) on a flat body), and `LandPoint`
  gains the radial and the latitude and longitude from the spin axis, so
  climate belts and snow lines are the world's own GLSL. Noise on a sphere
  is 3D noise at p, which has no seam and no pole.
- **Tiles on a cube-sphere:** six face roots of the surface layers' key
  space (§1, §5), each a quadtree, the cube's faces tan-warped (the largest
  texel 1.4 times the smallest in area, against 5.2 for a plain cube). A
  tile's border is evaluated from the body's directions like its inside,
  so a tile on a face's edge filters across it with no seam. The page
  tables are three toroidal windows a level, given to the faces the level
  reaches about the eye (a fine level spans a few kilometres and meets at
  most three faces, at a corner); the coarse levels, where a face fits in
  256² tiles, are whole.
- **The rings** stay concentric squares of fixed topology parked on the
  eye, laid out in the azimuthal-equidistant map about the anchor (as
  planet-bro's charts were: it covers the body to the antipode, its worst
  stretch 36% across at the limb, where the ground is foreshortened anyway).
  A vertex finds its body direction, its face and its texel by its
  difference from its ring's centre, in closed form (the warp's difference
  through atan's subtraction rule), so no float carries 6371 km. The
  rings continue to about 20 levels (0.5 m to 260 km cells, the last
  reaching the hemisphere), and a level whose cells are under a pixel's
  ground from the eye's altitude is not drawn, as a geometry clipmap drops
  its fine levels when it climbs: 10 to 12 rings drawn at any height. A
  block past the body's horizon is culled before it is drawn.
- **The land's sun** follows the same ladder: its levels continue up to
  kilometre texels about the eye, the ones finer than the eye's pixels not
  made, so a mountain range at the terminator throws its shade seen from
  orbit.
- **Horizon culling** (§1) folds the curve into the land's horizon: the
  body's drop d²/2R joins each distance's slope, so from orbit most of the
  body is culled by its own curve and at eye level the far hills still are.

### As built: the land on the body (9c)

- **The chart and the map** are `scene/world/place.h`'s: `chartOf` gives a
  body direction's face and its face metres (R·atan of the tan-warped
  coordinate, continued past the face's edge), `dirOf` the way back;
  `Place::mapToLand` / `landToMap` are the azimuthal-equidistant map about
  the anchor in the place frame, a point at map ρ and height h standing at
  (R+h)·sinθ along the map's direction and h·cosθ − 2R·sin²(θ/2) up.
  `bro_place_test` checks both round trips over every face, edge and corner.
- **The tiles** are keyed (face, level, x, z): the surface layers take a
  `SurfaceMap` (`world/body_surface_map.h`) with six roots, a grid of page
  tables for each, the page image's layer root·20 + level. A tile's patch is
  hidden when the body's limb hides it from the eye (`beyondLimb`, its
  height range included). The fill (`land_fill.comp` with
  `ground/land_body.glsl`'s `LAND_ROUND`) takes each tile's face axes, its
  middle's chart point and the double-float origin R·n of its middle, and
  works out each texel's body point as the origin plus a difference made
  in float (R(d − d_c), no number at 6371 km), so the module's
  `landHeight(vec3 p)` runs with `FN_ORIGIN3` set to it: split positions
  (9b) in the scene's shaders. Normals come from the chart's slopes, signed
  by each face's handedness.
- **The module** is a 3D tree on a body: `scene_land.js` emits
  `landHeight(vec3)` beside the plane's `landHeight(vec2)` from the one
  FastNoise2 tree, and `LandPoint` gains `at` (the body point) and `up`
  (its radial). The CPU's heights (`heightAt`, `sampleHeights`, the JS
  `land.height`) answer in the place frame, the curve included, from
  FastNoise2 at the body point in double (brokit's `genSingle3D` passes
  doubles now); `tests/scene/test_land_body.js` holds them to a centimetre
  of the GPU's tree.
- **The rings** are laid out in the map about the eye's map point. A vertex
  finds its face and texel from the anchor's chart on each face (the CPU's
  doubles, in the land's block as `LD_FACE`) and atan's difference rule, so
  the GPU never takes the atan of a number at 6371 km; the place point is
  the map's closed form in a precise order. On a body the land adds rings
  until the coarsest reaches the antipode (`ringLevels`, up to 20), and the
  finest drawn is the first whose cells are a pixel of ground from the eye's
  height (`firstRing`): ring 0's grid is drawn as that ring, the ones
  between left out. A block wholly past the limb is not drawn.
- **The horizon and the sun** take the curve: the land's horizon reads the
  ground's place points (its drop included) and the land's sun subtracts
  the drop s²/2R from what stands at s, and stops climbing once the
  ceiling less the climb and the drop is under the best.
- **The world service's cut** grew with the eye's height: a move larger
  than the eye's height over the ground times the tangent of half the field
  of view is a cut, not one over 10 m, so a descent from orbit is a flight
  (the fixed 10 m called every frame from 100 km a cut, and each one spent
  the settle budget: 8–18 ms spikes every 15 frames).
- **The oracle:** `test_land_body.js` (heights against the tree, a constant
  ground seen from above and not from below, the body mid-frame from 10 km,
  400 km and 36,000 km, a cube corner on a level land without a hole, the
  horizon's dip at 20 km within 2.5 px). A descent over 900 frames from
  36,000 km to 2 m at 1280×682: median 2.38 ms, worst 3.8 ms (one frame at
  9,300 km, no world jobs in it), the ground's change a frame no more than
  2.5 times its neighbours'. Still frames at 2560×1364: 0.16 ms at 1.7 m,
  0.26 ms at 10 km, 0.36 ms at 400 km. Biomes (no body) unchanged but for
  9b's moved flowers and the fronds' wind phase.
- **The plants on the body** (`world/plant_placement.h`): placement's
  leaves and candidates are the chart's, a face's, a candidate kept by the
  face its point lies on and thinned by the chart's area there ((1+a²)(1+b²)
  / (1+a²+b²)^1.5 of a chart square metre is ground, a and b the tangents),
  its fields and ground read in 3D at its datum point split about its
  leaf's origin, its record relative to that origin; `PlantsNode` turns a
  leaf into the place frame as it arrives and moves every plant at a
  re-anchor (a rigid move of the store, every shadow page over them
  redrawn). Which leaves are in range is measured through the chart's
  metric at the eye (a chart metre across a face's edge is 0.71 of the
  ground's). The canopy module has a `vec3` overload the round fill calls
  at the datum point. `bro_plant_placement_test`: on an Earth-radius body
  across a face's edge, every plant on its ground, both faces' leaves,
  as many a square metre on the edge as either side, the range reached
  every way, the same plants with the frame anchored 30 km off, and each
  where it stood on the body after a re-anchor.
- **Not yet on the body:** the turf (still on the anchor's tangent plane;
  the cover's blades, herbs and far tier are on the body since, below);
  the land's sun is one level about the eye (its ladder to
  kilometre texels comes with the plants' canopy on the body); the module
  gets no latitude or longitude (a spin axis is the world's: `at` is enough
  to compute them).

### As built: processes, the global grid (9d)

- **The world writes** a `processes` option on `createLand`
  (`docs/land-api.js`): GLSL `source` holding `procInit` and its pass
  functions, `passes` [{fn, iterations}], a `resolution` (cells a root's
  side, up to 2048), 1 to 4 vec4 `channels` and up to 64 vec4s of `data`.
  The kernel's API is `land_process_layer.glsl`'s: `procRead(c, offset)`,
  `procHere`, `procPoint`/`procPointAt`/`procUp`, `procChart`, `procRoot`,
  `procCellSize`, `procIteration`, `procData`.
- **The grid** (`world/land_grid.h`): a flat body one root over the land's
  extent, a round body the six cube faces of the chart, each root N² cells
  with a one-cell halo. The halo is a table of copies made on the CPU: an
  edge cell takes the nearest cell of the face its centre lies on, a corner
  the mean of its face's corner and the two edge halos beside it (no step
  at a cube's corner). Bilinear sampling between cell centres, the same
  arithmetic in C++ (`LandGrid::sample`/`sampleDir`) and GLSL
  (`ground/land_grid.glsl`).
- **The run** (`vulkan/ground/scene_land_process.cpp`): rgba32f 2D-array
  images (layer root·C + channel) ping-ponged, `land_process.comp` in
  three modes (init, a pass, the halo), up to 128 passes a frame; then
  channel 0 is copied to a host buffer and, once its ticket is done, handed
  to the land as a shared, immutable `LandBase`. The pipeline is cached by
  the hash of the source, its dispatch switch and the grid.
- **One height, two readers:** the land's module is `landHeight(p) =
  landTree(p) + landGrid(0, p).x`, and the CPU's heights add `LandBase`'s
  sample to FastNoise2's tree. Until the base arrives the land is not
  ready: its heights are NaN, it offers no tiles and draws nothing, and the
  plants standing on it (`createPlants`' `land`) place nothing; placement's
  rules add the base to their ground. Every channel is the module's to read
  (`landGrid(c, p)`), so a process can colour the surface too.
- **Not yet:** regional grids under the eye streamed by the world service,
  with the global grid's flow at their edges; a process run again while
  the world plays (128 passes a frame is a hitch, fine at creation only).
- **The oracle:** `bro_land_grid_test` (halo sources and corners, the field
  through bilinear sampling within 2 m anywhere, no step across a face's
  edge or corner), `bro_plant_placement_test`'s base checks (flat and round),
  `tests/scene/test_land_process.js` (not ready then ready, procInit exact
  at cell centres and bilinear between, the GPU standing on the CPU's
  heights, a pass's iterations, plants held until ready, a second channel
  colouring the surface, a kernel that does not compile never ready, and on
  an Earth-radius body the field across a face's edge). An Earth-radius
  global grid of six 512² faces with a 3×3 pass is ready, wall clock and
  the pipeline's compile included, in 315 ms at 64 iterations and 344 ms at
  2048: the target's "under a second". The test also found the land's sun
  comparing datum heights to place heights on a round body (the ground dark
  8 km from the anchor); it now takes the drop.

### Processes

Shape comes from process (lessons 5 and 6): drainage that connects,
ridges that divide basins, coastlines from erosion. A height kernel
evaluated a texel at a time cannot make any of it, so a world may also
write **processes**: GLSL compute kernels the engine iterates over grids
of the body, whose results its height and surface read.

- **Two scales.** A global grid (six faces of 512², 12 km cells: plates,
  the isostatic datum, the belts, hypsometric sea level, global drainage
  and its erosion), made when the world is made, under a second (lesson
  8). Regional grids under the eye (a face cell of 64 km at 64 m), made
  ahead of the eye by the world service as a streaming job, each taking
  the global grid's flow at its edges as its boundary, so a river crossing
  a continent crosses the regions too.
- **The kernels are the world's;** the engine owns the grids, their keys,
  their borders and halos, their iteration and scheduling, and the
  sampler the land's module reads them through (`landGrid(name, p)`).
- **The CPU reads the same grids:** a grid is read back once when it is
  made (a region is 4 MB, made long before the eye arrives) and the CPU's
  heights sample it as the GPU does, then add the FastNoise2 detail. One
  source still, and nothing read back per query: §13's decision 2 kept,
  with grids added to trees.
- **Every term switchable from the host** (lesson 2): a process or a
  tree term off is a uniform, so an ablation run is one command.

### Going up

- **Plants and cover** need nothing new: their cells become face cells,
  their fields take split positions, and they stand on the land the
  place frame gives. Past the plants' range the canopy is the land (§5),
  which from orbit is the forest's tone and height in the tiles.
- **Space:** the air's shell and the planet's disc are drawn now; the
  stars, the moon and other bodies are step 12, and the level above the
  face roots (a system, its bodies, each body's faces) is as before: a
  body a few pixels across is a point of light, then a lit disc, then a
  body with its own roots, every body's origin eye-relative in double.
- **Clouds** are a volume (§13, 4; step 11), drawn from below and above
  as one deck. Not part of this step.

### What it costs

- **Eye level** is biomes' frame plus the ring's closed-form mapping (a
  few ALU in the land's vertex stage) and nothing else: the place frame
  is free per frame.
- **Orbit:** 10 to 12 rings, the air's tables, the tiles' fills while the
  view moves, no plants (the canopy is the land). Under 1.5 ms expected.
- **Descent** refines each tile one level as the altitude halves: from
  400 km to the ground in 30 s is nine halvings, about 260 tiles a second,
  under one a frame at 360 fps. The plants come in as their cells reach
  the plants' range.
- **The atlas** holds the view's tiles: from 400 km a pixel's ground is
  about 300 m, the view's ground 400–850 tiles of 64² (a texel 1.5 to 1
  pixel), inside the 1024 slots with little room; the slots grow, or the tiles grow to 128², with
  that measurement.

### As built: the cover on the body

- **Regions** (`world/cover_regions.h`): the cover is laid out on the cells
  of the place's lattice about the eye, each a REGION with its own flat
  frame, the place frame that would be anchored on it (its azimuthal map's
  x and z, the height over the datum). A flat body is one region, the
  identity. Every cover draw (a blade or herb ring, a far-tier level, the
  casters) belongs to one region: its cells are the region's, so a cell's
  hash and its blades are the same whichever cell the place is anchored on,
  and a re-anchor changes none of them.
- **The module's frame:** a sward or herb module works in its region's
  frame (`coverEye()`, `coverHeightAt(xz)`, `grassGround`), as on a flat
  world; the engine stands what it builds on the body (`coverToEye`: the
  curve's drop as a series in the distance over the radius, then the
  region's turn into the place frame; `coverTurn` for directions).
- **Who owns a point:** the region whose chart cell holds it
  (`coverOwns`, judged by `landChartOff`'s offset from the anchor's
  chart), in a 48 m band about the edge jittered per blade so no line
  shows; a region's cells stop at its reach, the cell plus the band.
- **The push map** keeps place coordinates and is moved with the place
  (`GrassNode::placeMoved`: each cell resampled through the move, its bend
  turned).
- **Not yet:** the turf (`groundSurface`'s layers are keyed on place xz:
  it takes the regions as its surface roots next); the wind's gusts are in
  region coordinates, a little off the trees' far from the anchor; a round
  land's `heightRange` is the whole body's, so tiles cull loosely.
- **The oracle:** `tests/scene/test_cover_body.js` on an Earth-radius
  body: the blades seen standing 9.5 km from the anchor (the ground 7 m
  under its tangent plane) as at it (0.851 against 0.853 of the view's
  lower half), a re-anchor moving 11 px of 25,600, and either side of a
  region's edge as thick as a region's middle (0.494 and 0.490 against
  0.528). On the plane biomes is unchanged: with the wind still, HEAD
  against this 60.8 dB at the glade, two runs of this 64.4 dB.

### Build order

Each lands as a measured step on its own; biomes stays the oracle for
every eye-level cost and must not move.

| Step | What | Win it should show | Size |
|---|---|---|---|
| 9a | The body and the place frame: a flat world as the infinite body, an Earth-radius body, re-anchoring | biomes byte-identical; on the sphere a 100 km walk and a 1,000 m/s flight re-anchor with no frame over 2.78 ms and no step in the ground's change a frame at a re-anchor | ~1.5k |
| 9b | Split positions in noise_glsl and FastNoise2 | the two agree at 6371 km as they do at the origin; heights there with the same detail as at the origin | ~0.8k |
| 9c | The land on the body: cube-sphere tiles, the rings to orbit, the land's sun's ladder, horizon culling | a flight from the grass to 36,000 km and back, every frame under 2.78 ms, no crack over a cube's corner, the ground's change a frame steady through the descent | ~2k |
| 9d | Processes: global and regional grids, read back for the CPU | a world's erosion over the global grid under a second; drainage continuous across regions | ~1.5k |
| 9e | demos/planet (broworkshop): continents by hypsometry, belts, climate, biomes as plant and cover modules chosen by climate | planet-bro's legibility targets: from orbit a range, a river system, a desert basin and a shelf; from eye level the same place; reference renders at every scale | world code |

### Decisions (taken: the recommended side of each)

1. **The place frame on a 16 km anchor lattice** (recommended: every
   cache already scrolls, and a re-anchor's tilt is under a voxel; at
   1,000 m/s it re-anchors every 16 s, at a walk every few hours), against
   a frame re-centred on the eye every frame (no tilt, but every cache in
   it moves continuously, and the shadow pages could not stay put).
2. **Split positions inside FastNoise2** (recommended: the noise stays the
   one library on both sides, exact; it puts the fork back in use), against
   evaluating fine octaves in region-local coordinates blended across
   region edges (four evaluations in the overlaps, and the overlap's
   contrast lower).
3. **A flat world is the infinite body** (recommended: one path, biomes
   and every flat game keep their look and costs), against every world on
   a sphere (biomes' land re-tuned for 3D noise and its oracles reset).
4. **Processes on the GPU, read back once a grid** (recommended: a world's
   kernels are GLSL like the rest of it), against processes in C++ on the
   CPU (fast enough for the global grid, but code a world cannot write).
5. **The cube's warp: tan** (recommended: simple, invertible in closed
   form, 1.4 area ratio), against equal-area maps (closer to 1.0, no
   closed-form difference for the rings).

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
  world/            cells, surface layers' paging, the world service's
                    policy, the shade map, the shadow plan, the wind field,
                    the atmosphere's parameters, the fields, the plants'
                    placement and store, the world's worker threads; later
                    the planet's roots
  vulkan/
    frame/          device, allocator, descriptors, pipelines, the shader
                    compiler, frame graph, targets, timers, GPU resources,
                    defaults, the view, the snapshot pass, the bridge
    draw/           the mesh drawer core and the draw list; meshes, decals,
                    particles, billboards, splats; custom mesh shaders
    world/          the cell pass, the surface layers' atlas, the world
                    service's executor and its pass
    ground/         the ground and its cover: the land, turf, grass, herbs
    foliage/        the plants node's plan, poses and draws; the far field
    lighting/       scene lighting, environment and sky, the atmosphere's
                    tables, reflection probes, the probe grid; later
                    volumetrics, clustered lights
    shadow/         the shadow pages: marked, drawn, copied into the pool
    post/           post-fx, exposure, SSAO, SSR, DOF
    terrain/        (planned) planet terrain tiles, horizons
    sky/            (planned) clouds, weather, stars, the moon
    shaders/        the same folders, and common/ for what every area
                    includes (camera, lighting,
                    atmosphere, push blocks, vegetation sway); every
                    #include is a path from shaders/
```

A folder is made when something lands in it; the planned ones above have
nothing yet.

## 13. Worlds as code (design, for review)

Who builds a world on bro is a coding agent, not a person at a tool. Code
is cheap for the author; what is expensive is getting the shared rules
right again in every world. So the split is: **the engine owns the
invariants every world needs; a world is code, written freely, behind a
few stable interfaces.** It is not data filled into a schema. A table of
species parameters, a list of presets, or an enum of kinds can only make
the worlds its designer imagined. An ocean world, a low-gravity world, or
plants shaped by buoyancy instead of wind need new code, not new numbers.
When something new crosses every world (light under water, say), the
engine changes. The line is where a shared rule lives, not "no engine
code".

Before this section, the worlds were the data kind. In demos/biomes, the
trees were a table of shape numbers (`createPlants({ species })`), the
herbs named presets (`'daisy'`, `'poppy'`), the grass a few JS callbacks
for its density, dryness and height evaluated on the CPU (the trees, the
herbs and the blades are modules now, below; the grass's density,
dryness, height, canopy and herbs are the land's surface module's).
The ground was a CPU mesh coloured vertex by vertex (it is the land now,
below). Only the fields were already code: the
placement rules are FastNoise node trees.

### What the engine owns

Every world gets these and none rewrites them:
- Detail from pixels (the one measure).
- Visibility and culling.
- Cells and eye-relative coordinates (§1).
- The world service and its async compute (§4, §6).
- Surface layers (§5).
- Lighting: the probe grid, the atmosphere, exposure, shadow pages (§7, §8).
- The cover's plan, rings and handovers, its far tier and its thinning, and the sub-pixel resolve (§9).
- Placement's machinery (§10).
- Streaming.
- Per-pass cost accounting.
- **The observability**, which for an agent is the real tooling (below).

### What a world writes

Each interface is code: GLSL compiled at run time, as custom shader
chunks already are (`scene_vk_custom_shader.cpp` splices them into
mesh.frag). It is called by the engine where its machinery needs the
world's answer. A module has full freedom inside its functions: its own
noise, its own structs, any maths.

1. **The land.** A height (and later overhang) kernel: the surface at a
   point, in the world's own frame (a plane now, a sphere's direction on
   the planet).
   - The world service runs it into tiles (§6), the land's rings draw from them, and placement and the cover read them.
   - The CPU (physics, gameplay) runs the same FastNoise tree with FastNoise2 (decision 2). Free GLSL heights with no tree would read tiles back instead.
2. **The surface.** What the ground is at a point: its albedo now; later its normal detail and wetness. Also what it looks like from afar with nothing standing on it: the colour the turf and the far summary fold to.
   - Called by the land's tile fill.
3. **Kinds of things that stand on it.** A grass sward, a herb, a tree, a pebble field, kelp. A module per kind, with these functions:
   - **`stand`**: what stands in a cell of its grid, read in the cover's plan and in placement's compute. Its chance, which variety, its size, its own record (any struct the module declares, decision 3).
   - **`build`**: its geometry at a detail the engine asks for, in pixels. Blades, cards, branches, whatever it is.
   - **`shade`**: what a fragment of it is: the surface it hands the lighting, and its cover of the pixel. The engine keeps a part cover by its dither and marks it for the sub-pixel resolve from the cover and the size the module reports.
   - **`far`**: what it is as one element of the far tier, and what it folds into the surface layer as.
   - **`respond`**: how it moves under the world's forces (wind, current, gravity), for the sway and for what touches it.
   - The engine decides everything around those calls: which rings exist, how many draws, the handovers, the thinning, shadows, the plan.
4. **Volumes.** Water, cloud, fog, dust: a density and surface function (waves, a cloud deck). The engine owns their marching, their lighting (consistent with the land's: the sun's transmittance through them, the probe grid under them), and their far form.
5. **The world's rules.** The gravity field (radial on a planet, any strength), the media (air and water: density, drag, buoyancy), the wind and current fields.
   - The engine hands these to the sway, particles, characters and vehicles (Jolt), and to every kind's `build` and `respond`.
   - So a tree's morphology can read gravity, and a frond can stand up in water.
6. **The sky's physics.** The atmosphere's scattering and absorption, its sun or suns. These are physical coefficients the model takes, not presets: an alien sky is different numbers through the same light.

### Observability, in the engine

The oracles that steered the cover live in demos/biomes/capture.js today:
- cost by pass;
- the ground's change by distance as the eye walks;
- grain and mottle by band;
- stills at fixed shots.

They move into the engine (a `scene.measure` surface), so any world gets
them without copying a script. An agent building a world then sees the
same things this work has been tuned on:
- a pass over budget;
- flicker at a distance;
- a bald band or a hard handover;
- a still before and after.

### How today's code moves onto it

A refactor, not a layer on top. Each existing node becomes the first
implementation of its interface, and its data form goes:
- **Grass**: done, the sward kind. The contract is `ground/scene_sward_layer.glsl` (swardRoot, swardDensity, swardBuild under SWARD_BUILD, swardShade under SWARD_SHADE, swardFarLook, swardBake, swardTurf, swardLay; the structs they pass; swardData(i) over the node's 64 vec4s). The engine's half, `ground/scene_grass.glsl`, keeps the rings, the thinning, the field and screen tests, the far herbs' blooms a far blade carries, and the turf's composition; mesh.frag keeps the dither, the sub-pixel mark and the light. The built-in shaders compile with `ground/sward_module.glsl`, a module of no blades. A grass node's module (`sward.source`, `sward.data`, `sward.height`, `sward.tallest`, `sward.reach`) is spliced in with its herb module, the pair one cover set of twelve stages (the blades' fragment and shadow stages joining the nine), the shadow casters drawing the blades with their set's. The node keeps only the rings' width, density and range. The meadow (leaves, dead leaves, culms, tussocks, the far fans and the turf's look of them) is broworkshop's `lib/sward` module. Biomes with it: the same look, the same cost (meadow 2.72 ms against 2.74). The ring table becoming the engine's rings asked for by pixel size is still to do.
- **Herbs**: done, the first module. The engine's half is `ground/scene_herb_layer.glsl` (the contract: herbStand, herbBuild, herbShade, herbFarMean, herbFarLocal, herbFarLook, the structs they pass, herbData) and `ground/scene_herb.glsl` (the cells, rings and hand-overs); the built-in shaders compile with `ground/herb_module.glsl`, a module of no herbs. A grass node's module (`herbs.source`, `herbs.data`) is spliced in for that file into the nine cover shaders that call it (`ground/scene_cover_shaders.h`: the plan, the blades' and herbs' vertex stages, the herbs' and the turf's fragment stages, the far tier's vertex stage, the turf layer's fill), compiled on as many threads, once a process, through the SPIR-V cache (docs/code-cache.md); one that does not compile is logged and draws no herbs. broflora's species (daisy, poppy, bracken, wood sorrel, ...) are broworkshop's `lib/herbs` module. Biomes with it: the same plants, the same look, the same cost (meadow 2.74 ms against 2.75).
- **Plants**: done, the tree kind. The contract is `foliage/scene_plant_layer.glsl` (plantElement under PLANT_BUILD, plantLeafPoint, plantLimb, plantSway, plantSurface under PLANT_SHADE; PlantElementIn, PlantElement, PlantLimb, PlantFragment, PlantSurface; plantData(s, i) over a species' 20 floats). The engine keeps a species' size, detail and crown as the light sees it (height, base, radius, full elements, element length, limbs, shape, density, mean albedo: the plan, the bounds, the crown's medium in plants.frag, the far casters' discs and the probe grid read these), and the placement, the GPU plan, the poses, the element polygon and tube, and the light. The module is spliced into the plants' five stages (`foliage/scene_plant_shaders.h`: the pose pass, the vertex stage, both fragment stages, the shadow casters' vertex stage); the cover's and the plants' sets are one generic `frame/scene_module_shaders.h`. A node with no module draws nothing; there is no built-in tree. The lobed crowns, limbs, needle tiers, a leaf's face and the sway are broworkshop's `lib/trees` (`treeModule`, `treeSpecies`). Biomes with it: the same forest (the near trees within a level of step 2's), the same cost (meadow 2.71 ms against 2.72, plants:leaves unchanged).
- **The far tier and the turf**: engine. They call each kind's `far` and the surface's far colour.
- **The land**: done, replacing the CPU mesh and the clipmap terrain. The contract is `ground/scene_land_layer.glsl` (`landHeight`, `landSurface`, LandPoint, LandSurface, `landData(i)` over 64 vec4s). `createLand` writes the module from the height's FastNoise tree, emitted as GLSL by brokit's `noise_glsl` (`node.glsl()`: FastNoise2's nodes to float rounding, `bro_noise_glsl_test`), and the world's surface GLSL. The engine's half: a land node's tiles are a surface layer set (heights R32F, albedo RGBA16F) made by `ground/land_fill.comp` with the module (`ground/scene_land_shaders.h`), offered to the world service as the `land` kind ahead of the turf; its rings (camera-centred, fixed topology, the clipmap's crack-free maths) are placed on the tiles in `ground/mesh_land.vert`, a vertex's height a function of its position and its distance from the frame's eye, and shaded by mesh.frag, or by a grass node's turf where it is turf. The rings are drawn in blocks of 32 x 32 cells placed about the frame's eye each frame, so every pass (the camera's, a probe's, a turf tile's fill, a shadow page's) draws the blocks its view reaches; the land casts into the shadow pages with them (`ShadowLandVert`, the same stage built as a caster). The CPU's heights, raycasts and a grass node's bounds run the tree with FastNoise2. The surface also says what grows (`LandSurface.cover`: the sward's density, dryness and height multiplier and the canopy; `LandSurface.herbs`), kept in a third tile layer; a grass node stands on a land (`createGrass({ ground: land })`) and reads its heights, surface and cover from the tiles on the GPU (`grassGround` in `ground/scene_grass.glsl`, the land's page and words in the grass's set 4), so the grass reaches as far as the land does and its CPU-baked field, floor texture and per-tile density culling are gone.

A second, very different world proves the interfaces: one of the two below,
built with no engine change beyond what it shows to be shared.

**An ocean world.**
- The land kernel lies mostly under the sea level, and the water is a volume with its waves and absorption.
- Kelp is a kind whose `build` stands it up by buoyancy and whose `respond` follows the current, not the wind.
- The light under the surface is the engine's (it would be the shared change this world brings in).

**A low-gravity world.**
- The world's gravity is 0.3 g.
- The tree kind's `build` reads it and grows taller and thinner crowns.
- The sway's period lengthens, particles and characters fall slowly, and dust hangs in the air volume.
- None of it is a flag the engine knows about.

### Decisions for review

1. **The modules' language.** GLSL compiled at run time, as custom shader
   chunks are now (recommended: the engine already compiles it, and every
   function runs where the engine needs it).
   - The alternative is a JS-hosted language compiled to SPIR-V. It is more work for no gain an agent needs.
2. **The land on the CPU.** Decided: the height is a FastNoise node tree,
   emitted as GLSL for the GPU (brokit's `noise_glsl`, no node FastNoise2
   has not got, checked against FastNoise2 itself) and run by FastNoise2 on
   the CPU. One source, exact on both sides, nothing read back.
   - Free GLSL heights (no tree) would read the GPU's tiles back a frame later, asking ahead around what moves.
3. **A kind's record.** A module declares its per-instance struct in GLSL, and the engine reflects its layout (glslang gives it), sizing the plan's buffers.
   - The alternative is a fixed slot of N vec4s. It is simpler, but it is a schema again.
4. **Size.** Grass, herbs, plants, the ground and the observability onto these interfaces is roughly 6–9k lines changed. A second world on them is a few thousand of its own.

Each step lands as a measured win on its own: interleaved binaries on
demos/biomes (and, once it exists, the planet), the stills' numeric diff,
the motion oracle for anything temporal. Each step removes what it replaces.

| Step | What | Win it should show | Size |
|---|---|---|---|
| 1 | Done: the old forest path retired (ForestNode, its drawers, planner, impostors, HiZ; the plants' CPU planner) | one foliage path, the plants node planned on the GPU; biomes' stills identical, GPU ms 7–11% lower | −17k |
| 2 | Done: the layout (§12), moves only | frames byte-identical, suites green | moves |
| 3 | Done: the world service (§6), the surface layers' fills its first producer | biomes' stills byte-identical, GPU ms unchanged; a cut glade→flyover settles in 4 frames (24 fills a frame took 24), flyover→glade in 12 (took 38), no frame over 2.78 ms | +1.4k |
| 4 | Done: atmosphere tables and exposure (§7) | one sky from ground to orbit: the LUT path within 7% of a reference march by day (14% in a clear day's haze), 0.13 ms where the per-pixel march took 0.61; metered exposure in EV, a sunlit field at middle grey; biomes on it under a clear day's air, 0.10–0.24 ms over its HDR panorama, its sky bands gone | +3.1k |
| 5a | Done: cached shadow pages (§8) | plant shadows off the per-frame cost: away from eye level 0.17–0.21 ms less than the atlas, the canopy's dappled shadows at a texel a pixel; at eye level 0.08–0.19 ms more (the lookup), and a turning sun's pages built behind the shown ones | +2.5k |
| 6 | Done: the ground and its cover as one surface (§9), the cover planned on the GPU and drawn before the ground | the cover's hidden work gone: 0.36–0.42 ms off every eye-level shot, all of them under 2.0 ms; what is left is the far fans' lighting and overdraw, not worth its machinery yet | +0.5k |
| 7 | Done: the probe grid (§7), with the canopy's density it traces against | forests lit as places: a crown's shade within 2% of a CPU march, the open sky within 1%, the ground's bounce within 1.5%; biomes' forest 5–11% darker under its crowns, the meadow unchanged; +0.25–0.4 ms a frame at a 30 Hz clock (a refresh slice nearly every frame), no frame over 2.78 ms, no refresh step in motion | +2.9k |
| 8 | Done: placement per cell (§10), its fields FastNoise2 trees placed on worker threads, the plants' store paged by cell | a world of any size placed around the eye at a flat cost: 0.25 ms of the main thread a frame at 0 to 1,000 m/s, the resident set flat over 20 km; biomes placed in 9 frames; shadow pages and probe cascades redone only over the cells that changed | +1.7k |
| 9 | The planet (§11, steps 9a–9e there), with the terrain's height tiles and their horizons (§7) | ground to orbit, one place, the frame following the pixels; the sun's shadow and the sky's occlusion past the pages | ~6k |
| 10 | Far-field cell cards (§2) | far plants and terrain cost by the cells on screen | ~4k |
| 11 | Volumetric light and clouds (§7) | shafts, mist, weather from every altitude | ~5k |
| 12 | Night and many lights (§7) | moon, stars, cities from orbit, fires | ~2k |

The horizons were step 5b. The land's horizon for culling came with the
land's height tiles (§13); the per-tile horizons that shadow and occlude
the sky past the pages land with step 9. The canopy's density landed with the probes that read it (step 7).

The visibility pass (§3) joins when a measurement shows foliage shading, not
its count, is what the frame is spent on.
