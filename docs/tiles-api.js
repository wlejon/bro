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
//   layers              { name: KernelBuffer }: this tile's range of each
//                       pool, cells * cells elements laid out [x, y] at
//                       y * cells + x, a store bounded to the tile
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
// cache.update({ position: [x, y, z], fov = 60, viewportHeight = 1080 }) -> number
//   Selects the tiles this view wants, produces up to producePerUpdate of
//   the missing ones (calling produce), refreshes the draw set and draw
//   list, and returns how many it produced. Call it once a frame.
//
// cache.drawn       the draw set: [{ root, level, x, y, slot, parentSlot,
//                   center: [x, y, z], radius }], each tile's bounding sphere
//                   for culling; parentSlot is -1 for a root
// cache.drawCount   drawn.length
// cache.drawList    KernelBuffer of drawCount records of 8 int32s:
//                   slot, parentSlot, root, level, x, y, 0, 0, the draw set
//                   on the GPU for a shader or kernel to walk
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

function frame(camera) {
    terrain.update({ position: camera.position, fov: camera.fov, viewportHeight: 1080 });
    // node.setShaderBuffer('u_heights', terrain.layer('height'));
    // node.setShaderBuffer('u_tiles', terrain.drawList);
    // node.instanceCount = terrain.drawCount;
}

// Errors: options that are not an object, an unknown domain type or a
// produce that is not a function throw a TypeError; out-of-range integers
// or layer sizes a RangeError; no GPU device an Error. layer() of an unknown
// name throws a RangeError, update() without a position a TypeError.
