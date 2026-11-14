// ============================================================================
// bro.tiles: tile caches filled on demand by world code
// ============================================================================
//
// A tile cache holds a world's data (heights, materials, densities, anything
// a kernel computes) in GPU memory as a forest of quadtrees over a domain,
// and keeps the tiles the view needs resident. The engine owns *which* tiles
// exist (selection, residency, eviction, the draw set); world code owns
// *what* is in them, through one callback, `produce(batch)`, which typically
// dispatches bro.kernel kernels (kernel-api.js) over the whole batch at once:
// a grid of [cells, cells, batch.count] whose z is the tile's record in the
// batch table, its slot where in the layer pools it goes. One dispatch over
// the batch fills the GPU; a dispatch a tile is a few warps of it, and
// flying the planet demo, 24 tiles a frame took 5x the GPU time that way.
//
// - Selection is error-driven: a tile splits while its cells project larger
//   than `pixelsPerCell` pixels at the camera, down to `maxLevel`, within
//   `capacity` resident tiles (the tile projecting largest splits first).
//   With `detail`, also while the error its producer says splitting it
//   would take away projects larger than `pixelsPerDetail`: tiles go where
//   the world has something to show (a rough range, a coast) and flat sea
//   or plain stays coarse, `pixelsPerCell` then only bounding how coarse.
// - A tile is produced after its parent, in a later batch, and can read it:
//   its record names the parent's slot and the quarter of it the tile
//   covers, so a child refines what its parent holds rather than replacing
//   it (a child whose 2x2 cells average to its parent's cell is the same
//   world seen closer).
// - Production is spread over updates: at most `producePerUpdate` tiles a
//   call, paced by the frames (below), the largest projected error first (a
//   parent before its children), the four children of a parent together, so
//   the draw set refines quad by quad as they land.
// - Production runs between the frames: what produce records goes to the
//   background stream (VulkanCompute::beginBackground), held until the
//   frame's GPU work is all submitted, then run on the GPU's async compute
//   queue (in order on the graphics queue where the device has none) after
//   it: in the gap the frame leaves before the next. A batch's tiles are
//   ready, drawn, in the lookup and copied into textures filled from a
//   pool, at the first update after the GPU has done it, usually the next.
//   A tile's production is a few small dependent dispatches, far from
//   filling the GPU: in the frame's own queue it lengthened the frame by
//   all of it (a lone planet tile, 0.33 ms, pushed most frames of a fast
//   turn past 2.78 ms). produce must give its kernels only buffers nothing
//   rewrites until the batch is ready (the batch's own, the pools,
//   constants), as the GPU may still be reading them a frame later.
// - Batches pace themselves by the milliseconds that gap holds (the frame
//   target, 2.78 ms at 360 fps, less the scene's newest render and a
//   margin, and never under a seventh of the target, so a view heavy
//   because it is still coarse refines anyway) and what the cache's tiles
//   cost, timed batch by batch on the GPU and learned by level (a
//   producer's coarse tiles often cost more: each cell averages more
//   ground). The caches share the gap in proportion to the work each has
//   pending, and each spends its share on whole tiles in their order,
//   carrying the rest to the next update, so a share under a tile's cost
//   still produces one every few frames. Until a batch is timed, half of
//   producePerUpdate a batch; without a scene, producePerUpdate. A tile
//   dearer than the gap still runs past it when its turn comes.
// - Tiles the view no longer wants are evicted when a slot is needed, the
//   finest first (of equals, the least recently wanted), and produced again
//   if wanted again: coarse tiles are few, wanted again first, and dearest
//   to produce.
// - The draw set covers every root exactly once: a wanted tile whose
//   children are not all resident yet draws itself.
//
// Present with bro.kernel (BRO_WITH_KERNEL), on the main thread, and needs
// its GPU (`bro.kernel.gpu`): createCache throws without one.

// ============================================================================
// 1. bro.tiles.createCache(options) -> TileCache
// ============================================================================
//
// options:
//   domain            { type: 'plane', origin: [x, z], size, roots: [nx, nz] }
//                       an xz plane of nx * nz square roots of edge `size`
//                       from `origin` (root r at column r % nx, row r / nx;
//                       a tile's x runs along +x, its y along +z); nx * nz <= 255
//                     { type: 'cubeSphere', radius, center: [x, y, z] }
//                       a cube projected onto a sphere: six roots, faces
//                       0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z
//   tileSize          cells along a tile's edge (default 64)
//   border            extra cells on every side (default 0); a tile holds
//                     cells = tileSize + 2 * border along an edge, so a
//                     stencil can read its neighbours without a seam
//   layers            { name: bytesPerCell, ... }: one GPU pool per layer,
//                     4 to 4096 bytes a cell (a multiple of 4); a cell may
//                     hold a block of finer texels, so data finer than the
//                     cells costs no more tiles. The pool must still
//                     allocate, and a node binding it reads it whole
//                     (maxStorageBufferRange, 4 GiB on desktop GPUs)
//   capacity          resident tiles (default 256): each layer's pool is
//                     capacity * cells * cells * bytesPerCell bytes
//   maxLevel          deepest level (default 16, at most 24)
//   maxHeight         how far above the base surface data reaches (default
//                     0), widening each tile's bounds for selection
//   minHeight         how far below it data reaches (default the lesser of 0
//                     and maxHeight; at most maxHeight). On a cube-sphere the
//                     horizon test lowers the surface to radius + minHeight
//   ranges            true gives the cache a `ranges` store for producers to
//                     report the heights each tile actually spans (default
//                     false). A tile without a report takes its nearest
//                     reported ancestor's, else [minHeight, maxHeight]
//   pixelsPerCell     a tile splits while a cell spans more pixels than this
//                     (default 2)
//   detail            true gives the cache a `detail` store for producers to
//                     report what splitting each tile would take away
//                     (default false), and selection by it
//   pixelsPerDetail   with `detail`: a tile also splits while its detail
//                     projects larger than this many pixels (default 1)
//   producePerUpdate  tiles produced per update() (default 8): the most a
//                     batch holds, as paced by the frames
//   produce(batch)    fills the update's batch; `this` is the cache
//
// batch (the argument to produce):
//   count               its tiles, 1 to producePerUpdate
//   layers              string[]: the layer names to produce for this batch
//                       (all layers normally, or the subset passed to
//                       cache.invalidate(layers) during a selective refresh)
//   hasLayer(name)      true when `name` is in `batch.layers`
//   table               KernelBuffer of `count` records of 36 words for
//                       kernels (GPU code is f32), record i at word 36 * i,
//                       read as nine vec4<i32> by tiles::record (section 3):
//                         int32 0 slot, 1 parentSlot (-1 for a root), 2 root,
//                         3 level, 4 x, 5 y, 6 quadrant x, 7 quadrant y;
//                         float32 8-10 anchor, 11 extent, 12-14 lost,
//                         15 radius, 16-18 frame u, 20-22 frame v,
//                         24-26 dirHi, 28-30 dirLo, 32-34 point (19, 23,
//                         27, 31, 35 are 0).
//                       The tile is anchored at its frame's centre rounded
//                       to f32 (`anchor`, base coordinates), and every other
//                       float places its cells about it: `lost` the centre
//                       less the anchor, so base(s, t) = anchor + lost +
//                       (s - 0.5) u + (t - 0.5) v; dirHi + dirLo the base
//                       surface's normal at the anchor (a cube-sphere's
//                       normalize(anchor); a plane's (0, 1, 0)); `point` the
//                       anchor's base surface point less the tile's center.
//                       A cell's offset from the anchor is small, so f32
//                       holds it at any scale where its absolute position
//                       would not: tiles::sphere_vertex gives a cube-sphere
//                       vertex relative to the center. Work that needs an
//                       absolute position (a noise lattice's cell) is done
//                       on the CPU in f64 at the tile's `anchor` (below) and
//                       handed to the kernel. Written before produce is
//                       called, it lands before the producer's dispatches;
//                       the same KernelBuffer every update
//   tiles               the same tiles as objects, for the CPU:
//     root, level, x, y   which tile: x and y in [0, 2^level) within its root
//     slot                its index in the layer pools: its cells are
//                         elements slot * cells * cells on, laid out [x, y]
//                         at y * cells + x, so a kernel over the batch writes
//                         pool[x, y, slot] into the pool as a 3D store
//     cells, border, tileSize
//     origin              [x, y, z] of its cell grid's (0, 0) corner on the
//                         base surface (border excluded)
//     extent              its edge length on the base surface
//     frame               { corner, u, v }: the tile as a parallelogram in
//                         base coordinates, base(s, t) = corner + s * u + t * v
//                         for (s, t) in [0, 1]^2 across its interior. A
//                         plane's base coordinates are the world's; a
//                         cube-sphere's are the cube's ([-1, 1]^3), and the
//                         world point is center + normalize(base) * radius
//     anchor              { base, dir }: the table's anchor and dirHi, f32
//                         values: CPU work in f64 about the tile (a noise
//                         lattice's cell at the anchor's point) starts from
//                         these, so it lines up with the kernel's offsets
//     center, radius      the tile's bounding sphere (the one cache.drawn
//                         reports): a kernel that writes positions relative
//                         to `center` keeps them small enough for f32. The
//                         center is the tile's on the base surface and never
//                         moves; a narrower height range only shrinks radius
//     parent              null for a root, else { slot, quadrant: [qx, qy] }:
//                         the parent's slot (produced in an earlier batch,
//                         which the GPU runs first, ready or not yet, and
//                         resident while the child is produced),
//                         and which quarter of it this tile covers: the
//                         child's interior starts quadrant * tileSize / 2
//                         cells into the parent's interior
//
// With `ranges`, a producer writes a tile's [lo, hi] into cache.ranges at
// float32 2 * slot: the heights (above the base surface) the tile spans. It
// must bound the surface at this level and at every finer level, since the
// tile's children inherit it until they report their own. A batch's slots
// read NaN until written; NaN (or lo > hi) is no report. The cache reads
// them back once the GPU is done, without waiting, and selection narrows a
// frame or two after the tile is produced.
//
// With `detail`, a producer writes a tile's detail into cache.detail at
// float32 slot: the size, in domain units, of the error splitting it would
// take away from what it draws (how far its children's surface leaves its
// own, say: on self-similar ground the tile's own cells tell, as the rms of
// its odd vertices against the even ones about them, scaled by the share the
// next level keeps). It is read back as ranges
// are (NaN or negative: no report). A tile without a report takes half its
// nearest reported ancestor's a level, and one with none splits by its
// cells alone, so a fresh region refines a level or two ahead of its reports.
//
// A produce that throws leaves the batch missing; update() rethrows, and
// later updates offer it again.

// Roots from a function of the plane; children their parent plus detail
// that sums to zero over each 2x2 block. One kernel, the whole batch.
const mod = bro.kernel.compile(`
    import tiles;
    @kernel @grid(3)
    fn k_produce(g: grid<3>, table: read_store<vec4<i32>, 1>, pool: rw_store<f32, 3>, cells: f32) {
        let rec = tiles::record(table, g.z());
        let fx = g.x() as f32;
        let fy = g.y() as f32;
        if (rec.parent < 0) {
            // A root's plane spans 2048 m: its absolute coordinates are small.
            let b = rec.anchor + tiles::frame_offset(rec, (fx + 0.5) / cells, (fy + 0.5) / cells);
            pool[g.x(), g.y(), rec.slot] = sin(b.x * 0.11) * cos(b.z * 0.07) * 4.0;
            return;
        }
        let half = cells * 0.5;
        let px = floor(fx * 0.5);
        let py = floor(fy * 0.5);
        let sx = (fx - 2.0 * px) * 2.0 - 1.0;
        let sy = (fy - 2.0 * py) * 2.0 - 1.0;
        let p = pool[((rec.quadrant.x as f32) * half + px) as i32, ((rec.quadrant.y as f32) * half + py) as i32, rec.parent];
        let amp = 8.0 / exp2(rec.level as f32);
        pool[g.x(), g.y(), rec.slot] = p + amp * sin(px * 12.9898 + py * 78.233) * sx * sy;
    }
`, { file: 'terrain.bk', modules: { tiles: bro.tiles.module } });

const terrain = bro.tiles.createCache({
    domain: { type: 'plane', origin: [0, 0], size: 4096, roots: [2, 2] },
    tileSize: 64,
    layers: { height: 4 },
    capacity: 512,
    maxLevel: 12,
    maxHeight: 300,
    produce(batch) {
        mod.kernel('k_produce').dispatch([this.cells, this.cells, batch.count], batch.table,
                                         this.layer('height'), this.cells);
    },
});

// ============================================================================
// 2. TileCache
// ============================================================================
//
// cache.update({ position: [x, y, z], fov = 60, viewportHeight = 1080,
//                forward, up = [0, 1, 0], viewportWidth }) -> number
//   Selects the tiles this view wants, produces up to producePerUpdate of
//   the missing ones (calling produce), refreshes the draw set and draw
//   list, and returns how many it produced. Call it once a frame.
//   With `forward` (the camera's view direction; `up` its up, viewportWidth
//   / viewportHeight its aspect) the view is a frustum: tiles outside it,
//   and on a cube-sphere tiles whose top lies below the horizon of the
//   surface lowered by minHeight, are neither refined nor drawn, so the
//   capacity goes to what is seen. Without it every tile in range refines
//   and is drawn. A tile's distance for refinement is to its height slab, so
//   tight ranges (`ranges`) refine what is near and stop refining what only
//   the worst-case height would have brought near.
//   focus: [{ position: [x, y, z], radius, cell }]: places wanted whatever
//   the view sees, such as the ground a simulation's bodies stand on. A
//   tile whose bounds come within `radius` of `position` is seen (never
//   culled, so it is in the draw set) and splits while its cells are
//   larger than `cell`, as well as by the view's rule. Its splits come
//   before the view's (in the capacity and in production order); the view's
//   tiles take what capacity is left.
//
// cache.drawn       the draw set: [{ root, level, x, y, slot, parentSlot,
//                   coarser: [u0, u1, v0, v1], center: [x, y, z], radius,
//                   range: [lo, hi] }], each tile's bounding sphere and the
//                   height range it is selected by; parentSlot is -1 for a
//                   root. coarser: how many levels coarser the drawn tile
//                   across each edge (u = 0, u = 1, v = 0, v = 1) is, 0 where
//                   it is as fine or finer (or none is drawn): a draw that
//                   lays that edge's vertices on the line between its every
//                   2^k-th (the neighbour's own vertices) meets it without a
//                   crack, across a cube-sphere's faces too. Without a
//                   frustum it covers every root once; with one, less the
//                   tiles it culled. Nearest the view's position first (by
//                   the nearest point of the bounding sphere), so a draw in
//                   list order lets the depth test reject hidden ground
//                   before it shades.
// cache.drawCount   drawn.length
// cache.drawList    KernelBuffer of drawCount records of 16 words, the draw
//                   set on the GPU for a shader or kernel to walk: int32
//                   slot, parentSlot, root, level, x, y, coarser (4 bits an
//                   edge, u = 0 lowest, then u = 1, v = 0, v = 1), 0, then
//                   float32
//                   centerHi.xyz, radius, centerLo.xyz, 0, the bounding
//                   sphere's center split hi + lo. A frame needs nothing
//                   else from the CPU: draw drawCount instances, each
//                   reading record gl_InstanceIndex (`buffer ivec4 u_tiles[]`,
//                   four ivec4 a record, intBitsToFloat for the floats), and
//                   place it at (centerHi - camHi) + (centerLo - camLo) with
//                   the camera split the same way, a camera-relative position
//                   good to f64 at any distance.
//                   `drawn` builds an object a tile: for inspection, not for
//                   every frame.
// cache.drawnNear([x, y, z], reach)
//                   the tiles of `drawn`, in its order, whose footprint on
//                   the base surface (the bounding sphere less the height
//                   range) comes within `reach` of the point: the ground
//                   about a body, a few objects where `drawn` builds
//                   thousands.
// cache.layer(name) the layer's whole pool as a KernelBuffer: producers
//                   write it (a batch's tiles at their slots), and a scene
//                   node's custom shader binds it (setShaderBuffer, scene-
//                   nodes-api.js) and reads tile `slot` at slot * cells * cells,
//                   or binds it as a texture array, a slot a part
//                   (setShaderTexture({ buffer, width: cells, height: cells,
//                   layers: capacity, atlas, channels, format }); atlas k
//                   packs k x k slots a layer, as a capacity past the GPU's
//                   array layers needs) for one filtered fetch at
//                   tiles::atlas_uvw; each update's produced slots are
//                   logged on the pool, so only those are copied
// cache.lookup      KernelBuffer: every ready tile's slot by (root, level, x,
//                   y), a hash table rewritten once an update's batch is
//                   produced (after its dispatches, so a tile is found once
//                   its cells are there, and a slot taken from an evicted
//                   tile is gone from it in the same write). Pass it to
//                   tiles.bk (bro.tiles.module) to read the cache at any
//                   point rather than tile by tile: a ray, a kernel over
//                   other data, a volume's hook (setShaderBuffer, read as
//                   read_buffer<vec4<i32>>; a kernel takes
//                   read_store<vec4<i32>, 1>). A record's parent slot
//                   rides with it, so a read blending a tile with its
//                   parent (tiles::parent_of) costs one search
// cache.ranges      with `ranges`: KernelBuffer of capacity * 2 float32, a
//                   tile's reported [lo, hi] at 2 * slot; else null
// cache.detail      with `detail`: KernelBuffer of capacity float32, a tile's
//                   reported detail at slot; else null
// cache.pending     wanted tiles not yet produced or out of date (0 once
//                   settled); every
//                   live cache's, and its range batches in flight, report
//                   to perf.pendingWork / perf.settle as `tiles`
// cache.wanted      tiles the last update selected
// cache.resident    tiles holding data
// cache.cells, cache.capacity
// cache.stats       { produced, evicted } since creation, and reportsInFlight:
//                   batches whose ranges or detail the GPU has not handed
//                   back yet;
//                   batchSize, the most the next batch holds; tileMs, a
//                   produced tile's GPU ms (-1 before a batch is timed),
//                   and levelMs, by level (-1 where none was timed);
//                   batchMs, a batch's own ms whatever it holds (its
//                   queue's latency): a batch's span fitted as batchMs +
//                   tiles x their ms over the batches timed, so a batch of
//                   few tiles does not charge it to them. The refinement's
//                   budget pays the tiles' ms. A batch whose span ended in a
//                   later frame than it began (its produce waited, pumped,
//                   while frames went by) is not timed.
// cache.clear()     drops every tile; the next updates produce them again
//                   (after the world's source changed)
// cache.destroy()   releases its GPU memory (every pool, the draw list, the
//                   lookup, ranges, detail) once the work in flight is done,
//                   rather than when the cache is collected: a cache made
//                   over again (a new setting) frees the old one's at once.
//                   Any later use of the cache or its KernelBuffers throws
// cache.refresh(layers?) / cache.invalidate(layers?)
//                   every ready tile is out of date (what produce reads has
//                   changed: a sun, a parameter) but stays ready, drawn and
//                   in the lookup; the next updates hand the wanted ones to
//                   produce again, into their own slots, after the missing
//                   tiles and ranked as they are, a parent's batch before
//                   its children's (a child refined from its parent reads
//                   the new parent), so the view keeps its detail while it
//                   changes over. When `layers` (a layer name or array of
//                   layer names, e.g. `['color']`) is given, `batch.layers`
//                   and `batch.hasLayer(name)` indicate only those layers for
//                   refresh batches, and only those layers' GPU pool slices
//                   are marked dirty for texture array uploads while other
//                   layers and height ranges remain intact.
// cache.stale       wanted tiles out of date since a refresh (in pending)

function frame(camera, patch) {
    terrain.update({ position: camera.position, forward: camera.forward, up: camera.up,
                     fov: camera.fov, viewportWidth: 1920, viewportHeight: 1080 });
    // patch: an instanced mesh whose shader reads u_heights and u_tiles
    // (bound once: patch.setShaderBuffer('u_heights', terrain.layer('height'));
    // patch.setShaderBuffer('u_tiles', terrain.drawList)).
    if (patch.instanceCount !== terrain.drawCount) patch.setInstanceCount(terrain.drawCount);
    const hi = camera.position.map(Math.fround);
    patch.setShaderUniform('u_camHi', hi);
    patch.setShaderUniform('u_camLo', camera.position.map((v, i) => v - hi[i]));
}

// ============================================================================
// 3. bro.tiles.module: tiles.bk
// ============================================================================
//
// The brokernel module (src/tilecache/tiles.bk) that reads a cache at a
// point. Scene shader hooks (`createMesh`, `createVolume`, `setShaderModule`)
// pre-register `tiles` automatically so they can `import tiles;` without
// passing `modules: { tiles: bro.tiles.module }`. Standalone `bro.kernel.compile`
// callers can pass `{ modules: { tiles: bro.tiles.module } }` or register it
// via `bro.kernel.registerModule('tiles', bro.tiles.module)`.
// Its table argument is the cache's `lookup`, taken by any container of
// vec4<i32>.
//
//   tiles::cube_point(dir) -> Point { root, u, v }   a cube-sphere's root
//       under a unit direction and (u, v) in [0, 1] across it, the cache's
//       own projection
//   tiles::plane_point(x, z, origin, size, roots) -> Point   a plane's
//   tiles::locate(lookup, p, level) -> Tile { slot, level, x, y, parent }
//       the finest ready tile holding p at `level` or coarser (slot -1:
//       none of its root is ready), and its parent's slot (-1 unless
//       ready); one search a level tried, nearly always one load
//   tiles::parent_of(tile) -> Tile   its parent, from the slot it carries
//   tiles::find(lookup, root, level, x, y) -> slot or -1
//   tiles::probe(lookup, root, level, x, y) -> vec2<i32>(slot, parent)
//   tiles::cells(tile, p, tileSize, border) -> Cells { i00, i10, i01, i11,
//       f }   the four cells around p as indices into a layer's pool and
//       the bilinear weights: interior cell i is centred at (i + 0.5) /
//       tileSize across the tile; with a border of 1 or more filtering
//       crosses tiles without a seam, without one the outermost cells hold
//   tiles::atlas_uvw(tile, p, tileSize, border, atlas) -> vec3<f32>   where
//       p is in a texture array bound from a layer's pool (cache.layer
//       above): (u, v, layer), the tile's borders included, so one filtered
//       sample there gives what cells() would
//   tiles::level_for(w, w0, maxLevel) -> f32   the level whose cells are w
//       across where level 0's are w0, fractional (blend two, as mips)
//
// And a produce(batch)'s table (section 1), taken by any container of
// vec4<i32> (a kernel's read_store<vec4<i32>, 1>):
//
//   tiles::record(table, i) -> Record { slot, parent, root, level, x, y,
//       quadrant, anchor, extent, lost, radius, u, v, dir_hi, dir_lo, point }
//       batch record i (tiles::RECORD_VEC4S vec4s a record)
//   tiles::frame_offset(rec, s, t) -> vec3<f32>   base(s, t) less the anchor
//   tiles::sphere_offset(rec, d) -> vec3<f32>   on a cube-sphere,
//       normalize(anchor + d) less normalize(anchor) for a base offset d,
//       good to f32 of the offset (no difference of large numbers)
//   tiles::sphere_vertex(rec, d, radius, h) -> vec3<f32>   the surface point
//       h above the base at offset d, less the tile's center: a vertex
//
// A cube-sphere point's (u, v) is f32: good to a millionth of a face, about
// level 14 of a 64-cell tile.

const near = bro.kernel.compile(`
    import tiles;
    @kernel @grid(1)
    fn k_sample(g: grid<1>, lookup: read_store<vec4<i32>, 1>, heights: read_store<f32, 1>,
                dirs: read_store<f32, 1>, out: write_store<f32, 1>) {
        let n = g.x();
        let p = tiles::cube_point(vec3<f32>(dirs[3 * n], dirs[3 * n + 1], dirs[3 * n + 2]));
        let t = tiles::locate(lookup, p, 12);
        let c = tiles::cells(t, p, 64, 1);
        out[n] = mix(mix(heights[c.i00], heights[c.i10], c.f.x), mix(heights[c.i01], heights[c.i11], c.f.x), c.f.y);
    }
`, { file: 'sample.bk', modules: { tiles: bro.tiles.module } });

// Errors: options that are not an object, an unknown domain type or a
// produce that is not a function throw a TypeError; out-of-range integers
// or layer sizes a RangeError; no GPU device an Error. layer() of an unknown
// name throws a RangeError, update() without a position a TypeError.
