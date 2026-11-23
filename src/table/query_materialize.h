#pragma once

#include "table/query_ir.h"
#include "table/table.h"

#include <memory>
#include <vector>

namespace bro::table {

/// Executes Join / GroupBy / Aggregates / Projection / OrderBy / Limit from left RowLoc survivors.
std::shared_ptr<Table> finishPlanFromLeftMatchesImpl(
    const QueryPlan& plan, const TableSnapshot& left,
    const std::vector<RowLoc>& leftMatches, bool* outUsedIndex,
    bool useParallel = true);

}  // namespace bro::table
