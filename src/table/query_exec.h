#pragma once

#include "table/query_ir.h"

#include <memory>
#include <string>

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
    bool usedIndex = false;
};

struct MutationExecResult {
    size_t affectedRows = 0;
    bool ok = true;
    std::string error;
    bool usedJit = false;
};

/// Executes a query against an immutable TableSnapshot using brass JIT + parallel_for + HashIndexes.
QueryExecResult executeQuery(const TableSnapshot& left, QueryPlan plan,
                             ExecOptions opts = {});

/// Applies update assignments to rows matching plan.filters in-place on `table`.
MutationExecResult executeUpdate(Table& table, QueryPlan plan, ExecOptions opts = {});

/// Removes (tombstones) rows matching plan.filters in-place on `table`.
MutationExecResult executeDelete(Table& table, QueryPlan plan, ExecOptions opts = {});

}  // namespace bro::table
