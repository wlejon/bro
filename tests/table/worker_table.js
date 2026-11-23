// Worker companion for test_table_worker.js: receives immutable TableSnapshot
// via postMessage zero-copy sharing, queries it with brass JIT inside Worker
// realm, verifies zero-copy TypedArray views, and sends a derived snapshot back.

self.onmessage = (e) => {
    const q = bro.table;
    const { snap, minScore } = e.data;
    if (!snap || !q) {
        self.postMessage({ ok: false, error: 'missing snap or bro.table in worker' });
        return;
    }

    const totalRows = snap.rowCount;
    const view = snap.chunkColumnView(0, 'score');
    const indexed = snap.where({ id: 100 }).toArray();

    const filtered = snap.query()
        .where(q.ge('score', minScore))
        .select({
            id: 'id',
            kind: 'kind',
            boosted: q.col('score').mul(2).add(1),
        })
        .orderBy('id', 'asc')
        .run();

    const agg = snap.query()
        .groupBy('kind')
        .agg({ cnt: q.count(), total: q.sum('score') })
        .orderBy('kind', 'asc')
        .toArray();

    self.postMessage({
        ok: true,
        totalRows,
        viewType: view.constructor.name,
        viewSample: view[10],
        indexedTag: indexed.length === 1 ? indexed[0].kind : '',
        filteredCount: filtered.rowCount,
        agg,
        derivedSnap: filtered.snapshot(),
    });
};
