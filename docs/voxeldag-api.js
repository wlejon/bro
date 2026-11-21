/**
 * bro.voxeldag: sparse voxel DAG worlds (bromesh::VoxelDagPool) drawn by the
 * scene's raymarch pass, for dense mid and far geometry such as forests to
 * the horizon.
 *
 * A world owns one content-addressed pool and any number of regions.
 *
 * - Archetypes are subtrees made once (from a mesh, an SDF, a .vox file, a
 *   box) and placed by reference. Identical nodes and 4^3 bricks are stored
 *   once, so ten thousand placements of one tree cost the nodes above them.
 * - A region is an oriented box in world space (double-precision centre,
 *   orthonormal basis) of `voxelSize * (4 << maxDepth)` metres a side. Its
 *   cells at a target depth hold placed archetypes.
 * - Each frame the pass keeps the enabled regions whose content is in view
 *   and in their distance band, nearest first, and marches those per pixel
 *   (so a world of many fixed tiles costs what is on screen). A cell smaller than the
 *   pixel cone shades from its prefiltered colour, coverage and normal, so
 *   distance costs no more than near. Rays stop at the scene's depth and at
 *   the nearest region hit.
 * - Rays are clipped to each region's content bounds (the cells its
 *   placements cover), so an empty region, or the sky over a flat forest,
 *   is not marched.
 * - `minDist` / `fadeStart` / `maxDist` dither a region in and out, so nested
 *   rings of coarser voxels hand over without a seam.
 *
 * The pool never moves an index. `collect()` frees what no region and no
 * archetype reaches, and later interning reuses those slots, so a world that
 * rebuilds its rings as the camera moves stays at its working size.
 *
 * Example:
 *   const vd = bro.voxeldag.createWorld(scene);
 *   const tree = vd.addArchetypeFromMesh(mesh, 3, { unitBounds: true, roughness: 0.9 });
 *   const ring = vd.addRegion({
 *       center: [x, y, z], right, up, forward,
 *       voxelSize: 2, maxDepth: 10,           // 8192 m a side
 *       minDist: 1450, fadeStart: 3500, maxDist: 4200, castShadow: false,
 *   });
 *   const batch = new Uint32Array(count * 5);  // ix, iy, iz, ref, isLeaf
 *   ...
 *   vd.placeBatchTyped(ring, batch, 9, count); // cells at depth 9: 16 m
 *   // Later, recentred:
 *   vd.updateRegion(ring, { center: [nx, ny, nz] });
 *   vd.clearRegion(ring);
 *   vd.placeBatchTyped(ring, batch, 9, count2);
 *   vd.collect();
 */

/**
 * @typedef {Object} Archetype
 * @property {number} ref     node index (or brick index when `isLeaf`)
 * @property {boolean} isLeaf a single 4^3 brick, not a node
 * @property {number} depth   levels above its bricks: it spans 4 << depth voxels,
 *                            so it fills one cell at target depth maxDepth - depth
 *
 * Every archetype is pinned: `collect()` keeps it whether or not it is placed.
 */

/**
 * @typedef {Object} RegionOptions
 * @property {number[]} [center]   world centre, metres (double precision)
 * @property {number[]} [right]    basis x (normalised)
 * @property {number[]} [up]       basis y
 * @property {number[]} [forward]  basis z
 * @property {number} [voxelSize=1]
 * @property {number} [maxDepth=4] 1..16; extent = voxelSize * (4 << maxDepth)
 * @property {number} [minDist=0]  nearer than this is not drawn...
 * @property {number} [minFadeEnd=0] ...the near edge dithering in up to here
 *                                  (0: minDist * 1.35)
 * @property {number} [fadeStart=0] the far edge dithers from here...
 * @property {number} [maxDist=0]  ...to here (0: unlimited)
 *
 * A pixel has one dither value u, drawing a region from
 * mix(minDist, minFadeEnd, u) to mix(fadeStart, maxDist, u): a region whose
 * minDist / minFadeEnd are another's fadeStart / maxDist takes over from it
 * with neither a gap nor an overlap at any u.
 * @property {boolean} [enabled=true]
 * @property {boolean} [castShadow=true]
 * @property {number} [animTickOffset=0]
 */

const bro = {
    voxeldag: {
        /**
         * The world for a SceneGraph; its node is in the scene.
         * @param {SceneGraph} scene
         * @returns {VoxelWorld}
         */
        createWorld(scene) {},

        /** The `voxeldag` brokernel module's source (`import voxeldag;` in a
         *  kernel or hook reads a world's `buffers()`). */
        module: '',
    },
};

class VoxelWorld {
    /** The world's SceneNode. */
    get node() {}

    // ── Archetypes ──────────────────────────────────────────────────────────

    /**
     * Voxelise a triangle mesh (a bro.mesh Mesh or { positions, indices,
     * normals?, colors? }) into a subtree.
     * @param {Mesh|Object} mesh
     * @param {number} [depth=2] 1..5: (4 << depth)^3 voxels
     * @param {Object} [opts]
     * @param {boolean} [opts.unitBounds=false] the mesh's [-1, 1]^3 maps to the
     *        archetype's cube as is (parts outside are cut), so patches made at
     *        one scale line up; otherwise its bounds are fitted (`fitToCube`)
     * @param {boolean} [opts.fitToCube=true]
     * @param {boolean} [opts.fillInterior=false]
     * @param {number} [opts.roughness=0.8]
     * @param {number} [opts.emissive=0]
     * @param {string|number[]} [opts.color] where the mesh has no colours
     * @returns {Archetype}
     */
    addArchetypeFromMesh(mesh, depth, opts) {}

    /** A MagicaVoxel .vox file. @param {Uint8Array|ArrayBuffer} bytes
     *  @param {{roughness?: number}} [opts] @returns {Archetype} */
    addArchetypeFromVox(bytes, opts) {}

    /** A solid box filling the cube; depth 0 is one brick (`isLeaf`).
     *  @param {string|number[]} color @param {number} [depth=1] 0..4
     *  @param {{state?: 'solid'|'carve', roughness?: number, emissive?: number}} [opts]
     *  @returns {Archetype} */
    addArchetypeFromBox(color, depth, opts) {}

    /** From an analytic shape object or an (x, y, z) => distance function over
     *  [-1, 1]^3. @param {Object|Function} shape @param {number} [depth=2]
     *  @param {Object} [opts] as addArchetypeFromMesh @returns {Archetype} */
    addArchetypeFromSdf(shape, depth, opts) {}

    /** As addArchetypeFromSdf, but its voxels carve (remove what a region's
     *  solid holds there). */
    addCarveArchetypeFromSdf(shape, depth, opts) {}

    /** `overlay` over `base`: where both are solid the overlay wins; carves union.
     *  @param {Archetype} base @param {Archetype} overlay @returns {Archetype} */
    mergeArchetypes(base, overlay) {}

    /** A cyclic clip of archetypes (frames); returns its index. Frames are kept
     *  by `collect()`. @param {Archetype[]} frames @returns {number} */
    registerAnimClip(frames) {}

    /** A placeable reference to clip `clip` at phase `phase`.
     *  @returns {Archetype} */
    makeAnimRef(clip, phase) {}

    // ── Regions ─────────────────────────────────────────────────────────────

    /** @param {RegionOptions} opts @returns {number} region id */
    addRegion(opts) {}

    /** Change some options; unset ones keep their values. Its placements stay
     *  (in cell units), so moving a region moves what it holds.
     *  @returns {boolean} */
    updateRegion(id, opts) {}

    /** Empty a region (its tree is left for `collect()`). @returns {boolean} */
    clearRegion(id) {}

    /** @returns {boolean} */
    removeRegion(id) {}

    // ── Placement (cells at `targetDepth`: 2^targetDepth a side) ────────────

    /** @param {Archetype|number} archetype @returns {boolean} */
    place(regionId, ix, iy, iz, archetype, targetDepth) {}

    /** @param {Array<{ix, iy, iz, archetype?: Archetype, ref?: number, isLeaf?: boolean}>} list */
    placeBatch(regionId, list, targetDepth) {}

    /**
     * The fast path: one pass over the region's tree for every placement.
     * @param {Uint32Array|Int32Array} batch stride 5: ix, iy, iz, ref, isLeaf (0/1)
     * @param {number} targetDepth
     * @param {number} [count] placements to read (default: all of `batch`)
     */
    placeBatchTyped(regionId, batch, targetDepth, count) {}

    /** Place clip `clip` at `phase` (it advances with the world's anim tick). */
    placeAnimated(regionId, ix, iy, iz, clip, phase, isLeaf, targetDepth) {}

    /** Carve `carve` and add `solid` in one cell, merged so an overhang and its
     *  void share it. */
    stampCarveAndSolid(regionId, ix, iy, iz, carve, solid, targetDepth) {}

    /** Empty one cell. */
    clear(regionId, ix, iy, iz, targetDepth) {}

    // ── Pool ────────────────────────────────────────────────────────────────

    /**
     * Free the nodes and bricks no region root, archetype or animation clip
     * reaches. Their slots (and their children / attribute runs) are reused
     * by later interning; nothing moves, so every live reference stays valid.
     * Call it after a rebuild: a ring recentred without it leaves its old tree
     * in the pool for good.
     * @returns {number} nodes and bricks freed
     */
    collect() {}

    /**
     * @returns {{nodes: number, liveNodes: number, children: number,
     *            bricks: number, liveBricks: number, attrs: number,
     *            animClips: number, animFrames: number, regions: number,
     *            generation: number, animTick: number}}
     *   The array lengths (what the GPU holds: nodes 16 B, children 4 B,
     *   bricks 32 B, attrs 8 B each) and the slots in use.
     */
    stats() {}

    // ── Rendering ───────────────────────────────────────────────────────────

    /** Pixel-cone LOD scale (1: a cell under a pixel shades prefiltered). */
    setLodScale(s) {}
    /** March steps a region a pixel (default 128, min 8). */
    setMaxRaySteps(n) {}
    /** Shadow ray steps (0: no voxel shadows). */
    setShadowSteps(n) {}
    setAnimTick(tick) {}
    setAnimFps(fps) {}
    setAutoAnimate(on) {}

    /**
     * The pool and regions as KernelBuffers, synced now, regions relative to
     * `anchor` (for a kernel or hook importing `voxeldag`).
     * @param {number[]} [anchor]
     * @returns {{regions, nodes, children, bricks, attrs, animClips, animFrames: KernelBuffer, regionCount: number}}
     */
    buffers(anchor) {}

    /** Sync without returning the buffers. @returns {{synced: true, regionCount: number}} */
    syncBuffers(anchor) {}
}

/*
 * The regions buffer, for kernels: voxeldag::REGION_STRIDE vec4s a region.
 *   [0] xyz min corner (anchor-relative), w extent
 *   [1] xyz right, w voxelSize
 *   [2] xyz up, w maxDist
 *   [3] xyz forward, w fadeStart
 *   [4] bits: x root, y maxDepth | minDist << 5 (metres), z flags, w animTickOffset
 *   [5] xyz bits: content bounds in [0, 1]^3, unorm16 pairs (voxeldag::content_box);
 *       w minFadeEnd
 */
