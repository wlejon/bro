# Scene world architecture: cells, far field, visibility, async compute, surface layers

Five engine mechanisms for big worlds at high frame rates, from the ground up
to orbit. Each one is general:
terrain, forest, grass and static meshes all build on them, so none is shaped
around a single consumer. They share one rule: **no stage waits for another
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

Where the frame goes today (wilds, GPU ms; the details are in the git log):

- **Foliage fragments (crowns and clumps):** 2–4.5 ms on the forest routes.
  Crowns and clumps are ray-traced or discarding cards with no early-Z, drawn
  with overdraw. Fixed by the visibility pass (§3) and the far field (§2).
- **Work over every tree, every frame:** the planner (0.6–0.8 ms) plus the
  casters it feeds. Its cost grows with the world, not the screen. Fixed by
  world cells (§1).
- **Latency-bound compute:** about 90 small dependent dispatches leave the GPU
  mostly idle. Fixed by async compute (§4).

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
  cell. Terrain gives a min/max height for each cell; the forest gives its
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
- the forest planner measures only trees in near and mid cells;
- static chunks draw by cell;
- far cells go to the far field (§2).

### Per-system data

- **Forest:** instances are sorted by leaf cell once, at setInstances, and
  each cell holds a contiguous range of them. The planner's Measure stage
  walks those cell ranges instead of all instances.
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
cell. The forest draws its normal tiered output, terrain its patch, statics
their chunks. Nothing about a card is baked, and nothing is authored per
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
   - a sub-element: the clump run hit, the leaf, the triangle.

   Crowns still trace here, to find their hit, but write only depth and ID.
   No lighting, shadow lookups or colour.
2. **Resolve.** A full-screen pass, split into tiles by content kind,
   rebuilds each pixel's surface from its ID and shades it once:
   - crowns re-trace only the one run that was hit;
   - cards just sample.

   Overdraw then costs only the trace, never the shading.

**Occlusion culling, two-phase:**
1. Draw last frame's visible cells and instances.
2. Build the hierarchical depth.
3. Test everything else against it.
4. Draw what is newly visible.

The hierarchical depth also feeds the cell pass (§1). This replaces the
forest's hand-made far-depth batches with a general mechanism that rejects
whole trees, chunks and grass tiles before they rasterise.

**As built, for the forest's sprite cards (pass_hiz.h):**
- The opaque batches hold back each card the last frame's pyramid hides
  (reprojected through the eye's move) in the vertex stage, and list it
  with its rect in this frame.
- After the last batch, PassHiZ builds this frame's pyramid from the
  opaque depth, tests the list against it and gathers what it does not
  hide into an indirect draw for each sprite draw. The retest draws only
  those cards.
- A budget pass at the frame's head counts each draw's cards (the GPU
  planners' commands included), so every card held back has room in the
  list.
- Frames are byte-identical with occlusion on and off.

The win is the crowns behind terrain and nearer trees: wilds totals with
occlusion on against off were vista −0.93 ms, treetop −0.32, walk −0.1,
cruise 0, orbit +0.04 (the pyramid and the far depth it is built from).
The clumps barely move. Their overdraw is among visible overlapping cards,
which is the visibility pass's to remove.

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
- **Overlap:** a frame's compute work, such as the forest plan and the cell
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
past their rings, and, from far above, the forest and everything else on
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
  (`SurfaceLayers`, `scene/surface_layers.h`); nothing is read back from
  the GPU, unlike feedback-driven virtual texturing. A tile's heights, the
  slope its footprint sees and its version come from the provider's items
  (for the turf, each mesh's triangles in 32 m cells), gathered into a
  min/max pyramid so a tile reads a few cells. The atlas's slots bound the
  set, the tile most too coarse refined first: in wilds the footprint rule
  alone asks for 4–10× the 1024 slots on hilly, forested routes, since a
  frustum hides no ground behind hills or trees. Occlusion (§3's HiZ) is
  what would close that gap.
- **Lookup:** for each level, a toroidal grid of entries (256 a side), a
  clipmap of page tables, maps a position to its tile; an entry carries
  its tile's slot and the high bits of its key, so a tile that wraps onto
  another's entry is told apart and the lookup falls through. A fragment
  takes its level from its footprint, blends between that level and the
  next coarser one, and falls back to a coarser level when a tile is not
  resident. The coarsest level is always resident.

### Filling

- A layer has a **provider**: it fills a tile from the world's data (the
  grass field, the floor texture, the ground's vertex data, the forest's
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

## Build order

Each step must land as a measured win on its own (wilds, interleaved
binaries, the stills' per-band diff, the motion oracle for anything temporal).

| Step | Mechanism | Win it should show | Size |
|---|---|---|---|
| 1 | Async compute (§4) | plan time hidden: about 0.4–0.7 ms on every route | ~2k LOC |
| 2 | World cells (§1), cell-range planning, frustum only | plan cost tracks visible cells, not the world; a 4× larger world costs about the same | ~3k LOC |
| 3 | Visibility pass for foliage (§3) and HiZ two-phase culling, which then joins the cell pass | foliage shaded once per pixel, hidden trees never raster | ~4k LOC |
| 4 | Far-field cell cards (§2) | far forest and terrain cost proportional to cells on screen, not trees | ~4k LOC |
| 5 | Surface layers (§5): tiles, page-table clipmap, budgeted fills; the turf as the first provider | the ground's opaque:static down by most of its 0.5–0.7 ms on the open routes | ~3k LOC |
| 6 | Planet roots: cube-sphere cells, horizon culling, content folding into layers below its last representation | ground to orbit with the frame's cost tracking pixels | to size once 2–5 land |

Step 5 needs only step 2's keys and cell pass, so it can be built beside
steps 3 and 4.

Going up brings things in as well as folding them away: the atmosphere
seen as a shell, space, other bodies. They are providers like any other,
refined by the pixels they cover: a planet a few pixels across is a point
of light, then a lit disc, then a body with its own face roots, as a tree
goes from a layer's texels to a card to a mesh. Step 6 therefore adds a
level above the face roots (a system, its bodies, each body's six faces),
with every body's origin, like every cell's, taken eye-relative in double.
