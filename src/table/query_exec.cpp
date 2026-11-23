#include "table/query_exec.h"

#include "table/query_eval.h"
#include "table/query_jit.h"
#include "table/query_materialize.h"

#include <algorithm>
#include <cstring>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace bro::table {

namespace {

struct IndexProbeSpec {
    size_t filterIdx = 0;
    size_t colIdx = 0;
    uint64_t key = 0;
    bool valid = false;
};

IndexProbeSpec detectIndexProbe(const TableSnapshot& snap, const QueryPlan& plan) {
    for (size_t fi = 0; fi < plan.filters.size(); ++fi) {
        const ExprNode& n = plan.exprs.nodes[plan.filters[fi]];
        if (n.op != ExprOp::Eq && n.op != ExprOp::StrEq) continue;
        const ExprNode* colNode = &plan.exprs.nodes[n.lhs];
        const ExprNode* litNode = &plan.exprs.nodes[n.rhs];
        if (colNode->op == ExprOp::Const && litNode->op == ExprOp::ColRef) {
            std::swap(colNode, litNode);
        }
        if (colNode->op != ExprOp::ColRef || colNode->tableSide != 0 || colNode->resolvedCol < 0) {
            continue;
        }
        if (litNode->op != ExprOp::Const) continue;
        const size_t colIdx = static_cast<size_t>(colNode->resolvedCol);
        const HashIndex* idx = snap.findIndex(colIdx);
        if (!idx) continue;

        IndexProbeSpec spec;
        spec.filterIdx = fi;
        spec.colIdx = colIdx;
        const ColType colType = snap.schema.columns[colIdx].type;
        if (colType == ColType::Dict && litNode->literal.type == ColType::String) {
            if (litNode->cachedDictId == kInvalidDictId) {
                spec.key = kInvalidDictId;
                spec.valid = true;
                return spec;
            }
            spec.key = litNode->cachedDictId;
            spec.valid = true;
            return spec;
        }
        if (HashIndex::scalarToKey(colType, litNode->literal, snap.dict.get(), spec.key)) {
            spec.valid = true;
            return spec;
        }
    }
    return {};
}

bool tryRangeIndexLookup(const TableSnapshot& snap, const QueryPlan& plan,
                         std::vector<RowLoc>& outMatches) {
    struct ColBounds {
        bool hasLo = false;
        bool incLo = true;
        ScalarValue lo;
        bool hasHi = false;
        bool incHi = true;
        ScalarValue hi;
        bool empty = false;
    };

    std::unordered_map<size_t, ColBounds> colBounds;
    for (uint32_t fid : plan.filters) {
        const ExprNode& n = plan.exprs.nodes[fid];
        if (n.op != ExprOp::Eq && n.op != ExprOp::Lt && n.op != ExprOp::Le &&
            n.op != ExprOp::Gt && n.op != ExprOp::Ge && n.op != ExprOp::StrEq) {
            continue;
        }
        const ExprNode* colNode = &plan.exprs.nodes[n.lhs];
        const ExprNode* litNode = &plan.exprs.nodes[n.rhs];
        ExprOp op = n.op;
        if (colNode->op == ExprOp::Const && litNode->op == ExprOp::ColRef) {
            std::swap(colNode, litNode);
            if (op == ExprOp::Lt) op = ExprOp::Gt;
            else if (op == ExprOp::Le) op = ExprOp::Ge;
            else if (op == ExprOp::Gt) op = ExprOp::Lt;
            else if (op == ExprOp::Ge) op = ExprOp::Le;
        }
        if (colNode->op != ExprOp::ColRef || colNode->tableSide != 0 || colNode->resolvedCol < 0 ||
            litNode->op != ExprOp::Const || litNode->literal.isNull) {
            continue;
        }
        const size_t colIdx = static_cast<size_t>(colNode->resolvedCol);
        if (!snap.findRangeIndex(colIdx)) continue;
        ScalarValue bound = litNode->literal;
        if (colTypeIsFloat(colNode->valueType) || colTypeIsFloat(bound.type)) {
            const double d = colTypeIsFloat(bound.type) ? bound.f64 : static_cast<double>(bound.i64);
            if (std::isnan(d)) continue;
        }
        ColBounds& b = colBounds[colIdx];
        auto setLo = [&](const ScalarValue& sv, bool inc) {
            if (!b.hasLo) { b.hasLo = true; b.lo = sv; b.incLo = inc; }
            else {
                const int cmp = compareScalar(sv, b.lo);
                if (cmp > 0) { b.lo = sv; b.incLo = inc; }
                else if (cmp == 0 && !inc) { b.incLo = false; }
            }
        };
        auto setHi = [&](const ScalarValue& sv, bool inc) {
            if (!b.hasHi) { b.hasHi = true; b.hi = sv; b.incHi = inc; }
            else {
                const int cmp = compareScalar(sv, b.hi);
                if (cmp < 0) { b.hi = sv; b.incHi = inc; }
                else if (cmp == 0 && !inc) { b.incHi = false; }
            }
        };
        if (op == ExprOp::Eq || op == ExprOp::StrEq) { setLo(bound, true); setHi(bound, true); }
        else if (op == ExprOp::Gt) { setLo(bound, false); }
        else if (op == ExprOp::Ge) { setLo(bound, true); }
        else if (op == ExprOp::Lt) { setHi(bound, false); }
        else if (op == ExprOp::Le) { setHi(bound, true); }
    }

    size_t chosenCol = SIZE_MAX;
    for (const auto& kv : colBounds) {
        chosenCol = kv.first;
        if (kv.second.hasLo && kv.second.hasHi) break;
    }
    if (chosenCol == SIZE_MAX) return false;

    const ColBounds& b = colBounds[chosenCol];
    const RangeIndex* ridx = snap.findRangeIndex(chosenCol);
    std::vector<RowLoc> cands;
    if (b.hasLo && b.hasHi) {
        const int cmp = compareScalar(b.lo, b.hi);
        if (cmp > 0 || (cmp == 0 && (!b.incLo || !b.incHi))) {
            outMatches.clear();
            return true;
        }
    }
    ridx->lookupRange(b.hasLo ? &b.lo : nullptr, b.incLo,
                      b.hasHi ? &b.hi : nullptr, b.incHi,
                      snap.dict.get(), cands);
    std::sort(cands.begin(), cands.end(), [](RowLoc a, RowLoc bLoc) {
        return a.chunkIdx != bLoc.chunkIdx ? (a.chunkIdx < bLoc.chunkIdx) : (a.rowIdx < bLoc.rowIdx);
    });
    cands.erase(std::unique(cands.begin(), cands.end()), cands.end());
    for (RowLoc loc : cands) {
        if (loc.chunkIdx >= snap.chunks.size() || !snap.chunks[loc.chunkIdx].isAlive(loc.rowIdx)) {
            continue;
        }
        bool ok = true;
        for (uint32_t f : plan.filters) {
            if (!scalarTruthy(evalExpr(plan.exprs, f, snap, loc.chunkIdx, loc.rowIdx))) {
                ok = false;
                break;
            }
        }
        if (ok) outMatches.push_back(loc);
    }
    return true;
}

bool tryRangeTopKFastPath(const TableSnapshot& left, const QueryPlan& plan,
                          std::shared_ptr<Table>& outTable) {
    if (!plan.filters.empty() || plan.join.kind != JoinSpec::Kind::None ||
        !plan.groupKeys.empty() || !plan.aggregates.empty() ||
        plan.orderBy.size() != 1 || plan.limit < 0) {
        return false;
    }
    const ExprNode& on = plan.exprs.nodes[plan.orderBy[0].expr];
    if (on.op != ExprOp::ColRef || on.tableSide != 0 || on.resolvedCol < 0) return false;
    const RangeIndex* ridx = left.findRangeIndex(static_cast<size_t>(on.resolvedCol));
    if (!ridx || ridx->size() != left.totalAliveRows) return false;

    const size_t start = static_cast<size_t>(std::max<int64_t>(0, plan.offset));
    const size_t need = start + static_cast<size_t>(plan.limit);
    const bool desc = plan.orderBy[0].descending;

    std::vector<RowLoc> locs;
    locs.reserve(need + 16);
    ScalarValue boundKey;
    bool haveBound = false;
    ridx->forEachOrdered(desc, [&](RowLoc loc) {
        if (loc.chunkIdx >= left.chunks.size() || !left.chunks[loc.chunkIdx].isAlive(loc.rowIdx)) {
            return true;
        }
        if (desc && locs.size() >= need) {
            ScalarValue cur = left.getCell(static_cast<size_t>(on.resolvedCol), loc.chunkIdx, loc.rowIdx);
            if (!haveBound) {
                boundKey = left.getCell(static_cast<size_t>(on.resolvedCol), locs.back().chunkIdx, locs.back().rowIdx);
                haveBound = true;
            }
            if (compareScalar(cur, boundKey) != 0) return false;
        }
        locs.push_back(loc);
        return desc || locs.size() < need;
    });

    if (desc && locs.size() > 1) {
        std::stable_sort(locs.begin(), locs.end(), [&](RowLoc a, RowLoc bLoc) {
            ScalarValue ka = left.getCell(static_cast<size_t>(on.resolvedCol), a.chunkIdx, a.rowIdx);
            ScalarValue kb = left.getCell(static_cast<size_t>(on.resolvedCol), bLoc.chunkIdx, bLoc.rowIdx);
            const int cmp = compareScalar(ka, kb);
            if (cmp != 0) return cmp > 0;
            return a.chunkIdx != bLoc.chunkIdx ? (a.chunkIdx < bLoc.chunkIdx) : (a.rowIdx < bLoc.rowIdx);
        });
    }

    std::vector<ColumnDef> outSchema;
    for (const SelectItem& si : plan.selectItems) {
        outSchema.push_back(ColumnDef{si.alias, si.outputType});
    }
    outTable = std::make_shared<Table>(std::move(outSchema));
    outTable->setSharedDict(left.dict);
    const size_t end = std::min(locs.size(), need);
    std::vector<ScalarValue> rowBuf(plan.selectItems.size());
    for (size_t i = start; i < end; ++i) {
        const RowLoc p = locs[i];
        for (size_t k = 0; k < plan.selectItems.size(); ++k) {
            rowBuf[k] = evalExpr(plan.exprs, plan.selectItems[k].expr, left, p.chunkIdx, p.rowIdx);
        }
        outTable->appendRow(rowBuf);
    }
    return true;
}

bool passesResidualFilters(const QueryPlan& plan, const std::vector<uint32_t>& filterIndices,
                           const TableSnapshot& snap, uint32_t ci, uint32_t r) {
    for (uint32_t fi : filterIndices) {
        if (!scalarTruthy(evalExpr(plan.exprs, plan.filters[fi], snap, ci, r))) {
            return false;
        }
    }
    return true;
}

bool passesAllFilters(const QueryPlan& plan, const std::vector<uint32_t>& flt,
                      const TableSnapshot& left, uint32_t lc, uint32_t lr,
                      const TableSnapshot* right, uint32_t rc, uint32_t rr) {
    for (uint32_t f : flt) {
        if (!scalarTruthy(evalExpr(plan.exprs, f, left, lc, lr, right, rc, rr))) return false;
    }
    return true;
}

struct ChunkScanOutput {
    std::vector<uint32_t> matchRows;
    std::shared_ptr<TableChunk> projectedChunk;  // populated in FilterAndProject fast path
};

void runFilterKernelParallel(const CompiledKernel& kernel, const QueryPlan& plan,
                             const TableSnapshot& snap, bool useParallel,
                             std::vector<std::vector<uint32_t>>& chunkMatches) {
    const size_t nChunks = snap.chunks.size();
    const size_t nCols = snap.schema.size();
    chunkMatches.assign(nChunks, {});

    auto scanChunk = [&](size_t ci) {
        const auto& ch = snap.chunks[ci];
        if (ch.aliveCount == 0 || ch.rowCount == 0) return;
        std::vector<uint32_t>& matches = chunkMatches[ci];
        matches.resize(ch.rowCount);
        std::vector<const void*> colPtrs(nCols);
        for (size_t c = 0; c < nCols; ++c) colPtrs[c] = ch.columns[c]->rawData();

        JitChunkContext ctx;
        ctx.leftColPtrs = colPtrs.data();
        ctx.aliveWords = ch.aliveWords.data();
        ctx.hasAliveMask = ch.allAlive() ? 0u : 1u;
        ctx.rowCount = ch.rowCount;
        ctx.matchIndices = matches.data();
        ctx.matchCount = 0;
        kernel.entry(&ctx);

        uint32_t m = ctx.matchCount;
        if (!kernel.residualFilterIndices.empty() && m > 0) {
            uint32_t kept = 0;
            for (uint32_t i = 0; i < m; ++i) {
                if (passesResidualFilters(plan, kernel.residualFilterIndices,
                                          snap, static_cast<uint32_t>(ci), matches[i])) {
                    matches[kept++] = matches[i];
                }
            }
            m = kept;
        }
        matches.resize(m);
    };

    if (useParallel && nChunks > 1) {
        brass::codegen::parallel_for(
            0, static_cast<int64_t>(nChunks),
            [&](int64_t s, int64_t e) {
                for (int64_t ci = s; ci < e; ++ci) scanChunk(static_cast<size_t>(ci));
            },
            1);
    } else {
        for (size_t ci = 0; ci < nChunks; ++ci) scanChunk(ci);
    }
}

}  // namespace

QueryExecResult executeQuery(const TableSnapshot& left, QueryPlan plan, ExecOptions opts) {
    QueryExecResult res;
    if (!plan.bound && !bindQueryPlan(plan, left, res.error)) {
        res.ok = false;
        return res;
    }

    const bool hasJoin = plan.join.kind != JoinSpec::Kind::None && plan.join.rightTable;
    const bool hasAgg = !plan.groupKeys.empty() || !plan.aggregates.empty();

    // Only bypass SIMD filter into FilterAndProject if outputs perform real computation or SIMD filter doesn't apply.
    bool hasComputedSelect = false;
    for (const SelectItem& si : plan.selectItems) {
        if (si.expr != kInvalidExpr && plan.exprs.nodes[si.expr].op != ExprOp::ColRef) {
            hasComputedSelect = true;
            break;
        }
    }
    const bool wantsDirectProject =
        opts.useJit && !hasJoin && !hasAgg && plan.orderBy.empty() &&
        plan.spatialFilter.kind == SpatialFilterSpec::Kind::None &&
        !plan.selectItems.empty() &&
        (hasComputedSelect || !canCompileSimdFilter(plan, left));

    std::vector<RowLoc> leftMatches;
    bool usedIndex = false;

    // Priority 1: SpatialIndex probe
    if (opts.useIndexes && plan.spatialFilter.kind != SpatialFilterSpec::Kind::None) {
        if (const SpatialIndex* si = left.findSpatialIndex(plan.spatialFilter.indexName)) {
            std::vector<RowLoc> candidates;
            if (plan.spatialFilter.kind == SpatialFilterSpec::Kind::AABB) {
                si->queryAABB(plan.spatialFilter.a[0], plan.spatialFilter.a[1], plan.spatialFilter.a[2],
                              plan.spatialFilter.b[0], plan.spatialFilter.b[1], plan.spatialFilter.b[2],
                              candidates);
            } else {
                si->queryRadius(plan.spatialFilter.a[0], plan.spatialFilter.a[1], plan.spatialFilter.a[2],
                                plan.spatialFilter.radius, candidates);
            }
            std::sort(candidates.begin(), candidates.end(), [](const RowLoc& a, const RowLoc& b) {
                return (a.chunkIdx != b.chunkIdx) ? (a.chunkIdx < b.chunkIdx) : (a.rowIdx < b.rowIdx);
            });
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            for (const RowLoc& loc : candidates) {
                if (loc.chunkIdx >= left.chunks.size() || !left.chunks[loc.chunkIdx].isAlive(loc.rowIdx)) {
                    continue;
                }
                if (passesAllFilters(plan, plan.filters, left, loc.chunkIdx, loc.rowIdx, nullptr, 0, 0)) {
                    leftMatches.push_back(loc);
                }
            }
            usedIndex = true;
            res.usedSpatialIndex = true;
        }
    }

    // Priority 2: HashIndex equality probe
    if (!usedIndex && opts.useIndexes && !plan.filters.empty() &&
        plan.spatialFilter.kind == SpatialFilterSpec::Kind::None) {
        IndexProbeSpec probe = detectIndexProbe(left, plan);
        if (probe.valid) {
            const HashIndex* idx = left.findIndex(probe.colIdx);
            std::vector<RowLoc> candidates;
            idx->lookupKey(probe.key, candidates);
            std::sort(candidates.begin(), candidates.end(), [](const RowLoc& a, const RowLoc& b) {
                return (a.chunkIdx != b.chunkIdx) ? (a.chunkIdx < b.chunkIdx) : (a.rowIdx < b.rowIdx);
            });
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            for (const RowLoc& loc : candidates) {
                if (loc.chunkIdx >= left.chunks.size() || !left.chunks[loc.chunkIdx].isAlive(loc.rowIdx)) {
                    continue;
                }
                if (passesAllFilters(plan, plan.filters, left, loc.chunkIdx, loc.rowIdx, nullptr, 0, 0)) {
                    leftMatches.push_back(loc);
                }
            }
            usedIndex = true;
            res.usedIndex = true;
        }
    }

    // Priority 3: RangeIndex probe or Top-K fast path
    if (!usedIndex && opts.useIndexes && plan.spatialFilter.kind == SpatialFilterSpec::Kind::None) {
        if (!plan.filters.empty() && tryRangeIndexLookup(left, plan, leftMatches)) {
            usedIndex = true;
            res.usedRangeIndex = true;
        } else if (tryRangeTopKFastPath(left, plan, res.table)) {
            res.usedRangeIndex = true;
            return res;
        }
    }

    // Priority 4: Fused JIT / SIMD Ungrouped Numeric Aggregation fast path
    if (!usedIndex && !hasJoin && plan.groupKeys.empty() && !plan.aggregates.empty() &&
        plan.spatialFilter.kind == SpatialFilterSpec::Kind::None &&
        plan.orderBy.empty() && opts.useJit) {
        std::string aggErr;
        if (auto aggKernel = CompiledPlanCache::instance().getOrCompileAgg(plan, left, &aggErr)) {
            res.table = runCompiledAgg(*aggKernel, plan, left, opts.useParallel);
            res.usedJit = true;
            res.usedJitAgg = true;
            res.usedSimd = aggKernel->usesSimd;
            return res;
        }
    }

    // Priority 5: JIT (128-bit SIMD Filter or Scalar Filter/Project) parallel chunk scan
    if (!usedIndex) {
        if (plan.spatialFilter.kind != SpatialFilterSpec::Kind::None) {
            res.table = executePlanReference(left, plan);
            return res;
        }
        const size_t nChunks = left.chunks.size();
        std::shared_ptr<const CompiledKernel> kernel;
        if (opts.useJit) {
            std::string jitErr;
            kernel = CompiledPlanCache::instance().getOrCompile(
                plan, left,
                wantsDirectProject ? JitMode::FilterAndProject : JitMode::Filter,
                &jitErr);
            if (kernel) {
                res.usedJit = true;
                res.usedSimd = kernel->usesSimd;
            } else {
                res.jitError = jitErr;
            }
        }

        if (kernel) {
            std::vector<ChunkScanOutput> chunkOuts(nChunks);
            const size_t nCols = left.schema.size();
            const size_t nOutCols = plan.selectItems.size();
            const bool direct = (kernel->mode == JitMode::FilterAndProject);

            auto processChunk = [&](size_t ci) {
                const auto& ch = left.chunks[ci];
                if (ch.aliveCount == 0 || ch.rowCount == 0) return;

                ChunkScanOutput& out = chunkOuts[ci];
                out.matchRows.resize(ch.rowCount);

                std::vector<const void*> colPtrs(nCols);
                for (size_t c = 0; c < nCols; ++c) colPtrs[c] = ch.columns[c]->rawData();

                std::vector<void*> projPtrs;
                if (direct) {
                    out.projectedChunk = std::make_shared<TableChunk>();
                    out.projectedChunk->columns.reserve(nOutCols);
                    projPtrs.resize(nOutCols);
                    for (size_t k = 0; k < nOutCols; ++k) {
                        auto col = ColumnChunk::create(plan.selectItems[k].outputType);
                        projPtrs[k] = col->rawData();
                        out.projectedChunk->columns.push_back(std::move(col));
                    }
                }

                JitChunkContext ctx;
                ctx.leftColPtrs = colPtrs.data();
                ctx.outProjectPtrs = direct ? projPtrs.data() : nullptr;
                ctx.aliveWords = ch.aliveWords.data();
                ctx.hasAliveMask = ch.allAlive() ? 0u : 1u;
                ctx.rowCount = ch.rowCount;
                ctx.matchIndices = out.matchRows.data();
                ctx.matchCount = 0;

                kernel->entry(&ctx);
                uint32_t m = ctx.matchCount;

                if (!kernel->residualFilterIndices.empty() && m > 0) {
                    uint32_t kept = 0;
                    for (uint32_t i = 0; i < m; ++i) {
                        const uint32_t r = out.matchRows[i];
                        if (passesResidualFilters(plan, kernel->residualFilterIndices,
                                                  left, static_cast<uint32_t>(ci), r)) {
                            out.matchRows[kept++] = r;
                        }
                    }
                    m = kept;
                }
                out.matchRows.resize(m);
                if (out.projectedChunk) out.projectedChunk->rowCount = m;
            };

            if (opts.useParallel && nChunks > 1) {
                brass::codegen::parallel_for(
                    0, static_cast<int64_t>(nChunks),
                    [&](int64_t s, int64_t e) {
                        for (int64_t ci = s; ci < e; ++ci) processChunk(static_cast<size_t>(ci));
                    },
                    1);
            } else {
                for (size_t ci = 0; ci < nChunks; ++ci) processChunk(ci);
            }

            if (direct) {
                std::vector<ColumnDef> outSchema;
                outSchema.reserve(nOutCols);
                for (const SelectItem& si : plan.selectItems) {
                    outSchema.push_back(ColumnDef{si.alias, si.outputType});
                }
                auto outTable = std::make_shared<Table>(std::move(outSchema));
                outTable->setSharedDict(left.dict);

                size_t totalSeen = 0;
                const size_t start = static_cast<size_t>(std::max<int64_t>(0, plan.offset));
                const size_t end = (plan.limit >= 0) ? (start + static_cast<size_t>(plan.limit))
                                                     : SIZE_MAX;

                std::vector<const void*> batchPtrs(nOutCols);
                for (size_t ci = 0; ci < nChunks && totalSeen < end; ++ci) {
                    const ChunkScanOutput& co = chunkOuts[ci];
                    const uint32_t n = static_cast<uint32_t>(co.matchRows.size());
                    if (n == 0) continue;
                    if (totalSeen + n <= start) {
                        totalSeen += n;
                        continue;
                    }
                    const uint32_t localStart = (totalSeen < start)
                                                    ? static_cast<uint32_t>(start - totalSeen)
                                                    : 0u;
                    const uint32_t localEnd = static_cast<uint32_t>(
                        std::min<size_t>(n, end - totalSeen));
                    const uint32_t take = localEnd - localStart;
                    for (size_t k = 0; k < nOutCols; ++k) {
                        const auto& col = co.projectedChunk->columns[k];
                        batchPtrs[k] = col->rawData() +
                                       static_cast<size_t>(localStart) * colTypeElemSize(col->type());
                    }
                    outTable->appendTypedBatch(take, batchPtrs.data());
                    totalSeen += n;
                }
                res.table = std::move(outTable);
                return res;
            }

            size_t totalMatches = 0;
            for (const auto& co : chunkOuts) totalMatches += co.matchRows.size();
            leftMatches.reserve(totalMatches);
            for (uint32_t ci = 0; ci < nChunks; ++ci) {
                for (uint32_t r : chunkOuts[ci].matchRows) {
                    leftMatches.push_back(RowLoc{ci, r});
                }
            }
        } else {
            res.table = executePlanReference(left, plan);
            return res;
        }
    }

    bool joinUsedIdx = false;
    res.table = finishPlanFromLeftMatches(plan, left, leftMatches, &joinUsedIdx);
    if (joinUsedIdx) res.usedIndex = true;
    return res;
}

void scanSingleChunkMatches(const CompiledKernel* kernel, const QueryPlan& boundPlan,
                            const TableSnapshot& snap, size_t ci,
                            std::vector<uint32_t>& outMatches) {
    outMatches.clear();
    if (ci >= snap.chunks.size()) return;
    const auto& ch = snap.chunks[ci];
    if (ch.aliveCount == 0 || ch.rowCount == 0) return;

    if (kernel) {
        const size_t nCols = snap.schema.size();
        outMatches.resize(ch.rowCount);
        std::vector<const void*> colPtrs(nCols);
        for (size_t c = 0; c < nCols; ++c) colPtrs[c] = ch.columns[c]->rawData();

        JitChunkContext ctx;
        ctx.leftColPtrs = colPtrs.data();
        ctx.aliveWords = ch.aliveWords.data();
        ctx.hasAliveMask = ch.allAlive() ? 0u : 1u;
        ctx.rowCount = ch.rowCount;
        ctx.matchIndices = outMatches.data();
        ctx.matchCount = 0;
        kernel->entry(&ctx);

        uint32_t m = ctx.matchCount;
        if (!kernel->residualFilterIndices.empty() && m > 0) {
            uint32_t kept = 0;
            for (uint32_t i = 0; i < m; ++i) {
                if (passesResidualFilters(boundPlan, kernel->residualFilterIndices,
                                          snap, static_cast<uint32_t>(ci), outMatches[i])) {
                    outMatches[kept++] = outMatches[i];
                }
            }
            m = kept;
        }
        outMatches.resize(m);
        return;
    }

    for (uint32_t r = 0; r < ch.rowCount; ++r) {
        if (ch.isAlive(r) &&
            passesAllFilters(boundPlan, boundPlan.filters, snap,
                             static_cast<uint32_t>(ci), r, nullptr, 0, 0)) {
            outMatches.push_back(r);
        }
    }
}

std::shared_ptr<Table> finishPlanFromLeftMatches(const QueryPlan& plan,
                                                   const TableSnapshot& left,
                                                   const std::vector<RowLoc>& leftMatches,
                                                   bool* outUsedIndex) {
    return finishPlanFromLeftMatchesImpl(plan, left, leftMatches, outUsedIndex);
}

MutationExecResult executeUpdate(Table& table, QueryPlan plan, ExecOptions opts) {
    MutationExecResult res;
    auto snap = table.snapshot();
    if (!bindQueryPlan(plan, *snap, res.error)) {
        res.ok = false;
        return res;
    }
    if (plan.updates.empty()) return res;

    bool allJittable = opts.useJit;
    for (const UpdateAssignment& up : plan.updates) {
        const ColType targetType = snap->schema.columns[static_cast<size_t>(up.colIdx)].type;
        if (targetType == ColType::String || !canJitExpr(plan.exprs, up.expr, *snap)) {
            allJittable = false;
            break;
        }
    }

    if (!allJittable) {
        res.affectedRows = applyUpdateReference(table, plan);
        return res;
    }

    auto filterKernel = CompiledPlanCache::instance().getOrCompile(plan, *snap, JitMode::Filter);
    auto updateKernel = CompiledPlanCache::instance().getOrCompile(plan, *snap, JitMode::ScatterUpdate);
    if (!filterKernel || !updateKernel) {
        res.affectedRows = applyUpdateReference(table, plan);
        return res;
    }
    res.usedJit = true;

    const size_t nChunks = snap->chunks.size();
    const size_t nCols = snap->schema.size();
    std::vector<std::vector<uint32_t>> chunkMatches;
    runFilterKernelParallel(*filterKernel, plan, *snap, opts.useParallel, chunkMatches);

    // Track which columns changed so HashIndexes on modified columns rebuild cleanly.
    std::unordered_set<size_t> modifiedIndexedCols;
    std::vector<const void*> oldPtrs(nCols);
    std::vector<void*> targetPtrs(plan.updates.size());

    for (size_t ci = 0; ci < nChunks; ++ci) {
        auto& matches = chunkMatches[ci];
        if (matches.empty()) continue;

        const auto& frozen = snap->chunks[ci];
        for (size_t c = 0; c < nCols; ++c) oldPtrs[c] = frozen.columns[c]->rawData();

        TableChunk& mutChunk = table.chunkMut(ci);
        for (size_t k = 0; k < plan.updates.size(); ++k) {
            const size_t colIdx = static_cast<size_t>(plan.updates[k].colIdx);
            targetPtrs[k] = mutChunk.mutableColumn(colIdx)->rawData();
            modifiedIndexedCols.insert(colIdx);
        }

        JitChunkContext uctx;
        uctx.leftColPtrs = oldPtrs.data();
        uctx.outProjectPtrs = targetPtrs.data();
        uctx.matchIndices = matches.data();
        uctx.matchCount = static_cast<uint32_t>(matches.size());
        updateKernel->entry(&uctx);
        table.markChunkDirty(ci);
        res.affectedRows += matches.size();
    }

    for (size_t c : modifiedIndexedCols) {
        table.notifyColumnUpdated(c);
    }
    return res;
}

MutationExecResult executeDelete(Table& table, QueryPlan plan, ExecOptions opts) {
    MutationExecResult res;
    auto snap = table.snapshot();
    if (!bindQueryPlan(plan, *snap, res.error)) {
        res.ok = false;
        return res;
    }

    std::shared_ptr<const CompiledKernel> filterKernel;
    if (opts.useJit) {
        filterKernel = CompiledPlanCache::instance().getOrCompile(plan, *snap, JitMode::Filter);
    }
    if (!filterKernel) {
        res.affectedRows = applyDeleteReference(table, plan);
        return res;
    }
    res.usedJit = true;

    std::vector<std::vector<uint32_t>> chunkMatches;
    runFilterKernelParallel(*filterKernel, plan, *snap, opts.useParallel, chunkMatches);

    for (size_t ci = 0; ci < snap->chunks.size(); ++ci) {
        for (uint32_t r : chunkMatches[ci]) {
            if (table.removeRow(RowLoc{static_cast<uint32_t>(ci), r})) {
                ++res.affectedRows;
            }
        }
    }
    return res;
}

}  // namespace bro::table
