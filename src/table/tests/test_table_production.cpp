#include "brass/target/host_cpu.hpp"
#include "table/query_eval.h"
#include "table/query_exec.h"
#include "table/query_ir.h"
#include "table/query_jit.h"
#include "table/table.h"
#include "table/table_io.h"
#include "table/table_reactive.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace bro::table;

namespace {

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "CHECK FAILED [" << __FILE__ << ":" << __LINE__ << "]: " \
                      << #cond << "\n";                                          \
            std::abort();                                                        \
        }                                                                        \
    } while (0)

void addRow(Table& t, std::initializer_list<ScalarValue> vals) {
    std::vector<ScalarValue> row(vals);
    t.appendRow(row);
}

bool scalarEqualApprox(const ScalarValue& a, const ScalarValue& b) {
    if (a.isNull != b.isNull) return false;
    if (a.isNull) return true;
    if (colTypeIsFloat(a.type) || colTypeIsFloat(b.type)) {
        const double da = colTypeIsFloat(a.type) ? a.f64
                          : (colTypeIsUnsignedInt(a.type) ? static_cast<double>(a.u64)
                                                          : static_cast<double>(a.i64));
        const double db = colTypeIsFloat(b.type) ? b.f64
                          : (colTypeIsUnsignedInt(b.type) ? static_cast<double>(b.u64)
                                                          : static_cast<double>(b.i64));
        if (std::isnan(da) && std::isnan(db)) return true;
        const double diff = std::fabs(da - db);
        return diff <= 1e-4 * std::max(1.0, std::max(std::fabs(da), std::fabs(db)));
    }
    return compareScalar(a, b) == 0;
}

void expectTablesEqual(const Table& a, const Table& b) {
    CHECK(a.columnCount() == b.columnCount());
    CHECK(a.rowCount() == b.rowCount());
    for (size_t c = 0; c < a.columnCount(); ++c) {
        CHECK(a.schema().columns[c].name == b.schema().columns[c].name);
        CHECK(a.schema().columns[c].type == b.schema().columns[c].type);
    }
    for (size_t r = 0; r < a.rowCount(); ++r) {
        RowLoc la{}, lb{};
        CHECK(a.resolveAliveRow(r, la));
        CHECK(b.resolveAliveRow(r, lb));
        for (size_t c = 0; c < a.columnCount(); ++c) {
            CHECK(scalarEqualApprox(a.getCell(la, c), b.getCell(lb, c)));
        }
    }
}

std::vector<std::vector<ScalarValue>> collectRowsSorted(const Table& t) {
    std::vector<std::vector<ScalarValue>> rows;
    rows.reserve(t.rowCount());
    for (size_t r = 0; r < t.rowCount(); ++r) {
        RowLoc loc{};
        CHECK(t.resolveAliveRow(r, loc));
        std::vector<ScalarValue> row;
        for (size_t c = 0; c < t.columnCount(); ++c) row.push_back(t.getCell(loc, c));
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        for (size_t c = 0; c < a.size(); ++c) {
            int cmp = compareScalar(a[c], b[c]);
            if (cmp != 0) return cmp < 0;
        }
        return false;
    });
    return rows;
}

void expectTablesMultisetEqual(const Table& a, const Table& b) {
    CHECK(a.columnCount() == b.columnCount());
    CHECK(a.rowCount() == b.rowCount());
    auto ra = collectRowsSorted(a);
    auto rb = collectRowsSorted(b);
    CHECK(ra.size() == rb.size());
    for (size_t i = 0; i < ra.size(); ++i) {
        for (size_t c = 0; c < ra[i].size(); ++c) {
            CHECK(scalarEqualApprox(ra[i][c], rb[i][c]));
        }
    }
}

void testSchemaDuplicateAndStatsClearClone() {
    Schema s;
    CHECK(s.addColumn("id", ColType::I32));
    CHECK(!s.addColumn("id", ColType::F64));
    CHECK(s.size() == 1);

    Table tbl({
        {"id", ColType::I32},
        {"x", ColType::F64},
        {"y", ColType::F64},
        {"note", ColType::String},
    });
    for (int i = 0; i < 10; ++i) {
        addRow(tbl, {ScalarValue::makeI32(i), ScalarValue::makeF64(i * 1.5),
                     ScalarValue::makeF64(i * -0.5),
                     ScalarValue::makeString("row_" + std::to_string(i))});
    }
    CHECK(tbl.createIndex("id"));
    CHECK(tbl.createRangeIndex("x"));
    CHECK(tbl.createSpatialIndex("pos", {1, 2}, 4.0));
    RowLoc loc2{};
    CHECK(tbl.resolveAliveRow(2, loc2));
    CHECK(tbl.removeRow(loc2));

    const TableStats st = tbl.stats();
    CHECK(st.rowCount == 9);
    CHECK(st.allocatedRows == 10);
    CHECK(st.tombstoneCount == 1);
    CHECK(st.nullBitmapBytes == 0);
    CHECK(st.hashIndexes == 1);
    CHECK(st.rangeIndexes == 1);
    CHECK(st.spatialIndexes == 1);
    CHECK(st.tombstoneRatio > 0.09 && st.tombstoneRatio < 0.11);

    auto cloned = Table::fromSnapshot(*tbl.snapshot());
    CHECK(cloned != nullptr);
    CHECK(cloned->hasIndex(0));
    CHECK(cloned->hasRangeIndex(1));
    CHECK(cloned->hasSpatialIndex("pos"));
    expectTablesEqual(*cloned, tbl);

    tbl.updateCell(RowLoc{0, 0}, 1, ScalarValue::makeF64(999.0));
    CHECK(cloned->getCell(RowLoc{0, 0}, 1).f64 == 0.0);

    tbl.clear();
    CHECK(tbl.rowCount() == 0);
    CHECK(tbl.chunkCount() == 0);
    CHECK(tbl.columnCount() == 4);
    CHECK(tbl.stats().rowCount == 0);
}

void testNullBitmapLifecycle() {
    Table tbl({{"id", ColType::I32}, {"val", ColType::F64}, {"txt", ColType::String}});
    for (int i = 0; i < 100; ++i) {
        addRow(tbl, {ScalarValue::makeI32(i), ScalarValue::makeF64(i * 2.0),
                     ScalarValue::makeString("s" + std::to_string(i))});
    }
    CHECK(tbl.stats().nullBitmapBytes == 0);

    // Toggle null -> non-null -> null across rows + tombstones + compact
    CHECK(tbl.updateCell(RowLoc{0, 40}, 1, ScalarValue::makeNull()));
    CHECK(tbl.chunk(0).columns[1]->hasNulls());
    CHECK(tbl.chunk(0).columns[1]->isNull(40));
    CHECK(tbl.stats().nullBitmapBytes == 512);

    CHECK(tbl.updateCell(RowLoc{0, 40}, 1, ScalarValue::makeF64(80.0)));
    CHECK(!tbl.chunk(0).columns[1]->hasNulls());
    CHECK(!tbl.chunk(0).columns[1]->isNull(40));

    CHECK(tbl.updateCell(RowLoc{0, 5}, 1, ScalarValue::makeNull()));
    CHECK(tbl.updateCell(RowLoc{0, 90}, 2, ScalarValue::makeNull()));
    CHECK(tbl.removeRow(RowLoc{0, 0}));
    CHECK(tbl.removeRow(RowLoc{0, 2}));
    tbl.compact();

    CHECK(tbl.rowCount() == 98);
    RowLoc loc{};
    CHECK(tbl.resolveAliveRow(3, loc));
    CHECK(tbl.getCell(loc, 0).i64 == 5);
    CHECK(tbl.getCell(loc, 1).isNull);
    CHECK(tbl.resolveAliveRow(88, loc));
    CHECK(tbl.getCell(loc, 0).i64 == 90);
    CHECK(tbl.getCell(loc, 2).isNull);

    auto cloneTbl = Table::fromSnapshot(*tbl.snapshot());
    expectTablesEqual(*cloneTbl, tbl);
}

void testNullQuerySemanticsAndAggregates() {
    Table tbl({
        {"id", ColType::I32},
        {"grp", ColType::Dict},
        {"v", ColType::F64},
    });
    for (int i = 0; i < 4096; ++i) {
        addRow(tbl, {ScalarValue::makeI32(i),
                     ScalarValue::makeString((i % 2 == 0) ? "even" : "odd"),
                     ScalarValue::makeF64(static_cast<double>(i % 50))});
    }
    for (int i = 4096; i < 4200; ++i) {
        ScalarValue grp = (i % 5 == 0) ? ScalarValue::makeNull() : ScalarValue::makeString("mix");
        ScalarValue v = (i % 3 == 0) ? ScalarValue::makeNull()
                                     : ScalarValue::makeF64(static_cast<double>(i % 50));
        addRow(tbl, {ScalarValue::makeI32(i), grp, v});
    }
    for (int i = 4200; i < 8300; ++i) {
        addRow(tbl, {ScalarValue::makeI32(i), ScalarValue::makeString("tail"),
                     ScalarValue::makeF64(static_cast<double>(i % 50))});
    }
    CHECK(tbl.chunkCount() == 3);

    auto snap = tbl.snapshot();

    // 1. Strict false comparisons + mixed chunk SIMD vs reference
    {
        QueryPlan p;
        p.filters.push_back(p.exprs.bin(ExprOp::Lt, p.exprs.col("v"),
                                        p.exprs.lit(ScalarValue::makeF64(10.0))));
        p.selectItems.push_back(SelectItem{"id", p.exprs.col("id"), ColType::I32});
        p.selectItems.push_back(SelectItem{"v", p.exprs.col("v"), ColType::F64});
        std::string err;
        CHECK(bindQueryPlan(p, *snap, err));
        auto rJit = executeQuery(*snap, p);
        CHECK(rJit.ok && rJit.usedSimd);
        auto rRef = executePlanReference(*snap, p);
        expectTablesEqual(*rJit.table, *rRef);
    }

    // 2. Null comparison with NULL literal returns 0 rows cleanly
    {
        QueryPlan p;
        p.filters.push_back(p.exprs.bin(ExprOp::Eq, p.exprs.col("v"),
                                        p.exprs.lit(ScalarValue::makeNull())));
        std::string err;
        CHECK(bindQueryPlan(p, *snap, err));
        auto res = executeQuery(*snap, p);
        CHECK(res.ok && res.table->rowCount() == 0);
    }

    // 3. IsNull / IsNotNull / Coalesce / Arithmetic propagation
    {
        QueryPlan p;
        p.filters.push_back(p.exprs.un(ExprOp::IsNull, p.exprs.col("v")));
        auto added = p.exprs.bin(ExprOp::Add, p.exprs.col("v"),
                                 p.exprs.lit(ScalarValue::makeF64(5.0)));
        auto coal = p.exprs.bin(ExprOp::Coalesce, p.exprs.col("v"),
                                p.exprs.lit(ScalarValue::makeF64(-1.0)));
        p.selectItems.push_back(SelectItem{"id", p.exprs.col("id"), ColType::I32});
        p.selectItems.push_back(SelectItem{"added", added, ColType::F64});
        p.selectItems.push_back(SelectItem{"coal", coal, ColType::F64});
        std::string err;
        CHECK(bindQueryPlan(p, *snap, err));
        auto res = executeQuery(*snap, p);
        CHECK(res.ok && res.table->rowCount() == 34);
        for (size_t r = 0; r < res.table->rowCount(); ++r) {
            RowLoc loc{};
            CHECK(res.table->resolveAliveRow(r, loc));
            CHECK(res.table->getCell(loc, 1).isNull);
            CHECK(res.table->getCell(loc, 2).f64 == -1.0);
        }
    }

    // 4. Ungrouped & Grouped Count(*) vs Count(v)/Sum/Avg/Min/Max null skipping
    {
        QueryPlan p;
        p.groupKeys.push_back(p.exprs.col("grp"));
        p.groupAliases.push_back("grp");
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Count, "cnt_all", kInvalidExpr, ColType::I64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Count, "cnt_v", p.exprs.col("v"), ColType::I64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Sum, "sum_v", p.exprs.col("v"), ColType::F64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Avg, "avg_v", p.exprs.col("v"), ColType::F64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Min, "min_v", p.exprs.col("v"), ColType::F64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Max, "max_v", p.exprs.col("v"), ColType::F64});
        std::string err;
        CHECK(bindQueryPlan(p, *snap, err));
        auto rExec = executeQuery(*snap, p);
        auto rRef = executePlanReference(*snap, p);
        CHECK(rExec.ok);
        expectTablesMultisetEqual(*rExec.table, *rRef);

        QueryPlan ug;
        ug.exprs = p.exprs;
        ug.aggregates = p.aggregates;
        CHECK(bindQueryPlan(ug, *snap, err));
        auto ugExec = executeQuery(*snap, ug);
        auto ugRef = executePlanReference(*snap, ug);
        CHECK(ugExec.ok && !ugExec.usedJitAgg);
        expectTablesEqual(*ugExec.table, *ugRef);
        RowLoc l0{};
        CHECK(ugExec.table->resolveAliveRow(0, l0));
        CHECK(ugExec.table->getCell(l0, 0).i64 == 8300);
        CHECK(ugExec.table->getCell(l0, 1).i64 == 8300 - 34);
    }

    // 5. ReactiveQuery incremental correctness with nullable chunk edits
    {
        auto sharedTbl = Table::fromSnapshot(*snap);
        QueryPlan rp;
        rp.filters.push_back(rp.exprs.bin(ExprOp::Gt, rp.exprs.col("v"),
                                          rp.exprs.lit(ScalarValue::makeF64(45.0))));
        std::string err;
        CHECK(bindQueryPlan(rp, *sharedTbl->snapshot(), err));
        ReactiveQuery rq(sharedTbl, rp);
        rq.refresh();
        const size_t beforeCount = rq.latestResult()->rowCount();
        RowLoc loc{};
        CHECK(sharedTbl->resolveAliveRow(4098, loc));
        CHECK(sharedTbl->getCell(loc, 2).isNull);
        sharedTbl->updateCell(loc, 2, ScalarValue::makeF64(49.0));
        auto rr = rq.refresh();
        CHECK(rr.changed && rr.recomputedChunks == 1 && rr.reusedChunks == 2);
        CHECK(rq.latestResult()->rowCount() == beforeCount + 1);
    }
}

void testNullsWithIndexesAndMutations() {
    Table tbl({{"id", ColType::I32}, {"x", ColType::F64}, {"y", ColType::F64}});
    CHECK(tbl.createIndex("id"));
    CHECK(tbl.createRangeIndex("id"));
    CHECK(tbl.createSpatialIndex("pos", {1, 2}, 10.0));

    for (int i = 0; i < 20; ++i) {
        if (i % 4 == 0) {
            addRow(tbl, {ScalarValue::makeNull(), ScalarValue::makeNull(), ScalarValue::makeF64(i)});
        } else {
            addRow(tbl, {ScalarValue::makeI32(i), ScalarValue::makeF64(i * 2.0),
                         ScalarValue::makeF64(i * 2.0)});
        }
    }

    {
        auto snap = tbl.snapshot();
        QueryPlan p;
        p.orderBy.push_back(OrderByItem{p.exprs.col("id"), false});
        p.limit = 5;
        std::string err;
        CHECK(bindQueryPlan(p, *snap, err));
        auto res = executeQuery(*snap, p);
        auto ref = executePlanReference(*snap, p);
        CHECK(res.ok);
        expectTablesEqual(*res.table, *ref);
    }

    {
        QueryPlan u;
        u.spatialFilter.kind = SpatialFilterSpec::Kind::AABB;
        u.spatialFilter.indexName = "pos";
        u.spatialFilter.a[0] = 0.0;
        u.spatialFilter.a[1] = 0.0;
        u.spatialFilter.b[0] = 10.0;
        u.spatialFilter.b[1] = 10.0;
        u.updates.push_back(UpdateAssignment{"x", -1, u.exprs.lit(ScalarValue::makeNull())});

        auto copyTbl = Table::fromSnapshot(*tbl.snapshot());
        auto rIdx = executeUpdate(tbl, u);
        ExecOptions noIdx;
        noIdx.useIndexes = false;
        noIdx.useJit = false;
        auto rNoIdx = executeUpdate(*copyTbl, u, noIdx);
        CHECK(rIdx.ok && rIdx.usedSpatialIndex);
        CHECK(rIdx.affectedRows == rNoIdx.affectedRows && rIdx.affectedRows == 4);
        expectTablesEqual(tbl, *copyTbl);
    }

    {
        QueryPlan u;
        u.filters.push_back(u.exprs.un(ExprOp::IsNull, u.exprs.col("x")));
        u.updates.push_back(UpdateAssignment{"x", -1, u.exprs.lit(ScalarValue::makeF64(77.0))});
        auto ur = executeUpdate(tbl, u);
        CHECK(ur.ok && ur.affectedRows > 0);
        auto snap = tbl.snapshot();
        CHECK(!snap->chunks[0].columns[1]->hasNulls());

        QueryPlan d;
        d.filters.push_back(d.exprs.bin(ExprOp::Eq, d.exprs.col("id"),
                                        d.exprs.lit(ScalarValue::makeI32(1))));
        auto dr = executeDelete(tbl, d);
        CHECK(dr.ok && dr.usedIndex && dr.affectedRows == 1);
    }
}

void testAvx2SimdAndPrecisionParity() {
    const uint16_t expectedWidth = brass::host_has_avx2() ? 256 : 128;

    Table tbl({
        {"id", ColType::I32},
        {"i32b", ColType::I32},
        {"u32c", ColType::U32},
        {"big", ColType::I64},
        {"f32v", ColType::F32},
        {"f64a", ColType::F64},
        {"f64b", ColType::F64},
    });
    for (int i = 0; i < 10000; ++i) {
        int32_t id = (i == 0) ? INT32_MIN : (i == 1 ? INT32_MAX : (i - 5000));
        int32_t b = (i % 7) - 3;
        uint32_t u = (i == 2) ? UINT32_MAX : static_cast<uint32_t>(i * 13);
        int64_t big = (1LL << 53) + (i % 10);
        float f32v = (i == 3) ? -0.0f : static_cast<float>(i % 100) * 0.25f;
        double f64a = (i == 4) ? std::numeric_limits<double>::quiet_NaN()
                               : static_cast<double>(i % 250) * 0.5;
        double f64b = static_cast<double>((i * 3) % 250) * 0.5;
        addRow(tbl, {ScalarValue::makeI32(id), ScalarValue::makeI32(b),
                     ScalarValue::makeU32(u), ScalarValue::makeI64(big),
                     ScalarValue::makeF32(f32v), ScalarValue::makeF64(f64a),
                     ScalarValue::makeF64(f64b)});
    }
    for (int i = 10; i < 200; i += 17) {
        RowLoc loc{static_cast<uint32_t>(i / kChunkRows), static_cast<uint32_t>(i % kChunkRows)};
        tbl.removeRow(loc);
    }
    auto snap = tbl.snapshot();

    auto verifySimdPlan = [&](QueryPlan p, bool checkAvxWidth) {
        std::string err;
        CHECK(bindQueryPlan(p, *snap, err));
        auto rJit = executeQuery(*snap, p);
        auto rRef = executePlanReference(*snap, p);
        CHECK(rJit.ok);
        if (checkAvxWidth) {
            CHECK(rJit.usedSimd && rJit.simdWidth == expectedWidth);
        }
        expectTablesEqual(*rJit.table, *rRef);
    };

    {
        QueryPlan p;
        p.filters.push_back(p.exprs.bin(ExprOp::Gt, p.exprs.col("f64a"),
                                        p.exprs.lit(ScalarValue::makeF64(60.0))));
        p.selectItems.push_back(SelectItem{"id", p.exprs.col("id"), ColType::I32});
        verifySimdPlan(p, true);
    }
    {
        QueryPlan p;
        p.filters.push_back(p.exprs.bin(ExprOp::Le, p.exprs.col("f64a"), p.exprs.col("f64b")));
        p.selectItems.push_back(SelectItem{"id", p.exprs.col("id"), ColType::I32});
        verifySimdPlan(p, true);
    }
    {
        QueryPlan pF32;
        auto e1 = pF32.exprs.bin(ExprOp::Add, pF32.exprs.col("f32v"),
                                  pF32.exprs.lit(ScalarValue::makeI32(10)));
        auto e2 = pF32.exprs.bin(ExprOp::Mul, e1, pF32.exprs.lit(ScalarValue::makeI32(2)));
        pF32.filters.push_back(pF32.exprs.bin(ExprOp::Gt, e2, pF32.exprs.lit(ScalarValue::makeI32(50))));
        pF32.selectItems.push_back(SelectItem{"id", pF32.exprs.col("id"), ColType::I32});
        verifySimdPlan(pF32, true);

        QueryPlan pPromoted;
        auto pe1 = pPromoted.exprs.bin(ExprOp::Add, pPromoted.exprs.col("f32v"),
                                       pPromoted.exprs.lit(ScalarValue::makeF64(10.5)));
        pPromoted.filters.push_back(pPromoted.exprs.bin(ExprOp::Gt, pe1,
                                                        pPromoted.exprs.lit(ScalarValue::makeF64(25.0))));
        pPromoted.selectItems.push_back(SelectItem{"id", pPromoted.exprs.col("id"), ColType::I32});
        verifySimdPlan(pPromoted, false);
    }
    {
        QueryPlan p;
        auto dv = p.exprs.bin(ExprOp::Div, p.exprs.col("f64a"),
                              p.exprs.lit(ScalarValue::makeF64(2.0)));
        p.filters.push_back(p.exprs.bin(ExprOp::Gt, dv, p.exprs.lit(ScalarValue::makeF64(30.0))));
        p.selectItems.push_back(SelectItem{"id", p.exprs.col("id"), ColType::I32});
        verifySimdPlan(p, true);
    }
    {
        QueryPlan p;
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Count, "c", kInvalidExpr, ColType::I64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Sum, "s", p.exprs.col("f64b"), ColType::F64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Min, "mn", p.exprs.col("f64b"), ColType::F64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Max, "mx", p.exprs.col("f64b"), ColType::F64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Avg, "av", p.exprs.col("f64b"), ColType::F64});
        verifySimdPlan(p, true);
    }

    // RangeIndex fractional float & high I64 precision boundaries
    {
        Table rt({{"k", ColType::I64}, {"f", ColType::F32}});
        rt.createRangeIndex("k");
        rt.createRangeIndex("f");
        const int64_t base = (1LL << 53);
        addRow(rt, {ScalarValue::makeI64(base + 1), ScalarValue::makeF32(1.25f)});
        addRow(rt, {ScalarValue::makeI64(base + 2), ScalarValue::makeF32(1.75f)});
        addRow(rt, {ScalarValue::makeI64(base + 3), ScalarValue::makeF32(2.25f)});
        auto rsnap = rt.snapshot();

        QueryPlan qp1;
        qp1.filters.push_back(qp1.exprs.bin(ExprOp::Eq, qp1.exprs.col("k"),
                                            qp1.exprs.lit(ScalarValue::makeI64(base + 2))));
        std::string err;
        CHECK(bindQueryPlan(qp1, *rsnap, err));
        auto res1 = executeQuery(*rsnap, qp1);
        CHECK(res1.ok && res1.usedRangeIndex && res1.table->rowCount() == 1);

        QueryPlan qp2;
        qp2.filters.push_back(qp2.exprs.bin(ExprOp::Gt, qp2.exprs.col("f"),
                                            qp2.exprs.lit(ScalarValue::makeF64(1.3))));
        qp2.filters.push_back(qp2.exprs.bin(ExprOp::Lt, qp2.exprs.col("f"),
                                            qp2.exprs.lit(ScalarValue::makeF64(2.1))));
        CHECK(bindQueryPlan(qp2, *rsnap, err));
        auto res2 = executeQuery(*rsnap, qp2);
        CHECK(res2.ok && res2.usedRangeIndex && res2.table->rowCount() == 1);
    }
}

void testIndexFreelistChurnAndNearestKnn() {
    Table tbl({{"id", ColType::I32}, {"x", ColType::F64}, {"y", ColType::F64}});
    CHECK(tbl.createIndex("id"));
    CHECK(tbl.createSpatialIndex("pos", {1, 2}, 10.0));

    for (int i = 0; i < 2000; ++i) {
        addRow(tbl, {ScalarValue::makeI32(i), ScalarValue::makeF64(i * 0.5),
                     ScalarValue::makeF64(i * 0.5)});
    }
    const size_t initialHashCap = tbl.snapshot()->findIndex(0)->nodeCapacity();
    const size_t initialSpatialCap = tbl.snapshot()->findSpatialIndex("pos")->nodeCapacity();

    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 1000; ++i) {
            RowLoc loc{};
            if (tbl.resolveAliveRow(i, loc)) {
                tbl.updateCell(loc, 0, ScalarValue::makeI32(10000 + round * 1000 + i));
                tbl.updateCell(loc, 1, ScalarValue::makeF64(500.0 + i));
            }
        }
    }
    auto afterSnap = tbl.snapshot();
    CHECK(afterSnap->findIndex(0)->entryCount() == 2000);
    CHECK(afterSnap->findSpatialIndex("pos")->entryCount() == 2000);
    CHECK(afterSnap->findIndex(0)->nodeCapacity() < initialHashCap + 2500);
    CHECK(afterSnap->findSpatialIndex("pos")->nodeCapacity() < initialSpatialCap + 2500);

    Table farTbl({{"x", ColType::F64}, {"y", ColType::F64}});
    CHECK(farTbl.createSpatialIndex("sp", {0, 1}, 10.0));
    addRow(farTbl, {ScalarValue::makeF64(500.0), ScalarValue::makeF64(500.0)});
    addRow(farTbl, {ScalarValue::makeF64(600.0), ScalarValue::makeF64(600.0)});
    std::vector<std::pair<RowLoc, double>> knn;
    farTbl.snapshot()->findSpatialIndex("sp")->queryNearest(0.0, 0.0, 0.0, 2, -1.0, knn);
    CHECK(knn.size() == 2 && knn[0].first.rowIdx == 0 && knn[1].first.rowIdx == 1);
}

void testParallelFastGroupBy() {
    Table tbl({
        {"k_dict", ColType::Dict},
        {"k_i32", ColType::I32},
        {"k_u32", ColType::U32},
        {"k_i64", ColType::I64},
        {"k_bool", ColType::Bool},
        {"v", ColType::F64},
    });
    const char* words[3] = {"zebra", "apple", "mango"};
    for (int i = 0; i < 50000; ++i) {
        ScalarValue kd = (i % 29 == 0) ? ScalarValue::makeNull()
                                       : ScalarValue::makeString(words[i % 3]);
        ScalarValue v = (i % 17 == 0) ? ScalarValue::makeNull()
                                      : ScalarValue::makeF64(static_cast<double>(i % 100));
        addRow(tbl, {
            kd,
            ScalarValue::makeI32(i % 25),
            ScalarValue::makeU32(static_cast<uint32_t>(i % 19)),
            ScalarValue::makeI64(static_cast<int64_t>((i % 11) - 5)),
            ScalarValue::makeBool((i & 1) == 0),
            v,
        });
    }
    auto snap = tbl.snapshot();

    for (const char* keyCol : {"k_dict", "k_i32", "k_u32", "k_i64", "k_bool"}) {
        QueryPlan p;
        p.groupKeys.push_back(p.exprs.col(keyCol));
        p.groupAliases.push_back(keyCol);
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Count, "cnt", kInvalidExpr, ColType::I64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Count, "cnt_v", p.exprs.col("v"), ColType::I64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Sum, "sum_v", p.exprs.col("v"), ColType::F64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Min, "min_v", p.exprs.col("v"), ColType::F64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Max, "max_v", p.exprs.col("v"), ColType::F64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Avg, "avg_v", p.exprs.col("v"), ColType::F64});
        p.orderBy.push_back(OrderByItem{p.exprs.col("cnt"), true});
        p.limit = 10;
        std::string err;
        CHECK(bindQueryPlan(p, *snap, err));

        ExecOptions parOpts{true, true, true};
        ExecOptions serOpts{true, true, false};
        auto rPar = executeQuery(*snap, p, parOpts);
        auto rSer = executeQuery(*snap, p, serOpts);
        auto rRef = executePlanReference(*snap, p);
        CHECK(rPar.ok && rSer.ok);
        expectTablesMultisetEqual(*rPar.table, *rRef);
        expectTablesMultisetEqual(*rSer.table, *rRef);
    }

    // Min/Max on Dict must follow lexical string order ("apple" < "mango" < "zebra")
    {
        QueryPlan p;
        p.groupKeys.push_back(p.exprs.col("k_bool"));
        p.groupAliases.push_back("b");
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Min, "mn", p.exprs.col("k_dict"), ColType::Dict});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Max, "mx", p.exprs.col("k_dict"), ColType::Dict});
        std::string err;
        CHECK(bindQueryPlan(p, *snap, err));
        auto rExec = executeQuery(*snap, p);
        CHECK(rExec.ok && rExec.table->rowCount() == 2);
        RowLoc l0{};
        CHECK(rExec.table->resolveAliveRow(0, l0));
        CHECK(rExec.table->getCell(l0, 1).str == "apple");
        CHECK(rExec.table->getCell(l0, 2).str == "zebra");
    }
}

void testBrotableV3StreamingAndMappedIndexes() {
    Table tbl({
        {"id", ColType::I32},
        {"kind", ColType::Dict},
        {"x", ColType::F64},
        {"y", ColType::F64},
        {"note", ColType::String},
    });
    for (int i = 0; i < 500; ++i) {
        addRow(tbl, {
            ScalarValue::makeI32(i),
            ScalarValue::makeString((i & 1) ? "hero" : "npc"),
            (i % 10 == 0) ? ScalarValue::makeNull() : ScalarValue::makeF64(i * 1.0),
            ScalarValue::makeF64(i * 2.0),
            (i % 7 == 0) ? ScalarValue::makeNull() : ScalarValue::makeString("n" + std::to_string(i)),
        });
    }
    CHECK(tbl.createIndex("id"));
    CHECK(tbl.createRangeIndex("x"));
    CHECK(tbl.createRangeIndex("kind"));
    CHECK(tbl.createRangeIndex("note"));
    CHECK(tbl.createSpatialIndex("pos", {2, 3}, 8.0));

    const auto tmp = std::filesystem::temp_directory_path() / "bro_table_prod_v3.brotable";
    std::string err;
    auto snap = tbl.snapshot();
    CHECK(saveSnapshotToFile(*snap, tmp.string(), &err));

    std::vector<uint8_t> validBytes = serializeSnapshotBytes(*snap);
    CHECK(validBytes.size() > 80);
    CHECK(std::memcmp(validBytes.data(), "BROTBL03", 8) == 0);

    // Streaming file save must match in-memory serialization byte-for-byte.
    {
        std::ifstream ifs(tmp, std::ios::binary | std::ios::ate);
        CHECK(ifs.is_open());
        const size_t fsz = static_cast<size_t>(ifs.tellg());
        CHECK(fsz == validBytes.size());
        std::vector<uint8_t> diskBytes(fsz);
        ifs.seekg(0);
        ifs.read(reinterpret_cast<char*>(diskBytes.data()), static_cast<std::streamsize>(fsz));
        CHECK(diskBytes == validBytes);
    }

    auto loaded = loadTableFromFile(tmp.string(), &err);
    CHECK(loaded != nullptr);
    CHECK(loaded->hasIndex(0));
    CHECK(loaded->hasRangeIndex(1));
    CHECK(loaded->hasRangeIndex(2));
    CHECK(loaded->hasRangeIndex(4));
    CHECK(loaded->hasSpatialIndex("pos"));
    CHECK(!loaded->snapshot()->findIndex(0)->isMapped());
    expectTablesEqual(*loaded, tbl);

    // Zero-copy mmap must map columns AND all Hash/Range/Spatial indexes directly off OS pages.
    {
        auto mapped = mmapSnapshotFromFile(tmp.string(), &err);
        CHECK(mapped != nullptr);
        CHECK(mapped->findIndex(0) != nullptr && mapped->findIndex(0)->isMapped());
        CHECK(mapped->findRangeIndex(1) != nullptr && mapped->findRangeIndex(1)->isMapped());
        CHECK(mapped->findRangeIndex(2) != nullptr && mapped->findRangeIndex(2)->isMapped());
        CHECK(mapped->findRangeIndex(4) != nullptr && mapped->findRangeIndex(4)->isMapped());
        CHECK(mapped->findSpatialIndex("pos") != nullptr && mapped->findSpatialIndex("pos")->isMapped());
        CHECK(mapped->stats().mappedIndexes == 5);
        CHECK(reinterpret_cast<uintptr_t>(mapped->findIndex(0)->bucketData()) % 64 == 0);
        CHECK(reinterpret_cast<uintptr_t>(mapped->findRangeIndex(2)->numData()) % 64 == 0);
        CHECK(reinterpret_cast<uintptr_t>(mapped->findSpatialIndex("pos")->bucketData()) % 64 == 0);
        CHECK(mapped->chunks[0].columns[2]->hasNulls());
        CHECK(mapped->chunks[0].columns[2]->nullWordsData() != nullptr);

        // Hash index equality query on mmapped index
        {
            QueryPlan pq;
            pq.filters.push_back(pq.exprs.bin(ExprOp::Eq, pq.exprs.col("id"),
                                              pq.exprs.lit(ScalarValue::makeI32(42))));
            CHECK(bindQueryPlan(pq, *mapped, err));
            auto mr = executeQuery(*mapped, pq);
            CHECK(mr.ok && mr.usedIndex && mr.table->rowCount() == 1);
        }
        // Numeric range + Top-K orderBy on mmapped RangeIndex
        {
            QueryPlan rq;
            rq.filters.push_back(rq.exprs.bin(ExprOp::Ge, rq.exprs.col("x"),
                                              rq.exprs.lit(ScalarValue::makeF64(400.0))));
            rq.orderBy.push_back(OrderByItem{rq.exprs.col("x"), true});
            rq.limit = 5;
            CHECK(bindQueryPlan(rq, *mapped, err));
            auto mr = executeQuery(*mapped, rq);
            CHECK(mr.ok && mr.usedRangeIndex && mr.table->rowCount() == 5);
        }
        // String RangeIndex lookup + Top-K on mmapped string index
        {
            QueryPlan sq;
            sq.filters.push_back(sq.exprs.bin(ExprOp::Ge, sq.exprs.col("note"),
                                              sq.exprs.lit(ScalarValue::makeString("n9"))));
            sq.orderBy.push_back(OrderByItem{sq.exprs.col("note"), true});
            sq.limit = 3;
            CHECK(bindQueryPlan(sq, *mapped, err));
            auto mr = executeQuery(*mapped, sq);
            CHECK(mr.ok && mr.usedRangeIndex && mr.table->rowCount() == 3);
        }
        // Spatial AABB + Radius + kNN on mmapped SpatialIndex
        {
            QueryPlan spq;
            spq.spatialFilter.kind = SpatialFilterSpec::Kind::Radius;
            spq.spatialFilter.indexName = "pos";
            spq.spatialFilter.a[0] = 25.0;
            spq.spatialFilter.a[1] = 50.0;
            spq.spatialFilter.radius = 20.0;
            CHECK(bindQueryPlan(spq, *mapped, err));
            auto mr = executeQuery(*mapped, spq);
            CHECK(mr.ok && mr.usedSpatialIndex && mr.table->rowCount() > 0);

            std::vector<std::pair<RowLoc, double>> knn;
            mapped->findSpatialIndex("pos")->queryNearest(25.0, 50.0, 0.0, 3, -1.0, knn);
            CHECK(knn.size() == 3);
        }
        // Materialize mutable copy from mmap + mutate independently
        {
            auto cloned = Table::fromSnapshot(*mapped);
            CHECK(cloned->rowCount() == 500);
            CHECK(!cloned->snapshot()->findIndex(0)->isMapped());
            CHECK(cloned->updateCell(RowLoc{0, 42}, 0, ScalarValue::makeI32(9999)));
            RowLoc outLoc{};
            CHECK(cloned->resolveAliveRow(42, outLoc));
            CHECK(cloned->getCell(outLoc, 0).i64 == 9999);
            CHECK(mapped->getCell(0, 0, 42).i64 == 42);
        }
    }

    // Verified mmap check passes on clean file
    {
        auto verifiedMap = mmapSnapshotFromFile(tmp.string(), &err, true);
        CHECK(verifiedMap != nullptr && verifiedMap->stats().mappedIndexes == 5);
    }

    // Corrupting last payload byte is rejected by deserializeBytes AND mmap(verify=true)
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::in | std::ios::out);
        CHECK(ofs.is_open());
        ofs.seekp(-1, std::ios::end);
        char b = static_cast<char>(validBytes.back() ^ 0x5Au);
        ofs.write(&b, 1);
        ofs.flush();
    }
    {
        std::string verr;
        CHECK(mmapSnapshotFromFile(tmp.string(), &verr, true) == nullptr);
        CHECK(!verr.empty());
    }
    std::error_code ec;
    std::filesystem::remove(tmp, ec);

    {
        auto badCrc = validBytes;
        badCrc.back() ^= 0x5Au;
        std::string cerr;
        CHECK(deserializeTableBytes(badCrc.data(), badCrc.size(), &cerr) == nullptr);
        CHECK(!cerr.empty());
    }
    {
        auto badMeta = validBytes;
        badMeta[80] ^= 0xFFu;  // corrupt first metadata byte (metaChecksum must catch on fast mmap too)
        std::string merr;
        CHECK(deserializeTableBytes(badMeta.data(), badMeta.size(), &merr) == nullptr);
    }
    {
        std::string terr;
        CHECK(deserializeTableBytes(validBytes.data(), validBytes.size() - 10, &terr) == nullptr);
    }
    {
        auto badMag = validBytes;
        badMag[0] = 'X';
        std::string merr;
        CHECK(deserializeTableBytes(badMag.data(), badMag.size(), &merr) == nullptr);
    }
    {
        auto badCol = validBytes;
        badCol[36] = 250;
        std::string berr;
        CHECK(deserializeTableBytes(badCol.data(), badCol.size(), &berr) == nullptr);
    }
}

void testExplainQuery() {
    Table tbl({{"id", ColType::I32}, {"score", ColType::F64}, {"x", ColType::F64}, {"y", ColType::F64}});
    for (int i = 0; i < 100; ++i) {
        addRow(tbl, {ScalarValue::makeI32(i), ScalarValue::makeF64(i * 1.5),
                     ScalarValue::makeF64(i * 1.0), ScalarValue::makeF64(i * 1.0)});
    }
    CHECK(tbl.createIndex("id"));
    CHECK(tbl.createRangeIndex("score"));
    CHECK(tbl.createSpatialIndex("pos", {2, 3}, 10.0));
    auto snap = tbl.snapshot();

    auto strat = [&](QueryPlan p, ExecOptions o = {}) -> QueryExplainResult {
        std::string err;
        CHECK(bindQueryPlan(p, *snap, err));
        return explainQuery(*snap, std::move(p), o);
    };

    {
        QueryPlan p;
        p.spatialFilter.kind = SpatialFilterSpec::Kind::Radius;
        p.spatialFilter.indexName = "pos";
        p.spatialFilter.radius = 5.0;
        auto ex = strat(p);
        CHECK(ex.strategy == "spatial_index" && ex.willUseSpatialIndex);
    }
    {
        QueryPlan p;
        p.filters.push_back(p.exprs.bin(ExprOp::Eq, p.exprs.col("id"),
                                        p.exprs.lit(ScalarValue::makeI32(5))));
        auto ex = strat(p);
        CHECK(ex.strategy == "hash_index" && ex.willUseIndex && ex.indexColumn == "id");
    }
    {
        QueryPlan p;
        p.filters.push_back(p.exprs.bin(ExprOp::Gt, p.exprs.col("score"),
                                        p.exprs.lit(ScalarValue::makeF64(20.0))));
        auto ex = strat(p);
        CHECK(ex.strategy == "range_index" && ex.willUseRangeIndex);
    }
    {
        QueryPlan p;
        p.orderBy.push_back(OrderByItem{p.exprs.col("score"), true});
        p.limit = 5;
        auto ex = strat(p);
        CHECK(ex.strategy == "range_topk" && ex.willUseRangeIndex);
    }
    {
        QueryPlan p;
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Sum, "sx", p.exprs.col("x"), ColType::F64});
        auto ex = strat(p);
        CHECK(ex.strategy == "jit_agg" && ex.willUseJitAgg && ex.willUseSimd);
        CHECK(ex.simdWidth == (brass::host_has_avx2() ? 256 : 128));
    }
    {
        QueryPlan p;
        p.filters.push_back(p.exprs.bin(ExprOp::Gt, p.exprs.col("x"),
                                        p.exprs.lit(ScalarValue::makeF64(10.0))));
        auto ex = strat(p);
        CHECK(ex.strategy == "jit_simd" && ex.willUseSimd);
        CHECK(ex.simdWidth == (brass::host_has_avx2() ? 256 : 128));
    }
    {
        QueryPlan p;
        p.filters.push_back(p.exprs.un(ExprOp::IsNull, p.exprs.col("x")));
        ExecOptions noJit;
        noJit.useJit = false;
        auto ex = strat(p, noJit);
        CHECK(ex.strategy == "reference");
    }
}

}  // namespace

int main() {
    testSchemaDuplicateAndStatsClearClone();
    testNullBitmapLifecycle();
    testNullQuerySemanticsAndAggregates();
    testNullsWithIndexesAndMutations();
    testAvx2SimdAndPrecisionParity();
    testIndexFreelistChurnAndNearestKnn();
    testParallelFastGroupBy();
    testBrotableV3StreamingAndMappedIndexes();
    testExplainQuery();
    std::cout << "test_table_production OK\n";
    return 0;
}
