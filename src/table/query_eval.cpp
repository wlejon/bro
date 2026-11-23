#include "table/query_eval.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace bro::table {

namespace {

double asDouble(const ScalarValue& v) {
    if (v.isNull) return 0.0;
    switch (v.type) {
    case ColType::F32:
    case ColType::F64:
        return v.f64;
    case ColType::U32:
    case ColType::Bool:
    case ColType::Dict:
        return static_cast<double>(v.u64);
    case ColType::I32:
    case ColType::I64:
        return static_cast<double>(v.i64);
    case ColType::String:
        return v.str.empty() ? 0.0 : 1.0;
    }
    return 0.0;
}

int64_t asI64(const ScalarValue& v) {
    if (v.isNull) return 0;
    switch (v.type) {
    case ColType::F32:
    case ColType::F64:
        return static_cast<int64_t>(v.f64);
    case ColType::U32:
    case ColType::Bool:
    case ColType::Dict:
        return static_cast<int64_t>(v.u64);
    case ColType::I32:
    case ColType::I64:
        return v.i64;
    case ColType::String:
        return static_cast<int64_t>(v.str.size());
    }
    return 0;
}

uint64_t asU64(const ScalarValue& v) {
    if (v.isNull) return 0;
    switch (v.type) {
    case ColType::F32:
    case ColType::F64:
        return static_cast<uint64_t>(static_cast<int64_t>(v.f64));
    case ColType::U32:
    case ColType::Bool:
    case ColType::Dict:
        return v.u64;
    case ColType::I32:
    case ColType::I64:
        return static_cast<uint64_t>(v.i64);
    case ColType::String:
        return static_cast<uint64_t>(v.str.size());
    }
    return 0;
}

ScalarValue wrapTyped(ColType target, int64_t iv, uint64_t uv, double fv) {
    switch (target) {
    case ColType::I32: return ScalarValue::makeI32(static_cast<int32_t>(iv));
    case ColType::U32: return ScalarValue::makeU32(static_cast<uint32_t>(uv));
    case ColType::I64: return ScalarValue::makeI64(iv);
    case ColType::F32: return ScalarValue::makeF32(static_cast<float>(fv));
    case ColType::F64: return ScalarValue::makeF64(fv);
    case ColType::Bool: return ScalarValue::makeBool(iv != 0 || uv != 0 || fv != 0.0);
    case ColType::Dict: return ScalarValue::makeDict(static_cast<uint32_t>(uv));
    case ColType::String: return ScalarValue::makeNull();
    }
    return ScalarValue::makeNull();
}

std::string resolveText(const ExprNode& n, const ScalarValue& v, const TableSnapshot& left,
                        const TableSnapshot* right) {
    if (v.type == ColType::Dict) {
        if (!v.str.empty()) return v.str;
        const TableSnapshot& owner = (n.tableSide == 1 && right) ? *right : left;
        return owner.dict->get(static_cast<uint32_t>(v.u64));
    }
    return v.str;
}

}  // namespace

bool scalarTruthy(const ScalarValue& v) {
    if (v.isNull) return false;
    switch (v.type) {
    case ColType::F32:
    case ColType::F64:
        return v.f64 != 0.0 && !std::isnan(v.f64);
    case ColType::U32:
    case ColType::Bool:
    case ColType::Dict:
        return v.u64 != 0;
    case ColType::I32:
    case ColType::I64:
        return v.i64 != 0;
    case ColType::String:
        return !v.str.empty();
    }
    return false;
}

int compareScalar(const ScalarValue& a, const ScalarValue& b) {
    if (a.isNull && b.isNull) return 0;
    if (a.isNull) return -1;
    if (b.isNull) return 1;

    if (a.type == ColType::String || b.type == ColType::String ||
        (a.type == ColType::Dict && b.type == ColType::Dict)) {
        if (a.type == ColType::Dict && b.type == ColType::Dict && a.u64 == b.u64) return 0;
        if (a.str < b.str) return -1;
        if (a.str > b.str) return 1;
        return 0;
    }

    if (colTypeIsFloat(a.type) || colTypeIsFloat(b.type)) {
        const double da = asDouble(a);
        const double db = asDouble(b);
        if (da < db) return -1;
        if (da > db) return 1;
        return 0;
    }

    if (a.type == ColType::U32 && b.type == ColType::U32) {
        if (a.u64 < b.u64) return -1;
        if (a.u64 > b.u64) return 1;
        return 0;
    }
    if (a.type == ColType::Bool && b.type == ColType::Bool) {
        if (a.u64 < b.u64) return -1;
        if (a.u64 > b.u64) return 1;
        return 0;
    }
    const int64_t ia = asI64(a);
    const int64_t ib = asI64(b);
    if (ia < ib) return -1;
    if (ia > ib) return 1;
    return 0;
}

uint64_t hashScalarKey(ColType type, const ScalarValue& v) {
    if (v.isNull) return 0x9e3779b97f4a7c15ULL;
    switch (type) {
    case ColType::String:
        return hashBytes(v.str.data(), v.str.size());
    case ColType::F32: {
        float f = static_cast<float>(asDouble(v));
        if (f == 0.0f) f = 0.0f;
        uint32_t bits = 0;
        std::memcpy(&bits, &f, 4);
        return mixHash64(bits);
    }
    case ColType::F64:
        return mixHash64(canonicalFloatKey(asDouble(v)));
    case ColType::U32:
    case ColType::Bool:
    case ColType::Dict:
        return mixHash64(asU64(v));
    case ColType::I32:
        return mixHash64(static_cast<uint32_t>(static_cast<int32_t>(asI64(v))));
    case ColType::I64:
        return mixHash64(static_cast<uint64_t>(asI64(v)));
    }
    return 0;
}

ScalarValue evalExpr(const ExprArena& arena, uint32_t exprId,
                     const TableSnapshot& left, uint32_t lChunk, uint32_t lRow,
                     const TableSnapshot* right, uint32_t rChunk, uint32_t rRow) {
    if (exprId == kInvalidExpr || exprId >= arena.nodes.size()) {
        return ScalarValue::makeNull();
    }
    const ExprNode& n = arena.nodes[exprId];
    switch (n.op) {
    case ExprOp::ColRef: {
        if (n.resolvedCol < 0) return ScalarValue::makeNull();
        if (n.tableSide == 1) {
            if (!right || rChunk == UINT32_MAX) return ScalarValue::makeNull();
            return right->getCell(static_cast<size_t>(n.resolvedCol), rChunk, rRow);
        }
        return left.getCell(static_cast<size_t>(n.resolvedCol), lChunk, lRow);
    }
    case ExprOp::Const:
        return n.literal;

    case ExprOp::Neg: {
        ScalarValue a = evalExpr(arena, n.lhs, left, lChunk, lRow, right, rChunk, rRow);
        if (colTypeIsFloat(n.valueType)) return wrapTyped(n.valueType, 0, 0, -asDouble(a));
        const int64_t iv = -asI64(a);
        return wrapTyped(n.valueType, iv, static_cast<uint64_t>(iv), static_cast<double>(iv));
    }
    case ExprOp::Abs: {
        ScalarValue a = evalExpr(arena, n.lhs, left, lChunk, lRow, right, rChunk, rRow);
        if (colTypeIsFloat(n.valueType)) return wrapTyped(n.valueType, 0, 0, std::fabs(asDouble(a)));
        const int64_t iv = asI64(a);
        const int64_t av = iv < 0 ? -iv : iv;
        return wrapTyped(n.valueType, av, static_cast<uint64_t>(av), static_cast<double>(av));
    }
    case ExprOp::Sqrt: {
        ScalarValue a = evalExpr(arena, n.lhs, left, lChunk, lRow, right, rChunk, rRow);
        return wrapTyped(n.valueType, 0, 0, std::sqrt(asDouble(a)));
    }
    case ExprOp::Floor: {
        ScalarValue a = evalExpr(arena, n.lhs, left, lChunk, lRow, right, rChunk, rRow);
        return wrapTyped(n.valueType, 0, 0, std::floor(asDouble(a)));
    }
    case ExprOp::Ceil: {
        ScalarValue a = evalExpr(arena, n.lhs, left, lChunk, lRow, right, rChunk, rRow);
        return wrapTyped(n.valueType, 0, 0, std::ceil(asDouble(a)));
    }
    case ExprOp::BitNot: {
        ScalarValue a = evalExpr(arena, n.lhs, left, lChunk, lRow, right, rChunk, rRow);
        const int64_t iv = ~asI64(a);
        return wrapTyped(n.valueType, iv, ~asU64(a), static_cast<double>(iv));
    }
    case ExprOp::Not: {
        ScalarValue a = evalExpr(arena, n.lhs, left, lChunk, lRow, right, rChunk, rRow);
        return ScalarValue::makeBool(!scalarTruthy(a));
    }
    case ExprOp::StrLen: {
        ScalarValue a = evalExpr(arena, n.lhs, left, lChunk, lRow, right, rChunk, rRow);
        return ScalarValue::makeI32(static_cast<int32_t>(a.str.size()));
    }

    case ExprOp::Add:
    case ExprOp::Sub:
    case ExprOp::Mul:
    case ExprOp::Div:
    case ExprOp::Mod: {
        ScalarValue a = evalExpr(arena, n.lhs, left, lChunk, lRow, right, rChunk, rRow);
        ScalarValue b = evalExpr(arena, n.rhs, left, lChunk, lRow, right, rChunk, rRow);
        if (colTypeIsFloat(n.valueType)) {
            const double da = asDouble(a);
            const double db = asDouble(b);
            double res = 0.0;
            if (n.op == ExprOp::Add) res = da + db;
            else if (n.op == ExprOp::Sub) res = da - db;
            else if (n.op == ExprOp::Mul) res = da * db;
            else if (n.op == ExprOp::Div) res = da / db;
            else res = std::fmod(da, db);
            return wrapTyped(n.valueType, 0, 0, res);
        }
        if (n.valueType == ColType::U32) {
            const uint32_t ua = static_cast<uint32_t>(asU64(a));
            const uint32_t ub = static_cast<uint32_t>(asU64(b));
            uint32_t res = 0;
            if (n.op == ExprOp::Add) res = ua + ub;
            else if (n.op == ExprOp::Sub) res = ua - ub;
            else if (n.op == ExprOp::Mul) res = ua * ub;
            else if (n.op == ExprOp::Div) res = (ub == 0) ? 0u : (ua / ub);
            else res = (ub == 0) ? 0u : (ua % ub);
            return ScalarValue::makeU32(res);
        }
        if (n.valueType == ColType::I32) {
            const int32_t ia = static_cast<int32_t>(asI64(a));
            const int32_t ib = static_cast<int32_t>(asI64(b));
            int32_t res = 0;
            if (n.op == ExprOp::Add) res = static_cast<int32_t>(static_cast<uint32_t>(ia) + static_cast<uint32_t>(ib));
            else if (n.op == ExprOp::Sub) res = static_cast<int32_t>(static_cast<uint32_t>(ia) - static_cast<uint32_t>(ib));
            else if (n.op == ExprOp::Mul) res = static_cast<int32_t>(static_cast<uint32_t>(ia) * static_cast<uint32_t>(ib));
            else if (n.op == ExprOp::Div) res = (ib == 0) ? 0 : (ia == INT32_MIN && ib == -1 ? INT32_MIN : ia / ib);
            else res = (ib == 0) ? 0 : (ia == INT32_MIN && ib == -1 ? 0 : ia % ib);
            return ScalarValue::makeI32(res);
        }
        const int64_t ia = asI64(a);
        const int64_t ib = asI64(b);
        int64_t res = 0;
        if (n.op == ExprOp::Add) res = ia + ib;
        else if (n.op == ExprOp::Sub) res = ia - ib;
        else if (n.op == ExprOp::Mul) res = ia * ib;
        else if (n.op == ExprOp::Div) res = (ib == 0) ? 0 : (ia == INT64_MIN && ib == -1 ? INT64_MIN : ia / ib);
        else res = (ib == 0) ? 0 : (ia == INT64_MIN && ib == -1 ? 0 : ia % ib);
        return ScalarValue::makeI64(res);
    }

    case ExprOp::BitAnd:
    case ExprOp::BitOr:
    case ExprOp::BitXor:
    case ExprOp::Shl:
    case ExprOp::Shr:
    case ExprOp::UShr: {
        ScalarValue a = evalExpr(arena, n.lhs, left, lChunk, lRow, right, rChunk, rRow);
        ScalarValue b = evalExpr(arena, n.rhs, left, lChunk, lRow, right, rChunk, rRow);
        if (n.valueType == ColType::I64) {
            const int64_t ia = asI64(a);
            const uint32_t shift = static_cast<uint32_t>(asU64(b)) & 63u;
            int64_t res = 0;
            if (n.op == ExprOp::BitAnd) res = ia & asI64(b);
            else if (n.op == ExprOp::BitOr) res = ia | asI64(b);
            else if (n.op == ExprOp::BitXor) res = ia ^ asI64(b);
            else if (n.op == ExprOp::Shl) res = static_cast<int64_t>(static_cast<uint64_t>(ia) << shift);
            else if (n.op == ExprOp::Shr) res = ia >> shift;
            else res = static_cast<int64_t>(static_cast<uint64_t>(ia) >> shift);
            return ScalarValue::makeI64(res);
        }
        const uint32_t ua = static_cast<uint32_t>(asI64(a));
        const uint32_t ub = static_cast<uint32_t>(asI64(b));
        const uint32_t shift = ub & 31u;
        uint32_t res = 0;
        if (n.op == ExprOp::BitAnd) res = ua & ub;
        else if (n.op == ExprOp::BitOr) res = ua | ub;
        else if (n.op == ExprOp::BitXor) res = ua ^ ub;
        else if (n.op == ExprOp::Shl) res = ua << shift;
        else if (n.op == ExprOp::Shr) res = static_cast<uint32_t>(static_cast<int32_t>(ua) >> shift);
        else res = ua >> shift;
        return n.valueType == ColType::U32
                   ? ScalarValue::makeU32(res)
                   : ScalarValue::makeI32(static_cast<int32_t>(res));
    }

    case ExprOp::Eq:
    case ExprOp::Ne:
    case ExprOp::Lt:
    case ExprOp::Le:
    case ExprOp::Gt:
    case ExprOp::Ge: {
        const ExprNode& ln = arena.nodes[n.lhs];
        const ExprNode& rn = arena.nodes[n.rhs];
        if ((n.op == ExprOp::Eq || n.op == ExprOp::Ne) &&
            ln.valueType == ColType::Dict && rn.op == ExprOp::Const &&
            rn.literal.type == ColType::String) {
            ScalarValue a = evalExpr(arena, n.lhs, left, lChunk, lRow, right, rChunk, rRow);
            const bool eq = (!a.isNull && rn.cachedDictId != kInvalidDictId &&
                             a.u64 == rn.cachedDictId);
            return ScalarValue::makeBool(n.op == ExprOp::Eq ? eq : !eq);
        }
        if ((n.op == ExprOp::Eq || n.op == ExprOp::Ne) &&
            rn.valueType == ColType::Dict && ln.op == ExprOp::Const &&
            ln.literal.type == ColType::String) {
            ScalarValue b = evalExpr(arena, n.rhs, left, lChunk, lRow, right, rChunk, rRow);
            const bool eq = (!b.isNull && ln.cachedDictId != kInvalidDictId &&
                             b.u64 == ln.cachedDictId);
            return ScalarValue::makeBool(n.op == ExprOp::Eq ? eq : !eq);
        }
        ScalarValue a = evalExpr(arena, n.lhs, left, lChunk, lRow, right, rChunk, rRow);
        ScalarValue b = evalExpr(arena, n.rhs, left, lChunk, lRow, right, rChunk, rRow);
        if (colTypeIsFloat(a.type) || colTypeIsFloat(b.type)) {
            const double da = asDouble(a);
            const double db = asDouble(b);
            if (std::isnan(da) || std::isnan(db)) {
                return ScalarValue::makeBool(n.op == ExprOp::Ne);
            }
        }
        const int cmp = compareScalar(a, b);
        switch (n.op) {
        case ExprOp::Eq: return ScalarValue::makeBool(cmp == 0);
        case ExprOp::Ne: return ScalarValue::makeBool(cmp != 0);
        case ExprOp::Lt: return ScalarValue::makeBool(cmp < 0);
        case ExprOp::Le: return ScalarValue::makeBool(cmp <= 0);
        case ExprOp::Gt: return ScalarValue::makeBool(cmp > 0);
        case ExprOp::Ge: return ScalarValue::makeBool(cmp >= 0);
        default: break;
        }
        break;
    }

    case ExprOp::And: {
        ScalarValue a = evalExpr(arena, n.lhs, left, lChunk, lRow, right, rChunk, rRow);
        if (!scalarTruthy(a)) return ScalarValue::makeBool(false);
        ScalarValue b = evalExpr(arena, n.rhs, left, lChunk, lRow, right, rChunk, rRow);
        return ScalarValue::makeBool(scalarTruthy(b));
    }
    case ExprOp::Or: {
        ScalarValue a = evalExpr(arena, n.lhs, left, lChunk, lRow, right, rChunk, rRow);
        if (scalarTruthy(a)) return ScalarValue::makeBool(true);
        ScalarValue b = evalExpr(arena, n.rhs, left, lChunk, lRow, right, rChunk, rRow);
        return ScalarValue::makeBool(scalarTruthy(b));
    }

    case ExprOp::StrEq:
    case ExprOp::StrNe:
    case ExprOp::StrContains:
    case ExprOp::StrStartsWith:
    case ExprOp::StrEndsWith: {
        const ExprNode& ln = arena.nodes[n.lhs];
        const ExprNode& rn = arena.nodes[n.rhs];
        ScalarValue a = evalExpr(arena, n.lhs, left, lChunk, lRow, right, rChunk, rRow);
        if ((n.op == ExprOp::StrEq || n.op == ExprOp::StrNe) &&
            ln.valueType == ColType::Dict && rn.op == ExprOp::Const &&
            rn.literal.type == ColType::String) {
            const bool eq = (!a.isNull && rn.cachedDictId != kInvalidDictId &&
                             a.u64 == rn.cachedDictId);
            return ScalarValue::makeBool(n.op == ExprOp::StrEq ? eq : !eq);
        }
        ScalarValue b = evalExpr(arena, n.rhs, left, lChunk, lRow, right, rChunk, rRow);
        const std::string sa = resolveText(ln, a, left, right);
        const std::string sb = resolveText(rn, b, left, right);
        bool match = false;
        if (n.op == ExprOp::StrEq) match = (sa == sb);
        else if (n.op == ExprOp::StrNe) match = (sa != sb);
        else if (n.op == ExprOp::StrContains) match = (sa.find(sb) != std::string::npos);
        else if (n.op == ExprOp::StrStartsWith) match = (sa.rfind(sb, 0) == 0);
        else match = (sa.size() >= sb.size() && sa.compare(sa.size() - sb.size(), sb.size(), sb) == 0);
        return ScalarValue::makeBool(match);
    }

    case ExprOp::Cond: {
        ScalarValue c = evalExpr(arena, n.lhs, left, lChunk, lRow, right, rChunk, rRow);
        return evalExpr(arena, scalarTruthy(c) ? n.rhs : n.third,
                        left, lChunk, lRow, right, rChunk, rRow);
    }
    }
    return ScalarValue::makeNull();
}

namespace {

bool evalFilters(const QueryPlan& plan, const std::vector<uint32_t>& flt,
                 const TableSnapshot& left, uint32_t lc, uint32_t lr,
                 const TableSnapshot* right, uint32_t rc, uint32_t rr) {
    for (uint32_t f : flt) {
        if (!scalarTruthy(evalExpr(plan.exprs, f, left, lc, lr, right, rc, rr))) {
            return false;
        }
    }
    return true;
}

struct MatchPair {
    RowLoc left{};
    RowLoc right{UINT32_MAX, UINT32_MAX};
};

}  // namespace

std::shared_ptr<Table> executePlanReference(const TableSnapshot& left, const QueryPlan& inPlan) {
    QueryPlan plan = inPlan;
    if (!plan.bound) {
        std::string err;
        if (!bindQueryPlan(plan, left, err)) return std::make_shared<Table>();
    }

    std::vector<MatchPair> pairs;
    const bool hasJoin = plan.join.kind != JoinSpec::Kind::None && plan.join.rightTable;
    const TableSnapshot* right = hasJoin ? plan.join.rightTable.get() : nullptr;

    // Step 1: Scan left table with pre-join filters
    std::vector<RowLoc> leftMatches;
    for (uint32_t ci = 0; ci < left.chunks.size(); ++ci) {
        const auto& ch = left.chunks[ci];
        for (uint32_t r = 0; r < ch.rowCount; ++r) {
            if (!ch.isAlive(r)) continue;
            if (evalFilters(plan, plan.filters, left, ci, r, nullptr, 0, 0)) {
                leftMatches.push_back(RowLoc{ci, r});
            }
        }
    }

    // Step 2: Join if present
    if (!hasJoin) {
        pairs.reserve(leftMatches.size());
        for (RowLoc l : leftMatches) pairs.push_back(MatchPair{l, {UINT32_MAX, UINT32_MAX}});
    } else {
        std::unordered_map<uint64_t, std::vector<RowLoc>> rightHash;
        for (uint32_t rci = 0; rci < right->chunks.size(); ++rci) {
            const auto& rch = right->chunks[rci];
            for (uint32_t rr = 0; rr < rch.rowCount; ++rr) {
                if (!rch.isAlive(rr)) continue;
                uint64_t h = 0x517cc1b727220a95ULL;
                for (size_t k = 0; k < plan.join.rightKeyCols.size(); ++k) {
                    ScalarValue kv = right->getCell(plan.join.rightKeyCols[k], rci, rr);
                    h = combineHash64(h, hashScalarKey(plan.join.keyTypes[k], kv));
                }
                rightHash[h].push_back(RowLoc{rci, rr});
            }
        }

        for (RowLoc ll : leftMatches) {
            uint64_t h = 0x517cc1b727220a95ULL;
            std::vector<ScalarValue> lKeys(plan.join.leftKeyCols.size());
            for (size_t k = 0; k < plan.join.leftKeyCols.size(); ++k) {
                lKeys[k] = left.getCell(plan.join.leftKeyCols[k], ll.chunkIdx, ll.rowIdx);
                h = combineHash64(h, hashScalarKey(plan.join.keyTypes[k], lKeys[k]));
            }
            bool matchedAny = false;
            auto it = rightHash.find(h);
            if (it != rightHash.end()) {
                for (RowLoc rl : it->second) {
                    bool eq = true;
                    for (size_t k = 0; k < plan.join.rightKeyCols.size(); ++k) {
                        ScalarValue rk = right->getCell(plan.join.rightKeyCols[k], rl.chunkIdx, rl.rowIdx);
                        if (compareScalar(lKeys[k], rk) != 0) {
                            eq = false;
                            break;
                        }
                    }
                    if (!eq) continue;
                    if (!evalFilters(plan, plan.postJoinFilters, left, ll.chunkIdx, ll.rowIdx,
                                     right, rl.chunkIdx, rl.rowIdx)) {
                        continue;
                    }
                    matchedAny = true;
                    pairs.push_back(MatchPair{ll, rl});
                }
            }
            if (!matchedAny && plan.join.kind == JoinSpec::Kind::Left) {
                if (evalFilters(plan, plan.postJoinFilters, left, ll.chunkIdx, ll.rowIdx,
                                right, UINT32_MAX, UINT32_MAX)) {
                    pairs.push_back(MatchPair{ll, {UINT32_MAX, UINT32_MAX}});
                }
            }
        }
    }

    // Step 3: Build output schema & materialized rows (grouped or ungrouped)
    std::vector<ColumnDef> outSchema;
    std::vector<std::vector<ScalarValue>> outRows;

    if (!plan.groupKeys.empty() || !plan.aggregates.empty()) {
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

        auto ensureEmptyGlobalGroup = [&]() {
            if (plan.groupKeys.empty() && groups.empty()) {
                GroupState g;
                g.aggF.assign(plan.aggregates.size(), 0.0);
                g.aggI.assign(plan.aggregates.size(), 0);
                g.aggExt.assign(plan.aggregates.size(), ScalarValue::makeNull());
                groups.push_back(std::move(g));
            }
        };

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
                    if (match) {
                        gIdx = cand;
                        break;
                    }
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
                if (a.kind == AggSpec::Kind::Sum || a.kind == AggSpec::Kind::Avg) {
                    if (a.outputType == ColType::I64) g.aggI[ai] += asI64(val);
                    else g.aggF[ai] += asDouble(val);
                } else if (a.kind == AggSpec::Kind::Min) {
                    if (firstInGroup || compareScalar(val, g.aggExt[ai]) < 0) g.aggExt[ai] = std::move(val);
                } else if (a.kind == AggSpec::Kind::Max) {
                    if (firstInGroup || compareScalar(val, g.aggExt[ai]) > 0) g.aggExt[ai] = std::move(val);
                }
            }
        }
        ensureEmptyGlobalGroup();

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
                    row.push_back(a.outputType == ColType::I64
                                      ? ScalarValue::makeI64(g.aggI[ai])
                                      : ScalarValue::makeF64(g.aggF[ai]));
                } else if (a.kind == AggSpec::Kind::Avg) {
                    const double avg = g.count > 0 ? (g.aggF[ai] / static_cast<double>(g.count)) : 0.0;
                    row.push_back(ScalarValue::makeF64(avg));
                } else {
                    row.push_back(g.aggExt[ai]);
                }
            }
            outRows.push_back(std::move(row));
        }

        // OrderBy over post-agg columns if requested
        if (!plan.orderBy.empty()) {
            std::vector<size_t> orderIdx(outRows.size());
            for (size_t i = 0; i < orderIdx.size(); ++i) orderIdx[i] = i;
            auto findOutCol = [&](uint32_t exprId) -> int {
                const ExprNode& en = plan.exprs.nodes[exprId];
                if (en.op != ExprOp::ColRef) return -1;
                for (size_t c = 0; c < outSchema.size(); ++c) {
                    if (outSchema[c].name == en.colName) return static_cast<int>(c);
                }
                return -1;
            };
            std::stable_sort(orderIdx.begin(), orderIdx.end(), [&](size_t a, size_t b) {
                for (const OrderByItem& ob : plan.orderBy) {
                    int oc = findOutCol(ob.expr);
                    if (oc < 0) continue;
                    const int cmp = compareScalar(outRows[a][oc], outRows[b][oc]);
                    if (cmp != 0) return ob.descending ? (cmp > 0) : (cmp < 0);
                }
                return false;
            });
            std::vector<std::vector<ScalarValue>> sorted;
            sorted.reserve(outRows.size());
            for (size_t idx : orderIdx) sorted.push_back(std::move(outRows[idx]));
            outRows = std::move(sorted);
        }
    } else {
        // Optional OrderBy before projection/slice
        if (!plan.orderBy.empty()) {
            std::vector<std::vector<ScalarValue>> keys(pairs.size());
            for (size_t i = 0; i < pairs.size(); ++i) {
                keys[i].reserve(plan.orderBy.size());
                for (const OrderByItem& ob : plan.orderBy) {
                    keys[i].push_back(evalExpr(plan.exprs, ob.expr,
                                               left, pairs[i].left.chunkIdx, pairs[i].left.rowIdx,
                                               right, pairs[i].right.chunkIdx, pairs[i].right.rowIdx));
                }
            }
            std::vector<size_t> perm(pairs.size());
            for (size_t i = 0; i < perm.size(); ++i) perm[i] = i;
            std::stable_sort(perm.begin(), perm.end(), [&](size_t a, size_t b) {
                for (size_t k = 0; k < plan.orderBy.size(); ++k) {
                    const int cmp = compareScalar(keys[a][k], keys[b][k]);
                    if (cmp != 0) return plan.orderBy[k].descending ? (cmp > 0) : (cmp < 0);
                }
                return false;
            });
            std::vector<MatchPair> sortedPairs;
            sortedPairs.reserve(pairs.size());
            for (size_t idx : perm) sortedPairs.push_back(pairs[idx]);
            pairs = std::move(sortedPairs);
        }

        const size_t start = static_cast<size_t>(std::max<int64_t>(0, plan.offset));
        const size_t end = (plan.limit >= 0)
                               ? std::min(pairs.size(), start + static_cast<size_t>(plan.limit))
                               : pairs.size();

        for (const SelectItem& si : plan.selectItems) {
            outSchema.push_back(ColumnDef{si.alias, si.outputType});
        }
        if (start < end) {
            outRows.reserve(end - start);
            for (size_t i = start; i < end; ++i) {
                std::vector<ScalarValue> row;
                row.reserve(plan.selectItems.size());
                const MatchPair& p = pairs[i];
                for (const SelectItem& si : plan.selectItems) {
                    row.push_back(evalExpr(plan.exprs, si.expr,
                                           left, p.left.chunkIdx, p.left.rowIdx,
                                           right, p.right.chunkIdx, p.right.rowIdx));
                }
                outRows.push_back(std::move(row));
            }
        }
    }

    auto outTable = std::make_shared<Table>(std::move(outSchema));
    if (!plan.groupKeys.empty() || !plan.aggregates.empty()) {
        const size_t start = static_cast<size_t>(std::max<int64_t>(0, plan.offset));
        const size_t end = (plan.limit >= 0)
                               ? std::min(outRows.size(), start + static_cast<size_t>(plan.limit))
                               : outRows.size();
        for (size_t i = start; i < end; ++i) {
            outTable->appendRow(outRows[i]);
        }
    } else {
        for (const auto& row : outRows) {
            outTable->appendRow(row);
        }
    }
    return outTable;
}

size_t applyUpdateReference(Table& table, const QueryPlan& inPlan) {
    auto snap = table.snapshot();
    QueryPlan plan = inPlan;
    if (!plan.bound) {
        std::string err;
        if (!bindQueryPlan(plan, *snap, err)) return 0;
    }
    size_t updated = 0;
    for (uint32_t ci = 0; ci < snap->chunks.size(); ++ci) {
        const auto& ch = snap->chunks[ci];
        for (uint32_t r = 0; r < ch.rowCount; ++r) {
            if (!ch.isAlive(r)) continue;
            if (!evalFilters(plan, plan.filters, *snap, ci, r, nullptr, 0, 0)) continue;
            std::vector<ScalarValue> nextVals(plan.updates.size());
            for (size_t ui = 0; ui < plan.updates.size(); ++ui) {
                nextVals[ui] = evalExpr(plan.exprs, plan.updates[ui].expr, *snap, ci, r);
            }
            for (size_t ui = 0; ui < plan.updates.size(); ++ui) {
                table.updateCell(RowLoc{ci, r},
                                 static_cast<size_t>(plan.updates[ui].colIdx),
                                 nextVals[ui]);
            }
            ++updated;
        }
    }
    return updated;
}

size_t applyDeleteReference(Table& table, const QueryPlan& inPlan) {
    auto snap = table.snapshot();
    QueryPlan plan = inPlan;
    if (!plan.bound) {
        std::string err;
        if (!bindQueryPlan(plan, *snap, err)) return 0;
    }
    size_t removed = 0;
    for (uint32_t ci = 0; ci < snap->chunks.size(); ++ci) {
        const auto& ch = snap->chunks[ci];
        for (uint32_t r = 0; r < ch.rowCount; ++r) {
            if (!ch.isAlive(r)) continue;
            if (evalFilters(plan, plan.filters, *snap, ci, r, nullptr, 0, 0)) {
                if (table.removeRow(RowLoc{ci, r})) ++removed;
            }
        }
    }
    return removed;
}

}  // namespace bro::table
