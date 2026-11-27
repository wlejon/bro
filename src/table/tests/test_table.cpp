#include "table/query_eval.h"
#include "table/query_exec.h"
#include "table/query_ir.h"
#include "table/query_jit.h"
#include "table/table.h"
#include "brass/mir/builder.hpp"
#include "brass/mir/module.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using namespace bro::table;

namespace {

#define EXPECT(cond)                                                               \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "]: " #cond   \
                      << "\n";                                                     \
            std::abort();                                                          \
        }                                                                          \
    } while (0)

void expectTablesEqual(const Table& a, const Table& b) {
    EXPECT(a.columnCount() == b.columnCount());
    EXPECT(a.rowCount() == b.rowCount());
    for (size_t c = 0; c < a.columnCount(); ++c) {
        EXPECT(a.schema().columns[c].name == b.schema().columns[c].name);
        EXPECT(a.schema().columns[c].type == b.schema().columns[c].type);
    }
    for (size_t r = 0; r < a.rowCount(); ++r) {
        RowLoc la{}, lb{};
        EXPECT(a.resolveAliveRow(r, la));
        EXPECT(b.resolveAliveRow(r, lb));
        for (size_t c = 0; c < a.columnCount(); ++c) {
            const ScalarValue va = a.getCell(la, c);
            const ScalarValue vb = b.getCell(lb, c);
            if (colTypeIsFloat(va.type)) {
                if (std::isnan(va.f64)) {
                    EXPECT(std::isnan(vb.f64));
                } else {
                    EXPECT(std::fabs(va.f64 - vb.f64) <= 1e-5);
                }
            } else {
                EXPECT(compareScalar(va, vb) == 0);
            }
        }
    }
}

std::shared_ptr<Table> makeMultiTypeTable(size_t rows) {
    auto tbl = std::make_shared<Table>(std::vector<ColumnDef>{
        {"id", ColType::I32},
        {"flags", ColType::U32},
        {"big", ColType::I64},
        {"x", ColType::F32},
        {"y", ColType::F64},
        {"active", ColType::Bool},
        {"kind", ColType::Dict},
        {"label", ColType::String},
    });
    const char* kinds[] = {"hero", "npc", "item", "boss"};
    for (size_t i = 0; i < rows; ++i) {
        const int32_t id = static_cast<int32_t>(i) - 10;
        const uint32_t flags = (i % 7 == 0) ? 0x80000005u : static_cast<uint32_t>(i * 3 + 1);
        const int64_t big = static_cast<int64_t>(i) * 100000LL - 500000LL;
        const float x = static_cast<float>(i % 97) * 0.5f - 12.0f;
        const double y = (i == 5) ? -0.0 : (static_cast<double>(i % 131) * 1.25);
        const bool active = (i % 3 != 0);
        const char* kind = kinds[i % 4];
        std::string label = std::string(kind) + "_" + std::to_string(i);
        ScalarValue row[8] = {
            ScalarValue::makeI32(id),
            ScalarValue::makeU32(flags),
            ScalarValue::makeI64(big),
            ScalarValue::makeF32(x),
            ScalarValue::makeF64(y),
            ScalarValue::makeBool(active),
            ScalarValue::makeString(kind),
            ScalarValue::makeString(label),
        };
        tbl->appendRow(row);
    }
    return tbl;
}

void testAllTypesAndCompaction() {
    auto tbl = makeMultiTypeTable(10000);  // spans 3 chunks (4096 + 4096 + 1808)
    EXPECT(tbl->rowCount() == 10000);
    EXPECT(tbl->chunkCount() == 3);

    RowLoc loc{};
    EXPECT(tbl->resolveAliveRow(4096, loc));
    EXPECT(loc.chunkIdx == 1 && loc.rowIdx == 0);
    EXPECT(tbl->getCell(loc, 0).i64 == 4086);
    EXPECT(tbl->getCell(loc, 6).str == "hero");
    EXPECT(tbl->getCell(loc, 7).str == "hero_4096");

    // Delete alternating rows, verify alive resolution and compaction
    for (size_t i = 0; i < 5000; ++i) {
        EXPECT(tbl->removeRow(RowLoc{static_cast<uint32_t>(i / kChunkRows),
                                     static_cast<uint32_t>(i % kChunkRows)}));
    }
    EXPECT(tbl->rowCount() == 5000);
    tbl->compact();
    EXPECT(tbl->rowCount() == 5000);
    EXPECT(tbl->chunkCount() == 2);
    EXPECT(tbl->resolveAliveRow(0, loc));
    EXPECT(tbl->getCell(loc, 0).i64 == 4990);
}

void testCowSnapshotIsolationAndConcurrency() {
    auto tbl = makeMultiTypeTable(6000);
    tbl->createIndex("id");
    tbl->createIndex("kind");

    auto snapBefore = tbl->snapshot();
    const uint64_t expectedSumBefore = [&]() {
        QueryPlan p;
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Sum, "s", p.exprs.col("id"), ColType::I64});
        auto r = executeQuery(*snapBefore, p);
        RowLoc rl{};
        EXPECT(r.table->resolveAliveRow(0, rl));
        return static_cast<uint64_t>(r.table->getCell(rl, 0).i64);
    }();

    // Concurrent readers querying snapBefore while main thread heavily mutates table
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&, snapBefore, expectedSumBefore]() {
            while (!stop.load(std::memory_order_relaxed)) {
                QueryPlan p;
                p.aggregates.push_back(
                    AggSpec{AggSpec::Kind::Sum, "s", p.exprs.col("id"), ColType::I64});
                auto r = executeQuery(*snapBefore, p, ExecOptions{true, true, false});
                RowLoc rl{};
                EXPECT(r.table->resolveAliveRow(0, rl));
                EXPECT(static_cast<uint64_t>(r.table->getCell(rl, 0).i64) == expectedSumBefore);

                QueryPlan idxQ;
                idxQ.filters.push_back(
                    idxQ.exprs.bin(ExprOp::Eq, idxQ.exprs.col("id"),
                                   idxQ.exprs.lit(ScalarValue::makeI32(100))));
                auto ir = executeQuery(*snapBefore, idxQ, ExecOptions{true, true, false});
                EXPECT(ir.table->rowCount() == 1);
            }
        });
    }

    for (int step = 0; step < 50; ++step) {
        for (int r = 0; r < 20; ++r) {
            RowLoc loc{static_cast<uint32_t>(step % 2), static_cast<uint32_t>(r * 10 + 1)};
            tbl->updateCell(loc, 0, ScalarValue::makeI32(99999 + step));
        }
        ScalarValue extra[8] = {
            ScalarValue::makeI32(777777), ScalarValue::makeU32(1), ScalarValue::makeI64(2),
            ScalarValue::makeF32(1.0f), ScalarValue::makeF64(2.0), ScalarValue::makeBool(true),
            ScalarValue::makeString("boss"), ScalarValue::makeString("extra")};
        tbl->appendRow(extra);
        tbl->removeRow(RowLoc{0, static_cast<uint32_t>(step)});
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& th : readers) th.join();

    // Verify original snapshot remained 100% isolated
    EXPECT(snapBefore->rowCount() == 6000);
    EXPECT(tbl->rowCount() == 6000);
}

void testDifferentialFiltersProjectionsOrderLimit() {
    auto tbl = makeMultiTypeTable(20000);  // 5 chunks
    auto snap = tbl->snapshot();

    CompiledPlanCache::instance().clear();

    // Query 1: Math + Bitwise + Safe Div/Mod + Dict + Bool filter + direct JIT projection
    {
        QueryPlan p;
        auto& e = p.exprs;
        uint32_t f1 = e.bin(ExprOp::And,
                            e.bin(ExprOp::Gt, e.col("id"), e.lit(ScalarValue::makeI32(100))),
                            e.col("active"));
        uint32_t f2 = e.bin(ExprOp::Ne, e.col("kind"), e.lit(ScalarValue::makeString("boss")));
        uint32_t modZeroSafe = e.bin(
            ExprOp::Eq,
            e.bin(ExprOp::Mod, e.col("flags"),
                  e.cond(e.bin(ExprOp::Eq, e.col("id"), e.lit(ScalarValue::makeI32(101))),
                         e.lit(ScalarValue::makeI32(0)),
                         e.lit(ScalarValue::makeI32(3)))),
            e.lit(ScalarValue::makeI32(1)));
        p.filters = {f1, f2, modZeroSafe};

        uint32_t score = e.bin(
            ExprOp::Add,
            e.un(ExprOp::Sqrt, e.un(ExprOp::Abs, e.col("y"))),
            e.un(ExprOp::Floor, e.bin(ExprOp::Div, e.col("x"), e.lit(ScalarValue::makeF64(2.0)))));
        p.selectItems.push_back(SelectItem{"id", e.col("id"), ColType::I32});
        p.selectItems.push_back(SelectItem{"kind", e.col("kind"), ColType::Dict});
        p.selectItems.push_back(SelectItem{"score", score, ColType::F64});
        p.limit = 500;
        p.offset = 17;

        auto refTbl = executePlanReference(*snap, p);
        auto jitRes = executeQuery(*snap, p);
        if (!jitRes.usedJit) {
            std::cerr << "JIT error: " << jitRes.jitError << "\n";
        }
        EXPECT(jitRes.ok && jitRes.usedJit);
        expectTablesEqual(*refTbl, *jitRes.table);

        // Second execution must hit cache!
        auto before = CompiledPlanCache::instance().stats();
        auto jitRes2 = executeQuery(*snap, p);
        auto after = CompiledPlanCache::instance().stats();
        EXPECT(after.hits == before.hits + 1);
        EXPECT(after.misses == before.misses);
        expectTablesEqual(*refTbl, *jitRes2.table);
    }

    // Query 2: Hybrid JIT + String residual filter + OrderBy Top-K + Offset
    {
        QueryPlan p;
        auto& e = p.exprs;
        p.filters.push_back(e.bin(ExprOp::Ge, e.col("y"), e.lit(ScalarValue::makeF64(10.0))));
        p.filters.push_back(e.bin(ExprOp::StrContains, e.col("label"),
                                  e.lit(ScalarValue::makeString("_19"))));
        p.selectItems.push_back(SelectItem{"id", e.col("id"), ColType::I32});
        p.selectItems.push_back(SelectItem{"y", e.col("y"), ColType::F64});
        p.selectItems.push_back(SelectItem{"label", e.col("label"), ColType::String});
        p.orderBy.push_back(OrderByItem{e.col("y"), true});
        p.orderBy.push_back(OrderByItem{e.col("id"), false});
        p.offset = 5;
        p.limit = 25;

        auto refTbl = executePlanReference(*snap, p);
        auto jitRes = executeQuery(*snap, p);
        if (!jitRes.usedJit) {
            std::cerr << "JIT error: " << jitRes.jitError << "\n";
        }
        EXPECT(jitRes.ok && jitRes.usedJit);
        expectTablesEqual(*refTbl, *jitRes.table);
    }

    // Query 2b: OrderBy over computed SELECT alias ("score")
    {
        QueryPlan p;
        auto& e = p.exprs;
        p.filters.push_back(e.bin(ExprOp::Gt, e.col("id"), e.lit(ScalarValue::makeI32(19900))));
        p.selectItems.push_back(SelectItem{"id", e.col("id"), ColType::I32});
        p.selectItems.push_back(
            SelectItem{"score", e.bin(ExprOp::Mul, e.col("x"), e.lit(ScalarValue::makeF64(3.0))),
                       ColType::F64});
        p.orderBy.push_back(OrderByItem{e.col("score"), true});
        p.orderBy.push_back(OrderByItem{e.col("id"), false});
        p.limit = 15;

        auto refTbl = executePlanReference(*snap, p);
        auto jitRes = executeQuery(*snap, p);
        if (!jitRes.usedJit) {
            std::cerr << "JIT error: " << jitRes.jitError << "\n";
        }
        EXPECT(jitRes.ok && jitRes.usedJit);
        expectTablesEqual(*refTbl, *jitRes.table);
    }

    // Query 3: Bitwise ops & signed/unsigned mixed comparisons + INT32_MIN / -1 safety
    {
        auto edge = std::make_shared<Table>(std::vector<ColumnDef>{
            {"a", ColType::I32}, {"b", ColType::I32}, {"u", ColType::U32}});
        ScalarValue r0[3] = {ScalarValue::makeI32(INT32_MIN), ScalarValue::makeI32(-1),
                             ScalarValue::makeU32(0x80000000u)};
        ScalarValue r1[3] = {ScalarValue::makeI32(42), ScalarValue::makeI32(0),
                             ScalarValue::makeU32(7u)};
        edge->appendRow(r0);
        edge->appendRow(r1);
        auto es = edge->snapshot();

        QueryPlan p;
        auto& e = p.exprs;
        p.filters.push_back(e.bin(ExprOp::Gt, e.col("u"), e.lit(ScalarValue::makeI32(-5))));
        p.selectItems.push_back(SelectItem{"q", e.bin(ExprOp::Div, e.col("a"), e.col("b")), ColType::I32});
        p.selectItems.push_back(SelectItem{"m", e.bin(ExprOp::Mod, e.col("a"), e.col("b")), ColType::I32});
        p.selectItems.push_back(SelectItem{"sh", e.bin(ExprOp::UShr, e.col("u"), e.lit(ScalarValue::makeI32(1))), ColType::I32});
        auto refTbl = executePlanReference(*es, p);
        auto jitRes = executeQuery(*es, p);
        EXPECT(jitRes.ok && jitRes.usedJit);
        expectTablesEqual(*refTbl, *jitRes.table);
    }
}

void testIndexJoinGroupByAndUpdates() {
    auto leftTbl = makeMultiTypeTable(5000);
    leftTbl->createIndex("id");
    leftTbl->createIndex("y");
    leftTbl->createIndex("label");

    auto rightTbl = std::make_shared<Table>(std::vector<ColumnDef>{
        {"kind_name", ColType::String},
        {"bonus", ColType::F64},
    });
    ScalarValue rk0[2] = {ScalarValue::makeString("hero"), ScalarValue::makeF64(50.0)};
    ScalarValue rk1[2] = {ScalarValue::makeString("boss"), ScalarValue::makeF64(100.0)};
    ScalarValue rk2[2] = {ScalarValue::makeString("npc"), ScalarValue::makeF64(10.0)};
    rightTbl->appendRow(rk0);
    rightTbl->appendRow(rk1);
    rightTbl->appendRow(rk2);
    rightTbl->createIndex("kind_name");

    auto lSnap = leftTbl->snapshot();
    auto rSnap = rightTbl->snapshot();

    // 1. Index fast path including -0.0 == +0.0 on Float64 HashIndex!
    {
        QueryPlan p;
        p.filters.push_back(
            p.exprs.bin(ExprOp::Eq, p.exprs.col("y"), p.exprs.lit(ScalarValue::makeF64(0.0))));
        auto refTbl = executePlanReference(*lSnap, p);
        auto jitRes = executeQuery(*lSnap, p);
        EXPECT(jitRes.ok && jitRes.usedIndex);
        expectTablesEqual(*refTbl, *jitRes.table);
    }

    // 2. Inner + Left Hash Join + GroupBy + OrderBy
    for (JoinSpec::Kind jk : {JoinSpec::Kind::Inner, JoinSpec::Kind::Left}) {
        QueryPlan p;
        auto& e = p.exprs;
        p.join.kind = jk;
        p.join.rightTable = rSnap;
        p.join.leftKeys = {"kind"};
        p.join.rightKeys = {"kind_name"};
        p.filters.push_back(e.bin(ExprOp::Ge, e.col("id"), e.lit(ScalarValue::makeI32(0))));
        p.groupKeys.push_back(e.col("kind", 0));
        p.groupAliases = {"kind"};
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Count, "cnt", kInvalidExpr, ColType::I64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Sum, "bonus_sum", e.col("bonus", 1), ColType::F64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Avg, "avg_y", e.col("y", 0), ColType::F64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Max, "max_id", e.col("id", 0), ColType::I32});
        p.orderBy.push_back(OrderByItem{e.col("bonus_sum"), true});
        p.orderBy.push_back(OrderByItem{e.col("kind"), false});

        auto refTbl = executePlanReference(*lSnap, p);
        auto jitRes = executeQuery(*lSnap, p);
        EXPECT(jitRes.ok);
        expectTablesEqual(*refTbl, *jitRes.table);
    }

    // 3. In-place JIT scatter update vs reference update (with snapshot isolation!)
    auto copyForRef = makeMultiTypeTable(5000);
    auto snapHeld = leftTbl->snapshot();

    QueryPlan up;
    up.filters.push_back(
        up.exprs.bin(ExprOp::Lt, up.exprs.col("id"), up.exprs.lit(ScalarValue::makeI32(200))));
    up.updates.push_back(UpdateAssignment{
        "y", -1, up.exprs.bin(ExprOp::Add, up.exprs.col("y"), up.exprs.lit(ScalarValue::makeF64(42.5)))});
    up.updates.push_back(UpdateAssignment{
        "active", -1, up.exprs.un(ExprOp::Not, up.exprs.col("active"))});

    const size_t refAffected = applyUpdateReference(*copyForRef, up);
    auto jitUp = executeUpdate(*leftTbl, up);
    EXPECT(jitUp.ok && jitUp.usedJit);
    EXPECT(jitUp.affectedRows == refAffected);
    expectTablesEqual(*leftTbl, *copyForRef);
    // snapHeld must NOT observe updates (COW check against unmodified table copy):
    EXPECT(snapHeld->rowCount() == 5000);
    EXPECT(snapHeld->getCell(5, 0, 15).i64 != copyForRef->getCell(RowLoc{0, 15}, 5).i64);
}

void testFusedCrossModuleUdf() {
    auto tbl = makeMultiTypeTable(10000);
    auto snap = tbl->snapshot();

    // Build an external MIR module holding unboxed, NaN-boxed, and allocating predicates.
    brass::Module extMod("external_udf_domain");
    brass::Builder b(extMod);

    // 1. Unboxed predicate: (y: f64, id: i32) -> i32 : (y > 20.0) && (id >= 0) && ((id % 2) == 0)
    {
        brass::Function* fn = extMod.create_function(
            "pred_unboxed", brass::Type::i32(), {brass::Type::f64(), brass::Type::i32()});
        b.set_function(fn);
        brass::BasicBlock* entry = b.append_block("entry");
        brass::Value* y = b.add_block_param(entry, brass::Type::f64());
        brass::Value* id = b.add_block_param(entry, brass::Type::i32());
        b.position_at_end(entry);
        brass::Value* c0 = b.build_sgt(y, b.build_fconst_f64(20.0));
        brass::Value* c1 = b.build_sge(id, b.build_iconst_i32(0));
        brass::Value* even = b.build_eq(b.build_and(id, b.build_iconst_i32(1)), b.build_iconst_i32(0));
        b.build_ret(b.build_and(b.build_and(c0, c1), even));
        fn->rebuild_cfg_predecessors();
    }

    // 2. NaN-boxed JS-ABI predicate: (y_tag: tagged, id_tag: tagged) -> tagged
    {
        brass::Function* fn = extMod.create_function(
            "pred_nanboxed", brass::Type::tagged(), {brass::Type::tagged(), brass::Type::tagged()});
        b.set_function(fn);
        brass::BasicBlock* entry = b.append_block("entry");
        brass::Value* yTag = b.add_block_param(entry, brass::Type::tagged());
        brass::Value* idTag = b.add_block_param(entry, brass::Type::tagged());
        b.position_at_end(entry);
        brass::Value* y = b.build_bitcast_f64_i64(b.build_bitcast_i64_tagged(yTag));
        brass::Value* id = b.build_trunc_i32(b.build_bitcast_i64_tagged(idTag));
        brass::Value* c0 = b.build_sgt(y, b.build_fconst_f64(20.0));
        brass::Value* c1 = b.build_sge(id, b.build_iconst_i32(0));
        brass::Value* even = b.build_eq(b.build_and(id, b.build_iconst_i32(1)), b.build_iconst_i32(0));
        brass::Value* cond = b.build_and(b.build_and(c0, c1), even);
        brass::Value* boxed = b.build_bitcast_tagged_i64(b.build_or(
            b.build_zext_i64(cond), b.build_iconst_i64(static_cast<int64_t>(0x7FF9000000000000ULL))));
        b.build_ret(boxed);
        fn->rebuild_cfg_predecessors();
    }

    // 3. Disallowed allocating external callee (must be rejected by DomainAuditReport).
    {
        extMod.add_external_symbol("ext_gc_alloc");
        extMod.add_allocation_function("ext_gc_alloc");
        brass::Function* fn = extMod.create_function(
            "pred_alloc", brass::Type::i32(), {brass::Type::i32()});
        b.set_function(fn);
        brass::BasicBlock* entry = b.append_block("entry");
        brass::Value* id = b.add_block_param(entry, brass::Type::i32());
        b.position_at_end(entry);
        (void)b.build_call("ext_gc_alloc", brass::Type::i64(), {id});
        b.build_ret(b.build_sge(id, b.build_iconst_i32(0)));
        fn->rebuild_cfg_predecessors();
    }

    std::vector<uint32_t> expectedRows;
    for (uint32_t row = 0; row < 10000; ++row) {
        const int32_t id = static_cast<int32_t>(row) - 10;
        const double y = (row == 5) ? -0.0 : (static_cast<double>(row % 131) * 1.25);
        if (y > 20.0 && id >= 0 && ((id & 1) == 0)) expectedRows.push_back(row);
    }

    auto runAndVerify = [&](const std::shared_ptr<const CompiledKernel>& kern) {
        EXPECT(kern != nullptr && kern->entry != nullptr);
        std::vector<uint32_t> gotRows;
        uint32_t globalBase = 0;
        for (const auto& ch : snap->chunks) {
            std::vector<const void*> colPtrs(snap->schema.columns.size(), nullptr);
            for (size_t c = 0; c < snap->schema.columns.size(); ++c) {
                colPtrs[c] = ch.columns[c]->rawData();
            }
            std::vector<uint32_t> matchBuf(ch.rowCount, 0);
            JitChunkContext ctx{};
            ctx.leftColPtrs = colPtrs.data();
            ctx.aliveWords = ch.aliveWords.data();
            ctx.hasAliveMask = (ch.aliveCount < ch.rowCount) ? 1u : 0u;
            ctx.rowCount = ch.rowCount;
            ctx.matchIndices = matchBuf.data();
            kern->entry(&ctx);
            for (uint32_t m = 0; m < ctx.matchCount; ++m) {
                gotRows.push_back(globalBase + matchBuf[m]);
            }
            globalBase += ch.rowCount;
        }
        EXPECT(gotRows == expectedRows);
    };

    std::string err;
    FusedUdfSpec specUnboxed{&extMod, "pred_unboxed", {4u, 0u}, false};
    auto kUnboxed = compileFusedUdfFilterKernel(*snap, specUnboxed, &err);
    EXPECT(kUnboxed != nullptr);
    runAndVerify(kUnboxed);

    err.clear();
    FusedUdfSpec specNanBox{&extMod, "pred_nanboxed", {4u, 0u}, true};
    auto kNanBox = compileFusedUdfFilterKernel(*snap, specNanBox, &err);
    EXPECT(kNanBox != nullptr);
    runAndVerify(kNanBox);

    err.clear();
    FusedUdfSpec specBad{&extMod, "pred_alloc", {0u}, false};
    auto kBad = compileFusedUdfFilterKernel(*snap, specBad, &err);
    EXPECT(kBad == nullptr && !err.empty());
}

}  // namespace

int main() {
    testAllTypesAndCompaction();
    testCowSnapshotIsolationAndConcurrency();
    testDifferentialFiltersProjectionsOrderLimit();
    testIndexJoinGroupByAndUpdates();
    testFusedCrossModuleUdf();
    std::cout << "bro_table_test: all tests passed.\n";
    return 0;
}
