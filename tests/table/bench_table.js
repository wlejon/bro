// JS benchmark comparing bro.table vs native JS Array<Object> on 200,000 rows.
const q = bro.table;
const N = 200000;

function bench(name, fn) {
    const t0 = Date.now();
    const res = fn();
    const dt = Date.now() - t0;
    console.log(name.padEnd(40) + ': ' + dt + ' ms');
    return res;
}

const ids = new Int32Array(N);
const hps = new Float32Array(N);
const scores = new Float64Array(N);
const jsArr = new Array(N);
const kinds = ['hero', 'npc', 'boss', 'item'];

for (let i = 0; i < N; i++) {
    ids[i] = i + 1;
    hps[i] = 20 + (i % 180);
    scores[i] = (i % 5000) * 0.25;
    jsArr[i] = { id: i + 1, kind: kinds[i & 3], hp: hps[i], score: scores[i] };
}

const tbl = q.create({ id: 'i32', kind: 'dict', hp: 'f32', score: 'f64' });
bench('bro.table columnar+row setup (200k)', () => {
    const kindArr = new Array(N);
    for (let i = 0; i < N; i++) kindArr[i] = kinds[i & 3];
    // Fast columnar TypedArray insert for numeric cols, or hybrid insert
    const tNum = q.create({ id: 'i32', hp: 'f32', score: 'f64' });
    tNum.insertMany({ id: ids, hp: hps, score: scores });
    return tNum;
});

const kindCol = new Array(N);
for (let i = 0; i < N; i++) kindCol[i] = kinds[i & 3];
const tNum = q.create({ id: 'i32', hp: 'f32', score: 'f64' });
bench('bro.table typed insertMany (200k)', () => tNum.insertMany({ id: ids, hp: hps, score: scores }));

tNum.createIndex('id');
tNum.createRangeIndex('score');

const simdFilterQuery = tNum.query().where(q.gt('score', 1100.0)).select({ id: 'id' });
simdFilterQuery.run(); // warm JIT cache

const aggQuery = tNum.query().agg({
    cnt: q.count(),
    sum: q.sum('score'),
    avg: q.avg('score'),
    max: q.max('score'),
});
aggQuery.run(); // warm JIT cache

const rTable = bench('bro.table SIMD filter x10', () => {
    let s = 0;
    for (let k = 0; k < 10; k++) {
        const res = simdFilterQuery.runWithStats();
        s += res.table.rowCount;
    }
    return s;
});

const rJs = bench('JS Array.filter x10', () => {
    let s = 0;
    for (let k = 0; k < 10; k++) {
        const out = jsArr.filter((r) => r.score > 1100.0);
        s += out.length;
    }
    return s;
});
assert(rTable === rJs, 'filter count parity');

bench('bro.table SIMD ungrouped agg x10', () => {
    let total = 0;
    for (let k = 0; k < 10; k++) {
        total += aggQuery.toArray()[0].sum;
    }
    return total;
});

bench('JS Array agg loop x10', () => {
    let total = 0;
    for (let k = 0; k < 10; k++) {
        let sum = 0;
        let max = -Infinity;
        for (let i = 0; i < N; i++) {
            const sc = jsArr[i].score;
            sum += sc;
            if (sc > max) max = sc;
        }
        total += sum;
    }
    return total;
});

// GroupBy on Dict key
for (let i = 0; i < N; i++) {
    jsArr[i] = { id: i + 1, kind: kinds[i & 3], score: scores[i] };
}
tbl.insertMany({ id: ids, kind: kindCol, hp: hps, score: scores });
const grpQuery = tbl.query().groupBy('kind').agg({ cnt: q.count(), total: q.sum('score') });
grpQuery.toArray();
bench('bro.table parallel groupBy (200k)', () => grpQuery.toArray());

const idxQuery = tNum.query().where({ id: 123456 });
bench('bro.table HashIndex x1000', () => {
    let hits = 0;
    for (let k = 0; k < 1000; k++) hits += idxQuery.count();
    return hits;
});

console.log('bench_table JS OK');
