#include "table/query_groupby.h"

#include "brass/codegen/kernel_jit.hpp"
#include "table/query_eval.h"

#include <algorithm>
#include <cstring>
#include <thread>

namespace bro::table {

namespace {

struct FastAggMeta {
    AggSpec::Kind kind = AggSpec::Kind::Count;
    bool countStar = false;
    uint32_t colIdx = 0;
    ColType valType = ColType::I64;
    ColType outType = ColType::I64;
};

struct AggAcc {
    double sumF = 0.0;
    int64_t sumI = 0;
    uint64_t nonNullCount = 0;
    int64_t extI = 0;
    uint64_t extU = 0;
    double extF = 0.0;
};

struct GroupKey {
    uint64_t raw = 0;
    bool isNull = false;
};

inline uint64_t hashGroupKey(uint64_t raw, bool isNull) noexcept {
    return isNull ? 0x9e3779b97f4a7c15ULL : mixHash64(raw);
}

class FlatGroupMap {
public:
    explicit FlatGroupMap(size_t numAggs) : numAggs_(numAggs) { rehash(64); }

    uint32_t findOrInsert(uint64_t raw, bool isNull, uint64_t h) {
        if ((keys_.size() + 1) * 10 >= slots_.size() * 7) {
            rehash(slots_.size() * 2);
        }
        const size_t mask = slots_.size() - 1;
        size_t pos = static_cast<size_t>(h) & mask;
        while (true) {
            const uint32_t idx = slots_[pos];
            if (idx == UINT32_MAX) {
                const uint32_t newIdx = static_cast<uint32_t>(keys_.size());
                slots_[pos] = newIdx;
                keys_.push_back(GroupKey{raw, isNull});
                rowCounts_.push_back(0);
                if (numAggs_ > 0) accs_.resize((static_cast<size_t>(newIdx) + 1) * numAggs_);
                return newIdx;
            }
            const GroupKey& k = keys_[idx];
            if (k.isNull == isNull && (isNull || k.raw == raw)) return idx;
            pos = (pos + 1) & mask;
        }
    }

    size_t groupCount() const noexcept { return keys_.size(); }
    const GroupKey& keyAt(size_t gi) const noexcept { return keys_[gi]; }
    uint64_t& rowCountAt(size_t gi) noexcept { return rowCounts_[gi]; }
    uint64_t rowCountAt(size_t gi) const noexcept { return rowCounts_[gi]; }
    AggAcc* accRow(size_t gi) noexcept { return accs_.data() + gi * numAggs_; }
    const AggAcc* accRow(size_t gi) const noexcept { return accs_.data() + gi * numAggs_; }

private:
    void rehash(size_t newCap) {
        std::vector<uint32_t> next(newCap, UINT32_MAX);
        const size_t mask = newCap - 1;
        for (uint32_t gi = 0; gi < keys_.size(); ++gi) {
            const uint64_t h = hashGroupKey(keys_[gi].raw, keys_[gi].isNull);
            size_t pos = static_cast<size_t>(h) & mask;
            while (next[pos] != UINT32_MAX) pos = (pos + 1) & mask;
            next[pos] = gi;
        }
        slots_.swap(next);
    }

    size_t numAggs_ = 0;
    std::vector<uint32_t> slots_;
    std::vector<GroupKey> keys_;
    std::vector<uint64_t> rowCounts_;
    std::vector<AggAcc> accs_;
};

bool isSupportedKeyType(ColType t) noexcept {
    switch (t) {
    case ColType::Dict:
    case ColType::Bool:
    case ColType::I32:
    case ColType::U32:
    case ColType::I64:
        return true;
    case ColType::F32:
    case ColType::F64:
    case ColType::String:
        return false;
    }
    return false;
}

bool isCellNull(const uint64_t* nullWords, uint32_t rowIdx) noexcept {
    if (!nullWords) return false;
    return ((nullWords[rowIdx >> 6] >> (rowIdx & 63u)) & 1ULL) != 0;
}

uint64_t readRawKey(ColType kt, const uint8_t* data, uint32_t rowIdx) noexcept {
    switch (kt) {
    case ColType::Dict:
    case ColType::U32: {
        uint32_t v = 0;
        std::memcpy(&v, data + static_cast<size_t>(rowIdx) * 4u, 4);
        return static_cast<uint64_t>(v);
    }
    case ColType::Bool:
        return data[rowIdx] != 0 ? 1ULL : 0ULL;
    case ColType::I32: {
        int32_t v = 0;
        std::memcpy(&v, data + static_cast<size_t>(rowIdx) * 4u, 4);
        return static_cast<uint64_t>(static_cast<int64_t>(v));
    }
    case ColType::I64: {
        int64_t v = 0;
        std::memcpy(&v, data + static_cast<size_t>(rowIdx) * 8u, 8);
        return static_cast<uint64_t>(v);
    }
    case ColType::F32:
    case ColType::F64:
    case ColType::String:
        break;
    }
    return 0;
}

void readNumericCell(ColType ct, const uint8_t* data, uint32_t rowIdx,
                     int64_t& outI, uint64_t& outU, double& outF) noexcept {
    switch (ct) {
    case ColType::I32: {
        int32_t v = 0;
        std::memcpy(&v, data + static_cast<size_t>(rowIdx) * 4u, 4);
        outI = v;
        outU = 0;
        outF = static_cast<double>(v);
        break;
    }
    case ColType::U32: {
        uint32_t v = 0;
        std::memcpy(&v, data + static_cast<size_t>(rowIdx) * 4u, 4);
        outI = static_cast<int64_t>(v);
        outU = v;
        outF = static_cast<double>(v);
        break;
    }
    case ColType::I64: {
        int64_t v = 0;
        std::memcpy(&v, data + static_cast<size_t>(rowIdx) * 8u, 8);
        outI = v;
        outU = 0;
        outF = static_cast<double>(v);
        break;
    }
    case ColType::F32: {
        float v = 0.0f;
        std::memcpy(&v, data + static_cast<size_t>(rowIdx) * 4u, 4);
        outF = static_cast<double>(v);
        outI = static_cast<int64_t>(v);
        outU = 0;
        break;
    }
    case ColType::F64: {
        double v = 0.0;
        std::memcpy(&v, data + static_cast<size_t>(rowIdx) * 8u, 8);
        outF = v;
        outI = static_cast<int64_t>(v);
        outU = 0;
        break;
    }
    case ColType::Bool: {
        uint8_t v = data[rowIdx] != 0 ? 1u : 0u;
        outI = v;
        outU = v;
        outF = static_cast<double>(v);
        break;
    }
    case ColType::Dict:
    case ColType::String:
        outI = 0;
        outU = 0;
        outF = 0.0;
        break;
    }
}

bool extLess(ColType ct, int64_t ia, uint64_t ua, double fa,
             int64_t ib, uint64_t ub, double fb) noexcept {
    if (colTypeIsFloat(ct)) return fa < fb;
    if (colTypeIsUnsignedInt(ct) || ct == ColType::Bool) return ua < ub;
    return ia < ib;
}

bool extGreater(ColType ct, int64_t ia, uint64_t ua, double fa,
                int64_t ib, uint64_t ub, double fb) noexcept {
    if (colTypeIsFloat(ct)) return fa > fb;
    if (colTypeIsUnsignedInt(ct) || ct == ColType::Bool) return ua > ub;
    return ia > ib;
}

ScalarValue keyToScalar(ColType kt, const GroupKey& gk, const TableSnapshot& snap) {
    if (gk.isNull) return ScalarValue::makeNull();
    switch (kt) {
    case ColType::Dict: {
        const uint32_t id = static_cast<uint32_t>(gk.raw);
        return ScalarValue::makeDict(id, snap.dict ? snap.dict->get(id) : std::string{});
    }
    case ColType::Bool:
        return ScalarValue::makeBool(gk.raw != 0);
    case ColType::I32:
        return ScalarValue::makeI32(static_cast<int32_t>(static_cast<int64_t>(gk.raw)));
    case ColType::U32:
        return ScalarValue::makeU32(static_cast<uint32_t>(gk.raw));
    case ColType::I64:
        return ScalarValue::makeI64(static_cast<int64_t>(gk.raw));
    case ColType::F32:
    case ColType::F64:
    case ColType::String:
        break;
    }
    return ScalarValue::makeNull();
}

ScalarValue extToScalar(ColType ct, const AggAcc& acc) {
    switch (ct) {
    case ColType::I32: return ScalarValue::makeI32(static_cast<int32_t>(acc.extI));
    case ColType::U32: return ScalarValue::makeU32(static_cast<uint32_t>(acc.extU));
    case ColType::I64: return ScalarValue::makeI64(acc.extI);
    case ColType::F32: return ScalarValue::makeF32(static_cast<float>(acc.extF));
    case ColType::F64: return ScalarValue::makeF64(acc.extF);
    case ColType::Bool: return ScalarValue::makeBool(acc.extU != 0);
    case ColType::Dict:
    case ColType::String:
        break;
    }
    return ScalarValue::makeNull();
}

void mergeInto(FlatGroupMap& dst, const FlatGroupMap& src,
               const std::vector<FastAggMeta>& aggs) {
    const size_t nAggs = aggs.size();
    for (size_t gi = 0; gi < src.groupCount(); ++gi) {
        const GroupKey& gk = src.keyAt(gi);
        const uint64_t h = hashGroupKey(gk.raw, gk.isNull);
        const uint32_t dIdx = dst.findOrInsert(gk.raw, gk.isNull, h);
        dst.rowCountAt(dIdx) += src.rowCountAt(gi);
        if (nAggs == 0) continue;

        AggAcc* dRow = dst.accRow(dIdx);
        const AggAcc* sRow = src.accRow(gi);
        for (size_t ai = 0; ai < nAggs; ++ai) {
            const FastAggMeta& m = aggs[ai];
            AggAcc& d = dRow[ai];
            const AggAcc& s = sRow[ai];
            if (m.kind == AggSpec::Kind::Count) {
                d.sumI += s.sumI;
                d.nonNullCount += s.nonNullCount;
            } else if (m.kind == AggSpec::Kind::Sum || m.kind == AggSpec::Kind::Avg) {
                d.sumI += s.sumI;
                d.sumF += s.sumF;
                d.nonNullCount += s.nonNullCount;
            } else if (m.kind == AggSpec::Kind::Min && s.nonNullCount > 0) {
                if (d.nonNullCount == 0 ||
                    extLess(m.valType, s.extI, s.extU, s.extF, d.extI, d.extU, d.extF)) {
                    d.extI = s.extI;
                    d.extU = s.extU;
                    d.extF = s.extF;
                }
                d.nonNullCount += s.nonNullCount;
            } else if (m.kind == AggSpec::Kind::Max && s.nonNullCount > 0) {
                if (d.nonNullCount == 0 ||
                    extGreater(m.valType, s.extI, s.extU, s.extF, d.extI, d.extU, d.extF)) {
                    d.extI = s.extI;
                    d.extU = s.extU;
                    d.extF = s.extF;
                }
                d.nonNullCount += s.nonNullCount;
            }
        }
    }
}

struct ChunkColView {
    const uint8_t* data = nullptr;
    const uint64_t* nullWords = nullptr;
};

void accumulateRange(FlatGroupMap& map,
                     const TableSnapshot& left,
                     const std::vector<RowLoc>& matches,
                     size_t beginIdx,
                     size_t endIdx,
                     uint32_t keyColIdx,
                     ColType keyType,
                     const std::vector<FastAggMeta>& aggs) {
    const size_t nAggs = aggs.size();
    uint32_t cachedChunk = UINT32_MAX;
    ChunkColView keyView{};
    std::vector<ChunkColView> aggViews(nAggs);

    auto refreshChunk = [&](uint32_t ci) {
        cachedChunk = ci;
        const auto& fc = left.chunks[ci];
        const auto& kc = fc.columns[keyColIdx];
        keyView.data = kc->rawData();
        keyView.nullWords = kc->hasNulls() ? kc->nullWordsData() : nullptr;
        for (size_t ai = 0; ai < nAggs; ++ai) {
            if (aggs[ai].countStar) continue;
            const auto& ac = fc.columns[aggs[ai].colIdx];
            aggViews[ai].data = ac->rawData();
            aggViews[ai].nullWords = ac->hasNulls() ? ac->nullWordsData() : nullptr;
        }
    };

    for (size_t i = beginIdx; i < endIdx; ++i) {
        const RowLoc loc = matches[i];
        if (loc.chunkIdx != cachedChunk) refreshChunk(loc.chunkIdx);

        const bool kNull = isCellNull(keyView.nullWords, loc.rowIdx);
        const uint64_t rawKey = kNull ? 0ULL : readRawKey(keyType, keyView.data, loc.rowIdx);
        const uint32_t gIdx = map.findOrInsert(rawKey, kNull, hashGroupKey(rawKey, kNull));
        map.rowCountAt(gIdx) += 1;
        if (nAggs == 0) continue;

        AggAcc* row = map.accRow(gIdx);
        for (size_t ai = 0; ai < nAggs; ++ai) {
            const FastAggMeta& m = aggs[ai];
            AggAcc& acc = row[ai];
            if (m.countStar) {
                acc.sumI += 1;
                acc.nonNullCount += 1;
                continue;
            }
            if (isCellNull(aggViews[ai].nullWords, loc.rowIdx)) continue;
            if (m.kind == AggSpec::Kind::Count) {
                acc.sumI += 1;
                acc.nonNullCount += 1;
                continue;
            }
            int64_t iv = 0;
            uint64_t uv = 0;
            double fv = 0.0;
            readNumericCell(m.valType, aggViews[ai].data, loc.rowIdx, iv, uv, fv);
            if (m.kind == AggSpec::Kind::Sum || m.kind == AggSpec::Kind::Avg) {
                if (m.outType == ColType::I64) acc.sumI += iv;
                else acc.sumF += fv;
            } else if (m.kind == AggSpec::Kind::Min) {
                if (acc.nonNullCount == 0 ||
                    extLess(m.valType, iv, uv, fv, acc.extI, acc.extU, acc.extF)) {
                    acc.extI = iv;
                    acc.extU = uv;
                    acc.extF = fv;
                }
            } else if (m.kind == AggSpec::Kind::Max) {
                if (acc.nonNullCount == 0 ||
                    extGreater(m.valType, iv, uv, fv, acc.extI, acc.extU, acc.extF)) {
                    acc.extI = iv;
                    acc.extU = uv;
                    acc.extF = fv;
                }
            }
            acc.nonNullCount += 1;
        }
    }
}

}  // namespace

std::shared_ptr<Table> tryExecuteFastGroupBy(const QueryPlan& plan,
                                             const TableSnapshot& left,
                                             const std::vector<RowLoc>& leftMatches,
                                             bool useParallel) {
    if (plan.join.kind != JoinSpec::Kind::None && plan.join.rightTable) return nullptr;
    if (plan.groupKeys.size() != 1) return nullptr;

    const ExprNode& gkNode = plan.exprs.nodes[plan.groupKeys[0]];
    if (gkNode.op != ExprOp::ColRef || gkNode.tableSide != 0 || gkNode.resolvedCol < 0) {
        return nullptr;
    }
    const ColType keyType = gkNode.valueType;
    if (!isSupportedKeyType(keyType)) return nullptr;
    const uint32_t keyColIdx = static_cast<uint32_t>(gkNode.resolvedCol);

    std::vector<FastAggMeta> aggs;
    aggs.reserve(plan.aggregates.size());
    for (const AggSpec& a : plan.aggregates) {
        FastAggMeta m;
        m.kind = a.kind;
        m.outType = a.outputType;
        if (a.kind == AggSpec::Kind::Count && a.expr == kInvalidExpr) {
            m.countStar = true;
            aggs.push_back(m);
            continue;
        }
        if (a.expr == kInvalidExpr || a.expr >= plan.exprs.nodes.size()) return nullptr;
        const ExprNode& an = plan.exprs.nodes[a.expr];
        if (an.op != ExprOp::ColRef || an.tableSide != 0 || an.resolvedCol < 0) return nullptr;
        m.colIdx = static_cast<uint32_t>(an.resolvedCol);
        m.valType = an.valueType;
        if (a.kind == AggSpec::Kind::Min || a.kind == AggSpec::Kind::Max) {
            // Dict integer IDs do not match string sort order, and String min/max allocates strings.
            if (m.valType == ColType::Dict || m.valType == ColType::String) return nullptr;
        } else if (a.kind == AggSpec::Kind::Sum || a.kind == AggSpec::Kind::Avg) {
            if (m.valType == ColType::String || m.valType == ColType::Dict) return nullptr;
        }
        aggs.push_back(m);
    }

    FlatGroupMap merged(aggs.size());
    if (useParallel && leftMatches.size() >= 8192) {
        const size_t hw = std::max<size_t>(2, std::thread::hardware_concurrency());
        const size_t nShards = std::min<size_t>(std::min<size_t>(hw, 16),
                                                (leftMatches.size() + 4095) / 4096);
        if (nShards >= 2) {
            std::vector<FlatGroupMap> shards;
            shards.reserve(nShards);
            for (size_t s = 0; s < nShards; ++s) shards.emplace_back(aggs.size());

            brass::codegen::parallel_for(
                0, static_cast<int64_t>(nShards),
                [&](int64_t b, int64_t e) {
                    for (int64_t si = b; si < e; ++si) {
                        const size_t sIdx = static_cast<size_t>(si);
                        const size_t start = (leftMatches.size() * sIdx) / nShards;
                        const size_t end = (leftMatches.size() * (sIdx + 1)) / nShards;
                        accumulateRange(shards[sIdx], left, leftMatches, start, end,
                                        keyColIdx, keyType, aggs);
                    }
                },
                1);

            merged = std::move(shards[0]);
            for (size_t s = 1; s < nShards; ++s) {
                mergeInto(merged, shards[s], aggs);
            }
        } else {
            accumulateRange(merged, left, leftMatches, 0, leftMatches.size(),
                            keyColIdx, keyType, aggs);
        }
    } else {
        accumulateRange(merged, left, leftMatches, 0, leftMatches.size(),
                        keyColIdx, keyType, aggs);
    }

    std::vector<ColumnDef> outSchema;
    outSchema.reserve(1 + aggs.size());
    std::string keyAlias = !plan.groupAliases.empty() ? plan.groupAliases[0] : gkNode.colName;
    outSchema.push_back(ColumnDef{std::move(keyAlias), keyType});
    for (size_t ai = 0; ai < plan.aggregates.size(); ++ai) {
        std::string name = !plan.aggregates[ai].alias.empty()
                               ? plan.aggregates[ai].alias
                               : ("agg_" + std::to_string(ai));
        outSchema.push_back(ColumnDef{std::move(name), plan.aggregates[ai].outputType});
    }

    const size_t G = merged.groupCount();
    std::vector<std::vector<ScalarValue>> outRows(G);
    for (size_t gi = 0; gi < G; ++gi) {
        auto& row = outRows[gi];
        row.reserve(outSchema.size());
        row.push_back(keyToScalar(keyType, merged.keyAt(gi), left));
        const AggAcc* accRow = merged.accRow(gi);
        for (size_t ai = 0; ai < aggs.size(); ++ai) {
            const FastAggMeta& m = aggs[ai];
            const AggAcc& acc = accRow[ai];
            if (m.kind == AggSpec::Kind::Count) {
                row.push_back(ScalarValue::makeI64(acc.sumI));
            } else if (m.kind == AggSpec::Kind::Sum) {
                row.push_back(m.outType == ColType::I64
                                  ? ScalarValue::makeI64(acc.sumI)
                                  : ScalarValue::makeF64(acc.sumF));
            } else if (m.kind == AggSpec::Kind::Avg) {
                const double avg = acc.nonNullCount > 0
                                       ? (acc.sumF / static_cast<double>(acc.nonNullCount))
                                       : 0.0;
                row.push_back(ScalarValue::makeF64(avg));
            } else if (acc.nonNullCount == 0) {
                row.push_back(ScalarValue::makeNull());
            } else {
                row.push_back(extToScalar(m.valType, acc));
            }
        }
    }

    if (!plan.orderBy.empty() && outRows.size() > 1) {
        auto findOutCol = [&](uint32_t exprId) -> int {
            const ExprNode& en = plan.exprs.nodes[exprId];
            if (en.op != ExprOp::ColRef) return -1;
            for (size_t c = 0; c < outSchema.size(); ++c) {
                if (outSchema[c].name == en.colName) return static_cast<int>(c);
            }
            return -1;
        };
        std::vector<size_t> perm(G);
        for (size_t i = 0; i < G; ++i) perm[i] = i;
        std::stable_sort(perm.begin(), perm.end(), [&](size_t a, size_t b) {
            for (const OrderByItem& ob : plan.orderBy) {
                const int oc = findOutCol(ob.expr);
                if (oc < 0) continue;
                const int cmp = compareScalar(outRows[a][oc], outRows[b][oc]);
                if (cmp != 0) return ob.descending ? (cmp > 0) : (cmp < 0);
            }
            return a < b;
        });
        std::vector<std::vector<ScalarValue>> sorted;
        sorted.reserve(G);
        for (size_t idx : perm) sorted.push_back(std::move(outRows[idx]));
        outRows = std::move(sorted);
    }

    auto outTable = std::make_shared<Table>(std::move(outSchema));
    outTable->setSharedDict(left.dict);
    const size_t start = static_cast<size_t>(std::max<int64_t>(0, plan.offset));
    const size_t end = (plan.limit >= 0)
                           ? std::min(outRows.size(), start + static_cast<size_t>(plan.limit))
                           : outRows.size();
    for (size_t i = start; i < end; ++i) {
        outTable->appendRow(outRows[i]);
    }
    return outTable;
}

}  // namespace bro::table
