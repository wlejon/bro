#pragma once

#include "table/query_ir.h"
#include "table/query_jit.h"

#include <memory>
#include <string>
#include <vector>

namespace bro::table {

struct ExecOptions {
    bool useJit = true;
    bool useIndexes = true;
    bool useParallel = true;
};

struct QueryExecResult {
    std::shared_ptr<Table> table;
    bool ok = true;
    std::string error;
    std::string jitError;
    bool usedJit = false;
    bool usedSimd = false;
    uint16_t simdWidth = 0;  // 0 = none, 128 = SSE2/AVX2-128, 256 = AVX2-256
    bool usedJitAgg = false;
    bool usedIndex = false;
    bool usedRangeIndex = false;
    bool usedSpatialIndex = false;
};

struct QueryExplainResult {
    bool ok = true;
    std::string error;
    std::string strategy;  // "spatial_index" | "hash_index" | "range_index" | "range_topk" | "jit_agg" | "jit_simd" | "jit_scalar" | "reference"
    bool willUseJit = false;
    bool willUseSimd = false;
    uint16_t simdWidth = 0;
    bool willUseJitAgg = false;
    bool willUseIndex = false;
    bool willUseRangeIndex = false;
    bool willUseSpatialIndex = false;
    bool hasNullsInPlanCols = false;
    std::string indexColumn;
    std::vector<uint32_t> residualFilterIndices;
    std::string jitError;
};

struct MutationExecResult {
    size_t affectedRows = 0;
    bool ok = true;
    std::string error;
    bool usedJit = false;
    bool usedIndex = false;
    bool usedRangeIndex = false;
    bool usedSpatialIndex = false;
};

/// Executes a query against an immutable TableSnapshot using Spatial/Hash/Range indexes + brass JIT/SIMD.
QueryExecResult executeQuery(const TableSnapshot& left, QueryPlan plan,
                             ExecOptions opts = {});

/// Inspects which execution strategy and indexes `executeQuery` would pick without running it.
QueryExplainResult explainQuery(const TableSnapshot& left, QueryPlan plan,
                                ExecOptions opts = {});

/// Applies update assignments to rows matching plan.filters in-place on `table`.
MutationExecResult executeUpdate(Table& table, QueryPlan plan, ExecOptions opts = {});

/// Removes (tombstones) rows matching plan.filters in-place on `table`.
MutationExecResult executeDelete(Table& table, QueryPlan plan, ExecOptions opts = {});

/// Scans a single chunk using an optional compiled kernel + residual filters (used by ReactiveQuery).
void scanSingleChunkMatches(const CompiledKernel* kernel, const QueryPlan& boundPlan,
                            const TableSnapshot& snap, size_t chunkIdx,
                            std::vector<uint32_t>& outMatches);

/// Finishes Join / GroupBy / Project / OrderBy / Limit from pre-filtered left RowLocs.
std::shared_ptr<Table> finishPlanFromLeftMatches(const QueryPlan& boundPlan,
                                                   const TableSnapshot& left,
                                                   const std::vector<RowLoc>& leftMatches,
                                                   bool* outUsedIndex = nullptr,
                                                   bool useParallel = true);

}  // namespace bro::table
