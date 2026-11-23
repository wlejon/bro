#include "table/query_exec.h"
#include "table/table.h"
#include "table/table_io.h"
#include "table/table_reactive.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace bro::table;

#define CHECK(cond)                                                                           \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            std::exit(1);                                                                     \
        }                                                                                     \
    } while (0)

namespace {

bool approxEqual(double a, double b, double eps = 1e-4) {
    return std::fabs(a - b) <= eps;
}

std::shared_ptr<Table> buildSpatialDataset(int count) {
    auto tbl = std::make_shared<Table>(std::vector<ColumnDef>{
        {"id", ColType::I32},
        {"x", ColType::F64},
        {"y", ColType::F64},
        {"z", ColType::F64},
        {"hp", ColType::F32},
        {"score", ColType::I64},
        {"flags", ColType::U32},
        {"kind", ColType::Dict},
    });
    const char* kinds[4] = {"hero", "npc", "item", "boss"};
    for (int i = 0; i < count; ++i) {
        const double x = static_cast<double>((i * 17) % 200) - 100.0;
        const double y = static_cast<double>((i * 31) % 200) - 100.0;
        const double z = static_cast<double>((i * 13) % 80) - 40.0;
        const std::vector<ScalarValue> row = {
            ScalarValue::makeI32(i + 1),
            ScalarValue::makeF64(x),
            ScalarValue::makeF64(y),
            ScalarValue::makeF64(z),
            ScalarValue::makeF32(10.0f + static_cast<float>(i % 150)),
            ScalarValue::makeI64(static_cast<int64_t>(i) * 10),
            ScalarValue::makeU32((i % 7 == 0) ? 0x80000001u : static_cast<uint32_t>(i)),
            ScalarValue::makeString(kinds[i % 4]),
        };
        tbl->appendRow(row);
    }
    return tbl;
}

void testSimdFiltersAndAggs() {
    auto tbl = buildSpatialDataset(9000);
    auto snap = tbl->snapshot();

    // 1. SIMD float filter + comparison against reference
    {
        QueryPlan p;
        p.filters.push_back(p.exprs.bin(ExprOp::And,
                                        p.exprs.bin(ExprOp::Gt, p.exprs.col("hp"), p.exprs.lit(ScalarValue::makeF32(100.0f))),
                                        p.exprs.bin(ExprOp::Le, p.exprs.col("hp"), p.exprs.lit(ScalarValue::makeF32(140.0f)))));
        p.orderBy.push_back(OrderByItem{p.exprs.col("id"), false});

        QueryExecResult jitRes = executeQuery(*snap, p, ExecOptions{true, true, true});
        QueryExecResult refRes = executeQuery(*snap, p, ExecOptions{false, false, false});
        CHECK(jitRes.ok && refRes.ok);
        CHECK(jitRes.usedJit && jitRes.usedSimd);
        CHECK(jitRes.table->rowCount() == refRes.table->rowCount() && jitRes.table->rowCount() > 0);
        auto js = jitRes.table->snapshot();
        auto rs = refRes.table->snapshot();
        for (size_t i = 0; i < 10; ++i) {
            RowLoc jl{}, rl{};
            CHECK(jitRes.table->resolveAliveRow(i, jl) && refRes.table->resolveAliveRow(i, rl));
            CHECK(js->getCell(0, jl.chunkIdx, jl.rowIdx).i64 == rs->getCell(0, rl.chunkIdx, rl.rowIdx).i64);
        }
    }

    // 2. SIMD Dict equality filter + U32 unsigned literal range check
    {
        QueryPlan p;
        p.filters.push_back(p.exprs.bin(ExprOp::Eq, p.exprs.col("kind"), p.exprs.lit(ScalarValue::makeString("boss"))));
        QueryExecResult jitRes = executeQuery(*snap, p, ExecOptions{true, true, true});
        QueryExecResult refRes = executeQuery(*snap, p, ExecOptions{false, false, false});
        CHECK(jitRes.ok && jitRes.usedJit && jitRes.usedSimd);
        CHECK(jitRes.table->rowCount() == refRes.table->rowCount() && jitRes.table->rowCount() == 2250);
    }

    // 3. Ungrouped SIMD aggregations (Sum/Avg on I64 & F64, Min/Max on F32/I32)
    {
        QueryPlan p;
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Count, "cnt", kInvalidExpr});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Sum, "sumScore", p.exprs.col("score")});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Avg, "avgScore", p.exprs.col("score")});
        QueryExecResult jitRes = executeQuery(*snap, p, ExecOptions{true, true, true});
        QueryExecResult refRes = executeQuery(*snap, p, ExecOptions{false, false, false});
        CHECK(jitRes.ok && jitRes.usedJitAgg && jitRes.usedSimd);
        auto js = jitRes.table->snapshot();
        auto rs = refRes.table->snapshot();
        CHECK(js->getCell(0, 0, 0).i64 == rs->getCell(0, 0, 0).i64);
        CHECK(js->getCell(1, 0, 0).i64 == rs->getCell(1, 0, 0).i64);
        CHECK(approxEqual(js->getCell(2, 0, 0).f64, rs->getCell(2, 0, 0).f64, 1e-6));
    }

    // 4. Fused filtered JIT aggregation + Min/Max on F32
    {
        QueryPlan p;
        p.filters.push_back(p.exprs.bin(ExprOp::Ge, p.exprs.col("hp"), p.exprs.lit(ScalarValue::makeF32(120.0f))));
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Count, "cnt", kInvalidExpr});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Min, "minHp", p.exprs.col("hp")});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Max, "maxHp", p.exprs.col("hp")});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Avg, "avgHp", p.exprs.col("hp")});
        QueryExecResult jitRes = executeQuery(*snap, p, ExecOptions{true, true, true});
        QueryExecResult refRes = executeQuery(*snap, p, ExecOptions{false, false, false});
        CHECK(jitRes.ok && jitRes.usedJitAgg);
        auto js = jitRes.table->snapshot();
        auto rs = refRes.table->snapshot();
        CHECK(js->getCell(0, 0, 0).i64 == rs->getCell(0, 0, 0).i64);
        CHECK(approxEqual(js->getCell(1, 0, 0).f64, rs->getCell(1, 0, 0).f64, 1e-3));
        CHECK(approxEqual(js->getCell(2, 0, 0).f64, rs->getCell(2, 0, 0).f64, 1e-3));
        CHECK(approxEqual(js->getCell(3, 0, 0).f64, rs->getCell(3, 0, 0).f64, 1e-2));
    }

    // 5. Filtered empty match set must keep Count=0 and match reference default Min/Max
    {
        QueryPlan p;
        p.filters.push_back(p.exprs.bin(ExprOp::Gt, p.exprs.col("hp"), p.exprs.lit(ScalarValue::makeF32(9999.0f))));
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Count, "cnt", kInvalidExpr});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Min, "minHp", p.exprs.col("hp")});
        QueryExecResult jitRes = executeQuery(*snap, p, ExecOptions{true, true, true});
        QueryExecResult refRes = executeQuery(*snap, p, ExecOptions{false, false, false});
        CHECK(jitRes.ok && refRes.ok && jitRes.usedJitAgg);
        auto js = jitRes.table->snapshot();
        auto rs = refRes.table->snapshot();
        CHECK(js->getCell(0, 0, 0).i64 == 0 && rs->getCell(0, 0, 0).i64 == 0);
        CHECK(approxEqual(js->getCell(1, 0, 0).f64, rs->getCell(1, 0, 0).f64) &&
              approxEqual(js->getCell(1, 0, 0).f64, 0.0));
    }
    std::printf("PASS: testSimdFiltersAndAggs\n");
}

void testRangeIndexAndTopK() {
    auto tbl = buildSpatialDataset(5000);
    CHECK(tbl->createRangeIndex("score"));
    auto snap = tbl->snapshot();

    // Range query: score >= 10000 && score < 12000
    {
        QueryPlan p;
        p.filters.push_back(p.exprs.bin(ExprOp::Ge, p.exprs.col("score"), p.exprs.lit(ScalarValue::makeI64(10000))));
        p.filters.push_back(p.exprs.bin(ExprOp::Lt, p.exprs.col("score"), p.exprs.lit(ScalarValue::makeI64(12000))));
        p.orderBy.push_back(OrderByItem{p.exprs.col("id"), false});
        QueryExecResult idxRes = executeQuery(*snap, p, ExecOptions{true, true, false});
        QueryExecResult refRes = executeQuery(*snap, p, ExecOptions{false, false, false});
        CHECK(idxRes.ok && idxRes.usedRangeIndex);
        CHECK(idxRes.table->rowCount() == refRes.table->rowCount() && idxRes.table->rowCount() == 200);
    }

    // OrderBy + Limit Top-K using RangeIndex
    {
        QueryPlan p;
        p.orderBy.push_back(OrderByItem{p.exprs.col("score"), true});
        p.limit = 10;
        p.offset = 5;
        QueryExecResult idxRes = executeQuery(*snap, p, ExecOptions{true, true, false});
        QueryExecResult refRes = executeQuery(*snap, p, ExecOptions{false, false, false});
        CHECK(idxRes.ok && idxRes.usedRangeIndex && idxRes.table->rowCount() == 10);
        auto is = idxRes.table->snapshot();
        auto rs = refRes.table->snapshot();
        for (uint32_t r = 0; r < 10; ++r) {
            CHECK(is->getCell(0, 0, r).i64 == rs->getCell(0, 0, r).i64);
        }
    }

    // Mutate + delete + compact maintains RangeIndex accuracy
    RowLoc loc{};
    CHECK(tbl->resolveAliveRow(4999, loc));
    CHECK(tbl->updateCell(loc, 5, ScalarValue::makeI64(99999999)));
    CHECK(tbl->removeRow(RowLoc{0, 0}));
    tbl->compact();
    auto snap2 = tbl->snapshot();
    {
        QueryPlan p;
        p.filters.push_back(p.exprs.bin(ExprOp::Gt, p.exprs.col("score"), p.exprs.lit(ScalarValue::makeI64(90000000))));
        QueryExecResult r2 = executeQuery(*snap2, p, ExecOptions{true, true, false});
        CHECK(r2.ok && r2.usedRangeIndex && r2.table->rowCount() == 1);
    }
    std::printf("PASS: testRangeIndexAndTopK\n");
}

void testSpatialIndexes() {
    auto tbl = buildSpatialDataset(6000);
    CHECK(tbl->createSpatialIndex("pos2d", {1, 2}, 15.0));
    CHECK(tbl->createSpatialIndex("pos3d", {1, 2, 3}, 15.0));
    auto snap = tbl->snapshot();

    // 2D AABB + scalar filter
    {
        QueryPlan p;
        p.spatialFilter.kind = SpatialFilterSpec::Kind::AABB;
        p.spatialFilter.indexName = "pos2d";
        p.spatialFilter.a[0] = -25.0; p.spatialFilter.a[1] = -30.0;
        p.spatialFilter.b[0] = 35.0;  p.spatialFilter.b[1] = 20.0;
        p.filters.push_back(p.exprs.bin(ExprOp::Eq, p.exprs.col("kind"), p.exprs.lit(ScalarValue::makeString("hero"))));
        p.orderBy.push_back(OrderByItem{p.exprs.col("id"), false});

        QueryExecResult sp = executeQuery(*snap, p, ExecOptions{true, true, true});
        QueryExecResult rf = executeQuery(*snap, p, ExecOptions{false, false, false});
        CHECK(sp.ok && rf.ok && sp.usedSpatialIndex);
        CHECK(sp.table->rowCount() == rf.table->rowCount() && sp.table->rowCount() > 0);
        auto ss = sp.table->snapshot();
        auto rs = rf.table->snapshot();
        RowLoc sl{}, rl{};
        CHECK(sp.table->resolveAliveRow(0, sl) && rf.table->resolveAliveRow(0, rl));
        CHECK(ss->getCell(0, sl.chunkIdx, sl.rowIdx).i64 == rs->getCell(0, rl.chunkIdx, rl.rowIdx).i64);
    }

    // 3D Radius query
    {
        QueryPlan p;
        p.spatialFilter.kind = SpatialFilterSpec::Kind::Radius;
        p.spatialFilter.indexName = "pos3d";
        p.spatialFilter.a[0] = 10.0; p.spatialFilter.a[1] = -10.0; p.spatialFilter.a[2] = 5.0;
        p.spatialFilter.radius = 28.0;
        p.orderBy.push_back(OrderByItem{p.exprs.col("id"), false});

        QueryExecResult sp = executeQuery(*snap, p, ExecOptions{true, true, true});
        QueryExecResult rf = executeQuery(*snap, p, ExecOptions{false, false, false});
        CHECK(sp.ok && rf.ok && sp.usedSpatialIndex);
        CHECK(sp.table->rowCount() == rf.table->rowCount() && sp.table->rowCount() > 0);
    }

    // queryNearest K-NN ordering vs brute force
    {
        std::vector<std::pair<RowLoc, double>> nn;
        const SpatialIndex* si = snap->findSpatialIndex("pos3d");
        CHECK(si != nullptr);
        si->queryNearest(0.0, 0.0, 0.0, 8, -1.0, nn);
        CHECK(nn.size() == 8);
        for (size_t i = 1; i < nn.size(); ++i) {
            CHECK(nn[i].second >= nn[i - 1].second - 1e-12);
        }
    }
    std::printf("PASS: testSpatialIndexes\n");
}

void testPersistenceAndMmap() {
    auto tbl = std::make_shared<Table>(std::vector<ColumnDef>{
        {"id", ColType::I32},
        {"val", ColType::F64},
        {"kind", ColType::Dict},
        {"tag", ColType::String},
    });
    for (int i = 0; i < 4500; ++i) {
        const std::vector<ScalarValue> row = {
            ScalarValue::makeI32(i),
            ScalarValue::makeF64(i * 0.25),
            ScalarValue::makeString(i % 2 == 0 ? "even" : "odd"),
            ScalarValue::makeString("row_" + std::to_string(i)),
        };
        tbl->appendRow(row);
    }
    CHECK(tbl->removeRow(RowLoc{0, 1}));

    // 1. In-memory serialize / deserialize
    std::vector<uint8_t> bytes = serializeSnapshotBytes(*tbl->snapshot());
    std::string err;
    auto fromBytesTbl = deserializeTableBytes(bytes.data(), bytes.size(), &err);
    CHECK(fromBytesTbl && err.empty());
    CHECK(fromBytesTbl->rowCount() == 4499);
    CHECK(fromBytesTbl->getCell(RowLoc{0, 2}, 3).str == "row_2");

    // 2. File save, mmap zero-copy snapshot, SIMD/JIT query on mapped view, COW isolation, clean unmap
    const std::filesystem::path tmpFile =
        std::filesystem::temp_directory_path() / "bro_table_adv_test.brotable";
    std::filesystem::remove(tmpFile);
    {
        auto snapOrig = tbl->snapshot();
        CHECK(saveSnapshotToFile(*snapOrig, tmpFile.string(), &err));

        auto mmapSnap = mmapSnapshotFromFile(tmpFile.string(), &err);
        CHECK(mmapSnap && err.empty());
        CHECK(mmapSnap->rowCount() == 4499);
        CHECK(mmapSnap->chunkCount() == 2);
        CHECK(mmapSnap->chunks[0].columns[0]->isMapped());
        CHECK(mmapSnap->chunks[0].columns[3]->isMapped());
        CHECK(reinterpret_cast<uintptr_t>(mmapSnap->chunks[0].columns[1]->rawData()) % 64 == 0);
        CHECK(mmapSnap->getCell(3, 1, 10).str == "row_4106");

        // Query directly over zero-copy mmap pages
        QueryPlan qp;
        qp.aggregates.push_back(AggSpec{AggSpec::Kind::Sum, "total", qp.exprs.col("val")});
        QueryExecResult qr = executeQuery(*mmapSnap, qp);
        CHECK(qr.ok && qr.table->snapshot()->getCell(0, 0, 0).f64 > 0.0);

        // Materialize mutable COW copy from mapped snapshot
        auto clonedTbl = Table::fromSnapshot(*mmapSnap);
        CHECK(!clonedTbl->chunk(0).columns[0]->isMapped());
        CHECK(clonedTbl->updateCell(RowLoc{0, 0}, 1, ScalarValue::makeF64(12345.0)));
        CHECK(approxEqual(clonedTbl->getCell(RowLoc{0, 0}, 1).f64, 12345.0));
        CHECK(approxEqual(mmapSnap->getCell(1, 0, 0).f64, 0.0));
    }  // mmapSnap unmaps Windows handles here cleanly

    CHECK(std::filesystem::remove(tmpFile));
    std::printf("PASS: testPersistenceAndMmap\n");
}

void testReactiveQuery() {
    auto tbl = buildSpatialDataset(9000);  // 3 chunks (4096 + 4096 + 808)
    CHECK(tbl->createSpatialIndex("pos2d", {1, 2}, 16.0));

    QueryPlan plan;
    plan.filters.push_back(plan.exprs.bin(ExprOp::Gt, plan.exprs.col("hp"),
                                          plan.exprs.lit(ScalarValue::makeF32(100.0f))));
    ReactiveQuery rq(tbl, plan);
    CHECK(rq.isDirty());

    auto r0 = rq.refresh();
    CHECK(r0.changed && r0.recomputedChunks == 3 && r0.reusedChunks == 0);
    CHECK(!rq.isDirty());

    auto rNoop = rq.refresh();
    CHECK(!rNoop.changed && rNoop.recomputedChunks == 0 && rNoop.reusedChunks == 3);

    // Mutate only chunk 1 (rows [4096..8191])
    CHECK(tbl->updateCell(RowLoc{1, 10}, 4, ScalarValue::makeF32(155.0f)));
    CHECK(rq.isDirty());
    auto r1 = rq.refresh();
    CHECK(r1.changed && r1.recomputedChunks == 1 && r1.reusedChunks == 2);

    // Tombstone + compact bumps structureVersion -> recomputes cleanly
    CHECK(tbl->removeRow(RowLoc{0, 5}));
    tbl->compact();
    auto rComp = rq.refresh();
    CHECK(rComp.changed && rComp.recomputedChunks == tbl->chunkCount());

    // Spatial reactive query also updates when spatial rows change
    QueryPlan spPlan;
    spPlan.spatialFilter.kind = SpatialFilterSpec::Kind::AABB;
    spPlan.spatialFilter.indexName = "pos2d";
    spPlan.spatialFilter.a[0] = -10.0; spPlan.spatialFilter.a[1] = -10.0;
    spPlan.spatialFilter.b[0] = 10.0;  spPlan.spatialFilter.b[1] = 10.0;
    ReactiveQuery spRq(tbl, spPlan);
    auto sr0 = spRq.refresh();
    CHECK(sr0.changed && sr0.table->rowCount() > 0);
    std::printf("PASS: testReactiveQuery\n");
}

}  // namespace

int main() {
    testSimdFiltersAndAggs();
    testRangeIndexAndTopK();
    testSpatialIndexes();
    testPersistenceAndMmap();
    testReactiveQuery();
    std::printf("PASS: bro_table_advanced_test (all assertions passed)\n");
    return 0;
}
