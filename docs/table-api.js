// ============================================================================
// bro.table: off-heap chunked columnar store & brass JIT query engine
// ============================================================================
//
// An off-heap, chunk-oriented columnar table (`bro::table::Table`) with:
// - 4096-row fixed-capacity chunks (`bro.table.CHUNK_ROWS`) for zero-copy
//   TypedArray views whose backing pointers never move during appends.
// - Copy-on-write immutable snapshots (`TableSnapshot`) with O(1) refcounted
//   zero-copy sharing across threads and Worker `postMessage`.
// - Typed expression builder IR (`bro.table.col`, arithmetic/math/bitwise,
//   comparisons, dict/string predicates, conditionals) compiled by `brass` JIT
//   into register-native chunk kernels and run in parallel across chunks.
// - Secondary hash indexes (`createIndex`), grouped/ungrouped aggregates,
//   bounded top-K sorting, inner/left hash joins, and in-place updates/deletes.
//
// Column types:
//   'i32'    Int32Array      signed 32-bit integer
//   'u32'    Uint32Array     unsigned 32-bit integer
//   'i64'    BigInt64Array   signed 64-bit integer
//   'f32'    Float32Array    32-bit float
//   'f64'    Float64Array    64-bit float
//   'bool'   Uint8Array      1-byte boolean (0/1)
//   'dict'   Uint32Array     interned string IDs (zero-copy decoded via dict)
//   'string' (no view)       per-chunk UTF-8 arena + {offset, len} spans
//
// ----------------------------------------------------------------------------
// Creation & Mutation
// ----------------------------------------------------------------------------
//   const q = bro.table;
//   const tbl = q.create({
//       id: 'i32',
//       kind: 'dict',
//       hp: 'f32',
//       score: 'f64',
//       active: 'bool',
//       name: 'string',
//   });
//   // Or ordered schema array: q.create([['id', 'i32'], ['hp', 'f32']])
//
//   tbl.insert({ id: 1, kind: 'hero', hp: 100, score: 42.5, active: true, name: 'A' });
//   tbl.insertMany([ ...rows ]);
//   tbl.insertMany({ id: new Int32Array(...), hp: new Float32Array(...) }); // columnar fast path
//   tbl.updateRow(aliveIdx, { hp: 90 });
//   tbl.removeRow(aliveIdx);
//   tbl.compact(); // packs tombstones, shrinks string arenas, rebuilds indexes
//
// ----------------------------------------------------------------------------
// Secondary Indexes, Views & Snapshots
// ----------------------------------------------------------------------------
//   tbl.createIndex('id');
//   const view = tbl.chunkColumnView(0, 'hp'); // zero-copy Float32Array (COW-pinned)
//   const snap = tbl.snapshot();               // immutable COW TableSnapshot
//   worker.postMessage({ snap });              // zero-copy O(1) transfer/share
//
// ----------------------------------------------------------------------------
// Query Builder IR & Execution (compiled by brass JIT + CompiledPlanCache)
// ----------------------------------------------------------------------------
//   // Prefix style OR fluent Expr method chaining, plus { col: value } equality:
//   const res = tbl.query()
//       .where(q.and(q.gt('hp', 50), q.eq('kind', 'hero')))
//       .select({ id: 'id', doubled: q.col('hp').mul(2).add(q.col('score')) })
//       .orderBy('doubled', 'desc')
//       .limit(20)
//       .offset(0)
//       .run(); // returns off-heap Table
//
//   // Grouping & Aggregates:
//   const agg = tbl.query()
//       .where({ active: true })
//       .groupBy('kind')
//       .agg({ cnt: q.count(), totalScore: q.sum('score'), avgHp: q.avg('hp') })
//       .orderBy('totalScore', 'desc')
//       .toArray();
//
//   // Joins ('join' = inner join, 'leftJoin' = left outer join):
//   const joined = leftTbl.query()
//       .join(rightTbl, 'kind', 'kind_name') // or composite keys: ['k1','k2'], ['r1','r2']
//       .select({ id: 'left.id', bonus: 'right.bonus' })
//       .run();
//
//   // Bulk in-place JIT mutations:
//   tbl.updateWhere(q.lt('hp', 20), { hp: q.col('hp').add(10), active: false });
//   tbl.deleteWhere(q.eq('active', false));
//
//   // JIT cache inspection:
//   const { hits, misses, entries } = bro.table.jitCacheStats();
//   bro.table.clearJitCache();
