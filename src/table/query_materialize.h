#pragma once

#include "table/query_ir.h"
#include "table/table.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace bro::table {

struct MatchPair {
    RowLoc left{};
    RowLoc right{UINT32_MAX, UINT32_MAX};
};

/// Executes Join / GroupBy / Aggregates / Projection / OrderBy / Limit from left RowLoc survivors.
std::shared_ptr<Table> finishPlanFromLeftMatchesImpl(
    const QueryPlan& plan, const TableSnapshot& left,
    const std::vector<RowLoc>& leftMatches, bool* outUsedIndex,
    bool useParallel = true);

/// Builds the hash table of right-table join keys for hash joins.
void buildRightJoinHashMap(const TableSnapshot& right, const JoinSpec& join,
                           std::unordered_map<uint64_t, std::vector<RowLoc>>& out);

/// Projects, sorts, and limits already-materialized (left, right) MatchPairs.
std::shared_ptr<Table> buildTableFromMatchPairsImpl(
    const QueryPlan& plan, const TableSnapshot& left, std::vector<MatchPair>& pairs);

/// Sorts, slices, and materializes pre-aggregated grouped output rows.
std::shared_ptr<Table> finalizeGroupedRowsImpl(
    const QueryPlan& plan, const TableSnapshot& left,
    std::vector<ColumnDef> outSchema, std::vector<std::vector<ScalarValue>> outRows);

}  // namespace bro::table
