// Worker + zero-copy TableSnapshot sharing + concurrent mutation isolation test
const q = bro.table;

const tbl = q.create({
    id: 'i32',
    kind: 'dict',
    score: 'f64',
});

const kinds = ['alpha', 'beta', 'gamma', 'delta'];
const rows = [];
for (let i = 0; i < 8000; i++) {
    rows.push({
        id: i + 1,
        kind: kinds[i % 4],
        score: (i % 100) * 1.5,
    });
}
tbl.insertMany(rows);
tbl.createIndex('id');

const expectedBefore10 = tbl.get(10, 'score');
const snap = tbl.snapshot();

const worker = new Worker('../table/worker_table.js');
let reply = null;
worker.onmessage = (e) => { reply = e.data; };

// Zero-copy share of immutable TableSnapshot across thread boundary
worker.postMessage({ snap, minScore: 100.0 });

// Heavily mutate main-thread table immediately while worker queries `snap`
tbl.updateWhere(q.gt('id', 0), { score: -1.0 });
tbl.deleteWhere(q.eq('kind', 'alpha'));
tbl.compact();

const deadline = Date.now() + 15000;
while (reply === null && Date.now() < deadline) {
    advanceTime(16);
    wallSleep(2);
}

assert(reply !== null, 'worker replied');
assert(reply.ok === true, 'worker query succeeded: ' + reply.error);
assert(reply.totalRows === 8000, 'worker saw all 8000 rows from pre-mutation snapshot');
assert(reply.viewType === 'Float64Array', 'worker got Float64Array chunk view');
assert(reply.viewSample === expectedBefore10, 'worker view kept pre-mutation value');
assert(reply.indexedTag === 'delta', 'worker index lookup on snapshot succeeded');
assert(reply.filteredCount > 0, 'worker produced filtered derived table');
assert(reply.agg.length === 4, 'worker grouped 4 kinds');
assert(reply.derivedSnap && reply.derivedSnap.rowCount === reply.filteredCount,
       'main thread received derived TableSnapshot back from worker');

const mainCheck = reply.derivedSnap.query().limit(3).toArray();
assert(mainCheck.length === 3 && mainCheck[0].boosted >= 201,
       'main thread queried derived TableSnapshot returned from worker');

worker.terminate();
console.log('test_table_worker OK');
