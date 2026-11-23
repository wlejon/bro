#pragma once

#include "table/query_ir.h"
#include "table/table.h"

#include <memory>
#include <vector>

namespace bro::table {

// Fast open-addressed grouped aggregation path for single-table queries with:
// - Single group key column of type Dict, Bool, I32, U32, or I64 (nulls grouped together).
// - Zero or more aggregates whose inputs are Count(*) or direct numeric/bool ColRefs
//   (Min/Max exclude Dict and String columns where dictionary ID != string order).
// Returns nullptr when the plan falls outside this fast path so the caller can fall back
// to the general grouped builder.
std::shared_ptr<Table> tryExecuteFastGroupBy(const QueryPlan& plan,
                                             const TableSnapshot& left,
                                             const std::vector<RowLoc>& leftMatches,
                                             bool useParallel);

}  // namespace bro::table
