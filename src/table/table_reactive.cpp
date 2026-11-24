#include "table/table_reactive.h"

#include "table/query_eval.h"
#include "table/query_jit.h"

#include <algorithm>
#include <unordered_map>

namespace bro::table {

namespace {

bool passesPostJoin(const QueryPlan& plan, const TableSnapshot& left, uint32_t lc, uint32_t lr,
                    const TableSnapshot* right, uint32_t rc, uint32_t rr) {
    for (uint32_t f : plan.postJoinFilters) {
        if (!scalarTruthy(evalExpr(plan.exprs, f, left, lc, lr, right, rc, rr))) return false;
    }
    return true;
}

std::vector<ColumnDef> buildAggOutputSchema(const QueryPlan& plan) {
    std::vector<ColumnDef> outSchema;
    for (size_t gi = 0; gi < plan.groupKeys.size(); ++gi) {
        std::string name = gi < plan.groupAliases.size()
                               ? plan.groupAliases[gi]
                               : plan.exprs.nodes[plan.groupKeys[gi]].colName;
        outSchema.push_back(ColumnDef{std::move(name), plan.exprs.nodes[plan.groupKeys[gi]].valueType});
    }
    for (size_t ai = 0; ai < plan.aggregates.size(); ++ai) {
        std::string name = !plan.aggregates[ai].alias.empty()
                               ? plan.aggregates[ai].alias
                               : ("agg_" + std::to_string(ai));
        outSchema.push_back(ColumnDef{std::move(name), plan.aggregates[ai].outputType});
    }
    return outSchema;
}

}  // namespace

ReactiveQuery::ReactiveQuery(std::shared_ptr<Table> table, QueryPlan plan, ExecOptions opts)
    : table_(std::move(table)), plan_(std::move(plan)), opts_(opts) {}

bool ReactiveQuery::isDirty() const {
    return !lastResult_ || !table_ || table_->version() != lastVersion_;
}

ReactiveRefreshResult ReactiveQuery::refreshIncrementalAgg(const std::shared_ptr<const TableSnapshot>& snap,
                                                           const QueryPlan& boundPlan,
                                                           const CompiledKernel* kernel) {
    ReactiveRefreshResult out;
    const bool canReuseChunks =
        lastResult_ &&
        snap->structureVersion == lastStructureVersion_ &&
        cache_.size() == snap->chunks.size();

    if (!canReuseChunks) {
        cache_.clear();
        cache_.resize(snap->chunks.size());
    }

    const size_t nAggs = boundPlan.aggregates.size();
    std::vector<uint32_t> chunkMatches;

    for (size_t ci = 0; ci < snap->chunks.size(); ++ci) {
        const auto& fc = snap->chunks[ci];
        CachedChunk& cc = cache_[ci];
        if (canReuseChunks && cc.chunkId == fc.chunkId && cc.dirtyVersion == fc.dirtyVersion) {
            ++out.reusedChunks;
            continue;
        }

        cc.chunkId = fc.chunkId;
        cc.dirtyVersion = fc.dirtyVersion;
        cc.aggGroups.clear();
        cc.aggBuckets.clear();
        chunkMatches.clear();
        scanSingleChunkMatches(kernel, boundPlan, *snap, ci, chunkMatches);

        const uint32_t chunkIdx = static_cast<uint32_t>(ci);
        for (uint32_t r : chunkMatches) {
            std::vector<ScalarValue> keys;
            keys.reserve(boundPlan.groupKeys.size());
            uint64_t h = 0x517cc1b727220a95ULL;
            for (size_t gi = 0; gi < boundPlan.groupKeys.size(); ++gi) {
                ScalarValue kv = evalExpr(boundPlan.exprs, boundPlan.groupKeys[gi],
                                          *snap, chunkIdx, r, nullptr, UINT32_MAX, UINT32_MAX);
                h = combineHash64(h, hashScalarKey(boundPlan.exprs.nodes[boundPlan.groupKeys[gi]].valueType, kv));
                keys.push_back(std::move(kv));
            }

            size_t gIdx = SIZE_MAX;
            if (auto it = cc.aggBuckets.find(h); it != cc.aggBuckets.end()) {
                for (size_t cand : it->second) {
                    bool match = true;
                    for (size_t k = 0; k < keys.size(); ++k) {
                        if (compareScalar(cc.aggGroups[cand].keys[k], keys[k]) != 0) {
                            match = false;
                            break;
                        }
                    }
                    if (match) { gIdx = cand; break; }
                }
            }
            if (gIdx == SIZE_MAX) {
                gIdx = cc.aggGroups.size();
                ChunkGroupEntry g;
                g.keys = std::move(keys);
                g.keyHash = h;
                g.aggs.resize(nAggs);
                cc.aggGroups.push_back(std::move(g));
                cc.aggBuckets[h].push_back(gIdx);
            }

            ChunkGroupEntry& grp = cc.aggGroups[gIdx];
            for (size_t ai = 0; ai < nAggs; ++ai) {
                const AggSpec& a = boundPlan.aggregates[ai];
                PartialAgg& pa = grp.aggs[ai];
                if (a.kind == AggSpec::Kind::Count && a.expr == kInvalidExpr) {
                    ++pa.count;
                    continue;
                }
                ScalarValue val = evalExpr(boundPlan.exprs, a.expr, *snap, chunkIdx, r,
                                           nullptr, UINT32_MAX, UINT32_MAX);
                if (val.isNull) continue;
                const double dv = colTypeIsFloat(val.type)
                                      ? val.f64
                                      : (colTypeIsUnsignedInt(val.type) ? static_cast<double>(val.u64)
                                                                       : static_cast<double>(val.i64));
                if (a.kind == AggSpec::Kind::Count) {
                    ++pa.count;
                } else if (a.kind == AggSpec::Kind::Sum || a.kind == AggSpec::Kind::Avg) {
                    ++pa.count;
                    if (a.outputType == ColType::I64) {
                        pa.sumI64 += colTypeIsUnsignedInt(val.type) ? static_cast<int64_t>(val.u64) : val.i64;
                    } else {
                        pa.sumF64 += dv;
                    }
                } else if (a.kind == AggSpec::Kind::Min) {
                    ++pa.count;
                    if (!pa.hasExt || compareScalar(val, pa.extVal) < 0) {
                        pa.extVal = std::move(val);
                        pa.hasExt = true;
                    }
                } else if (a.kind == AggSpec::Kind::Max) {
                    ++pa.count;
                    if (!pa.hasExt || compareScalar(val, pa.extVal) > 0) {
                        pa.extVal = std::move(val);
                        pa.hasExt = true;
                    }
                }
            }
        }
        ++out.recomputedChunks;
    }

    // Merge all chunks' partial aggregates.
    std::vector<ChunkGroupEntry> merged;
    std::unordered_map<uint64_t, std::vector<size_t>> mergedBuckets;
    for (const CachedChunk& cc : cache_) {
        for (const ChunkGroupEntry& cg : cc.aggGroups) {
            size_t mIdx = SIZE_MAX;
            if (auto it = mergedBuckets.find(cg.keyHash); it != mergedBuckets.end()) {
                for (size_t cand : it->second) {
                    bool eq = true;
                    for (size_t k = 0; k < cg.keys.size(); ++k) {
                        if (compareScalar(merged[cand].keys[k], cg.keys[k]) != 0) {
                            eq = false;
                            break;
                        }
                    }
                    if (eq) { mIdx = cand; break; }
                }
            }
            if (mIdx == SIZE_MAX) {
                mIdx = merged.size();
                merged.push_back(cg);
                mergedBuckets[cg.keyHash].push_back(mIdx);
                continue;
            }
            ChunkGroupEntry& dst = merged[mIdx];
            for (size_t ai = 0; ai < nAggs; ++ai) {
                const AggSpec::Kind kind = boundPlan.aggregates[ai].kind;
                const PartialAgg& srcA = cg.aggs[ai];
                PartialAgg& dstA = dst.aggs[ai];
                dstA.count += srcA.count;
                dstA.sumF64 += srcA.sumF64;
                dstA.sumI64 += srcA.sumI64;
                if (srcA.hasExt && kind == AggSpec::Kind::Min) {
                    if (!dstA.hasExt || compareScalar(srcA.extVal, dstA.extVal) < 0) {
                        dstA.extVal = srcA.extVal;
                        dstA.hasExt = true;
                    }
                } else if (srcA.hasExt && kind == AggSpec::Kind::Max) {
                    if (!dstA.hasExt || compareScalar(srcA.extVal, dstA.extVal) > 0) {
                        dstA.extVal = srcA.extVal;
                        dstA.hasExt = true;
                    }
                }
            }
        }
    }

    if (boundPlan.groupKeys.empty() && merged.empty()) {
        ChunkGroupEntry emptyRow;
        emptyRow.aggs.resize(nAggs);
        merged.push_back(std::move(emptyRow));
    }

    std::vector<ColumnDef> outSchema = buildAggOutputSchema(boundPlan);
    std::vector<std::vector<ScalarValue>> outRows;
    outRows.reserve(merged.size());
    for (const ChunkGroupEntry& g : merged) {
        std::vector<ScalarValue> row;
        row.reserve(outSchema.size());
        for (const ScalarValue& k : g.keys) row.push_back(k);
        for (size_t ai = 0; ai < nAggs; ++ai) {
            const AggSpec& a = boundPlan.aggregates[ai];
            const PartialAgg& pa = g.aggs[ai];
            if (a.kind == AggSpec::Kind::Count) {
                row.push_back(ScalarValue::makeI64(static_cast<int64_t>(pa.count)));
            } else if (a.kind == AggSpec::Kind::Sum) {
                row.push_back(a.outputType == ColType::I64 ? ScalarValue::makeI64(pa.sumI64)
                                                           : ScalarValue::makeF64(pa.sumF64));
            } else if (a.kind == AggSpec::Kind::Avg) {
                row.push_back(ScalarValue::makeF64(
                    pa.count > 0 ? (pa.sumF64 / static_cast<double>(pa.count)) : 0.0));
            } else {
                row.push_back(pa.hasExt ? pa.extVal : ScalarValue::makeNull());
            }
        }
        outRows.push_back(std::move(row));
    }

    lastResult_ = finalizeGroupedRowsImpl(boundPlan, *snap, std::move(outSchema), std::move(outRows));
    lastVersion_ = snap->version;
    lastStructureVersion_ = snap->structureVersion;

    out.table = lastResult_;
    out.changed = true;
    out.version = snap->version;
    out.usedIncrementalAgg = true;
    return out;
}

ReactiveRefreshResult ReactiveQuery::refreshIncrementalJoin(const std::shared_ptr<const TableSnapshot>& snap,
                                                            const QueryPlan& boundPlan,
                                                            const CompiledKernel* kernel) {
    ReactiveRefreshResult out;
    const TableSnapshot* right = boundPlan.join.rightTable.get();
    const uint64_t rVer = right ? right->version : 0;
    const HashIndex* rightSingleIdx =
        (right && boundPlan.join.rightKeyCols.size() == 1)
            ? right->findIndex(boundPlan.join.rightKeyCols[0])
            : nullptr;

    const bool rightSame = (right == cachedRightPtr_) && (rVer == cachedRightVersion_);
    if (!rightSingleIdx && (!hasRightHashMap_ || !rightSame)) {
        rightHashMap_.clear();
        if (right) buildRightJoinHashMap(*right, boundPlan.join, rightHashMap_);
        hasRightHashMap_ = true;
    }

    const bool canReuseChunks =
        lastResult_ &&
        rightSame &&
        snap->structureVersion == lastStructureVersion_ &&
        cache_.size() == snap->chunks.size();

    cachedRightPtr_ = right;
    cachedRightVersion_ = rVer;

    if (!canReuseChunks) {
        cache_.clear();
        cache_.resize(snap->chunks.size());
    }

    std::vector<uint32_t> chunkMatches;
    std::vector<RowLoc> candidates;
    std::vector<ScalarValue> lKeys(boundPlan.join.leftKeyCols.size());
    size_t totalPairs = 0;

    for (size_t ci = 0; ci < snap->chunks.size(); ++ci) {
        const auto& fc = snap->chunks[ci];
        CachedChunk& cc = cache_[ci];
        if (canReuseChunks && cc.chunkId == fc.chunkId && cc.dirtyVersion == fc.dirtyVersion) {
            ++out.reusedChunks;
            totalPairs += cc.joinPairs.size();
            continue;
        }

        cc.chunkId = fc.chunkId;
        cc.dirtyVersion = fc.dirtyVersion;
        cc.joinPairs.clear();
        chunkMatches.clear();
        scanSingleChunkMatches(kernel, boundPlan, *snap, ci, chunkMatches);

        const uint32_t lChunk = static_cast<uint32_t>(ci);
        for (uint32_t lRow : chunkMatches) {
            candidates.clear();
            if (rightSingleIdx) {
                lKeys[0] = snap->getCell(boundPlan.join.leftKeyCols[0], lChunk, lRow);
                uint64_t key = 0;
                if (HashIndex::scalarToKey(right->schema.columns[boundPlan.join.rightKeyCols[0]].type,
                                           lKeys[0], right->dict.get(), key)) {
                    rightSingleIdx->lookupKey(key, candidates);
                }
            } else {
                uint64_t h = 0x517cc1b727220a95ULL;
                for (size_t k = 0; k < boundPlan.join.leftKeyCols.size(); ++k) {
                    lKeys[k] = snap->getCell(boundPlan.join.leftKeyCols[k], lChunk, lRow);
                    h = combineHash64(h, hashScalarKey(boundPlan.join.keyTypes[k], lKeys[k]));
                }
                if (auto it = rightHashMap_.find(h); it != rightHashMap_.end()) {
                    candidates = it->second;
                }
            }

            bool matchedAny = false;
            for (RowLoc rl : candidates) {
                if (rl.chunkIdx >= right->chunks.size() || !right->chunks[rl.chunkIdx].isAlive(rl.rowIdx)) {
                    continue;
                }
                bool eq = true;
                for (size_t k = 0; k < boundPlan.join.rightKeyCols.size(); ++k) {
                    ScalarValue rk = right->getCell(boundPlan.join.rightKeyCols[k], rl.chunkIdx, rl.rowIdx);
                    if (compareScalar(lKeys[k], rk) != 0) {
                        eq = false;
                        break;
                    }
                }
                if (!eq) continue;
                if (!passesPostJoin(boundPlan, *snap, lChunk, lRow, right, rl.chunkIdx, rl.rowIdx)) {
                    continue;
                }
                matchedAny = true;
                cc.joinPairs.push_back(MatchPair{RowLoc{lChunk, lRow}, rl});
            }
            if (!matchedAny && boundPlan.join.kind == JoinSpec::Kind::Left) {
                if (passesPostJoin(boundPlan, *snap, lChunk, lRow, right, UINT32_MAX, UINT32_MAX)) {
                    cc.joinPairs.push_back(MatchPair{RowLoc{lChunk, lRow}, RowLoc{UINT32_MAX, UINT32_MAX}});
                }
            }
        }
        totalPairs += cc.joinPairs.size();
        ++out.recomputedChunks;
    }

    std::vector<MatchPair> allPairs;
    allPairs.reserve(totalPairs);
    for (const CachedChunk& cc : cache_) {
        allPairs.insert(allPairs.end(), cc.joinPairs.begin(), cc.joinPairs.end());
    }

    lastResult_ = buildTableFromMatchPairsImpl(boundPlan, *snap, allPairs);
    lastVersion_ = snap->version;
    lastStructureVersion_ = snap->structureVersion;

    out.table = lastResult_;
    out.changed = true;
    out.version = snap->version;
    out.usedIncrementalJoin = true;
    return out;
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

    std::shared_ptr<const CompiledKernel> kernel;
    if (opts_.useJit) {
        kernel = CompiledPlanCache::instance().getOrCompile(boundPlan, *snap, JitMode::Filter);
    }

    const bool hasJoin = boundPlan.join.kind != JoinSpec::Kind::None && boundPlan.join.rightTable;
    const bool hasAgg = !boundPlan.groupKeys.empty() || !boundPlan.aggregates.empty();

    if (!hasJoin && hasAgg) {
        return refreshIncrementalAgg(snap, boundPlan, kernel.get());
    }
    if (hasJoin && !hasAgg) {
        return refreshIncrementalJoin(snap, boundPlan, kernel.get());
    }
    if (hasJoin && hasAgg) {
        // Fallback when both join and agg are present.
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
        cache_.size() == snap->chunks.size();

    if (!canIncrementalChunks) {
        cache_.clear();
        cache_.resize(snap->chunks.size());
    }

    size_t totalMatches = 0;
    for (size_t ci = 0; ci < snap->chunks.size(); ++ci) {
        const auto& fc = snap->chunks[ci];
        CachedChunk& cc = cache_[ci];
        if (canIncrementalChunks && cc.chunkId == fc.chunkId && cc.dirtyVersion == fc.dirtyVersion) {
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

    lastResult_ = finishPlanFromLeftMatchesImpl(boundPlan, *snap, leftMatches, nullptr, opts_.useParallel);
    lastVersion_ = snap->version;
    lastStructureVersion_ = snap->structureVersion;

    out.table = lastResult_;
    out.changed = true;
    out.version = snap->version;
    return out;
}

}  // namespace bro::table
