// Headless test for bro.tiles: a tile cache over a plane, filled on demand by
// GPU kernels, each tile refined from its parent's cells.
//   - the drawn set covers the domain once, without overlap, and refines
//     near the camera;
//   - refine, never replace: every child's 2x2 cell averages are its parent's
//     cells over the quarter it covers;
//   - the draw list on the GPU is the drawn set;
//   - a small cache evicts what the view no longer wants and refills;
//   - a producer that throws leaves the tiles missing, and the next update
//     offers them again;
//   - a layer's pool binds to a scene node; bad options are refused.

if (!bro.kernel.available || !bro.tiles) {
    skipTest('bro.kernel is compiled out of this build');
} else if (!bro.kernel.gpu) {
    skipTest('no GPU device for bro.kernel');
} else {
    const mod = bro.kernel.compile(`
        @kernel @grid(2)
        fn k_root(g: grid<2>, out: write_store<f32, 2>, ox: f32, oz: f32, step: f32) {
            let x = ox + (g.x() as f32 + 0.5) * step;
            let z = oz + (g.y() as f32 + 0.5) * step;
            out[g.x(), g.y()] = sin(x * 0.11) * cos(z * 0.07) * 4.0;
        }

        // A child's cell is its parent's cell plus detail that sums to zero
        // over each 2x2 block, so the child averages back to the parent.
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
            let d = amp * sin((px + qx * half) * 12.9898 + (py + qy * half) * 78.233);
            out[g.x(), g.y()] = p + d * sx * sy;
        }
    `, { file: 'tiles.bk' });
    const kRoot = mod.kernel('k_root'), kRefine = mod.kernel('k_refine');

    const SIZE = 64, T = 16;
    let planeFrameOk = true;
    const produce = (tile) => {
        const f = tile.frame;
        planeFrameOk = planeFrameOk && f.corner[0] === tile.origin[0] && f.corner[2] === tile.origin[2] &&
            f.u[0] === tile.extent && f.v[2] === tile.extent && f.u[2] === 0 && f.v[0] === 0;
        const step = tile.extent / tile.cells;
        if (!tile.parent) {
            kRoot.dispatch([tile.cells, tile.cells], tile.layers.height, tile.origin[0], tile.origin[2], step);
        } else {
            const [qx, qy] = tile.parent.quadrant;
            kRefine.dispatch([tile.cells, tile.cells], tile.parent.layers.height, tile.layers.height,
                             qx, qy, tile.cells / 2, 0.5 / (1 << tile.level));
        }
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
    const records = cache.drawList.read(new Int32Array(cache.drawCount * 8));
    let listOk = cache.drawCount === drawn.length;
    for (let i = 0; i < drawn.length && listOk; i++) {
        const r = records.subarray(i * 8, i * 8 + 8), t = drawn[i];
        listOk = r[0] === t.slot && r[1] === t.parentSlot && r[2] === t.root && r[3] === t.level &&
                 r[4] === t.x && r[5] === t.y;
    }
    assert(listOk, 'the draw list holds the drawn tiles\' slots and ids');

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

    // A producer that throws: the tile stays missing and is offered again.
    let failLevel = 2, calls = 0;
    const flaky = makeCache({
        produce: (tile) => {
            calls++;
            if (tile.level === failLevel) throw new Error('no data yet');
            produce(tile);
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

    // A cube-sphere refines toward a camera above its surface.
    const sphere = bro.tiles.createCache({
        domain: { type: 'cubeSphere', radius: 100, center: [0, 0, 0] },
        tileSize: T, capacity: 256, maxLevel: 6, layers: { height: 4 },
        produce: (tile) => {
            if (tile.level === 0 && !tile.parent) produced0.push(tile.root);
            // The frame's far corner, pushed onto the sphere, is the point
            // diagonally across from `origin`, at the tile's extent scale.
            const f = tile.frame, onSphere = (b) => { const l = Math.hypot(...b); return b.map((c) => c / l * 100); };
            const o = onSphere(f.corner);
            frameErr = Math.max(frameErr, Math.hypot(o[0] - tile.origin[0], o[1] - tile.origin[1], o[2] - tile.origin[2]));
            const far = onSphere(f.corner.map((c, i) => c + f.u[i] + f.v[i]));
            const diag = Math.hypot(far[0] - o[0], far[1] - o[1], far[2] - o[2]) / tile.extent;
            diagMin = Math.min(diagMin, diag); diagMax = Math.max(diagMax, diag);
        },
    });
    const produced0 = [];
    let frameErr = 0, diagMin = Infinity, diagMax = 0;
    assert(settle(sphere, { position: [0, 103, 0], fov: 60, viewportHeight: 720 }), 'a cube-sphere settles');
    assert(produced0.length === 6, `its six faces are roots (${produced0.length})`);
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
