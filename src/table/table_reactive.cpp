#include "table/table_reactive.h"

#include "table/query_eval.h"
#include "table/query_jit.h"

namespace bro::table {

ReactiveQuery::ReactiveQuery(std::shared_ptr<Table> table, QueryPlan plan, ExecOptions opts)
    : table_(std::move(table)), plan_(std::move(plan)), opts_(opts) {}

bool ReactiveQuery::isDirty() const {
    return !lastResult_ || !table_ || table_->version() != lastVersion_;
}

ReactiveRefreshResult ReactiveQuery::refresh() {
    ReactiveRefreshResult out;
    if (!table_) return out;

    const uint64_t curVer = table_->version();
    if (lastResult_ && curVer == lastVersion_) {
        out.table = lastResult_;
        out.changed = false;
        out.version = curVer;
        out.recomputedChunks = 0;
        out.reusedChunks = cache_.size();
        return out;
    }

    auto snap = table_->snapshot();
    QueryPlan boundPlan = plan_;
    std::string err;
    if (!boundPlan.bound && !bindQueryPlan(boundPlan, *snap, err)) {
        out.table = std::make_shared<Table>();
        out.changed = true;
        out.version = snap->version;
        return out;
    }

    if (boundPlan.spatialFilter.kind != SpatialFilterSpec::Kind::None) {
        QueryExecResult qr = executeQuery(*snap, boundPlan, opts_);
        lastResult_ = qr.table ? qr.table : std::make_shared<Table>();
        lastVersion_ = snap->version;
        lastStructureVersion_ = snap->structureVersion;
        cache_.clear();
        out.recomputedChunks = snap->chunks.size();
        out.reusedChunks = 0;
        out.table = lastResult_;
        out.changed = true;
        out.version = snap->version;
        return out;
    }

    const bool canIncrementalChunks =
        lastResult_ &&
        snap->structureVersion == lastStructureVersion_ &&
        boundPlan.spatialFilter.kind == SpatialFilterSpec::Kind::None &&
        cache_.size() == snap->chunks.size();

    if (!canIncrementalChunks) {
        cache_.clear();
        cache_.resize(snap->chunks.size());
    }

    std::shared_ptr<const CompiledKernel> kernel;
    if (opts_.useJit) {
        kernel = CompiledPlanCache::instance().getOrCompile(boundPlan, *snap, JitMode::Filter);
    }

    size_t totalMatches = 0;
    for (size_t ci = 0; ci < snap->chunks.size(); ++ci) {
        const auto& fc = snap->chunks[ci];
        CachedChunk& cc = cache_[ci];
        if (canIncrementalChunks &&
            cc.chunkId == fc.chunkId &&
            cc.dirtyVersion == fc.dirtyVersion) {
            ++out.reusedChunks;
        } else {
            cc.chunkId = fc.chunkId;
            cc.dirtyVersion = fc.dirtyVersion;
            scanSingleChunkMatches(kernel.get(), boundPlan, *snap, ci, cc.matchRows);
            ++out.recomputedChunks;
        }
        totalMatches += cc.matchRows.size();
    }

    std::vector<RowLoc> leftMatches;
    leftMatches.reserve(totalMatches);
    for (uint32_t ci = 0; ci < snap->chunks.size(); ++ci) {
        for (uint32_t r : cache_[ci].matchRows) {
            leftMatches.push_back(RowLoc{ci, r});
        }
    }

    lastResult_ = finishPlanFromLeftMatches(boundPlan, *snap, leftMatches, nullptr);
    lastVersion_ = snap->version;
    lastStructureVersion_ = snap->structureVersion;

    out.table = lastResult_;
    out.changed = true;
    out.version = snap->version;
    return out;
}

}  // namespace bro::table
