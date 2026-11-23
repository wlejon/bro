// Headless integration test for bro.table (off-heap columnar store + brass JIT queries)
const q = bro.table;
assert(q && typeof q.create === 'function', 'bro.table namespace is mounted');
assert(q.CHUNK_ROWS === 4096, 'CHUNK_ROWS is 4096');

// 1. All 8 column types + multi-chunk batch insert (10,000 rows = 3 chunks)
const tbl = q.create({
    id: 'i32',
    flags: 'u32',
    big: 'i64',
    x: 'f32',
    score: 'f64',
    active: 'bool',
    kind: 'dict',
    label: 'string',
});

const N = 10000;
const ids = new Int32Array(N);
const flags = new Uint32Array(N);
const bigs = new BigInt64Array(N);
const xs = new Float32Array(N);
const scores = new Float64Array(N);
const actives = new Uint8Array(N);

const kinds = ['hero', 'npc', 'item', 'boss'];
for (let i = 0; i < N; i++) {
    ids[i] = i - 50;
    flags[i] = (i % 5 === 0) ? 0x80000001 : (i + 1);
    bigs[i] = BigInt(i) * 100000n;
    xs[i] = (i % 100) * 0.5;
    scores[i] = (i % 200) * 1.25;
    actives[i] = (i % 2 === 0) ? 1 : 0;
}

// Columnar bulk insert for numeric/bool cols, then verify Dict/String via row insert
tbl.insertMany({ id: ids, flags: flags, big: bigs, x: xs, score: scores, active: actives });
assert(tbl.rowCount === N, 'bulk insert rowCount');
assert(tbl.chunkCount === 3, 'multi-chunk count = 3');

// 2. Zero-copy TypedArray chunk views + COW snapshot isolation on mutation
const viewBefore = tbl.chunkColumnView(0, 'score');
assert(viewBefore instanceof Float64Array, 'score view is Float64Array');
assert(viewBefore.length === 4096, 'chunk 0 has 4096 rows');
assert(viewBefore[10] === scores[10], 'view matches score[10]');

const idView = tbl.chunkColumnView(0, 'id');
assert(idView instanceof Int32Array, 'id view is Int32Array');
const bigView = tbl.chunkColumnView(0, 'big');
assert(bigView instanceof BigInt64Array, 'big view is BigInt64Array');
assert(bigView[5] === 500000n, 'BigInt64Array zero-copy read matches');

// Updating table must COW-clone chunk column so existing external TypedArray view stays isolated!
const snapBefore = tbl.snapshot();
tbl.updateRow(10, { score: 9999.0 });
assert(viewBefore[10] === scores[10], 'prior zero-copy view stays immutable after table mutation');
assert(snapBefore.get(10, 'score') === scores[10], 'snapshot stays immutable after table mutation');
assert(tbl.get(10, 'score') === 9999.0, 'table updated row 10');

// Restore row 10 and insert a typed Dict+String table for full query tests
tbl.updateRow(10, { score: scores[10] });

const entities = q.create([
    ['id', 'i32'],
    ['kind', 'dict'],
    ['hp', 'f32'],
    ['bonus', 'f64'],
    ['alive', 'bool'],
    ['tag', 'string'],
]);
const batch = [];
for (let i = 0; i < 6000; i++) {
    batch.push({
        id: i + 1,
        kind: kinds[i % 4],
        hp: 20 + (i % 100),
        bonus: (i % 10) * 1.5,
        alive: i % 3 !== 0,
        tag: kinds[i % 4] + '_' + i,
    });
}
entities.insertMany(batch);
assert(entities.rowCount === 6000, 'entities count 6000');

// 3. Secondary index + JIT Plan cache hits
assert(entities.createIndex('id') === true, 'createIndex(id)');
const idxRes = entities.where({ id: 4501 }).toArray();
assert(idxRes.length === 1 && idxRes[0].tag === 'hero_4500', 'indexed lookup exact');
const idxNpc = entities.where({ id: 4502 }).toArray();
assert(idxNpc.length === 1 && idxNpc[0].tag === 'npc_4501', 'indexed lookup npc exact');

q.clearJitCache();
const filteredQ = entities.query()
    .where(q.and(q.col('alive'), q.col('hp').ge(80), q.ne('kind', 'boss')))
    .select({
        id: 'id',
        kind: 'kind',
        eff: q.col('hp').mul(2).add(q.col('bonus')),
        adj: q.sqrt(q.col('hp')),
    });

const r1 = filteredQ.run();
const stats1 = q.jitCacheStats();
assert(stats1.misses >= 1, 'first run compiled JIT kernel');
const r2 = filteredQ.run();
const stats2 = q.jitCacheStats();
assert(stats2.hits >= stats1.hits + 1, 'second run hit JIT plan cache');
assert(r1.rowCount === r2.rowCount && r1.rowCount > 0, 'cached query returns matching rows');

// Check zero-copy Dict decoding + computed math projection on result Table
const r1Arr = r1.toArray();
assert(r1Arr[0].kind !== 'boss' && Number.isFinite(r1Arr[0].eff) && r1Arr[0].eff >= 160 &&
       Math.abs(r1Arr[0].adj * r1Arr[0].adj - entities.get(r1Arr[0].id - 1, 'hp')) < 1e-4,
       'projected fields match exact formula');

// 4. Hybrid string predicate + OrderBy Top-K + Offset
const top5 = entities.query()
    .where(q.contains('tag', 'hero_4'))
    .where(q.gt('hp', 50))
    .orderBy('hp', 'desc')
    .orderBy('id', 'asc')
    .limit(5)
    .offset(1)
    .toArray();
assert(top5.length === 5, 'top5 with offset length');
assert(top5[0].hp >= top5[1].hp && top5[1].hp >= top5[4].hp, 'descending hp order');

// OrderBy computed select alias
const bySelectAlias = entities.query()
    .select({ id: 'id', doubled: q.col('hp').mul(2) })
    .orderBy('doubled', 'desc')
    .limit(5)
    .toArray();
assert(bySelectAlias.length === 5 && bySelectAlias[0].doubled >= bySelectAlias[4].doubled,
       'orderBy computed select alias works');

// 5. GroupBy + Aggregates
const grouped = entities.query()
    .where({ alive: true })
    .groupBy('kind')
    .agg({
        cnt: q.count(),
        sumHp: q.sum('hp'),
        maxHp: q.max('hp'),
        avgBonus: q.avg('bonus'),
    })
    .orderBy('kind', 'asc')
    .toArray();
assert(grouped.length === 4, '4 kind groups');
assert(grouped[0].cnt === 1000, 'each kind has 1000 alive rows out of 6000 (2/3 of 1500)');

const groupedOrdered = entities.query()
    .groupBy('kind')
    .agg({ total: q.sum('hp') })
    .orderBy('total', 'desc')
    .toArray();
assert(groupedOrdered.length === 4 && groupedOrdered[0].total >= groupedOrdered[3].total,
       'orderBy aggregate alias works');

// 6. Inner & Left Joins
const meta = q.create({ name: 'string', mult: 'f64' });
meta.insertMany([
    { name: 'hero', mult: 3.0 },
    { name: 'boss', mult: 5.0 },
]);
const innerJ = entities.query()
    .where(q.lt('id', 9))
    .join(meta, 'kind', 'name')
    .select({ id: 'left.id', kind: 'kind', mult: 'right.mult' })
    .orderBy('id', 'asc')
    .toArray();
assert(innerJ.length === 4, 'inner join on id < 9 (heroes + bosses = 4)');

const leftJ = entities.query()
    .where(q.lt('id', 5))
    .leftJoin(meta, 'kind', 'name')
    .select({ id: 'left.id', mult: 'right.mult' })
    .orderBy('id', 'asc')
    .toArray();
assert(leftJ.length === 4 && leftJ[0].mult === 3.0 && leftJ[1].mult === null, 'left join unmatched is null');

// 7. In-place JIT updateWhere + deleteWhere + compact
const upd = entities.updateWhere(q.eq('kind', 'boss'), { hp: q.col('hp').add(50) });
assert(upd.usedJit === true && upd.affectedRows === 1500, 'updateWhere ran JIT on 1500 bosses');
const del = entities.deleteWhere(q.eq('kind', 'npc'));
assert(del.usedJit === true && del.affectedRows === 1500, 'deleteWhere removed 1500 npcs');
assert(entities.rowCount === 4500, '4500 rows remain before compact');
entities.compact();
assert(entities.rowCount === 4500, '4500 rows remain after compact');
assert(entities.chunkCount === 2, 'compacted down to 2 chunks');
assert(entities.where({ id: 4502 }).count() === 0, 'index updated after delete+compact');
assert(entities.where({ id: 4501 }).count() === 1, 'surviving hero stays indexed after delete+compact');

console.log('test_table OK');
