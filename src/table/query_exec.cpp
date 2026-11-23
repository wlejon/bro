#include "table/query_exec.h"

#include "table/query_eval.h"
#include "table/query_jit.h"
#include "table/query_materialize.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace bro::table {

namespace {

bool chunkHasNullInCols(const TableSnapshot::FrozenChunk& ch,
                        const std::vector<uint32_t>& cols) {
    for (uint32_t c : cols) {
        if (c < ch.columns.size() && ch.columns[c]->hasNulls()) return true;
    }
    return false;
}

bool snapshotHasNullInCols(const TableSnapshot& snap, const std::vector<uint32_t>& cols) {
    if (cols.empty()) return false;
    for (const auto& ch : snap.chunks) {
        if (ch.aliveCount == 0 || ch.rowCount == 0) continue;
        if (chunkHasNullInCols(ch, cols)) return true;
    }
    return false;
}

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
        if (litNode->op != ExprOp::Const || litNode->literal.isNull) continue;
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

struct RangeProbePlan {
    size_t colIdx = SIZE_MAX;
    bool hasLo = false;
    bool incLo = true;
    ScalarValue lo;
    bool hasHi = false;
    bool incHi = true;
    ScalarValue hi;
    bool provablyEmpty = false;
};

bool detectRangeIndexProbe(const TableSnapshot& snap, const QueryPlan& plan, RangeProbePlan& out) {
    std::unordered_map<size_t, RangeProbePlan> colBounds;
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
        RangeProbePlan& b = colBounds[colIdx];
        b.colIdx = colIdx;
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

    if (colBounds.empty()) return false;
    size_t chosenCol = SIZE_MAX;
    for (const auto& kv : colBounds) {
        chosenCol = kv.first;
        if (kv.second.hasLo && kv.second.hasHi) break;
    }
    out = colBounds[chosenCol];
    if (out.hasLo && out.hasHi) {
        const int cmp = compareScalar(out.lo, out.hi);
        if (cmp > 0 || (cmp == 0 && (!out.incLo || !out.incHi))) {
            out.provablyEmpty = true;
        }
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

enum class IndexProbeKind : uint8_t {
    None = 0,
    Spatial,
    Hash,
    Range,
};

struct CandidateLookupResult {
    IndexProbeKind kind = IndexProbeKind::None;
    std::string indexColName;
    std::vector<RowLoc> matches;
};

void filterDedupCandidates(const TableSnapshot& snap, const QueryPlan& plan,
                           std::vector<RowLoc>& candidates, std::vector<RowLoc>& outMatches) {
    outMatches.clear();
    std::sort(candidates.begin(), candidates.end(), [](const RowLoc& a, const RowLoc& b) {
        return (a.chunkIdx != b.chunkIdx) ? (a.chunkIdx < b.chunkIdx) : (a.rowIdx < b.rowIdx);
    });
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    outMatches.reserve(candidates.size());
    for (const RowLoc& loc : candidates) {
        if (loc.chunkIdx >= snap.chunks.size() || !snap.chunks[loc.chunkIdx].isAlive(loc.rowIdx)) {
            continue;
        }
        if (passesAllFilters(plan, plan.filters, snap, loc.chunkIdx, loc.rowIdx, nullptr, 0, 0)) {
            outMatches.push_back(loc);
        }
    }
}

bool tryIndexCandidateLookup(const TableSnapshot& left, const QueryPlan& plan,
                             const ExecOptions& opts, CandidateLookupResult& out) {
    if (!opts.useIndexes) return false;

    if (plan.spatialFilter.kind != SpatialFilterSpec::Kind::None) {
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
            filterDedupCandidates(left, plan, candidates, out.matches);
            out.kind = IndexProbeKind::Spatial;
            out.indexColName = plan.spatialFilter.indexName;
            return true;
        }
        return false;
    }

    if (plan.filters.empty()) return false;

    IndexProbeSpec hashProbe = detectIndexProbe(left, plan);
    if (hashProbe.valid) {
        const HashIndex* idx = left.findIndex(hashProbe.colIdx);
        std::vector<RowLoc> candidates;
        idx->lookupKey(hashProbe.key, candidates);
        filterDedupCandidates(left, plan, candidates, out.matches);
        out.kind = IndexProbeKind::Hash;
        out.indexColName = left.schema.columns[hashProbe.colIdx].name;
        return true;
    }

    RangeProbePlan rangeProbe;
    if (detectRangeIndexProbe(left, plan, rangeProbe)) {
        out.kind = IndexProbeKind::Range;
        out.indexColName = left.schema.columns[rangeProbe.colIdx].name;
        if (!rangeProbe.provablyEmpty) {
            const RangeIndex* ridx = left.findRangeIndex(rangeProbe.colIdx);
            std::vector<RowLoc> candidates;
            ridx->lookupRange(rangeProbe.hasLo ? &rangeProbe.lo : nullptr, rangeProbe.incLo,
                              rangeProbe.hasHi ? &rangeProbe.hi : nullptr, rangeProbe.incHi,
                              left.dict.get(), candidates);
            filterDedupCandidates(left, plan, candidates, out.matches);
        }
        return true;
    }
    return false;
}

bool detectRangeTopKFastPath(const TableSnapshot& left, const QueryPlan& plan, size_t* outCol = nullptr) {
    if (!plan.filters.empty() || plan.join.kind != JoinSpec::Kind::None ||
        !plan.groupKeys.empty() || !plan.aggregates.empty() ||
        plan.orderBy.size() != 1 || plan.limit < 0) {
        return false;
    }
    const ExprNode& on = plan.exprs.nodes[plan.orderBy[0].expr];
    if (on.op != ExprOp::ColRef || on.tableSide != 0 || on.resolvedCol < 0) return false;
    const RangeIndex* ridx = left.findRangeIndex(static_cast<size_t>(on.resolvedCol));
    if (!ridx || ridx->size() != left.totalAliveRows) return false;
    if (outCol) *outCol = static_cast<size_t>(on.resolvedCol);
    return true;
}

bool tryRangeTopKFastPath(const TableSnapshot& left, const QueryPlan& plan,
                          std::shared_ptr<Table>& outTable) {
    size_t colIdx = 0;
    if (!detectRangeTopKFastPath(left, plan, &colIdx)) return false;
    const RangeIndex* ridx = left.findRangeIndex(colIdx);

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
            ScalarValue cur = left.getCell(colIdx, loc.chunkIdx, loc.rowIdx);
            if (!haveBound) {
                boundKey = left.getCell(colIdx, locs.back().chunkIdx, locs.back().rowIdx);
                haveBound = true;
            }
            if (compareScalar(cur, boundKey) != 0) return false;
        }
        locs.push_back(loc);
        return desc || locs.size() < need;
    });

    if (desc && locs.size() > 1) {
        std::stable_sort(locs.begin(), locs.end(), [&](RowLoc a, RowLoc bLoc) {
            ScalarValue ka = left.getCell(colIdx, a.chunkIdx, a.rowIdx);
            ScalarValue kb = left.getCell(colIdx, bLoc.chunkIdx, bLoc.rowIdx);
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

        // Nullable chunk fallback ensures SQL-like null propagation on predicates.
        if (chunkHasNullInCols(ch, kernel.usedColIndices)) {
            for (uint32_t r = 0; r < ch.rowCount; ++r) {
                if (ch.isAlive(r) &&
                    passesAllFilters(plan, plan.filters, snap,
                                     static_cast<uint32_t>(ci), r, nullptr, 0, 0)) {
                    matches.push_back(r);
                }
            }
            return;
        }

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

bool shouldWantDirectProject(const QueryPlan& plan, const TableSnapshot& left,
                             const ExecOptions& opts) {
    const bool hasJoin = plan.join.kind != JoinSpec::Kind::None && plan.join.rightTable;
    const bool hasAgg = !plan.groupKeys.empty() || !plan.aggregates.empty();
    if (!opts.useJit || hasJoin || hasAgg || !plan.orderBy.empty() ||
        plan.spatialFilter.kind != SpatialFilterSpec::Kind::None ||
        plan.selectItems.empty()) {
        return false;
    }
    for (const SelectItem& si : plan.selectItems) {
        if (si.expr != kInvalidExpr && plan.exprs.nodes[si.expr].op != ExprOp::ColRef) {
            return true;
        }
    }
    return !canCompileSimdFilter(plan, left);
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

    std::vector<RowLoc> leftMatches;
    bool usedIndex = false;

    // Priority 1..3a: SpatialIndex, HashIndex, RangeIndex probes
    CandidateLookupResult idxLookup;
    if (tryIndexCandidateLookup(left, plan, opts, idxLookup)) {
        usedIndex = true;
        leftMatches = std::move(idxLookup.matches);
        if (idxLookup.kind == IndexProbeKind::Spatial) res.usedSpatialIndex = true;
        else if (idxLookup.kind == IndexProbeKind::Hash) res.usedIndex = true;
        else if (idxLookup.kind == IndexProbeKind::Range) res.usedRangeIndex = true;
    } else if (opts.useIndexes && plan.spatialFilter.kind == SpatialFilterSpec::Kind::None) {
        // Priority 3b: RangeIndex Top-K fast path
        if (tryRangeTopKFastPath(left, plan, res.table)) {
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
            if (!aggSnapshotHasNulls(*aggKernel, left)) {
                res.table = runCompiledAgg(*aggKernel, plan, left, opts.useParallel);
                res.usedJit = true;
                res.usedJitAgg = true;
                res.usedSimd = aggKernel->usesSimd;
                res.simdWidth = aggKernel->simdWidth;
                return res;
            }
        }
    }

    // Priority 5: JIT (256/128-bit SIMD Filter or Scalar Filter/Project) parallel chunk scan
    if (!usedIndex) {
        if (plan.spatialFilter.kind != SpatialFilterSpec::Kind::None) {
            res.table = executePlanReference(left, plan);
            return res;
        }
        const size_t nChunks = left.chunks.size();
        const bool wantsDirect = shouldWantDirectProject(plan, left, opts);
        std::shared_ptr<const CompiledKernel> kernel;
        if (opts.useJit) {
            std::string jitErr;
            kernel = CompiledPlanCache::instance().getOrCompile(
                plan, left,
                wantsDirect ? JitMode::FilterAndProject : JitMode::Filter,
                &jitErr);
            // If direct projection touches nullable columns, downgrade to Filter + materializer
            // so dense chunks keep SIMD filtering while nulls materialize accurately.
            if (kernel && kernel->mode == JitMode::FilterAndProject &&
                snapshotHasNullInCols(left, kernel->usedColIndices)) {
                kernel = CompiledPlanCache::instance().getOrCompile(
                    plan, left, JitMode::Filter, &jitErr);
            }
            if (kernel) {
                res.usedJit = true;
                res.usedSimd = kernel->usesSimd;
                res.simdWidth = kernel->simdWidth;
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
                if (!direct && chunkHasNullInCols(ch, kernel->usedColIndices)) {
                    for (uint32_t r = 0; r < ch.rowCount; ++r) {
                        if (ch.isAlive(r) &&
                            passesAllFilters(plan, plan.filters, left,
                                             static_cast<uint32_t>(ci), r, nullptr, 0, 0)) {
                            out.matchRows.push_back(r);
                        }
                    }
                    return;
                }

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
    res.table = finishPlanFromLeftMatches(plan, left, leftMatches, &joinUsedIdx, opts.useParallel);
    if (joinUsedIdx) res.usedIndex = true;
    return res;
}

QueryExplainResult explainQuery(const TableSnapshot& left, QueryPlan plan, ExecOptions opts) {
    QueryExplainResult out;
    if (!plan.bound && !bindQueryPlan(plan, left, out.error)) {
        out.ok = false;
        out.strategy = "invalid";
        return out;
    }

    const bool hasJoin = plan.join.kind != JoinSpec::Kind::None && plan.join.rightTable;
    const bool hasAgg = !plan.groupKeys.empty() || !plan.aggregates.empty();

    if (opts.useIndexes) {
        if (plan.spatialFilter.kind != SpatialFilterSpec::Kind::None &&
            left.findSpatialIndex(plan.spatialFilter.indexName) != nullptr) {
            out.strategy = "spatial_index";
            out.willUseSpatialIndex = true;
            out.indexColumn = plan.spatialFilter.indexName;
            return out;
        }
        if (plan.spatialFilter.kind == SpatialFilterSpec::Kind::None && !plan.filters.empty()) {
            IndexProbeSpec hashProbe = detectIndexProbe(left, plan);
            if (hashProbe.valid) {
                out.strategy = "hash_index";
                out.willUseIndex = true;
                out.indexColumn = left.schema.columns[hashProbe.colIdx].name;
                return out;
            }
            RangeProbePlan rp;
            if (detectRangeIndexProbe(left, plan, rp)) {
                out.strategy = "range_index";
                out.willUseRangeIndex = true;
                out.indexColumn = left.schema.columns[rp.colIdx].name;
                return out;
            }
        }
        size_t topKCol = 0;
        if (plan.spatialFilter.kind == SpatialFilterSpec::Kind::None &&
            detectRangeTopKFastPath(left, plan, &topKCol)) {
            out.strategy = "range_topk";
            out.willUseRangeIndex = true;
            out.indexColumn = left.schema.columns[topKCol].name;
            return out;
        }
    }

    if (plan.spatialFilter.kind != SpatialFilterSpec::Kind::None || !opts.useJit) {
        out.strategy = "reference";
        return out;
    }

    if (!hasJoin && plan.groupKeys.empty() && !plan.aggregates.empty() && plan.orderBy.empty()) {
        if (auto aggKernel = CompiledPlanCache::instance().getOrCompileAgg(plan, left, &out.jitError)) {
            out.hasNullsInPlanCols = aggSnapshotHasNulls(*aggKernel, left);
            out.residualFilterIndices = aggKernel->residualFilterIndices;
            if (!out.hasNullsInPlanCols) {
                out.strategy = "jit_agg";
                out.willUseJit = true;
                out.willUseJitAgg = true;
                out.willUseSimd = aggKernel->usesSimd;
                out.simdWidth = aggKernel->simdWidth;
                return out;
            }
        }
    }

    const bool wantsDirect = shouldWantDirectProject(plan, left, opts);
    auto kernel = CompiledPlanCache::instance().getOrCompile(
        plan, left, wantsDirect ? JitMode::FilterAndProject : JitMode::Filter, &out.jitError);
    if (kernel && kernel->mode == JitMode::FilterAndProject &&
        snapshotHasNullInCols(left, kernel->usedColIndices)) {
        kernel = CompiledPlanCache::instance().getOrCompile(
            plan, left, JitMode::Filter, &out.jitError);
    }
    if (kernel) {
        out.willUseJit = true;
        out.willUseSimd = kernel->usesSimd;
        out.simdWidth = kernel->simdWidth;
        out.residualFilterIndices = kernel->residualFilterIndices;
        out.hasNullsInPlanCols = snapshotHasNullInCols(left, kernel->usedColIndices);
        out.strategy = kernel->usesSimd ? "jit_simd" : "jit_scalar";
        return out;
    }

    out.strategy = "reference";
    return out;
}

void scanSingleChunkMatches(const CompiledKernel* kernel, const QueryPlan& boundPlan,
                            const TableSnapshot& snap, size_t ci,
                            std::vector<uint32_t>& outMatches) {
    outMatches.clear();
    if (ci >= snap.chunks.size()) return;
    const auto& ch = snap.chunks[ci];
    if (ch.aliveCount == 0 || ch.rowCount == 0) return;

    if (kernel && !chunkHasNullInCols(ch, kernel->usedColIndices)) {
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
                                                   bool* outUsedIndex,
                                                   bool useParallel) {
    return finishPlanFromLeftMatchesImpl(plan, left, leftMatches, outUsedIndex, useParallel);
}

MutationExecResult executeUpdate(Table& table, QueryPlan plan, ExecOptions opts) {
    MutationExecResult res;
    auto snap = table.snapshot();
    if (!bindQueryPlan(plan, *snap, res.error)) {
        res.ok = false;
        return res;
    }
    if (plan.updates.empty()) return res;

    // Fast path 1: indexed / spatial candidate update
    CandidateLookupResult idxLookup;
    if (tryIndexCandidateLookup(*snap, plan, opts, idxLookup)) {
        if (idxLookup.kind == IndexProbeKind::Spatial) res.usedSpatialIndex = true;
        else if (idxLookup.kind == IndexProbeKind::Hash) res.usedIndex = true;
        else if (idxLookup.kind == IndexProbeKind::Range) res.usedRangeIndex = true;

        std::vector<ScalarValue> nextVals(plan.updates.size());
        for (const RowLoc& loc : idxLookup.matches) {
            for (size_t ui = 0; ui < plan.updates.size(); ++ui) {
                nextVals[ui] = evalExpr(plan.exprs, plan.updates[ui].expr, *snap, loc.chunkIdx, loc.rowIdx);
            }
            for (size_t ui = 0; ui < plan.updates.size(); ++ui) {
                table.updateCell(loc, static_cast<size_t>(plan.updates[ui].colIdx), nextVals[ui]);
            }
            ++res.affectedRows;
        }
        return res;
    }

    if (plan.spatialFilter.kind != SpatialFilterSpec::Kind::None) {
        res.affectedRows = applyUpdateReference(table, plan);
        return res;
    }

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

    std::vector<uint32_t> targetCols;
    targetCols.reserve(plan.updates.size());
    for (const UpdateAssignment& up : plan.updates) targetCols.push_back(static_cast<uint32_t>(up.colIdx));

    const size_t nChunks = snap->chunks.size();
    const size_t nCols = snap->schema.size();
    std::vector<std::vector<uint32_t>> chunkMatches;
    runFilterKernelParallel(*filterKernel, plan, *snap, opts.useParallel, chunkMatches);

    std::unordered_set<size_t> scatterModifiedCols;
    std::vector<const void*> oldPtrs(nCols);
    std::vector<void*> targetPtrs(plan.updates.size());
    std::vector<ScalarValue> nextVals(plan.updates.size());

    for (size_t ci = 0; ci < nChunks; ++ci) {
        auto& matches = chunkMatches[ci];
        if (matches.empty()) continue;

        const auto& frozen = snap->chunks[ci];
        // Chunks containing nulls in source or target columns use updateCell so null bitmaps & indexes stay exact.
        if (chunkHasNullInCols(frozen, updateKernel->usedColIndices) ||
            chunkHasNullInCols(frozen, targetCols)) {
            for (uint32_t r : matches) {
                for (size_t ui = 0; ui < plan.updates.size(); ++ui) {
                    nextVals[ui] = evalExpr(plan.exprs, plan.updates[ui].expr, *snap,
                                            static_cast<uint32_t>(ci), r);
                }
                for (size_t ui = 0; ui < plan.updates.size(); ++ui) {
                    table.updateCell(RowLoc{static_cast<uint32_t>(ci), r},
                                     static_cast<size_t>(plan.updates[ui].colIdx), nextVals[ui]);
                }
            }
            res.affectedRows += matches.size();
            continue;
        }

        for (size_t c = 0; c < nCols; ++c) oldPtrs[c] = frozen.columns[c]->rawData();

        TableChunk& mutChunk = table.chunkMut(ci);
        for (size_t k = 0; k < plan.updates.size(); ++k) {
            const size_t colIdx = static_cast<size_t>(plan.updates[k].colIdx);
            targetPtrs[k] = mutChunk.mutableColumn(colIdx)->rawData();
            scatterModifiedCols.insert(colIdx);
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

    for (size_t c : scatterModifiedCols) {
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

    CandidateLookupResult idxLookup;
    if (tryIndexCandidateLookup(*snap, plan, opts, idxLookup)) {
        if (idxLookup.kind == IndexProbeKind::Spatial) res.usedSpatialIndex = true;
        else if (idxLookup.kind == IndexProbeKind::Hash) res.usedIndex = true;
        else if (idxLookup.kind == IndexProbeKind::Range) res.usedRangeIndex = true;
        for (const RowLoc& loc : idxLookup.matches) {
            if (table.removeRow(loc)) ++res.affectedRows;
        }
        return res;
    }

    if (plan.spatialFilter.kind != SpatialFilterSpec::Kind::None) {
        res.affectedRows = applyDeleteReference(table, plan);
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
