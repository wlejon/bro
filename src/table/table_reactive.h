#pragma once

#include "table/query_exec.h"
#include "table/query_ir.h"
#include "table/table.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace bro::table {

struct ReactiveRefreshResult {
    std::shared_ptr<Table> table;
    bool changed = false;
    uint64_t version = 0;
    size_t recomputedChunks = 0;
    size_t reusedChunks = 0;
};

/// Incrementally maintains query results across Table mutations by re-evaluating only dirty chunks.
class ReactiveQuery {
public:
    ReactiveQuery(std::shared_ptr<Table> table, QueryPlan plan, ExecOptions opts = {});

    bool isDirty() const;
    uint64_t lastVersion() const { return lastVersion_; }
    const std::shared_ptr<Table>& latestResult() const { return lastResult_; }

    ReactiveRefreshResult refresh();

private:
    struct CachedChunk {
        uint64_t chunkId = 0;
        uint64_t dirtyVersion = 0;
        std::vector<uint32_t> matchRows;
    };

    std::shared_ptr<Table> table_;
    QueryPlan plan_;
    ExecOptions opts_;
    std::vector<CachedChunk> cache_;
    uint64_t lastVersion_ = 0;
    uint64_t lastStructureVersion_ = 0;
    std::shared_ptr<Table> lastResult_;
};

}  // namespace bro::table
