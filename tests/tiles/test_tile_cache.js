// Headless test for bro.tiles: a tile cache over a plane, filled on demand by
// GPU kernels, each tile refined from its parent's cells.
//   - the drawn set covers the domain once, without overlap, and refines
//     near the camera;
//   - refine, never replace: every child's 2x2 cell averages are its parent's
//     cells over the quarter it covers;
//   - the draw list on the GPU is the drawn set;
//   - a small cache evicts what the view no longer wants and refills;
//   - production runs beside the frames: a batch is ready at a later update;
//   - produce takes the update's batch in one call, its table on the GPU;
//   - a producer that throws leaves the batch missing, and the next update
//     offers them again;
//   - a refresh produces every wanted tile again in place, parents first,
//     still drawn meanwhile, and drops from the lookup the ready tiles no
//     view wants;
//   - the batches pace themselves by the frames;
//   - a layer's pool binds to a scene node, as a buffer or a texture array
//     copied slot by slot as tiles are produced; bad options are refused.

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
    // A batch is ready once the GPU has done it (it runs beside the frames),
    // so each update is a frame apart.
    const settle = (cache, view) => {
        for (let i = 0; i < 64; i++) {
            cache.update(view);
            if (cache.pending === 0) return true;
            advanceTime(16);
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
    // Production runs in the background stream, beside the frames: a batch
    // is ready at a later update, once the GPU has done it, and its kernels
    // are timed as background work.
    perf.kernelTimes();
    const fresh = makeCache({});
    fresh.update(near);
    assert(fresh.stats.produced === 0 && fresh.drawn.length === 0 && fresh.pending > 0,
           `a batch is not ready in the update that produced it (${fresh.stats.produced} produced, ${fresh.drawn.length} drawn)`);
    assert(settle(fresh, near) && fresh.drawn.length > 0, 'and is at a later one');
    const times = perf.kernelTimes();
    assert(times['k_produce (background)']?.background === true && !times.k_produce,
           `production is timed as background work (${Object.keys(times)})`);

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

    // A refresh: every wanted tile is produced again into its own slot, a
    // parent's batch before its children's, the drawn set unchanged meanwhile.
    let seen = [];
    const redo = makeCache({
        produce: function (batch) {
            seen.push(batch.tiles.map((t) => ({ key: tileKey(t), slot: t.slot, parent: t.parent ? t.parent.slot : -1 })));
            produce.call(this, batch);
        },
    });
    assert(settle(redo, near), 'a cache to refresh settles');
    const slotOf = new Map(seen.flat().map((t) => [t.key, t.slot]));
    const drawnKeys = redo.drawn.map(tileKey).sort().join();
    const producedBefore = redo.stats.produced;
    seen = [];
    redo.refresh();
    assert(redo.stale === redo.wanted && redo.pending === redo.wanted && redo.stale > 0,
           `a refresh leaves every wanted tile out of date (${redo.stale} of ${redo.wanted})`);
    let keptDrawn = true, settled = false;
    for (let i = 0; i < 64 && !settled; i++) {
        redo.update(near);
        keptDrawn = keptDrawn && redo.drawn.map(tileKey).sort().join() === drawnKeys;
        settled = redo.pending === 0;
        advanceTime(16);
    }
    const again = seen.flat();
    assert(settled && redo.stale === 0, `the updates produce them again (${redo.pending} pending)`);
    assert(keptDrawn, 'the drawn tiles stay drawn while they are produced again');
    assert(again.length === redo.wanted && again.every((t) => slotOf.get(t.key) === t.slot) &&
           redo.stats.produced === producedBefore + again.length && redo.stats.evicted === 0,
           `each once, into its own slot (${again.length} for ${redo.wanted} wanted)`);
    const batchOf = new Map();
    seen.forEach((b, i) => b.forEach((t) => batchOf.set(t.slot, i)));
    assert(again.every((t) => t.parent < 0 || batchOf.get(t.parent) < batchOf.get(t.slot)),
           'a parent in an earlier batch than its children');

    // The batches pace themselves by the frames: a quarter of
    // producePerUpdate at first, then a
    // tile more an update while no scene runs over its frame target (none
    // here), never past producePerUpdate.
    const paced = makeCache({ producePerUpdate: 6 });
    const made = [];
    for (let i = 0; i < 12; i++) {
        made.push(paced.update(near));
        advanceTime(16);
    }
    assert(Math.max(...made) === 6 && made.every((n, i) => n <= i + 2),
           `batches grow a tile an update up to producePerUpdate (${made})`);
    assert(paced.stats.batchSize === 6, `and hold there (${paced.stats.batchSize})`);

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

    // The lookup: a kernel finds the ready tile holding a direction through
    // tiles.bk and filters a layer there. Each cell holds its centre's
    // direction (x, y, z) and its tile's slot, border cells included.
    const LT = 8, LB = 1, LC = LT + 2 * LB;
    const looked = bro.kernel.compile(`
        import tiles;
        @kernel @grid(3)
        fn k_dirs(g: grid<3>, table: read_store<f64, 1>, dirs: rw_store<f32, 1>, slots: rw_store<f32, 1>) {
            let r = g.z() * 24;
            let slot = table[r] as i32;
            let s = ((g.x() as f64) - ${LB}.0 + 0.5) / ${LT}.0;
            let t = ((g.y() as f64) - ${LB}.0 + 0.5) / ${LT}.0;
            let b = vec3<f64>(table[r + 8], table[r + 9], table[r + 10])
                  + vec3<f64>(table[r + 11], table[r + 12], table[r + 13]) * s
                  + vec3<f64>(table[r + 14], table[r + 15], table[r + 16]) * t;
            let d = normalize(b);
            let i = (slot * ${LC} + g.y()) * ${LC} + g.x();
            dirs[3 * i] = d.x as f32;
            dirs[3 * i + 1] = d.y as f32;
            dirs[3 * i + 2] = d.z as f32;
            slots[i] = slot as f32;
        }
        fn filtered(dirs: read_store<f32, 1>, c: tiles::Cells, k: i32) -> f32 {
            return mix(mix(dirs[3 * c.i00 + k], dirs[3 * c.i10 + k], c.f.x),
                       mix(dirs[3 * c.i01 + k], dirs[3 * c.i11 + k], c.f.x), c.f.y);
        }
        // For each direction: the tile found (slot, level, x, y, root) and
        // the direction filtered from the cells around it, and the slot
        // read at its first cell.
        @kernel @grid(1)
        fn k_find(g: grid<1>, lookup: read_store<vec4<i32>, 1>, q: read_store<f32, 1>, dirs: read_store<f32, 1>,
                  slots: read_store<f32, 1>, out: write_store<f32, 1>, level: i32) {
            let n = g.x();
            let d = normalize(vec3<f32>(q[3 * n], q[3 * n + 1], q[3 * n + 2]));
            let p = tiles::cube_point(d);
            let t = tiles::locate(lookup, p, level);
            let c = tiles::cells(t, p, ${LT}, ${LB});
            let f = vec3<f32>(filtered(dirs, c, 0), filtered(dirs, c, 1), filtered(dirs, c, 2));
            out[9 * n] = t.slot as f32;
            out[9 * n + 1] = t.level as f32;
            out[9 * n + 2] = t.x as f32;
            out[9 * n + 3] = t.y as f32;
            out[9 * n + 4] = p.root as f32;
            out[9 * n + 5] = dot(normalize(f), d);
            out[9 * n + 6] = slots[c.i00];
            out[9 * n + 7] = slots[c.i11];
            out[9 * n + 8] = t.parent as f32;
        }
    `, { file: 'lookup.bk', modules: { tiles: bro.tiles.module } });
    const kDirs = looked.kernel('k_dirs'), kFind = looked.kernel('k_find');
    const globe = bro.tiles.createCache({
        domain: { type: 'cubeSphere', radius: 100, center: [0, 0, 0] },
        tileSize: LT, border: LB, capacity: 512, maxLevel: 7, producePerUpdate: 64,
        layers: { dirs: 12, slots: 4 },
        produce(batch) { kDirs.dispatch([LC, LC, batch.count], batch.table, this.layer('dirs'), this.layer('slots')); },
    });
    assert(settle(globe, { position: [30, 104, -20], fov: 60, viewportHeight: 720 }), 'a cube-sphere with a border settles');
    const N = 512, qs = new Float32Array(3 * N);
    let seed = 12345;
    const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296) * 2 - 1;
    for (let i = 0; i < N; i++) {
        // Half of them near the camera's point, where tiles are finest.
        const near = i % 2 === 0;
        qs[3 * i] = near ? 0.3 + rnd() * 0.05 : rnd();
        qs[3 * i + 1] = near ? 1.0 : rnd();
        qs[3 * i + 2] = near ? -0.2 + rnd() * 0.05 : rnd();
    }
    const qBuf = bro.kernel.createBuffer(qs), outBuf = bro.kernel.createBuffer(N * 36);
    kFind.dispatch([N], globe.lookup, qBuf, globe.layer('dirs'), globe.layer('slots'), outBuf, 7);
    const res = outBuf.read(new Float32Array(N * 9));
    // A direction's root and (u, v) as the cache's frames place it.
    const cubePoint = (x, y, z) => {
        const ax = Math.abs(x), ay = Math.abs(y), az = Math.abs(z);
        if (ax >= ay && ax >= az) return x > 0 ? [0, -z / ax, y / ax] : [1, z / ax, y / ax];
        if (ay >= az) return y > 0 ? [2, x / ay, -z / ay] : [3, x / ay, z / ay];
        return z > 0 ? [4, x / az, y / az] : [5, -x / az, y / az];
    };
    const drawnLevel = new Map(globe.drawn.map((t) => [tileKey(t), t]));
    let found = 0, inTile = 0, finest = 0, slotOk = 0, minDot = 1, parents = 0, parentOk = 0;
    for (let i = 0; i < N; i++) {
        const r = res.subarray(9 * i, 9 * i + 9);
        const [root, s, q] = cubePoint(qs[3 * i], qs[3 * i + 1], qs[3 * i + 2]);
        if (r[0] < 0) continue;
        found++;
        const n = 2 ** r[1], u = (s + 1) / 2 * n, v = (q + 1) / 2 * n;
        if (r[4] === root && u >= r[2] - 1e-4 && u <= r[2] + 1 + 1e-4 && v >= r[3] - 1e-4 && v <= r[3] + 1 + 1e-4) inTile++;
        // No drawn tile holding it is finer than the one found.
        let deepest = 0;
        for (let l = 0; l <= 7; l++) {
            const m = 2 ** l, k = tileKey({ root, level: l, x: Math.min(m - 1, Math.floor((s + 1) / 2 * m)), y: Math.min(m - 1, Math.floor((q + 1) / 2 * m)) });
            if (drawnLevel.has(k)) deepest = l;
        }
        if (r[1] >= deepest) finest++;
        if (r[6] === r[0] && r[7] === r[0]) slotOk++;
        minDot = Math.min(minDot, r[5]);
        // A drawn tile's record names its parent's slot as the draw set does.
        const d = drawnLevel.get(tileKey({ root: r[4], level: r[1], x: r[2], y: r[3] }));
        if (d && d.level > 0) {
            parents++;
            if (r[8] === d.parentSlot) parentOk++;
        }
    }
    assert(parents > N / 4 && parentOk === parents, `the tile found carries its parent's slot (${parentOk} of ${parents})`);
    assert(found === N, `every direction finds a ready tile (${found} of ${N})`);
    assert(inTile === N, `the tile found holds it (${inTile} of ${N})`);
    assert(finest === N, `and is at least as fine as the drawn tile there (${finest} of ${N})`);
    assert(slotOk === N, `its cells are in its slot, the border included (${slotOk} of ${N})`);
    assert(minDot > 0.99999, `filtering the cells around it gives its direction (cos ${minDot})`);

    // After a refresh, a ready tile no view wants leaves the lookup: nothing
    // would produce it again, so a reader asking finer than the view wants
    // (where an earlier view refined) never finds cells made for what has
    // changed since. The directions near the first view's point, found again
    // from the far side of the globe, in a cache with room for both views.
    const roomy = bro.tiles.createCache({
        domain: { type: 'cubeSphere', radius: 100, center: [0, 0, 0] },
        tileSize: LT, border: LB, capacity: 4096, maxLevel: 7, producePerUpdate: 64, pixelsPerCell: 64,
        layers: { dirs: 12, slots: 4 },
        produce(batch) { kDirs.dispatch([LC, LC, batch.count], batch.table, this.layer('dirs'), this.layer('slots')); },
    });
    const nearLevels = () => {
        kFind.dispatch([N], roomy.lookup, qBuf, roomy.layer('dirs'), roomy.layer('slots'), outBuf, 7);
        const r = outBuf.read(new Float32Array(N * 9));
        return Array.from({ length: N / 2 }, (_, j) => r[9 * 2 * j + 1]);
    };
    assert(settle(roomy, { position: [30, 104, -20], fov: 60, viewportHeight: 720 }), 'a roomier cube-sphere settles');
    const nearBefore = nearLevels(), wantedNear = roomy.wanted;
    const away = { position: [-30, -104, 20], fov: 60, viewportHeight: 720 };
    assert(settle(roomy, away), 'and settles from its far side');
    const nearKept = nearLevels();
    assert(nearKept.every((l, j) => l === nearBefore[j]) && Math.min(...nearBefore) > 1 && roomy.stats.evicted === 0,
           `the tiles the view left are still found while the cache has room (levels ${Math.min(...nearKept)}-` +
           `${Math.max(...nearKept)}; ${wantedNear} then ${roomy.wanted} wanted, ${roomy.stats.evicted} evicted)`);
    roomy.refresh();
    roomy.update(away);
    const nearAfter = nearLevels();
    assert(nearAfter.every((l, j) => l < nearBefore[j]),
           `after a refresh they are not; the coarser tiles the view wants are (levels ${Math.min(...nearAfter)}-${Math.max(...nearAfter)})`);

    // A layer's pool is a KernelBuffer a scene node reads in place.
    const cv = document.createElement('canvas');
    cv.setAttribute('width', '32');
    cv.setAttribute('height', '32');
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        const node = sc.createMesh({ mesh: 'box' });
        node.setShader({ module: `
            module hook;
            import mesh;
            @binding(4, 14) var tiles: read_buffer<f32>;
            pub fn vertex(v: mesh::Vertex) -> mesh::Vertex {
                var out = v;
                if (tiles.len() > 0) { out.pos.y += tiles[0] * 0.0; }
                return out;
            }` });
        let bindErr = null;
        try { node.setShaderBuffer('tiles', cache.layer('height')); } catch (e) { bindErr = e; }
        assert(bindErr === null, 'a layer pool binds to a scene node: ' + (bindErr && bindErr.message));
        sc.captureFrame();

        // Bound as a texture array, a slot a part, a texel of a tile's
        // layer is its cell; only the slots an update produces are copied
        // after the first, so a write to the pool no update produced does
        // not show, and a tile produced again does.
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 4], target: [0, 0, 0] });
        sc.setToneMap({ mode: 'linear', gamma: 1.0 });
        const tex = makeCache({ capacity: 64 });
        assert(settle(tex, near), 'the textured cache settles');
        const quad = sc.createMesh({ mesh: 'box', color: 'white' });
        quad.setShader({
            module: `
                module hook;
                import mesh;
                import custom;
                struct Params { at: vec3<f32> }
                @binding(4, 0) var params: uniform<Params>;
                @binding(4, 1) var cells: texture2d_array<vec4<f32>>;
                pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
                    var out = s;
                    out.base_color = vec3<f32>(0.0);
                    out.metallic = 0.0;
                    out.roughness = 1.0;
                    out.emissive = vec3<f32>(clamp(cells.sample_lod(custom::linear, params.at, 0.0).x * 0.1 + 0.5, 0.0, 1.0));
                    return out;
                }`,
            uniforms: { at: [0.5, 0.5, 0] },
        });
        // Two by two slots a layer (tiles::atlas_uvw).
        quad.setShaderTexture('cells', { buffer: tex.layer('height'), width: T, height: T, layers: tex.capacity, atlas: 2 });
        const pool = new Float32Array(tex.capacity * T * T);
        const cellAt = (slot) => (slot * T + T / 2) * T + T / 2;
        const shown = (slot) => {
            const cell = slot % 4, c = (T / 2 + 0.5) / T;
            quad.setShaderUniform('at', [((cell % 2) + c) / 2, (Math.floor(cell / 2) + c) / 2, Math.floor(slot / 4)]);
            const img = sc.captureFrame();
            const i = (Math.floor(img.height / 2) * img.width + Math.floor(img.width / 2)) * 4;
            return img.data[i];
        };
        const expect = (v) => 255 * Math.min(1, Math.max(0, v * 0.1 + 0.5));
        const slot = tex.drawn[0].slot;
        tex.layer('height').read(pool);
        const first = shown(slot);
        assert(Math.abs(first - expect(pool[cellAt(slot)])) < 4,
               `a tile's texel is its cell (${first} for ${pool[cellAt(slot)].toFixed(3)})`);
        tex.layer('height').write(new Float32Array([pool[cellAt(slot)] + 3]), cellAt(slot) * 4);
        tex.update(near);
        const unproduced = shown(slot);
        assert(Math.abs(unproduced - first) < 4, `a slot no update produced is not copied again (${unproduced})`);
        tex.clear();
        assert(settle(tex, near), 'the cleared cache settles');
        tex.layer('height').read(pool);
        const again = tex.drawn.find((t) => t.slot === slot) ? slot : tex.drawn[0].slot;
        const reproduced = shown(again);
        assert(Math.abs(reproduced - expect(pool[cellAt(again)])) < 4,
               `a slot produced again is copied (${reproduced} for ${pool[cellAt(again)].toFixed(3)})`);
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
