#pragma once

#include "table/query_exec.h"
#include "table/query_ir.h"
#include "table/query_materialize.h"
#include "table/table.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace bro::table {

struct ReactiveRefreshResult {
    std::shared_ptr<Table> table;
    bool changed = false;
    uint64_t version = 0;
    size_t recomputedChunks = 0;
    size_t reusedChunks = 0;
    bool usedIncrementalAgg = false;
    bool usedIncrementalJoin = false;
};

/// Incrementally maintains query results across Table mutations by re-evaluating only dirty chunks
/// for filters, grouped/ungrouped aggregations, and left-side joins.
class ReactiveQuery {
public:
    ReactiveQuery(std::shared_ptr<Table> table, QueryPlan plan, ExecOptions opts = {});

    bool isDirty() const;
    uint64_t lastVersion() const { return lastVersion_; }
    const std::shared_ptr<Table>& latestResult() const { return lastResult_; }

    ReactiveRefreshResult refresh();

private:
    struct PartialAgg {
        uint64_t count = 0;
        double sumF64 = 0.0;
        int64_t sumI64 = 0;
        bool hasExt = false;
        ScalarValue extVal = ScalarValue::makeNull();
    };

    struct ChunkGroupEntry {
        std::vector<ScalarValue> keys;
        uint64_t keyHash = 0;
        std::vector<PartialAgg> aggs;
    };

    struct CachedChunk {
        uint64_t chunkId = 0;
        uint64_t dirtyVersion = 0;
        std::vector<uint32_t> matchRows;
        std::vector<ChunkGroupEntry> aggGroups;
        std::unordered_map<uint64_t, std::vector<size_t>> aggBuckets;
        std::vector<MatchPair> joinPairs;
    };

    ReactiveRefreshResult refreshIncrementalAgg(const std::shared_ptr<const TableSnapshot>& snap,
                                                const QueryPlan& boundPlan,
                                                const CompiledKernel* kernel);
    ReactiveRefreshResult refreshIncrementalJoin(const std::shared_ptr<const TableSnapshot>& snap,
                                                 const QueryPlan& boundPlan,
                                                 const CompiledKernel* kernel);

    std::shared_ptr<Table> table_;
    QueryPlan plan_;
    ExecOptions opts_;
    std::vector<CachedChunk> cache_;
    const TableSnapshot* cachedRightPtr_ = nullptr;
    uint64_t cachedRightVersion_ = 0;
    std::unordered_map<uint64_t, std::vector<RowLoc>> rightHashMap_;
    bool hasRightHashMap_ = false;
    uint64_t lastVersion_ = 0;
    uint64_t lastStructureVersion_ = 0;
    std::shared_ptr<Table> lastResult_;
};

}  // namespace bro::table
