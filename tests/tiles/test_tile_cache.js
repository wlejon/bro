// Headless test for bro.tiles: a tile cache over a plane, filled on demand by
// GPU kernels, each tile refined from its parent's cells.
//   - the drawn set covers the domain once, without overlap, and refines
//     near the camera;
//   - refine, never replace: every child's 2x2 cell averages are its parent's
//     cells over the quarter it covers;
//   - the draw list on the GPU is the drawn set;
//   - a small cache evicts what the view no longer wants and refills;
//   - produce takes the update's batch in one call, its table on the GPU;
//   - a producer that throws leaves the batch missing, and the next update
//     offers them again;
//   - a layer's pool binds to a scene node; bad options are refused.

if (!bro.kernel.available || !bro.tiles) {
    skipTest('bro.kernel is compiled out of this build');
} else if (!bro.kernel.gpu) {
    skipTest('no GPU device for bro.kernel');
} else {
    // A batch in one dispatch: grid z is the tile's batch table record, its
    // slot where in the pool ([cells, cells, capacity]) it goes. A root's
    // cells come from a function of the plane; a child's are its parent's
    // cell plus detail that sums to zero over each 2x2 block, so the child
    // averages back to the parent.
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
            let qx = table[r + 6] as f32;
            let qy = table[r + 7] as f32;
            let amp = 0.5 / exp2(table[r + 3] as f32);
            let px = floor(fx * 0.5);
            let py = floor(fy * 0.5);
            let sx = (fx - 2.0 * px) * 2.0 - 1.0;
            let sy = (fy - 2.0 * py) * 2.0 - 1.0;
            let p = pool[(qx * half + px) as i32, (qy * half + py) as i32, parent];
            let d = amp * sin((px + qx * half) * 12.9898 + (py + qy * half) * 78.233);
            pool[g.x(), g.y(), slot] = p + d * sx * sy;
        }

        // A range of [lo, hi] for every tile of the batch at level <= upTo.
        @kernel @grid(1)
        fn k_report(g: grid<1>, table: read_store<f64, 1>, ranges: write_store<f32, 1>,
                    lo: f32, hi: f32, upTo: f32) {
            let r = g.x() * 24;
            if ((table[r + 3] as f32) > upTo) { return; }
            let slot = table[r] as i32;
            ranges[2 * slot] = lo;
            ranges[2 * slot + 1] = hi;
        }
    `, { file: 'tiles.bk' });
    const kProduce = mod.kernel('k_produce'), kReport = mod.kernel('k_report');

    const SIZE = 64, T = 16;
    let planeFrameOk = true, batches = 0, sameBatchParent = 0;
    const produce = function (batch) {
        batches++;
        const slots = new Set(batch.tiles.map((t) => t.slot));
        for (const tile of batch.tiles) {
            const f = tile.frame;
            planeFrameOk = planeFrameOk && f.corner[0] === tile.origin[0] && f.corner[2] === tile.origin[2] &&
                f.u[0] === tile.extent && f.v[2] === tile.extent && f.u[2] === 0 && f.v[0] === 0;
            if (tile.parent && slots.has(tile.parent.slot)) sameBatchParent++;
        }
        kProduce.dispatch([this.cells, this.cells, batch.count], batch.table, this.layer('height'), this.cells);
    };
    const makeCache = (opts) => bro.tiles.createCache(Object.assign({
        domain: { type: 'plane', origin: [0, 0], size: SIZE, roots: [1, 1] },
        tileSize: T, capacity: 128, maxLevel: 5, pixelsPerCell: 2,
        layers: { height: 4 }, producePerUpdate: 1000, produce,
    }, opts));
    const settle = (cache, view) => {
        for (let i = 0; i < 32; i++) {
            cache.update(view);
            if (cache.pending === 0) return true;
        }
        return false;
    };
    const tileKey = (t) => `${t.root}/${t.level}/${t.x}/${t.y}`;
    // Is `a` an ancestor of (or the same tile as) `b`?
    const covers = (a, b) => a.root === b.root && a.level <= b.level &&
        (b.x >> (b.level - a.level)) === a.x && (b.y >> (b.level - a.level)) === a.y;

    const checkCoverage = (cache, what) => {
        const drawn = cache.drawn;
        let area = 0;
        for (const t of drawn) area += (SIZE / (1 << t.level)) ** 2;
        assert(Math.abs(area - SIZE * SIZE) < 1e-6, `${what}: the drawn tiles cover the plane once (area ${area})`);
        let overlap = 0;
        for (let i = 0; i < drawn.length; i++)
            for (let j = 0; j < drawn.length; j++)
                if (i !== j && covers(drawn[i], drawn[j])) overlap++;
        assert(overlap === 0, `${what}: no drawn tile covers another (${overlap})`);
        return drawn;
    };

    const near = { position: [10, 3, 12], fov: 60, viewportHeight: 720 };
    const cache = makeCache({});
    assert(cache instanceof Object && cache.cells === T && cache.capacity === 128, 'createCache makes a TileCache');
    assert(settle(cache, near), 'the cache settles near the ground');
    const drawn = checkCoverage(cache, 'near');
    const deepest = Math.max(...drawn.map((t) => t.level));
    assert(planeFrameOk, 'a plane tile\'s frame is its origin and extent along +x and +z');
    assert(batches < cache.stats.produced && sameBatchParent === 0,
           `produce takes a batch a call, never a tile beside its parent (${batches} calls for ${cache.stats.produced} tiles)`);
    assert(deepest >= 3,`tiles refine near the camera (deepest level ${deepest})`);
    assert(drawn.some((t) => t.level < deepest), 'and stay coarse away from it');
    assert(cache.resident <= cache.capacity && cache.stats.produced === cache.resident,
           `every resident tile was produced once (${cache.stats.produced} produced, ${cache.resident} resident)`);

    // Refine, never replace: the child's 2x2 averages are the parent's cells.
    const cells = cache.cells, tileFloats = cells * cells;
    const pool = cache.layer('height').read(new Float32Array(cache.capacity * tileFloats));
    let worst = 0, compared = 0, detail = 0;
    for (const t of drawn) {
        if (t.parentSlot < 0) continue;
        const qx = t.x & 1, qy = t.y & 1;
        for (let y = 0; y < cells; y += 2)
            for (let x = 0; x < cells; x += 2) {
                const c = (xx, yy) => pool[t.slot * tileFloats + yy * cells + xx];
                const avg = (c(x, y) + c(x + 1, y) + c(x, y + 1) + c(x + 1, y + 1)) / 4;
                const p = pool[t.parentSlot * tileFloats + (qy * cells / 2 + y / 2) * cells + (qx * cells / 2 + x / 2)];
                worst = Math.max(worst, Math.abs(avg - p));
                detail = Math.max(detail, Math.abs(c(x, y) - p));
                compared++;
            }
    }
    assert(compared > 0 && worst < 1e-5,
           `every child averages back to its parent (${compared} blocks, worst ${worst})`);
    assert(detail > 1e-3, `and adds detail of its own (${detail})`);

    // The draw list on the GPU is the drawn set.
    const records = cache.drawList.read(new Int32Array(cache.drawCount * 16));
    const floats = new Float32Array(records.buffer);
    let listOk = cache.drawCount === drawn.length, centerErr = 0;
    for (let i = 0; i < drawn.length && listOk; i++) {
        const r = records.subarray(i * 16, i * 16 + 16), t = drawn[i];
        listOk = r[0] === t.slot && r[1] === t.parentSlot && r[2] === t.root && r[3] === t.level &&
                 r[4] === t.x && r[5] === t.y && Math.abs(floats[i * 16 + 11] - t.radius) <= t.radius * 1e-6;
        for (let k = 0; k < 3; k++)
            centerErr = Math.max(centerErr, Math.abs(floats[i * 16 + 8 + k] + floats[i * 16 + 12 + k] - t.center[k]));
    }
    assert(listOk, 'the draw list holds the drawn tiles\' slots, ids and radii');
    assert(centerErr < 1e-9, `and their centers as hi + lo, to f64 (${centerErr})`);

    // A cache too small to hold both views evicts and refills.
    const small = makeCache({ capacity: 24 });
    assert(settle(small, near), 'a small cache settles');
    const before = new Set(small.drawn.map(tileKey));
    const far = { position: [56, 3, 54], fov: 60, viewportHeight: 720 };
    assert(settle(small, far), 'and settles again across the plane');
    checkCoverage(small, 'after moving');
    const after = new Set(small.drawn.map(tileKey));
    assert(small.stats.evicted > 0 && small.resident <= 24,
           `moving evicts what the view no longer wants (${small.stats.evicted} evicted, ${small.resident} resident)`);
    assert([...after].some((k) => !before.has(k)), 'and refines where the camera went');

    // A producer that throws: the batch stays missing and is offered again.
    let failLevel = 2;
    const flaky = makeCache({
        produce: function (batch) {
            if (batch.tiles.some((t) => t.level === failLevel)) throw new Error('no data yet');
            produce.call(this, batch);
        },
    });
    let threw = null;
    for (let i = 0; i < 8 && !threw; i++) {
        try { flaky.update(near); } catch (e) { threw = e; }
    }
    assert(threw && threw.message === 'no data yet', 'update rethrows what produce threw');
    assert(flaky.pending > 0, `the failed tiles stay pending (${flaky.pending})`);
    checkCoverage(flaky, 'after a throw');
    failLevel = -1;
    assert(settle(flaky, near), 'the next updates produce them');
    assert(Math.max(...flaky.drawn.map((t) => t.level)) === deepest, 'and the cache refines as far as before');

    // Height ranges: 40 m up, under a 300 m worst case every tile's slab
    // reaches the camera, so all of them refine; once each tile reports the
    // [-5, 5] m it really spans, only those near the camera do. Tiles that
    // report nothing inherit.
    const ranged = (upTo) => makeCache({
        capacity: 1024, maxLevel: 6, pixelsPerCell: 16, minHeight: -10, maxHeight: 300, ranges: true,
        produce: function (batch) {
            produce.call(this, batch);
            kReport.dispatch([batch.count], batch.table, this.ranges, -5, 5, upTo);
        },
    });
    // Ranges come back once the GPU is done, never by waiting: a frame or two.
    const settleRanged = (c, view) => {
        let ok = true;
        for (let i = 0; i < 8; i++) { ok = settle(c, view) && ok; advanceTime(16); }
        return ok;
    };
    const wide = ranged(-1), tight = ranged(99), rootsOnly = ranged(0);
    const above = { position: [10, 40, 12], fov: 60, viewportHeight: 720 };
    assert(settleRanged(wide, above) && settleRanged(tight, above) && settleRanged(rootsOnly, above), 'ranged caches settle');
    assert(wide.drawn.every((t) => t.range[0] === -10 && t.range[1] === 300),
           'a tile that reports nothing has the configured range');
    assert(tight.drawn.every((t) => t.range[0] === -5 && t.range[1] === 5), 'a reported range is the tile\'s');
    assert(rootsOnly.drawn.every((t) => t.range[0] === -5 && t.range[1] === 5), 'and its descendants\' until they report');
    checkCoverage(tight, 'ranged');
    assert(tight.drawCount * 2 < wide.drawCount,
           `tight ranges select far fewer tiles (${tight.drawCount} drawn, ${wide.drawCount} under the worst case)`);
    const under = tight.drawn.find((t) => {
        const e = SIZE / (1 << t.level);
        return t.x * e <= 10 && 10 < (t.x + 1) * e && t.y * e <= 12 && 12 < (t.y + 1) * e;
    });
    assert(under && under.level === Math.max(...tight.drawn.map((t) => t.level)) && under.level >= 2,
           `and still refine most under the camera (level ${under && under.level})`);
    const r0 = tight.drawn[0], w0 = wide.drawn.find((t) => tileKey(t) === tileKey(r0));
    if (w0) assert(r0.radius < w0.radius && r0.center.every((c, i) => c === w0.center[i]),
                   'a narrower range shrinks the bounds about the same center');

    // A cube-sphere refines toward a camera above its surface.
    const sphere = bro.tiles.createCache({
        domain: { type: 'cubeSphere', radius: 100, center: [0, 0, 0] },
        tileSize: T, capacity: 256, maxLevel: 6, layers: { height: 4 },
        produce: (batch) => {
            // The table holds what the tile objects do.
            const table = batch.table.read(new Float64Array(batch.count * 24));
            batch.tiles.forEach((tile, i) => {
                const r = table.subarray(i * 24, i * 24 + 24);
                const f = tile.frame;
                const same = r[0] === tile.slot && r[1] === (tile.parent ? tile.parent.slot : -1) &&
                    r[2] === tile.root && r[3] === tile.level && r[4] === tile.x && r[5] === tile.y &&
                    [...f.corner, ...f.u, ...f.v, ...tile.center].every((v, k) => r[8 + k] === v) &&
                    r[20] === tile.extent && r[21] === tile.radius &&
                    (!tile.parent || (r[6] === tile.parent.quadrant[0] && r[7] === tile.parent.quadrant[1]));
                tableOk = tableOk && same;
                if (tile.level === 0 && !tile.parent) produced0.push(tile.root);
                // The frame's far corner, pushed onto the sphere, is the point
                // diagonally across from `origin`, at the tile's extent scale.
                const onSphere = (b) => { const l = Math.hypot(...b); return b.map((c) => c / l * 100); };
                const o = onSphere(f.corner);
                frameErr = Math.max(frameErr, Math.hypot(o[0] - tile.origin[0], o[1] - tile.origin[1], o[2] - tile.origin[2]));
                const far = onSphere(f.corner.map((c, k) => c + f.u[k] + f.v[k]));
                const diag = Math.hypot(far[0] - o[0], far[1] - o[1], far[2] - o[2]) / tile.extent;
                diagMin = Math.min(diagMin, diag); diagMax = Math.max(diagMax, diag);
            });
        },
    });
    const produced0 = [];
    let frameErr = 0, diagMin = Infinity, diagMax = 0, tableOk = true;
    assert(settle(sphere, { position: [0, 103, 0], fov: 60, viewportHeight: 720 }), 'a cube-sphere settles');
    assert(produced0.length === 6, `its six faces are roots (${produced0.length})`);
    assert(tableOk, 'the batch table holds each tile\'s slot, parent, id, quadrant, frame, center, extent and radius');
    assert(frameErr < 1e-9, `a tile's frame corner on the sphere is its origin (${frameErr})`);
    assert(diagMin > 0.8 && diagMax < 2.2, `and its frame spans the tile (diagonal / extent in [${diagMin}, ${diagMax}])`);
    const top = sphere.drawn.filter((t) => t.root === 2);
    assert(top.length > 1 && Math.max(...top.map((t) => t.level)) > Math.max(...sphere.drawn.filter((t) => t.root === 3).map((t) => t.level)),
           'the face under the camera refines further than the one opposite');

    // A layer's pool is a KernelBuffer a scene node reads in place.
    const cv = document.createElement('canvas');
    cv.setAttribute('width', '32');
    cv.setAttribute('height', '32');
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        const node = sc.createMesh({ mesh: 'box' });
        node.setShader({ vertex: `buffer float u_tiles[];
            void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv) {
                if (u_tiles_length > 0u) pos.y += u_tiles[0] * 0.0;
            }` });
        let bindErr = null;
        try { node.setShaderBuffer('u_tiles', cache.layer('height')); } catch (e) { bindErr = e; }
        assert(bindErr === null, 'a layer pool binds to a scene node: ' + (bindErr && bindErr.message));
        sc.captureFrame();
    }

    // The errors a script gets.
    const throws = (fn, check, what) => {
        try { fn(); } catch (e) { assert(check(e), what + ': ' + e.message); return; }
        assert(false, what + ': did not throw');
    };
    throws(() => cache.layer('colour'), (e) => e instanceof RangeError, 'an unknown layer');
    throws(() => makeCache({ domain: { type: 'torus' } }), (e) => e instanceof TypeError, 'an unknown domain');
    throws(() => makeCache({ layers: { height: 3 } }), (e) => e instanceof RangeError, 'a layer of 3 bytes a cell');
    throws(() => makeCache({ produce: 7 }), (e) => e instanceof TypeError, 'a produce that is not a function');
    throws(() => cache.update({}), (e) => e instanceof TypeError, 'an update without a position');
}
