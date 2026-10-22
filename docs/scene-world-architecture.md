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

### As built

- **The policy** (`world/world_service.h`, CPU only, unit-tested by
  `bro_world_service_test`) holds the kinds (a name, raster or compute, a
  first guess at a job's cost), takes each frame's offers (key, producer,
  error in pixels, size, required) and grants them: required jobs first,
  then by error, ties to the larger tile, then by kind, producer and key, so
  the order never depends on who offered first. A job is granted while the
  frame's spending plus its kind's cost fits the budget; a kind that does
  not fit leaves the rest of its jobs, and a cheaper kind's may still fit.
- **The budget** is the target less the most the last four timed frames
  needed outside the service (each frame's GPU ms less its service spans on
  the graphics queue; spans on the async queue are not subtracted), floored
  at 0.1 ms. A frame over the target, or no timed frame yet, gives the floor.
  The target is a scene setting, `scene.setFrameTarget(ms)`, 2.78 ms by
  default. A kind's cost per job is its span's ms over its jobs: the first
  measurement replaces the guess, later ones move it by a quarter.
- **A cut.** The slack a view left says nothing of the next view after a
  jump, and a frame or two of timings is read late. So when the eye moves
  more than 10 m or the view turns more than half its vertical field of view
  between two renders, the service forgets its slack (the floor until the
  new view is measured), and the frames rendered before the cut count only
  for the kinds' costs.
- **The executor** (`vulkan/world/scene_world_service.h`, run by the
  `world-service` pass at the frame's head) feeds the policy the GPU
  timer's frames, collects the producers' offers, and runs each kind's
  granted jobs inside one span of the timer (`SceneGpuTimer::beginSpan`,
  stamped always, with the jobs it held). A raster kind records into the
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
  columns a move brings in are fresh boxes (required, traced afresh), cut
  into slices' rows so every job costs about a slice's; the rest refresh a
  slice (2 rows) at a time.
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
  the probes' spacing there (`sceneProbeSpacing`), a quarter for the leaves'
  crowding where they resolve it.
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
- **Drawing pages.** A needed page that isn't drawn, or is stale, is a job
  for the world service (§6). Its error is what shows meanwhile: 2^k px for
  a drawn ancestor k levels up, 1 px when stale. The top of a needed chain
  is required, and the base pages covering the view are drawn at once.
  There are two kinds of job: pages the plants draw into (a sun page past
  the sway level) and pages only what stands still draws into. In biomes
  the first costs about 0.012 ms a page and the second 0.007, mostly its
  copy. A kind's GPU timer span runs in segments, its draw and its copy,
  so its price is what it costs.
- **What moves** is drawn each frame into overlays of the needed pages it
  touches, sampled in the scratch target. Plants and grass sway in the sway
  level's overlays (8 mm texels), which the finer levels read; past it their
  sway is under a texel and they sit in the cached depth.
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
  of the pixel, lit in proportion).

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
| 9 | The planet (§11), with the terrain's height tiles and their horizons (§7) | ground to orbit, one place, the frame following the pixels; the sun's shadow and the sky's occlusion past the pages | ~5k |
| 10 | Far-field cell cards (§2) | far plants and terrain cost by the cells on screen | ~4k |
| 11 | Volumetric light and clouds (§7) | shafts, mist, weather from every altitude | ~5k |
| 12 | Night and many lights (§7) | moon, stars, cities from orbit, fires | ~2k |

The horizons were step 5b. They need the terrain's height tiles, which no
step before 9 makes, and biomes has nothing past the pages to show them on
(its ground is a 1.4 km mesh, the pages' window 262 km), so they land with
step 9. The canopy's density landed with the probes that read it (step 7).

The visibility pass (§3) joins when a measurement shows foliage shading, not
its count, is what the frame is spent on.
