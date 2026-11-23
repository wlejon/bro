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
    bool usedJitAgg = false;
    bool usedIndex = false;
    bool usedRangeIndex = false;
    bool usedSpatialIndex = false;
};

struct MutationExecResult {
    size_t affectedRows = 0;
    bool ok = true;
    std::string error;
    bool usedJit = false;
};

/// Executes a query against an immutable TableSnapshot using Spatial/Hash/Range indexes + brass JIT/SIMD.
QueryExecResult executeQuery(const TableSnapshot& left, QueryPlan plan,
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
                                                   bool* outUsedIndex = nullptr);

}  // namespace bro::table
