#include "table/query_materialize.h"

#include "table/query_eval.h"
#include "table/query_groupby.h"

#include <algorithm>
#include <unordered_map>

namespace bro::table {

namespace {

struct MatchPair {
    RowLoc left{};
    RowLoc right{UINT32_MAX, UINT32_MAX};
};

bool passesAll(const QueryPlan& plan, const std::vector<uint32_t>& flt,
               const TableSnapshot& left, uint32_t lc, uint32_t lr,
               const TableSnapshot* right, uint32_t rc, uint32_t rr) {
    for (uint32_t f : flt) {
        if (!scalarTruthy(evalExpr(plan.exprs, f, left, lc, lr, right, rc, rr))) return false;
    }
    return true;
}

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
    if (!right) outTable->setSharedDict(left.dict);

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
        std::vector<uint64_t> aggCounts;
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
            g.aggCounts.assign(plan.aggregates.size(), 0);
            g.aggExt.assign(plan.aggregates.size(), ScalarValue::makeNull());
            groups.push_back(std::move(g));
            hashToGroup[h].push_back(gIdx);
        }
        GroupState& g = groups[gIdx];
        ++g.count;
        for (size_t ai = 0; ai < plan.aggregates.size(); ++ai) {
            const AggSpec& a = plan.aggregates[ai];
            if (a.kind == AggSpec::Kind::Count && a.expr == kInvalidExpr) {
                g.aggI[ai] += 1;
                g.aggCounts[ai] += 1;
                continue;
            }
            ScalarValue val = evalExpr(plan.exprs, a.expr,
                                       left, p.left.chunkIdx, p.left.rowIdx,
                                       right, p.right.chunkIdx, p.right.rowIdx);
            if (val.isNull) continue;
            double dv = colTypeIsFloat(val.type)
                            ? val.f64
                            : (colTypeIsUnsignedInt(val.type) ? static_cast<double>(val.u64)
                                                             : static_cast<double>(val.i64));
            if (a.kind == AggSpec::Kind::Count) {
                g.aggI[ai] += 1;
            } else if (a.kind == AggSpec::Kind::Sum || a.kind == AggSpec::Kind::Avg) {
                if (a.outputType == ColType::I64) {
                    g.aggI[ai] += colTypeIsUnsignedInt(val.type)
                                      ? static_cast<int64_t>(val.u64)
                                      : val.i64;
                } else {
                    g.aggF[ai] += dv;
                }
            } else if (a.kind == AggSpec::Kind::Min) {
                if (g.aggCounts[ai] == 0 || compareScalar(val, g.aggExt[ai]) < 0) {
                    g.aggExt[ai] = std::move(val);
                }
            } else if (a.kind == AggSpec::Kind::Max) {
                if (g.aggCounts[ai] == 0 || compareScalar(val, g.aggExt[ai]) > 0) {
                    g.aggExt[ai] = std::move(val);
                }
            }
            ++g.aggCounts[ai];
        }
    }

    if (plan.groupKeys.empty() && groups.empty()) {
        GroupState g;
        g.aggF.assign(plan.aggregates.size(), 0.0);
        g.aggI.assign(plan.aggregates.size(), 0);
        g.aggCounts.assign(plan.aggregates.size(), 0);
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
                const uint64_t cnt = g.aggCounts[ai];
                row.push_back(ScalarValue::makeF64(
                    cnt > 0 ? (g.aggF[ai] / static_cast<double>(cnt)) : 0.0));
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
    if (!right) outTable->setSharedDict(left.dict);
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

std::shared_ptr<Table> finishPlanFromLeftMatchesImpl(
    const QueryPlan& plan, const TableSnapshot& left,
    const std::vector<RowLoc>& leftMatches, bool* outUsedIndex, bool useParallel) {
    const bool hasJoin = plan.join.kind != JoinSpec::Kind::None && plan.join.rightTable;
    const bool hasAgg = !plan.groupKeys.empty() || !plan.aggregates.empty();
    if (!hasJoin && hasAgg) {
        if (auto fast = tryExecuteFastGroupBy(plan, left, leftMatches, useParallel)) {
            return fast;
        }
    }
    const TableSnapshot* right = hasJoin ? plan.join.rightTable.get() : nullptr;

    std::vector<MatchPair> pairs;
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
        } else if (outUsedIndex) {
            *outUsedIndex = true;
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
                if (!passesAll(plan, plan.postJoinFilters, left, ll.chunkIdx, ll.rowIdx,
                               right, rl.chunkIdx, rl.rowIdx)) {
                    continue;
                }
                matchedAny = true;
                pairs.push_back(MatchPair{ll, rl});
            }
            if (!matchedAny && plan.join.kind == JoinSpec::Kind::Left) {
                if (passesAll(plan, plan.postJoinFilters, left, ll.chunkIdx, ll.rowIdx,
                              right, UINT32_MAX, UINT32_MAX)) {
                    pairs.push_back(MatchPair{ll, {UINT32_MAX, UINT32_MAX}});
                }
            }
        }
    }

    if (hasAgg) {
        return buildGroupedTable(plan, left, pairs);
    }
    return buildTableFromMatchPairs(plan, left, pairs);
}

}  // namespace bro::table
