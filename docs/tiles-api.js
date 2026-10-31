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
// - A tile is produced after its parent, in a later batch, and can read it:
//   its record names the parent's slot and the quarter of it the tile
//   covers, so a child refines what its parent holds rather than replacing
//   it (a child whose 2x2 cells average to its parent's cell is the same
//   world seen closer).
// - Production is spread over updates: at most `producePerUpdate` tiles a
//   call, paced by the frames (below), the largest projected error first (a
//   parent before its children), the four children of a parent together, so
//   the draw set refines quad by quad as they land.
// - Production runs beside the frames: what produce records goes to the
//   GPU's async compute queue (VulkanCompute::beginBackground; in order on
//   the graphics queue where the device has none), after the frames
//   submitted before it, beside the ones after. A batch's tiles are ready,
//   drawn, in the lookup and copied into textures filled from a pool, at the
//   first update after the GPU has done it, usually the next. A tile's
//   production is a few small dependent dispatches, far from filling the
//   GPU: in the frame's own queue it lengthened the frame by all of it (a
//   lone planet tile, 0.33 ms, pushed most frames of a fast turn past 2.78
//   ms), where beside it costs the frame only what it slows its work by.
//   produce must give its kernels only buffers nothing rewrites until the
//   batch is ready (the batch's own, the pools, constants), as the GPU may
//   still be reading them a frame later.
// - The batch size paces itself by the scenes' renders: one tile more each
//   update while the newest finished render is under 90% of its frame
//   target (2.78 ms, 360 fps), half as many once one is over it, at least
//   a quarter of producePerUpdate (a view still coarse is often what makes
//   the frames heavy, and starving it keeps it so). A batch's own GPU time
//   says nothing of its cost beside the frames
//   (sharing the GPU stretches it: a lone planet tile read 1 to 6 ms beside
//   a render), what it slows their renders by does. Without a scene the
//   batches grow to producePerUpdate.
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
//                     4 to 256 bytes a cell (a multiple of 4)
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
//   producePerUpdate  tiles produced per update() (default 8): the most a
//                     batch holds, as paced by the frames
//   produce(batch)    fills the update's batch; `this` is the cache
//
// batch (the argument to produce):
//   count               its tiles, 1 to producePerUpdate
//   table               KernelBuffer of `count` records of 24 float64
//                       (integers exact) for kernels, record i at 24 * i:
//                         0 slot, 1 parentSlot (-1 for a root), 2 root,
//                         3 level, 4 x, 5 y, 6 quadrant x, 7 quadrant y,
//                         8-10 frame corner, 11-13 frame u, 14-16 frame v,
//                         17-19 center, 20 extent, 21 radius, 22-23 0
//                       (the tile fields below). Written before produce is
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
//                         world point is center + normalize(base) * radius. A
//                         kernel given these in f64 places each cell exactly
//                         at any scale.
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
// A produce that throws leaves the batch missing; update() rethrows, and
// later updates offer it again.

// Roots from a function of the plane; children their parent plus detail
// that sums to zero over each 2x2 block. One kernel, the whole batch.
const mod = bro.kernel.compile(`
    @kernel @grid(3)
    fn k_produce(g: grid<3>, table: read_store<f64, 1>, pool: rw_store<f32, 3>, cells: f32) {
        let r = g.z() * 24;
        let slot = table[r] as i32;
        let parent = table[r + 1] as i32;
        let fx = g.x() as f32;
        let fy = g.y() as f32;
        if (parent < 0) {
            let step = (table[r + 20] as f32) / cells;
            let x = (table[r + 8] as f32) + (fx + 0.5) * step;
            let z = (table[r + 10] as f32) + (fy + 0.5) * step;
            pool[g.x(), g.y(), slot] = sin(x * 0.11) * cos(z * 0.07) * 4.0;
            return;
        }
        let half = cells * 0.5;
        let px = floor(fx * 0.5);
        let py = floor(fy * 0.5);
        let sx = (fx - 2.0 * px) * 2.0 - 1.0;
        let sy = (fy - 2.0 * py) * 2.0 - 1.0;
        let p = pool[((table[r + 6] as f32) * half + px) as i32, ((table[r + 7] as f32) * half + py) as i32, parent];
        let amp = 8.0 / exp2(table[r + 3] as f32);
        pool[g.x(), g.y(), slot] = p + amp * sin(px * 12.9898 + py * 78.233) * sx * sy;
    }
`, { file: 'terrain.bk' });

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
//
// cache.drawn       the draw set: [{ root, level, x, y, slot, parentSlot,
//                   center: [x, y, z], radius, range: [lo, hi] }], each
//                   tile's bounding sphere and the height range it is
//                   selected by; parentSlot is -1 for a root. Without a
//                   frustum it covers every root once; with one, less the
//                   tiles it culled. Nearest the view's position first (by
//                   the nearest point of the bounding sphere), so a draw in
//                   list order lets the depth test reject hidden ground
//                   before it shades.
// cache.drawCount   drawn.length
// cache.drawList    KernelBuffer of drawCount records of 16 words, the draw
//                   set on the GPU for a shader or kernel to walk: int32
//                   slot, parentSlot, root, level, x, y, 0, 0, then float32
//                   centerHi.xyz, radius, centerLo.xyz, 0, the bounding
//                   sphere's center split hi + lo. A frame needs nothing
//                   else from the CPU: draw drawCount instances, each
//                   reading record gl_InstanceIndex (`buffer ivec4 u_tiles[]`,
//                   four ivec4 a record, intBitsToFloat for the floats), and
//                   place it at (centerHi - camHi) + (centerLo - camLo) with
//                   the camera split the same way, a camera-relative position
//                   good to f64 at any distance (demos/planet in broworkshop).
//                   `drawn` builds an object a tile: for inspection, not for
//                   every frame.
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
// cache.pending     wanted tiles not yet produced (0 once settled); every
//                   live cache's, and its range batches in flight, report
//                   to perf.pendingWork / perf.settle as `tiles`
// cache.wanted      tiles the last update selected
// cache.resident    tiles holding data
// cache.cells, cache.capacity
// cache.stats       { produced, evicted } since creation, and rangesInFlight:
//                   batches whose ranges the GPU has not handed back yet;
//                   batchSize, the most the next batch holds (as paced)
// cache.clear()     drops every tile; the next updates produce them again
//                   (after the world's source changed)

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
// point, registered under the name `tiles` by whoever imports it:
// bro.kernel.compile(src, { modules: { tiles: bro.tiles.module } }), or a
// volume's createVolume({ module, modules: { tiles: bro.tiles.module } }).
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
