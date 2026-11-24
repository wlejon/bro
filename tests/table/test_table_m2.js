// Headless JS integration test for Milestone 2:
//   1. Streaming CSV/TSV ingest (fromCSV & fromCSVFile, quoted newlines, nulls, schema inference)
//   2. Native Apache Parquet read/write (fromParquet, fromParquetFile, saveParquet, toParquetBytes, LZ4)
//   3. Incremental ReactiveQuery for groupBy/agg & leftJoin/innerJoin
//   4. Compressed .brotable v4 (compress: true) & Multi-table 4096B-aligned bundles

const fs = require('fs');
const os = require('os');
const path = require('path');

const q = bro.table;
const tmpDir = os.tmpdir();
const suffix = `${Date.now()}_${Math.floor(Math.random() * 1e6)}`;

// ----------------------------------------------------------------------------
// 1. Streaming CSV/TSV ingest
// ----------------------------------------------------------------------------
{
    const csvText = [
        'id,region,score,note,active',
        '1,"us,east",95.5,"line1\nline2",true',
        '2,"eu-west",,NA,false',
        '3,"us,east",82.25,"quoted ""hello""",true',
        '4,"ap-south",100.0,"plain",false',
    ].join('\n') + '\n';

    const tMem = q.fromCSV(csvText);
    assert(tMem.rowCount === 4 && tMem.columnCount === 5, 'fromCSV parsed 4 rows x 5 cols');
    assert(tMem.get(0, 'id') === 1 && tMem.get(0, 'region') === 'us,east', 'quoted comma preserved');
    assert(tMem.get(0, 'note') === 'line1\nline2', 'quoted newline preserved');
    assert(tMem.get(1, 'score') === null && tMem.get(1, 'note') === null, 'empty and NA treated as null');
    assert(tMem.get(2, 'note') === 'quoted "hello"', 'escaped double quote preserved');
    assert(tMem.get(3, 'active') === false, 'bool column inferred cleanly');

    // Large >10k-row streaming file check
    const csvPath = path.join(tmpDir, `bro_m2_${suffix}.tsv`);
    try {
        const lines = ['k\tval\ttag'];
        for (let i = 0; i < 10500; i++) {
            lines.push(`${i}\t${(i * 0.25).toFixed(2)}\tcat_${i % 5}`);
        }
        fs.writeFileSync(csvPath, lines.join('\n') + '\n');
        const tStream = q.fromCSVFile(csvPath, { delimiter: '\t' });
        assert(tStream.rowCount === 10500 && tStream.chunkCount === 3, 'fromCSVFile streamed 10500 rows into 3 chunks');
        assert(tStream.get(10000, 'tag') === 'cat_0' && Math.abs(tStream.get(10000, 'val') - 2500.0) < 1e-4,
               'streaming CSV values verified at tail');
    } finally {
        try { fs.unlinkSync(csvPath); } catch (_) {}
    }
}

// ----------------------------------------------------------------------------
// 2. Native Apache Parquet round-trip (Memory & File + LZ4 + Column Projection)
// ----------------------------------------------------------------------------
{
    const tbl = q.create({ id: 'i32', big: 'i64', f: 'f32', d: 'f64', ok: 'bool', kind: 'dict', txt: 'string' });
    const rows = [];
    for (let i = 0; i < 200; i++) {
        rows.push({
            id: i,
            big: BigInt(i) * 1000000n,
            f: i * 1.5,
            d: (i % 7 === 0) ? null : i * 0.125,
            ok: (i % 2) === 0,
            kind: ['alpha', 'beta', 'gamma'][i % 3],
            txt: (i % 10 === 0) ? null : ('row_' + i),
        });
    }
    tbl.insertMany(rows);

    const pqBytes = tbl.toParquetBytes({ compress: 'lz4' });
    assert(pqBytes instanceof Uint8Array && pqBytes.byteLength > 20, 'toParquetBytes generated LZ4_RAW bytes');
    const rt = q.fromParquet(pqBytes, { columns: ['id', 'd', 'kind', 'txt'], stringAsDict: true });
    assert(rt.rowCount === 200 && rt.columnCount === 4, 'fromParquet projected 4 columns x 200 rows');
    assert(rt.get(7, 'd') === null && rt.get(10, 'txt') === null, 'Parquet null levels preserved');
    assert(Math.abs(rt.get(5, 'd') - 0.625) < 1e-9 && rt.get(5, 'kind') === 'gamma' && rt.get(5, 'txt') === 'row_5',
           'Parquet LZ4 values round-tripped cleanly');

    const pqPath = path.join(tmpDir, `bro_m2_${suffix}.parquet`);
    try {
        tbl.saveParquet(pqPath, { compress: true });
        const fromDisk = q.fromParquetFile(pqPath);
        assert(fromDisk.rowCount === 200 && fromDisk.get(199, 'id') === 199 &&
               fromDisk.get(199, 'ok') === false, 'fromParquetFile verified');
    } finally {
        try { fs.unlinkSync(pqPath); } catch (_) {}
    }
}

// ----------------------------------------------------------------------------
// 3. Incremental ReactiveQuery for groupBy/agg & leftJoin
// ----------------------------------------------------------------------------
{
    const left = q.create({ id: 'i32', grp: 'dict', dept_id: 'i32', val: 'f64' });
    const right = q.create({ id: 'i32', label: 'string', weight: 'f64' });
    right.insertMany([
        { id: 0, label: 'eng', weight: 10.0 },
        { id: 1, label: 'sci', weight: 20.0 },
        { id: 2, label: 'ops', weight: 30.0 },
    ]);

    const batch = [];
    for (let i = 0; i < 16384; i++) { // 4 full chunks (4096 each)
        batch.push({ id: i, grp: 'g' + (i % 4), dept_id: i % 4, val: 1.0 + (i % 10) });
    }
    left.insertMany(batch);
    assert(left.chunkCount === 4, '4 chunks created for reactive tests');

    // 3A. Incremental groupBy + agg
    const rqAgg = left.query()
        .where(q.ge('val', 1.0))
        .groupBy(['grp'])
        .agg({
            n: q.count(),
            total: q.sum('val'),
            mean: q.avg('val'),
            hi: q.max('val'),
        })
        .orderBy('grp')
        .reactive();

    left.updateRow(5000, { val: 500.0 }); // chunk 1 only!
    const rAgg = rqAgg.refresh();
    assert(rAgg.changed === true && rAgg.usedIncrementalAgg === true, 'ReactiveQuery used incremental agg');
    assert(rAgg.recomputedChunks === 1 && rAgg.reusedChunks === 3,
           `expected 1 recomputed & 3 reused, got ${rAgg.recomputedChunks}/${rAgg.reusedChunks}`);

    const refAgg = left.query()
        .where(q.ge('val', 1.0))
        .groupBy(['grp'])
        .agg({ n: q.count(), total: q.sum('val'), mean: q.avg('val'), hi: q.max('val') })
        .orderBy('grp')
        .run();
    assert(rAgg.table.rowCount === refAgg.rowCount, 'incremental groupBy rowCount matches reference');
    for (let i = 0; i < refAgg.rowCount; i++) {
        assert(Math.abs(rAgg.table.get(i, 'total') - refAgg.get(i, 'total')) < 1e-6 &&
               Math.abs(rAgg.table.get(i, 'mean') - refAgg.get(i, 'mean')) < 1e-6 &&
               rAgg.table.get(i, 'hi') === refAgg.get(i, 'hi'),
               `groupBy[${i}] matches fresh reference`);
    }
    rqAgg.dispose();

    // 3B. Incremental leftJoin
    const rightSnap = right.snapshot();
    const rqJoin = left.query()
        .where(q.lt('id', 20000))
        .leftJoin(rightSnap, 'dept_id', 'id')
        .select({ lid: 'left.id', dlabel: 'right.label', val: 'left.val' })
        .reactive();

    left.updateRow(13000, { val: 999.0 }); // chunk 3 only!
    const rJoin = rqJoin.refresh();
    assert(rJoin.changed === true && rJoin.usedIncrementalJoin === true, 'ReactiveQuery used incremental join');
    assert(rJoin.recomputedChunks === 1 && rJoin.reusedChunks === 3,
           `join expected 1 recomputed & 3 reused, got ${rJoin.recomputedChunks}/${rJoin.reusedChunks}`);
    assert(rJoin.table.rowCount === 16384 && rJoin.table.get(13000, 'val') === 999.0 &&
           rJoin.table.get(3, 'dlabel') === null,
           'leftJoin unmatched right row yielded null and dirty row updated');
    rqJoin.dispose();
}

// ----------------------------------------------------------------------------
// 4. Compressed .brotable v4 & Multi-Table 4096B-Aligned Bundles
// ----------------------------------------------------------------------------
{
    const a = q.create({ x: 'i32', y: 'f64', name: 'string' });
    const b = q.create({ k: 'i32', score: 'f32' });
    const rowsA = [];
    const rowsB = [];
    for (let i = 0; i < 4000; i++) {
        rowsA.push({ x: i % 16, y: 42.5, name: 'repeating_prefix_' + (i % 8) });
        rowsB.push({ k: i, score: i * 0.5 });
    }
    a.insertMany(rowsA);
    b.insertMany(rowsB);
    a.createIndex('x');

    const rawBytes = a.toBytes();
    const cmpBytes = a.toBytes({ compress: true });
    assert(cmpBytes.byteLength < rawBytes.byteLength,
           `compressed .brotable v4 (${cmpBytes.byteLength}) smaller than v3 (${rawBytes.byteLength})`);
    const loadedFromBytes = q.fromBytes(cmpBytes);
    assert(loadedFromBytes.rowCount === 4000 && loadedFromBytes.hasIndex('x') &&
           loadedFromBytes.get(123, 'name') === 'repeating_prefix_' + (123 % 8),
           'v4 fromBytes round-tripped cleanly');

    const cmpPath = path.join(tmpDir, `bro_m2_v4_${suffix}.brotable`);
    const bndPath = path.join(tmpDir, `bro_m2_bnd_${suffix}.brotable`);
    let mapped = null;
    try {
        a.save(cmpPath, { compress: true });
        const loadedV4 = q.load(cmpPath);
        assert(loadedV4.rowCount === 4000, 'v4 load from file OK');

        let mmapThrew = false;
        try {
            q.mmap(cmpPath);
        } catch (_) {
            mmapThrew = true;
        }
        assert(mmapThrew, 'mmap rejects compressed v4 cleanly');

        q.saveBundle(bndPath, { alpha: a, beta: b }, { compress: false });
        const loadedBundle = q.loadBundle(bndPath);
        assert(loadedBundle.alpha.rowCount === 4000 && loadedBundle.beta.rowCount === 4000,
               'loadBundle loaded both tables');

        mapped = q.mmapBundle(bndPath, { verify: true });
        assert(mapped.alpha.isMapped === true && mapped.beta.isMapped === true,
               'mmapBundle shared zero-copy mapping across tables');
        assert(mapped.alpha.get(100, 'y') === 42.5 && mapped.beta.get(100, 'score') === 50.0,
               'mmapBundle values match');
        mapped.alpha.close();
        mapped.beta.close();
        mapped = null;
    } finally {
        if (mapped) {
            try { mapped.alpha.close(); } catch (_) {}
            try { mapped.beta.close(); } catch (_) {}
        }
        try { fs.unlinkSync(cmpPath); } catch (_) {}
        try { fs.unlinkSync(bndPath); } catch (_) {}
    }
}

console.log('test_table_m2: OK');
