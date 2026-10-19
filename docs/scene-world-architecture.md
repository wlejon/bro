# Scene world architecture: cells, far field, visibility, async compute

Four engine mechanisms for big worlds at high frame rates. Each one is general:
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
- finite worlds up to about 64 km a side. Floats in camera-relative drawing
  hold millimetres out to 32 km. Past that, cell-local coordinates are the
  path (§1).

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

A quadtree over the ground plane that every system's content hangs off.

- **Cell keys** are integer coordinates (level, x, z), never floats.
- **Leaves** are a fixed size, 64 m by default.
- **Cell bounds** take their y range from whatever registers content in the
  cell. Terrain gives a min/max height for each cell; the forest gives its
  crowns' tops.

A system registers a **cell content provider**:
- its content's bounds for each cell;
- a draw entry that renders that content for any view, the main camera or a
  far-field capture (§2);
- a generation number, bumped whenever the content changes.

### The per-frame cell pass

A small GPU pass walks the tree top-down. It tests each cell against:
- the frustum;
- the hierarchical depth from the visibility pass (§3);
- each shadow tile's frustum.

Each surviving cell is given a band by its projected size: near, mid or far.

The pass writes compact lists of visible cells for each band and each view.
Systems consume those lists instead of their whole content:
- the forest planner measures only trees in near and mid cells;
- static chunks draw by cell;
- far cells go to the far field (§2).

### Per-system data

- **Forest:** instances are sorted by leaf cell once, at setInstances, and
  each cell holds a contiguous range of them. The planner's Measure stage
  walks those cell ranges instead of all instances.
- **Terrain:** the clipmap already costs the same whatever the world's size,
  so it keeps its rings. It joins the cells as the source of their heights and
  as a provider for far-field captures.
- **Grass:** already ring cells around the eye. Its tiles map onto leaf
  cells, so the cell pass's occlusion result can reject them.

### Precision and streaming

- **Precision past 32 km:** a cell's content can be stored relative to the
  cell's origin. The cell pass hands each cell to the GPU as an eye-relative
  origin, computed in double on the CPU. Optional per provider.
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

## Build order

Each step must land as a measured win on its own (wilds, interleaved
binaries, the stills' per-band diff, the motion oracle for anything temporal).

| Step | Mechanism | Win it should show | Size |
|---|---|---|---|
| 1 | Async compute (§4) | plan time hidden: about 0.4–0.7 ms on every route | ~2k LOC |
| 2 | World cells (§1), cell-range planning, frustum only | plan cost tracks visible cells, not the world; a 4× larger world costs about the same | ~3k LOC |
| 3 | Visibility pass for foliage (§3) and HiZ two-phase culling, which then joins the cell pass | foliage shaded once per pixel, hidden trees never raster | ~4k LOC |
| 4 | Far-field cell cards (§2) | far forest and terrain cost proportional to cells on screen, not trees | ~4k LOC |
