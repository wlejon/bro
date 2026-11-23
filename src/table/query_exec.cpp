#include "table/query_exec.h"

#include "table/query_eval.h"
#include "table/query_jit.h"

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

struct MatchPair {
    RowLoc left{};
    RowLoc right{UINT32_MAX, UINT32_MAX};
};

void buildRightHashMap(const TableSnapshot& right, const JoinSpec& join,
                       std::unordered_map<uint64_t, std::vector<RowLoc>>& out) {
    for (uint32_t rci = 0; rci < right.chunks.size(); ++rci) {
        const auto& rch = right.chunks[rci];
        for (uint32_t rr = 0; rr < rch.rowCount; ++rr) {
            if (!rch.isAlive(rr)) continue;
            uint64_t h = 0x517cc1b727220a95ULL;
            for (size_t k = 0; k < join.rightKeyCols.size(); ++k) {
                ScalarValue kv = right.getCell(join.rightKeyCols[k], rci, rr);
                h = combineHash64(h, hashScalarKey(join.keyTypes[k], kv));
            }
            out[h].push_back(RowLoc{rci, rr});
        }
    }
}

void writeOutCell(TableChunk& ch, size_t colIdx, uint32_t row, const ScalarValue& v,
                  StringDictionary* dict) {
    if (!v.isNull) ch.columns[colIdx]->writeCell(row, v, dict);
}

std::shared_ptr<Table> buildTableFromMatchPairs(const QueryPlan& plan, const TableSnapshot& left,
                                                std::vector<MatchPair>& pairs) {
    const TableSnapshot* right = (plan.join.kind != JoinSpec::Kind::None)
                                     ? plan.join.rightTable.get()
                                     : nullptr;

    std::vector<ColumnDef> outSchema;
    for (const SelectItem& si : plan.selectItems) {
        outSchema.push_back(ColumnDef{si.alias, si.outputType});
    }
    auto outTable = std::make_shared<Table>(std::move(outSchema));

    if (!plan.orderBy.empty() && !pairs.empty()) {
        std::vector<std::vector<ScalarValue>> sortKeys(pairs.size());
        for (size_t i = 0; i < pairs.size(); ++i) {
            sortKeys[i].reserve(plan.orderBy.size());
            for (const OrderByItem& ob : plan.orderBy) {
                sortKeys[i].push_back(evalExpr(plan.exprs, ob.expr,
                                               left, pairs[i].left.chunkIdx, pairs[i].left.rowIdx,
                                               right, pairs[i].right.chunkIdx, pairs[i].right.rowIdx));
            }
        }
        std::vector<size_t> perm(pairs.size());
        for (size_t i = 0; i < perm.size(); ++i) perm[i] = i;

        auto cmp = [&](size_t a, size_t b) {
            for (size_t k = 0; k < plan.orderBy.size(); ++k) {
                const int c = compareScalar(sortKeys[a][k], sortKeys[b][k]);
                if (c != 0) return plan.orderBy[k].descending ? (c > 0) : (c < 0);
            }
            return a < b;
        };

        const size_t start = static_cast<size_t>(std::max<int64_t>(0, plan.offset));
        const size_t need = (plan.limit >= 0) ? (start + static_cast<size_t>(plan.limit))
                                              : perm.size();
        if (need < perm.size()) {
            std::partial_sort(perm.begin(), perm.begin() + static_cast<std::ptrdiff_t>(need),
                              perm.end(), cmp);
            perm.resize(need);
        } else {
            std::stable_sort(perm.begin(), perm.end(), cmp);
        }

        const size_t end = std::min(perm.size(), need);
        std::vector<ScalarValue> rowBuf(plan.selectItems.size());
        for (size_t i = start; i < end; ++i) {
            const MatchPair& p = pairs[perm[i]];
            for (size_t k = 0; k < plan.selectItems.size(); ++k) {
                rowBuf[k] = evalExpr(plan.exprs, plan.selectItems[k].expr,
                                     left, p.left.chunkIdx, p.left.rowIdx,
                                     right, p.right.chunkIdx, p.right.rowIdx);
            }
            outTable->appendRow(rowBuf);
        }
        return outTable;
    }

    const size_t start = static_cast<size_t>(std::max<int64_t>(0, plan.offset));
    const size_t end = (plan.limit >= 0)
                           ? std::min(pairs.size(), start + static_cast<size_t>(plan.limit))
                           : pairs.size();
    std::vector<ScalarValue> rowBuf(plan.selectItems.size());
    for (size_t i = start; i < end; ++i) {
        const MatchPair& p = pairs[i];
        for (size_t k = 0; k < plan.selectItems.size(); ++k) {
            rowBuf[k] = evalExpr(plan.exprs, plan.selectItems[k].expr,
                                 left, p.left.chunkIdx, p.left.rowIdx,
                                 right, p.right.chunkIdx, p.right.rowIdx);
        }
        outTable->appendRow(rowBuf);
    }
    return outTable;
}

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

std::shared_ptr<Table> buildGroupedTable(const QueryPlan& plan, const TableSnapshot& left,
                                         const std::vector<MatchPair>& pairs) {
    const TableSnapshot* right = (plan.join.kind != JoinSpec::Kind::None)
                                     ? plan.join.rightTable.get()
                                     : nullptr;

    std::vector<ColumnDef> outSchema;
    for (size_t gi = 0; gi < plan.groupKeys.size(); ++gi) {
        std::string name = gi < plan.groupAliases.size() ? plan.groupAliases[gi]
                                                         : plan.exprs.nodes[plan.groupKeys[gi]].colName;
        outSchema.push_back(ColumnDef{std::move(name),
                                      plan.exprs.nodes[plan.groupKeys[gi]].valueType});
    }
    for (size_t ai = 0; ai < plan.aggregates.size(); ++ai) {
        std::string name = !plan.aggregates[ai].alias.empty()
                               ? plan.aggregates[ai].alias
                               : ("agg_" + std::to_string(ai));
        outSchema.push_back(ColumnDef{std::move(name), plan.aggregates[ai].outputType});
    }

    struct GroupState {
        std::vector<ScalarValue> keys;
        std::vector<double> aggF;
        std::vector<int64_t> aggI;
        std::vector<ScalarValue> aggExt;
        uint64_t count = 0;
    };
    std::vector<GroupState> groups;
    std::unordered_map<uint64_t, std::vector<size_t>> hashToGroup;

    for (const MatchPair& p : pairs) {
        std::vector<ScalarValue> keys;
        uint64_t h = 0x517cc1b727220a95ULL;
        for (size_t gi = 0; gi < plan.groupKeys.size(); ++gi) {
            ScalarValue kv = evalExpr(plan.exprs, plan.groupKeys[gi],
                                      left, p.left.chunkIdx, p.left.rowIdx,
                                      right, p.right.chunkIdx, p.right.rowIdx);
            h = combineHash64(h, hashScalarKey(plan.exprs.nodes[plan.groupKeys[gi]].valueType, kv));
            keys.push_back(std::move(kv));
        }
        size_t gIdx = SIZE_MAX;
        auto it = hashToGroup.find(h);
        if (it != hashToGroup.end()) {
            for (size_t cand : it->second) {
                bool match = true;
                for (size_t k = 0; k < keys.size(); ++k) {
                    if (compareScalar(groups[cand].keys[k], keys[k]) != 0) {
                        match = false;
                        break;
                    }
                }
                if (match) { gIdx = cand; break; }
            }
        }
        if (gIdx == SIZE_MAX) {
            gIdx = groups.size();
            GroupState g;
            g.keys = std::move(keys);
            g.aggF.assign(plan.aggregates.size(), 0.0);
            g.aggI.assign(plan.aggregates.size(), 0);
            g.aggExt.assign(plan.aggregates.size(), ScalarValue::makeNull());
            groups.push_back(std::move(g));
            hashToGroup[h].push_back(gIdx);
        }
        GroupState& g = groups[gIdx];
        const bool firstInGroup = (g.count == 0);
        ++g.count;
        for (size_t ai = 0; ai < plan.aggregates.size(); ++ai) {
            const AggSpec& a = plan.aggregates[ai];
            if (a.kind == AggSpec::Kind::Count) {
                g.aggI[ai] += 1;
                continue;
            }
            ScalarValue val = evalExpr(plan.exprs, a.expr,
                                       left, p.left.chunkIdx, p.left.rowIdx,
                                       right, p.right.chunkIdx, p.right.rowIdx);
            double dv = colTypeIsFloat(val.type)
                            ? val.f64
                            : (colTypeIsUnsignedInt(val.type) ? static_cast<double>(val.u64)
                                                             : static_cast<double>(val.i64));
            if (a.kind == AggSpec::Kind::Sum || a.kind == AggSpec::Kind::Avg) {
                if (a.outputType == ColType::I64) g.aggI[ai] += val.i64;
                else g.aggF[ai] += dv;
            } else if (a.kind == AggSpec::Kind::Min) {
                if (firstInGroup || compareScalar(val, g.aggExt[ai]) < 0) g.aggExt[ai] = std::move(val);
            } else if (a.kind == AggSpec::Kind::Max) {
                if (firstInGroup || compareScalar(val, g.aggExt[ai]) > 0) g.aggExt[ai] = std::move(val);
            }
        }
    }

    if (plan.groupKeys.empty() && groups.empty()) {
        GroupState g;
        g.aggF.assign(plan.aggregates.size(), 0.0);
        g.aggI.assign(plan.aggregates.size(), 0);
        g.aggExt.assign(plan.aggregates.size(), ScalarValue::makeNull());
        groups.push_back(std::move(g));
    }

    std::vector<std::vector<ScalarValue>> outRows;
    outRows.reserve(groups.size());
    for (const GroupState& g : groups) {
        std::vector<ScalarValue> row;
        row.reserve(outSchema.size());
        for (const ScalarValue& k : g.keys) row.push_back(k);
        for (size_t ai = 0; ai < plan.aggregates.size(); ++ai) {
            const AggSpec& a = plan.aggregates[ai];
            if (a.kind == AggSpec::Kind::Count) {
                row.push_back(ScalarValue::makeI64(g.aggI[ai]));
            } else if (a.kind == AggSpec::Kind::Sum) {
                row.push_back(a.outputType == ColType::I64 ? ScalarValue::makeI64(g.aggI[ai])
                                                           : ScalarValue::makeF64(g.aggF[ai]));
            } else if (a.kind == AggSpec::Kind::Avg) {
                row.push_back(ScalarValue::makeF64(g.count > 0 ? (g.aggF[ai] / static_cast<double>(g.count)) : 0.0));
            } else {
                row.push_back(g.aggExt[ai]);
            }
        }
        outRows.push_back(std::move(row));
    }

    if (!plan.orderBy.empty()) {
        auto findOutCol = [&](uint32_t exprId) -> int {
            const ExprNode& en = plan.exprs.nodes[exprId];
            if (en.op != ExprOp::ColRef) return -1;
            for (size_t c = 0; c < outSchema.size(); ++c) {
                if (outSchema[c].name == en.colName) return static_cast<int>(c);
            }
            return -1;
        };
        std::vector<size_t> perm(outRows.size());
        for (size_t i = 0; i < perm.size(); ++i) perm[i] = i;
        std::stable_sort(perm.begin(), perm.end(), [&](size_t a, size_t b) {
            for (const OrderByItem& ob : plan.orderBy) {
                int oc = findOutCol(ob.expr);
                if (oc < 0) continue;
                const int cmp = compareScalar(outRows[a][oc], outRows[b][oc]);
                if (cmp != 0) return ob.descending ? (cmp > 0) : (cmp < 0);
            }
            return a < b;
        });
        std::vector<std::vector<ScalarValue>> sorted;
        sorted.reserve(outRows.size());
        for (size_t idx : perm) sorted.push_back(std::move(outRows[idx]));
        outRows = std::move(sorted);
    }

    auto outTable = std::make_shared<Table>(std::move(outSchema));
    const size_t start = static_cast<size_t>(std::max<int64_t>(0, plan.offset));
    const size_t end = (plan.limit >= 0)
                           ? std::min(outRows.size(), start + static_cast<size_t>(plan.limit))
                           : outRows.size();
    for (size_t i = start; i < end; ++i) {
        outTable->appendRow(outRows[i]);
    }
    return outTable;
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
    const bool wantsDirectProject =
        opts.useJit && !hasJoin && !hasAgg && plan.orderBy.empty() && !plan.selectItems.empty();

    std::vector<RowLoc> leftMatches;

    // Step 1: Index probe fast path if eligible
    bool usedIndex = false;
    if (opts.useIndexes && !plan.filters.empty()) {
        IndexProbeSpec probe = detectIndexProbe(left, plan);
        if (probe.valid) {
            const HashIndex* idx = left.findIndex(probe.colIdx);
            std::vector<RowLoc> candidates;
            idx->lookupKey(probe.key, candidates);
            // Index lookup order may differ from physical order; restore chunk/row order so tie behavior & tests match!
            std::sort(candidates.begin(), candidates.end(), [](const RowLoc& a, const RowLoc& b) {
                return (a.chunkIdx != b.chunkIdx) ? (a.chunkIdx < b.chunkIdx) : (a.rowIdx < b.rowIdx);
            });
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

    // Step 2: JIT parallel scan if index did not satisfy the scan
    if (!usedIndex) {
        const size_t nChunks = left.chunks.size();
        std::shared_ptr<const CompiledKernel> kernel;
        if (opts.useJit) {
            std::string jitErr;
            kernel = CompiledPlanCache::instance().getOrCompile(
                plan, left,
                wantsDirectProject ? JitMode::FilterAndProject : JitMode::Filter,
                &jitErr);
            if (kernel) res.usedJit = true;
            else res.jitError = jitErr;
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
                // Fast columnar pack directly from JIT output chunks!
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
            // Non-JIT fallback scan (opts.useJit == false or fallback path)
            res.table = executePlanReference(left, plan);
            return res;
        }
    }

    // Step 3: Join + GroupBy + Project/Order/Limit on survivors
    std::vector<MatchPair> pairs;
    const TableSnapshot* right = hasJoin ? plan.join.rightTable.get() : nullptr;

    if (!hasJoin) {
        pairs.reserve(leftMatches.size());
        for (RowLoc l : leftMatches) pairs.push_back(MatchPair{l, {UINT32_MAX, UINT32_MAX}});
    } else {
        const HashIndex* rightSingleIdx =
            (plan.join.rightKeyCols.size() == 1)
                ? right->findIndex(plan.join.rightKeyCols[0])
                : nullptr;
        std::unordered_map<uint64_t, std::vector<RowLoc>> rightHash;
        if (!rightSingleIdx) {
            buildRightHashMap(*right, plan.join, rightHash);
        } else {
            res.usedIndex = true;
        }

        std::vector<RowLoc> candidates;
        std::vector<ScalarValue> lKeys(plan.join.leftKeyCols.size());
        for (RowLoc ll : leftMatches) {
            candidates.clear();
            if (rightSingleIdx) {
                lKeys[0] = left.getCell(plan.join.leftKeyCols[0], ll.chunkIdx, ll.rowIdx);
                uint64_t key = 0;
                if (HashIndex::scalarToKey(right->schema.columns[plan.join.rightKeyCols[0]].type,
                                           lKeys[0], right->dict.get(), key)) {
                    rightSingleIdx->lookupKey(key, candidates);
                }
            } else {
                uint64_t h = 0x517cc1b727220a95ULL;
                for (size_t k = 0; k < plan.join.leftKeyCols.size(); ++k) {
                    lKeys[k] = left.getCell(plan.join.leftKeyCols[k], ll.chunkIdx, ll.rowIdx);
                    h = combineHash64(h, hashScalarKey(plan.join.keyTypes[k], lKeys[k]));
                }
                auto it = rightHash.find(h);
                if (it != rightHash.end()) candidates = it->second;
            }

            bool matchedAny = false;
            for (RowLoc rl : candidates) {
                if (rl.chunkIdx >= right->chunks.size() ||
                    !right->chunks[rl.chunkIdx].isAlive(rl.rowIdx)) {
                    continue;
                }
                bool eq = true;
                for (size_t k = 0; k < plan.join.rightKeyCols.size(); ++k) {
                    ScalarValue rk = right->getCell(plan.join.rightKeyCols[k], rl.chunkIdx, rl.rowIdx);
                    if (compareScalar(lKeys[k], rk) != 0) {
                        eq = false;
                        break;
                    }
                }
                if (!eq) continue;
                if (!passesAllFilters(plan, plan.postJoinFilters, left, ll.chunkIdx, ll.rowIdx,
                                      right, rl.chunkIdx, rl.rowIdx)) {
                    continue;
                }
                matchedAny = true;
                pairs.push_back(MatchPair{ll, rl});
            }
            if (!matchedAny && plan.join.kind == JoinSpec::Kind::Left) {
                if (passesAllFilters(plan, plan.postJoinFilters, left, ll.chunkIdx, ll.rowIdx,
                                     right, UINT32_MAX, UINT32_MAX)) {
                    pairs.push_back(MatchPair{ll, {UINT32_MAX, UINT32_MAX}});
                }
            }
        }
    }

    if (hasAgg) {
        res.table = buildGroupedTable(plan, left, pairs);
    } else {
        res.table = buildTableFromMatchPairs(plan, left, pairs);
    }
    return res;
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
            if (table.hasIndex(colIdx)) modifiedIndexedCols.insert(colIdx);
        }

        JitChunkContext uctx;
        uctx.leftColPtrs = oldPtrs.data();
        uctx.outProjectPtrs = targetPtrs.data();
        uctx.matchIndices = matches.data();
        uctx.matchCount = static_cast<uint32_t>(matches.size());
        updateKernel->entry(&uctx);
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
