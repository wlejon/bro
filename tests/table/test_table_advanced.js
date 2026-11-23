// Headless integration test for bro.table advanced capabilities:
//   1. 128-bit SIMD vectorization & ungrouped JIT/SIMD aggregations
//   2. RangeIndex & Top-K queries
//   3. 2D / 3D SpatialIndex (whereInAABB, whereInRadius, nearest)
//   4. .brotable binary serialization, zero-copy mmap, Worker sharing, close()
//   5. Per-chunk dirty tracking & incremental ReactiveQuery
//   6. bro.kernel & bro.tensor zero-copy / bulk interop

const fs = require('fs');
const os = require('os');
const path = require('path');

const q = bro.table;
assert(typeof q.load === 'function' && typeof q.mmap === 'function', 'bro.table persistence helpers mounted');

// Build a 9,000-row (3-chunk) dataset
const tbl = q.create({
    id: 'i32',
    x: 'f64',
    y: 'f64',
    z: 'f64',
    hp: 'f32',
    score: 'i64',
    kind: 'dict',
    tag: 'string',
});

const kinds = ['hero', 'npc', 'item', 'boss'];
const rows = [];
for (let i = 0; i < 9000; i++) {
    rows.push({
        id: i + 1,
        x: ((i * 17) % 200) - 100.0,
        y: ((i * 31) % 200) - 100.0,
        z: ((i * 13) % 80) - 40.0,
        hp: 10.0 + (i % 150),
        score: BigInt(i) * 10n,
        kind: kinds[i % 4],
        tag: kinds[i % 4] + '_' + i,
    });
}
tbl.insertMany(rows);
assert(tbl.rowCount === 9000 && tbl.chunkCount === 3, '9000 rows in 3 chunks');

// ----------------------------------------------------------------------------
// 1. SIMD filter & ungrouped JIT/SIMD aggregations
// ----------------------------------------------------------------------------
q.clearJitCache();
const simdStats = tbl.query()
    .where(q.and(q.gt('hp', 100), q.le('hp', 140)))
    .runWithStats();
const simdRef = tbl.query()
    .where(q.and(q.gt('hp', 100), q.le('hp', 140)))
    .run({ jit: false, index: false });
assert(simdStats.usedJit === true && simdStats.usedSimd === true, 'SIMD filter used brass SIMD');
assert(simdStats.table.rowCount === simdRef.rowCount && simdRef.rowCount > 0,
       'SIMD filter matches scalar reference count');

const aggSimd = tbl.query()
    .agg({
        cnt: q.count(),
        sumScore: q.sum('score'),
        avgScore: q.avg('score'),
    })
    .runWithStats();
assert(aggSimd.usedJitAgg === true && aggSimd.usedSimd === true, 'ungrouped agg used SIMD kernel');
const aggRow = aggSimd.table.toArray()[0];
assert(aggRow.cnt === 9000, 'SIMD agg count = 9000');
assert(aggRow.sumScore === 9000 * 8999 * 5, 'SIMD I64 sum matches arithmetic series');
assert(Math.abs(aggRow.avgScore - 8999 * 5) < 1e-6, 'SIMD avg matches exact mean');

const fusedAgg = tbl.query()
    .where(q.ge('hp', 120))
    .agg({
        cnt: q.count(),
        minHp: q.min('hp'),
        maxHp: q.max('hp'),
        avgHp: q.avg('hp'),
    })
    .runWithStats();
const fusedRef = tbl.query()
    .where(q.ge('hp', 120))
    .agg({
        cnt: q.count(),
        minHp: q.min('hp'),
        maxHp: q.max('hp'),
        avgHp: q.avg('hp'),
    })
    .run({ jit: false, index: false });
assert(fusedAgg.usedJitAgg === true, 'filtered agg used JIT kernel');
const fr0 = fusedAgg.table.toArray()[0];
const fr1 = fusedRef.toArray()[0];
assert(fr0.cnt === fr1.cnt && Math.abs(fr0.minHp - fr1.minHp) < 1e-4 &&
       Math.abs(fr0.maxHp - fr1.maxHp) < 1e-4 && Math.abs(fr0.avgHp - fr1.avgHp) < 1e-3,
       'fused filtered JIT agg matches reference');

// ----------------------------------------------------------------------------
// 2. RangeIndex & Top-K
// ----------------------------------------------------------------------------
assert(tbl.createRangeIndex('score') === true && tbl.hasRangeIndex('score') === true, 'createRangeIndex');
const rangeStats = tbl.query()
    .where(q.ge('score', 10000))
    .where(q.lt('score', 12000))
    .runWithStats();
assert(rangeStats.usedRangeIndex === true && rangeStats.table.rowCount === 200,
       'RangeIndex range query accelerated & accurate');

const topKStats = tbl.query()
    .orderBy('score', 'desc')
    .limit(10)
    .offset(5)
    .runWithStats();
const topKRef = tbl.query()
    .orderBy('score', 'desc')
    .limit(10)
    .offset(5)
    .run({ jit: false, index: false });
assert(topKStats.usedRangeIndex === true, 'Top-K orderBy+limit used RangeIndex');
const topKA = topKStats.table.toArray();
const topKB = topKRef.toArray();
assert(topKA.length === 10 && topKA[0].id === topKB[0].id && topKA[9].id === topKB[9].id,
       'RangeIndex Top-K matches exact ordering');

// ----------------------------------------------------------------------------
// 3. 2D & 3D SpatialIndex (AABB, Radius, nearest)
// ----------------------------------------------------------------------------
assert(tbl.createSpatialIndex('pos2d', ['x', 'y'], 16.0) === true, 'createSpatialIndex 2D');
assert(tbl.createSpatialIndex('pos3d', ['x', 'y', 'z'], 16.0) === true, 'createSpatialIndex 3D');
assert(tbl.hasSpatialIndex('pos2d') === true && tbl.hasSpatialIndex('pos3d') === true, 'hasSpatialIndex');

const boxStats = tbl.query()
    .whereInAABB('pos2d', [-25, -30], [35, 20])
    .where(q.eq('kind', 'hero'))
    .runWithStats({ index: true });
const boxRef = tbl.query()
    .whereInAABB('pos2d', [-25, -30], [35, 20])
    .where(q.eq('kind', 'hero'))
    .runWithStats({ jit: false, index: false });
assert(boxStats.usedSpatialIndex === true, 'whereInAABB used SpatialIndex');
assert(boxStats.table.rowCount === boxRef.table.rowCount && boxRef.table.rowCount > 0,
       'whereInAABB matches reference');

const radStats = tbl.query()
    .whereInRadius('pos3d', [10, -10, 5], 28.0)
    .runWithStats({ index: true });
const radRef = tbl.query()
    .whereInRadius('pos3d', [10, -10, 5], 28.0)
    .runWithStats({ jit: false, index: false });
assert(radStats.usedSpatialIndex === true, 'whereInRadius used SpatialIndex');
assert(radStats.table.rowCount === radRef.table.rowCount && radRef.table.rowCount > 0,
       'whereInRadius matches reference');

const knn = tbl.nearest('pos3d', [0, 0, 0], 6).toArray();
assert(knn.length === 6, 'nearest returns 6 rows');
for (let i = 1; i < knn.length; i++) {
    assert(knn[i]._distance >= knn[i - 1]._distance - 1e-12, 'nearest sorted by _distance');
}

// ----------------------------------------------------------------------------
// 4. .brotable binary persistence, zero-copy mmap, Worker sharing & close()
// ----------------------------------------------------------------------------
const bytes = tbl.toBytes();
assert(bytes instanceof Uint8Array && bytes.byteLength > 64, 'toBytes returns Uint8Array');
const rtBytesTbl = q.fromBytes(bytes);
assert(rtBytesTbl.rowCount === 9000 && rtBytesTbl.get(4100, 'tag') === tbl.get(4100, 'tag'),
       'fromBytes round-trips full schema & strings');

const wTbl = q.create({ id: 'i32', kind: 'dict', score: 'f64' });
const wRows = [];
for (let i = 0; i < 5000; i++) {
    wRows.push({ id: i + 1, kind: kinds[i % 4], score: (i % 100) * 1.5 });
}
wTbl.insertMany(wRows);

const tmpFile = path.join(os.tmpdir(), 'bro_table_adv_' + process.pid + '_' + Date.now() + '.brotable');
let mmapSnap = null;
let worker = null;
try {
    assert(wTbl.save(tmpFile) === true, 'Table.save wrote .brotable file');
    const loaded = q.load(tmpFile);
    assert(loaded.rowCount === 5000 && loaded.get(99, 'score') === wTbl.get(99, 'score'),
           'bro.table.load matches saved table');

    mmapSnap = q.mmap(tmpFile);
    assert(mmapSnap.isMapped === true && mmapSnap.chunkCount === 2 && mmapSnap.rowCount === 5000,
           'bro.table.mmap returns mapped TableSnapshot');
    const mappedAgg = mmapSnap.query().agg({ total: q.sum('score') }).toArray()[0].total;
    const origAgg = wTbl.query().agg({ total: q.sum('score') }).toArray()[0].total;
    assert(Math.abs(mappedAgg - origAgg) < 1e-6, 'JIT query on mmap snapshot matches original');

    // Share zero-copy mmap snapshot with Worker
    worker = new Worker('../table/worker_table.js');
    let reply = null;
    worker.onmessage = (e) => { reply = e.data; };
    worker.postMessage({ snap: mmapSnap, minScore: 100.0 });
    const deadline = Date.now() + 15000;
    while (reply === null && Date.now() < deadline) {
        advanceTime(16);
        wallSleep(2);
    }
    assert(reply !== null && reply.ok === true && reply.totalRows === 5000,
           'worker queried mmap snapshot successfully');

    // COW materialize mutable copy without mutating mmap pages
    const cowMut = mmapSnap.toTable();
    cowMut.updateRow(10, { score: 7777.0 });
    assert(cowMut.get(10, 'score') === 7777.0 && mmapSnap.get(10, 'score') !== 7777.0,
           'mmapSnap.toTable() COW isolates mutations');

    worker.terminate();
    worker = null;
    assert(mmapSnap.close() === true && mmapSnap.isMapped === false, 'mmapSnap.close() releases mapping');
    mmapSnap = null;
} finally {
    if (worker) worker.terminate();
    if (mmapSnap) mmapSnap.close();
    try { fs.unlinkSync(tmpFile); } catch (e) {}
}

// ----------------------------------------------------------------------------
// 5. Per-chunk dirty tracking & incremental ReactiveQuery
// ----------------------------------------------------------------------------
const rq = tbl.query().where(q.gt('hp', 100)).reactive();
assert(rq.dirty() === false && rq.table.rowCount > 0, 'ReactiveQuery initialized eagerly');
let changeEvents = 0;
rq.onChange((ev) => {
    changeEvents++;
    assert(ev.changed === true && ev.recomputedChunks === 1 && ev.reusedChunks === 2,
           'incremental refresh recomputes only dirty chunk');
});

const verBefore = tbl.version;
tbl.updateRow(4100, { hp: 250 });  // chunk 1 only
assert(rq.dirty() === true, 'mutation dirties ReactiveQuery');
const dirtyList = tbl.dirtyChunksSince(verBefore);
assert(dirtyList.length === 1 && dirtyList[0] === 1, 'dirtyChunksSince identifies chunk 1');
const rr = rq.refresh();
assert(rr.changed === true && rr.recomputedChunks === 1 && rr.reusedChunks === 2,
       'ReactiveQuery.refresh recomputed 1 chunk and reused 2');
assert(changeEvents === 1 && rq.dirty() === false, 'onChange fired and rq clean');
rq.dispose();

// ----------------------------------------------------------------------------
// 6. bro.kernel & bro.tensor interop
// ----------------------------------------------------------------------------
if (bro.kernel && bro.kernel.available) {
    const mod = bro.kernel.compile(`
        @kernel @grid(1)
        fn k_boost(g: grid<1>, x: rw_store<f32, 1>, delta: f32) {
            let i = g.x();
            x[i] = x[i] + delta;
        }
    `, { file: 'table_kernel_interop.bk' });
    const kBoost = mod.kernel('k_boost');

    const before0 = tbl.get(0, 'hp');
    const c0Kb = tbl.chunkKernelBuffer(0, 'hp');
    assert(c0Kb.byteLength === 4096 * 4, 'chunkKernelBuffer byteLength = 4096 * 4');
    kBoost.run(4096, c0Kb, 10.0);
    tbl.notifyColumnUpdated('hp');
    assert(Math.abs(tbl.get(0, 'hp') - (before0 + 10.0)) < 1e-4,
           'chunkKernelBuffer mutated table chunk zero-copy');

    const packedKb = tbl.toKernelBuffer('hp', { residency: 'host' });
    assert(packedKb.byteLength === tbl.rowCount * 4, 'toKernelBuffer packs all alive rows');
    kBoost.run(tbl.rowCount, packedKb, 5.0);
    const written = tbl.readFromKernelBuffer('hp', packedKb);
    assert(written === tbl.rowCount, 'readFromKernelBuffer updated all rows');
    assert(Math.abs(tbl.get(0, 'hp') - (before0 + 15.0)) < 1e-4,
           'readFromKernelBuffer propagated kernel output to table');
}

if (bro.tensor && bro.tensor.available) {
    bro.tensor.init();
    const t = tbl.toTensor(['x', 'y'], { dtype: 'f32', device: 'cpu' });
    assert(t instanceof bro.tensor.GpuTensor && t.rows === 9000 && t.cols === 2,
           'toTensor exported (9000, 2) FP32 tensor');
    const back = q.fromTensor(t, ['rx', 'ry']);
    assert(back.rowCount === 9000 && Math.abs(back.get(7, 'rx') - tbl.get(7, 'x')) < 1e-4 &&
           Math.abs(back.get(7, 'ry') - tbl.get(7, 'y')) < 1e-4,
           'fromTensor round-trips tensor back into off-heap Table');
}

// ----------------------------------------------------------------------------
// 7. Production APIs: duplicate schema guard, negative index guard, stats/clone/clear/columnArray
// ----------------------------------------------------------------------------
let dupThrew = false;
try {
    q.create({ a: 'i32', a: 'f64' });
} catch (e) {
    // JS object literals deduplicate keys before native call; test array schema!
}
try {
    q.create([['a', 'i32'], ['a', 'f64']]);
} catch (e) {
    dupThrew = true;
}
assert(dupThrew === true, 'duplicate column in schema throws');

assert(tbl.row(-1) === undefined && tbl.get(-5, 'id') === undefined, 'negative Table indices return undefined');
assert(tbl.updateRow(-1, { hp: 1 }) === false && tbl.removeRow(-1) === false, 'negative mutations return false');
assert(tbl.row(10).id === 11, 'Table.row returns expected row without snapshotting');

const tblStats = tbl.stats();
assert(tblStats.rowCount === 9000 && tblStats.nullBitmapBytes === 0 && tblStats.rangeIndexes === 1,
       'dense table stats have zero nullBitmapBytes');

const cloned = tbl.clone();
assert(cloned.rowCount === tbl.rowCount && cloned.hasRangeIndex('score') === true, 'clone copies rows and indexes');
cloned.clear();
assert(cloned.rowCount === 0 && cloned.chunkCount === 0 && tbl.rowCount === 9000, 'clear empties clone cleanly');

const denseHp = tbl.columnArray('hp');
assert(denseHp instanceof Float32Array && denseHp.length === 9000, 'columnArray returns contiguous Float32Array when dense');
const dictKinds = tbl.columnArray('kind');
assert(Array.isArray(dictKinds) && dictKinds.length === 9000 && typeof dictKinds[0] === 'string',
       'columnArray returns string Array for Dict columns');

// ----------------------------------------------------------------------------
// 8. Nullability, isNull / isNotNull / coalesce, count(col) & SIMD simdWidth / explain()
// ----------------------------------------------------------------------------
const nt = q.create({ id: 'i32', grp: 'dict', val: 'f64', x: 'f64', y: 'f64' });
nt.insertMany([
    { id: 1, grp: 'a', val: 10.0, x: 1.0, y: 1.0 },
    { id: 2, grp: 'a', val: null, x: 2.0, y: 2.0 },
    { id: 3, grp: null, val: 30.0, x: 3.0, y: 3.0 },
    { id: 4, grp: 'b', val: 40.0, x: 50.0, y: 50.0 },
]);
nt.createIndex('id');
nt.createRangeIndex('val');
nt.createSpatialIndex('sp', ['x', 'y'], 10.0);

const ntColArr = nt.columnArray('val');
assert(Array.isArray(ntColArr) && ntColArr.length === 4 && ntColArr[1] === null && ntColArr[2] === 30.0,
       'columnArray falls back to Array preserving null when column has nulls');

const isNullRows = nt.query().where(q.isNull('val')).toArray();
assert(isNullRows.length === 1 && isNullRows[0].id === 2, 'isNull predicate matches null row');
const coalRows = nt.query()
    .where(q.col('val').isNull())
    .select({ id: 'id', c: q.col('val').coalesce(-7.5), added: q.col('val').add(1.0) })
    .toArray();
assert(coalRows.length === 1 && coalRows[0].c === -7.5 && coalRows[0].added === null,
       'coalesce & arithmetic null propagation work');

const cntBoth = nt.query()
    .agg({ total: q.count(), nonNullVal: q.count(q.col('val'), 'nonNullVal'), sumVal: q.sum('val'), avgVal: q.avg('val') })
    .toArray()[0];
assert(cntBoth.total === 4 && cntBoth.nonNullVal === 3 && cntBoth.sumVal === 80 &&
       Math.abs(cntBoth.avgVal - 80 / 3) < 1e-6,
       'count(col) skips nulls while count() counts all rows');

const exSimd = tbl.query().where(q.gt('hp', 50)).explain();
assert(exSimd.strategy === 'jit_simd' && exSimd.willUseSimd === true &&
       (exSimd.simdWidth === 256 || exSimd.simdWidth === 128),
       'explain reports jit_simd and simdWidth');
const runSimd = tbl.query().where(q.gt('hp', 50)).runWithStats();
assert(runSimd.simdWidth === exSimd.simdWidth, 'runWithStats matches explain simdWidth');

const exHash = nt.query().where(q.eq('id', 1)).explain();
assert(exHash.strategy === 'hash_index' && exHash.willUseIndex === true && exHash.indexColumn === 'id',
       'explain reports hash_index');
const exSpatial = nt.query().whereInRadius('sp', [0, 0], 5).explain();
assert(exSpatial.strategy === 'spatial_index' && exSpatial.willUseSpatialIndex === true,
       'explain reports spatial_index');

// ----------------------------------------------------------------------------
// 9. Spatial query update() & remove() + .brotable v2 index/null persistence
// ----------------------------------------------------------------------------
const spUpd = nt.query().whereInAABB('sp', [0, 0], [5, 5]).update({ val: q.coalesce(q.col('val'), 0).add(5) });
assert(spUpd.usedSpatialIndex === true && spUpd.affectedRows === 3, 'spatial AABB update works');
assert(nt.get(1, 'val') === 5.0, 'null row transitioned to 5.0 via coalesce+update');

const ntFile = path.join(os.tmpdir(), 'bro_table_v2_js_test.brotable');
try {
    nt.save(ntFile);
    const reloaded = q.load(ntFile);
    assert(reloaded.hasIndex('id') && reloaded.hasRangeIndex('val') && reloaded.hasSpatialIndex('sp'),
           '.brotable v2 restored Hash, Range, and Spatial indexes on load');
    assert(reloaded.get(2, 'grp') === null && reloaded.get(1, 'val') === 5.0,
           '.brotable v2 preserved nulls and updated cells');

    const mSnap = q.mmap(ntFile);
    assert(mSnap.isMapped === true && mSnap.hasSpatialIndex('sp') === true && mSnap.stats().nullBitmapBytes > 0,
           '.brotable v2 mmap preserves indexes and zero-copy null bitmap');
    assert(mSnap.row(2).grp === null && mSnap.get(0, 'val') === 15.0, 'mmap row/get preserves null semantics');
    mSnap.close();
} finally {
    try { fs.unlinkSync(ntFile); } catch (e) {}
}

const spDel = nt.query().whereInRadius('sp', [50, 50], 2).remove();
assert(spDel.usedSpatialIndex === true && spDel.affectedRows === 1 && nt.rowCount === 3,
       'spatial radius remove deleted target row');

console.log('test_table_advanced OK');
