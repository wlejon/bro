#include "brass/target/host_cpu.hpp"
#include "table/query_eval.h"
#include "table/query_exec.h"
#include "table/query_ir.h"
#include "table/query_jit.h"
#include "table/table.h"
#include "table/table_io.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace bro::table;

namespace {

template <typename Fn>
double measureMs(int iters, Fn&& fn) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) fn();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(iters);
}

void printBench(const char* name, double ms, double rowsOrOps, const char* extra = "") {
    const double mRowsPerSec = (ms > 0.0) ? (rowsOrOps / (ms * 1000.0)) : 0.0;
    std::cout << std::left << std::setw(38) << name << " | "
              << std::right << std::setw(8) << std::fixed << std::setprecision(3) << ms << " ms | "
              << std::setw(8) << std::setprecision(2) << mRowsPerSec << " M/s"
              << (extra && extra[0] ? std::string(" | ") + extra : std::string{}) << "\n";
}

}  // namespace

int main() {
    constexpr size_t kRows = 1000000;
    std::cout << "=== bro_table benchmark (" << kRows << " rows, AVX2="
              << (brass::host_has_avx2() ? "ON" : "OFF") << ") ===\n";

    Table tbl({
        {"id", ColType::I32},
        {"kind", ColType::Dict},
        {"hp", ColType::F32},
        {"score", ColType::F64},
        {"x", ColType::F64},
        {"y", ColType::F64},
    });

    const char* kinds[8] = {"hero", "npc", "boss", "item", "trap", "quest", "merchant", "guard"};
    std::vector<uint32_t> kindDictIds(8);
    for (int i = 0; i < 8; ++i) kindDictIds[i] = tbl.internString(kinds[i]);

    const auto tBuild0 = std::chrono::steady_clock::now();
    std::vector<int32_t> idBuf(kRows);
    std::vector<uint32_t> kindBuf(kRows);
    std::vector<float> hpBuf(kRows);
    std::vector<double> scoreBuf(kRows);
    std::vector<double> xBuf(kRows);
    std::vector<double> yBuf(kRows);
    for (size_t i = 0; i < kRows; ++i) {
        idBuf[i] = static_cast<int32_t>(i + 1);
        kindBuf[i] = kindDictIds[i & 7];
        hpBuf[i] = 10.0f + static_cast<float>(i % 200);
        scoreBuf[i] = static_cast<double>((i * 17) % 10000) * 0.5;
        xBuf[i] = static_cast<double>((i * 37) % 2000) - 1000.0;
        yBuf[i] = static_cast<double>((i * 53) % 2000) - 1000.0;
    }
    const void* colPtrs[6] = {idBuf.data(), kindBuf.data(), hpBuf.data(),
                              scoreBuf.data(), xBuf.data(), yBuf.data()};
    tbl.appendTypedBatch(kRows, colPtrs);
    const double insertMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tBuild0).count();
    printBench("appendTypedBatch (1M x 6 cols)", insertMs, static_cast<double>(kRows));

    tbl.createIndex("id");
    tbl.createRangeIndex("score");
    tbl.createSpatialIndex("pos", {4, 5}, 32.0);
    auto snap = tbl.snapshot();

    // 1. SIMD filter (col > lit) vs Reference
    {
        QueryPlan p;
        p.filters.push_back(p.exprs.bin(ExprOp::Gt, p.exprs.col("score"),
                                        p.exprs.lit(ScalarValue::makeF64(4500.0))));
        p.selectItems.push_back(SelectItem{"id", p.exprs.col("id"), ColType::I32});
        std::string err;
        bindQueryPlan(p, *snap, err);
        CompiledPlanCache::instance().getOrCompile(p, *snap, JitMode::Filter);

        uint16_t width = 0;
        double simdMs = measureMs(10, [&]() {
            auto r = executeQuery(*snap, p);
            width = r.simdWidth;
        });
        double refMs = measureMs(2, [&]() {
            ExecOptions o;
            o.useJit = false;
            o.useIndexes = false;
            executeQuery(*snap, p, o);
        });
        std::string info = "simdWidth=" + std::to_string(width);
        printBench("SIMD Filter (score > 4500)", simdMs, static_cast<double>(kRows), info.c_str());
        printBench("Reference Filter (score > 4500)", refMs, static_cast<double>(kRows));
    }

    // 2. Compound arithmetic SIMD filter: ((x + 10.0) * 2.0) > 1900.0
    {
        QueryPlan p;
        auto xAdd = p.exprs.bin(ExprOp::Add, p.exprs.col("x"),
                                p.exprs.lit(ScalarValue::makeF64(10.0)));
        auto mul = p.exprs.bin(ExprOp::Mul, xAdd, p.exprs.lit(ScalarValue::makeF64(2.0)));
        p.filters.push_back(p.exprs.bin(ExprOp::Gt, mul,
                                        p.exprs.lit(ScalarValue::makeF64(1900.0))));
        p.selectItems.push_back(SelectItem{"id", p.exprs.col("id"), ColType::I32});
        std::string err;
        bindQueryPlan(p, *snap, err);
        CompiledPlanCache::instance().getOrCompile(p, *snap, JitMode::Filter);

        uint16_t width = 0;
        double ms = measureMs(10, [&]() {
            auto r = executeQuery(*snap, p);
            width = r.simdWidth;
        });
        std::string info = "simdWidth=" + std::to_string(width);
        printBench("SIMD Arith Filter ((x+10)*2 > 1900)", ms, static_cast<double>(kRows), info.c_str());
    }

    // 3. SIMD ungrouped Agg (Count, Sum, Min, Max, Avg)
    {
        QueryPlan p;
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Count, "cnt", kInvalidExpr, ColType::I64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Sum, "sum_score", p.exprs.col("score"), ColType::F64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Min, "min_score", p.exprs.col("score"), ColType::F64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Max, "max_score", p.exprs.col("score"), ColType::F64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Avg, "avg_score", p.exprs.col("score"), ColType::F64});
        std::string err;
        bindQueryPlan(p, *snap, err);
        CompiledPlanCache::instance().getOrCompileAgg(p, *snap);

        uint16_t width = 0;
        double simdMs = measureMs(10, [&]() {
            auto r = executeQuery(*snap, p);
            width = r.simdWidth;
        });
        double refMs = measureMs(2, [&]() {
            ExecOptions o;
            o.useJit = false;
            o.useIndexes = false;
            executeQuery(*snap, p, o);
        });
        std::string info = "simdWidth=" + std::to_string(width);
        printBench("SIMD Ungrouped Agg (5 metrics)", simdMs, static_cast<double>(kRows), info.c_str());
        printBench("Ref Ungrouped Agg (5 metrics)", refMs, static_cast<double>(kRows));
    }

    // 4. Parallel Fast GroupBy (Dict key)
    {
        QueryPlan p;
        p.groupKeys.push_back(p.exprs.col("kind"));
        p.groupAliases.push_back("kind");
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Count, "cnt", kInvalidExpr, ColType::I64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Sum, "sum_score", p.exprs.col("score"), ColType::F64});
        p.aggregates.push_back(AggSpec{AggSpec::Kind::Avg, "avg_hp", p.exprs.col("hp"), ColType::F64});
        std::string err;
        bindQueryPlan(p, *snap, err);
        CompiledPlanCache::instance().getOrCompile(p, *snap, JitMode::Filter);

        double parMs = measureMs(5, [&]() { executeQuery(*snap, p); });
        double serMs = measureMs(2, [&]() {
            ExecOptions o;
            o.useParallel = false;
            executeQuery(*snap, p, o);
        });
        printBench("Parallel Fast GroupBy (8 groups)", parMs, static_cast<double>(kRows));
        printBench("Serial Fast GroupBy (8 groups)", serMs, static_cast<double>(kRows));
    }

    // 5. Index probes (Hash, Range Top-K, Spatial AABB)
    {
        QueryPlan hp;
        hp.filters.push_back(hp.exprs.bin(ExprOp::Eq, hp.exprs.col("id"),
                                          hp.exprs.lit(ScalarValue::makeI32(543210))));
        std::string err;
        bindQueryPlan(hp, *snap, err);
        double hashMs = measureMs(500, [&]() { executeQuery(*snap, hp); });
        printBench("HashIndex point lookup", hashMs, 1.0);

        QueryPlan tk;
        tk.orderBy.push_back(OrderByItem{tk.exprs.col("score"), true});
        tk.limit = 20;
        bindQueryPlan(tk, *snap, err);
        double topKMs = measureMs(200, [&]() { executeQuery(*snap, tk); });
        printBench("RangeIndex Top-20 query", topKMs, 20.0);

        QueryPlan sp;
        sp.spatialFilter.kind = SpatialFilterSpec::Kind::AABB;
        sp.spatialFilter.indexName = "pos";
        sp.spatialFilter.a[0] = -40.0;
        sp.spatialFilter.a[1] = -40.0;
        sp.spatialFilter.b[0] = 40.0;
        sp.spatialFilter.b[1] = 40.0;
        bindQueryPlan(sp, *snap, err);
        double spMs = measureMs(100, [&]() { executeQuery(*snap, sp); });
        printBench("SpatialIndex AABB lookup", spMs, 1600.0);
    }

    // 6. .brotable v2 save + zero-copy mmap
    {
        const auto tmp = std::filesystem::temp_directory_path() / "bro_table_bench_1m.brotable";
        std::string err;
        const auto tS0 = std::chrono::steady_clock::now();
        saveSnapshotToFile(*snap, tmp.string(), &err);
        const double saveMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tS0).count();
        const auto tM0 = std::chrono::steady_clock::now();
        auto mapped = mmapSnapshotFromFile(tmp.string(), &err);
        const double mmapMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tM0).count();
        printBench(".brotable v2 save (w/ 3 indexes)", saveMs, static_cast<double>(kRows));
        printBench(".brotable v2 mmap (zero-copy + idx)", mmapMs, static_cast<double>(kRows));
        mapped.reset();
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    }

    std::cout << "=== benchmark complete ===\n";
    return 0;
}
