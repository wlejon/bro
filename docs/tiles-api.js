// ============================================================================
// bro.tiles: tile caches filled on demand by world code
// ============================================================================
//
// A tile cache holds a world's data (heights, materials, densities, anything
// a kernel computes) in GPU memory as a forest of quadtrees over a domain,
// and keeps the tiles the view needs resident. The engine owns *which* tiles
// exist (selection, residency, eviction, the draw set); world code owns
// *what* is in them, through one callback, `produce(tile)`, which typically
// dispatches a bro.kernel kernel (kernel-api.js) into the tile's buffers.
//
// - Selection is error-driven: a tile splits while its cells project larger
//   than `pixelsPerCell` pixels at the camera, down to `maxLevel`, within
//   `capacity` resident tiles (the tile projecting largest splits first).
// - A tile is produced after its parent and can read it: `tile.parent`
//   carries the parent's buffers, so a child refines what its parent holds
//   rather than replacing it (a child whose 2x2 cells average to its
//   parent's cell is the same world seen closer).
// - Production is spread over updates: at most `producePerUpdate` tiles a
//   call, parents before children, coarse before fine.
// - Tiles the view no longer wants are evicted least recently wanted first
//   when a slot is needed, and produced again if wanted again.
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
//   ranges            true gives each produce a `tile.range` to report the
//                     heights the tile actually spans (default false). A
//                     tile without a report takes its nearest reported
//                     ancestor's, else [minHeight, maxHeight]
//   pixelsPerCell     a tile splits while a cell spans more pixels than this
//                     (default 2)
//   producePerUpdate  tiles produced per update() (default 8)
//   produce(tile)     fills one tile; `this` is the cache
//
// tile (the argument to produce):
//   root, level, x, y   which tile: x and y in [0, 2^level) within its root
//   slot                its index in the layer pools
//   cells, border, tileSize
//   origin              [x, y, z] of its cell grid's (0, 0) corner on the
//                       base surface (border excluded)
//   extent              its edge length on the base surface
//   frame               { corner, u, v }: the tile as a parallelogram in base
//                       coordinates, base(s, t) = corner + s * u + t * v for
//                       (s, t) in [0, 1]^2 across its interior. A plane's base
//                       coordinates are the world's; a cube-sphere's are the
//                       cube's ([-1, 1]^3), and the world point is
//                       center + normalize(base) * radius. A kernel given
//                       these in f64 places each cell exactly at any scale.
//   center, radius      the tile's bounding sphere (the one cache.drawn
//                       reports): a kernel that writes positions relative
//                       to `center` keeps them small enough for f32. The
//                       center is the tile's on the base surface and never
//                       moves; a narrower height range only shrinks radius
//   range               with `ranges`: a KernelBuffer of 2 float32, [lo, hi],
//                       for a kernel to write the heights (above the base
//                       surface) this tile spans. It must bound the surface
//                       at this level and at every finer level, since the
//                       tile's children inherit it until they report their
//                       own. It reads NaN until written; NaN (or lo > hi) is
//                       no report. The cache reads it back once the GPU is
//                       done, without waiting, and selection narrows a frame
//                       or two after the tile is produced
//   layers              { name: KernelBuffer }: this tile's range of each
//                       pool, cells * cells elements laid out [x, y] at
//                       y * cells + x, a store bounded to the tile; the same
//                       object (and views) every time that slot is produced
//   parent              null for a root, else { slot, layers, quadrant: [qx, qy] }:
//                       the parent's buffers (resident while the child is
//                       produced), and which quarter of it this tile covers:
//                       the child's interior starts quadrant * tileSize / 2
//                       cells into the parent's interior
//
// A produce that throws leaves that tile and the rest of the update's batch
// missing; update() rethrows, and later updates offer them again.

const mod = bro.kernel.compile(`
    @kernel @grid(2)
    fn k_root(g: grid<2>, out: write_store<f32, 2>, ox: f32, oz: f32, step: f32) {
        let x = ox + (g.x() as f32 + 0.5) * step;
        let z = oz + (g.y() as f32 + 0.5) * step;
        out[g.x(), g.y()] = sin(x * 0.11) * cos(z * 0.07) * 4.0;
    }

    // The parent's cell plus detail that sums to zero over each 2x2 block.
    @kernel @grid(2)
    fn k_refine(g: grid<2>, parent: read_store<f32, 2>, out: write_store<f32, 2>,
                qx: f32, qy: f32, half: f32, amp: f32) {
        let fx = g.x() as f32;
        let fy = g.y() as f32;
        let px = floor(fx * 0.5);
        let py = floor(fy * 0.5);
        let sx = (fx - 2.0 * px) * 2.0 - 1.0;
        let sy = (fy - 2.0 * py) * 2.0 - 1.0;
        let p = parent[(qx * half + px) as i32, (qy * half + py) as i32];
        out[g.x(), g.y()] = p + amp * sin(px * 12.9898 + py * 78.233) * sx * sy;
    }
`, { file: 'terrain.bk' });

const terrain = bro.tiles.createCache({
    domain: { type: 'plane', origin: [0, 0], size: 4096, roots: [2, 2] },
    tileSize: 64,
    layers: { height: 4 },
    capacity: 512,
    maxLevel: 12,
    maxHeight: 300,
    produce(tile) {
        const cells = [tile.cells, tile.cells];
        if (!tile.parent) {
            mod.kernel('k_root').dispatch(cells, tile.layers.height,
                tile.origin[0], tile.origin[2], tile.extent / tile.cells);
        } else {
            const [qx, qy] = tile.parent.quadrant;
            mod.kernel('k_refine').dispatch(cells, tile.parent.layers.height, tile.layers.height,
                qx, qy, tile.cells / 2, 8 / (1 << tile.level));
        }
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
//                   tiles it culled.
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
// cache.layer(name) the layer's whole pool as a KernelBuffer: a scene
//                   node's custom shader binds it (setShaderBuffer, scene-
//                   nodes-api.js) and reads tile `slot` at slot * cells * cells
// cache.pending     wanted tiles not yet produced (0 once settled)
// cache.wanted      tiles the last update selected
// cache.resident    tiles holding data
// cache.cells, cache.capacity
// cache.stats       { produced, evicted } since creation
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

// Errors: options that are not an object, an unknown domain type or a
// produce that is not a function throw a TypeError; out-of-range integers
// or layer sizes a RangeError; no GPU device an Error. layer() of an unknown
// name throws a RangeError, update() without a position a TypeError.
