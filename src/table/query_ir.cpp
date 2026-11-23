#include "table/query_ir.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace bro::table {

uint32_t ExprArena::col(std::string name, uint32_t side) {
    ExprNode n;
    n.op = ExprOp::ColRef;
    n.colName = std::move(name);
    n.tableSide = side;
    const uint32_t id = static_cast<uint32_t>(nodes.size());
    nodes.push_back(std::move(n));
    return id;
}

uint32_t ExprArena::lit(ScalarValue v) {
    ExprNode n;
    n.op = ExprOp::Const;
    n.valueType = v.type;
    n.literal = std::move(v);
    const uint32_t id = static_cast<uint32_t>(nodes.size());
    nodes.push_back(std::move(n));
    return id;
}

uint32_t ExprArena::un(ExprOp op, uint32_t a) {
    ExprNode n;
    n.op = op;
    n.lhs = a;
    const uint32_t id = static_cast<uint32_t>(nodes.size());
    nodes.push_back(std::move(n));
    return id;
}

uint32_t ExprArena::bin(ExprOp op, uint32_t a, uint32_t b) {
    ExprNode n;
    n.op = op;
    n.lhs = a;
    n.rhs = b;
    const uint32_t id = static_cast<uint32_t>(nodes.size());
    nodes.push_back(std::move(n));
    return id;
}

uint32_t ExprArena::cond(uint32_t c, uint32_t t, uint32_t e) {
    ExprNode n;
    n.op = ExprOp::Cond;
    n.lhs = c;
    n.rhs = t;
    n.third = e;
    const uint32_t id = static_cast<uint32_t>(nodes.size());
    nodes.push_back(std::move(n));
    return id;
}

ColType promoteNumericType(ColType a, ColType b) {
    if (a == ColType::F64 || b == ColType::F64) return ColType::F64;
    if (a == ColType::F32 || b == ColType::F32) return ColType::F32;
    if (a == ColType::I64 || b == ColType::I64) return ColType::I64;
    if ((a == ColType::I32 && b == ColType::U32) || (a == ColType::U32 && b == ColType::I32)) {
        return ColType::I64;
    }
    if (a == ColType::U32 || b == ColType::U32) return ColType::U32;
    return ColType::I32;
}

namespace {

bool resolveColRef(ExprNode& n, const TableSnapshot& left, const TableSnapshot* right,
                   std::string& err) {
    std::string_view raw = n.colName;
    int forcedSide = -1;
    if (raw.rfind("left.", 0) == 0) {
        raw.remove_prefix(5);
        forcedSide = 0;
    } else if (raw.rfind("right.", 0) == 0) {
        raw.remove_prefix(6);
        forcedSide = 1;
    } else if (n.tableSide == 1) {
        forcedSide = 1;
    }

    if (forcedSide != 1) {
        int idx = left.findColumn(raw);
        if (idx >= 0) {
            n.tableSide = 0;
            n.resolvedCol = idx;
            n.valueType = left.schema.columns[static_cast<size_t>(idx)].type;
            return true;
        }
    }
    if (right && forcedSide != 0) {
        int idx = right->findColumn(raw);
        if (idx >= 0) {
            n.tableSide = 1;
            n.resolvedCol = idx;
            n.valueType = right->schema.columns[static_cast<size_t>(idx)].type;
            return true;
        }
    }
    err = "unknown column: " + n.colName;
    return false;
}

bool bindExprNode(ExprArena& arena, uint32_t id, const TableSnapshot& left,
                  const TableSnapshot* right, std::string& err) {
    if (id == kInvalidExpr || id >= arena.nodes.size()) {
        err = "invalid expression index";
        return false;
    }
    ExprNode& n = arena.nodes[id];
    switch (n.op) {
    case ExprOp::ColRef:
        return resolveColRef(n, left, right, err);
    case ExprOp::Const:
        n.valueType = n.literal.type;
        return true;

    case ExprOp::Neg:
    case ExprOp::Abs: {
        if (!bindExprNode(arena, n.lhs, left, right, err)) return false;
        ColType lt = arena.nodes[n.lhs].valueType;
        n.valueType = (lt == ColType::U32 || lt == ColType::Bool) ? ColType::I32 : lt;
        return true;
    }
    case ExprOp::Sqrt:
    case ExprOp::Floor:
    case ExprOp::Ceil: {
        if (!bindExprNode(arena, n.lhs, left, right, err)) return false;
        ColType lt = arena.nodes[n.lhs].valueType;
        n.valueType = (lt == ColType::F32) ? ColType::F32 : ColType::F64;
        return true;
    }
    case ExprOp::BitNot: {
        if (!bindExprNode(arena, n.lhs, left, right, err)) return false;
        ColType lt = arena.nodes[n.lhs].valueType;
        n.valueType = (lt == ColType::I64 || lt == ColType::U32 || lt == ColType::I32) ? lt : ColType::I32;
        return true;
    }
    case ExprOp::Not:
        if (!bindExprNode(arena, n.lhs, left, right, err)) return false;
        n.valueType = ColType::Bool;
        return true;
    case ExprOp::StrLen:
        if (!bindExprNode(arena, n.lhs, left, right, err)) return false;
        n.valueType = ColType::I32;
        return true;

    case ExprOp::Add:
    case ExprOp::Sub:
    case ExprOp::Mul:
    case ExprOp::Div:
    case ExprOp::Mod: {
        if (!bindExprNode(arena, n.lhs, left, right, err)) return false;
        if (!bindExprNode(arena, n.rhs, left, right, err)) return false;
        n.valueType = promoteNumericType(arena.nodes[n.lhs].valueType,
                                           arena.nodes[n.rhs].valueType);
        return true;
    }

    case ExprOp::BitAnd:
    case ExprOp::BitOr:
    case ExprOp::BitXor:
    case ExprOp::Shl:
    case ExprOp::Shr:
    case ExprOp::UShr: {
        if (!bindExprNode(arena, n.lhs, left, right, err)) return false;
        if (!bindExprNode(arena, n.rhs, left, right, err)) return false;
        ColType lt = arena.nodes[n.lhs].valueType;
        ColType rt = arena.nodes[n.rhs].valueType;
        if (lt == ColType::I64 || rt == ColType::I64) n.valueType = ColType::I64;
        else if (lt == ColType::U32 && (rt == ColType::U32 || rt == ColType::Bool)) n.valueType = ColType::U32;
        else n.valueType = ColType::I32;
        return true;
    }

    case ExprOp::Eq:
    case ExprOp::Ne:
    case ExprOp::Lt:
    case ExprOp::Le:
    case ExprOp::Gt:
    case ExprOp::Ge: {
        if (!bindExprNode(arena, n.lhs, left, right, err)) return false;
        if (!bindExprNode(arena, n.rhs, left, right, err)) return false;
        // Resolve Dict string literal comparison directly to dict ID lookup
        ExprNode& lnode = arena.nodes[n.lhs];
        ExprNode& rnode = arena.nodes[n.rhs];
        if (lnode.valueType == ColType::Dict && rnode.op == ExprOp::Const &&
            rnode.literal.type == ColType::String) {
            const TableSnapshot& owner = (lnode.tableSide == 1 && right) ? *right : left;
            if (auto id = owner.dict->find(rnode.literal.str)) {
                rnode.cachedDictId = *id;
            } else {
                rnode.cachedDictId = kInvalidDictId;
            }
        } else if (rnode.valueType == ColType::Dict && lnode.op == ExprOp::Const &&
                   lnode.literal.type == ColType::String) {
            const TableSnapshot& owner = (rnode.tableSide == 1 && right) ? *right : left;
            if (auto id = owner.dict->find(lnode.literal.str)) {
                lnode.cachedDictId = *id;
            } else {
                lnode.cachedDictId = kInvalidDictId;
            }
        }
        n.valueType = ColType::Bool;
        return true;
    }

    case ExprOp::And:
    case ExprOp::Or:
        if (!bindExprNode(arena, n.lhs, left, right, err)) return false;
        if (!bindExprNode(arena, n.rhs, left, right, err)) return false;
        n.valueType = ColType::Bool;
        return true;

    case ExprOp::StrEq:
    case ExprOp::StrNe:
    case ExprOp::StrContains:
    case ExprOp::StrStartsWith:
    case ExprOp::StrEndsWith: {
        if (!bindExprNode(arena, n.lhs, left, right, err)) return false;
        if (!bindExprNode(arena, n.rhs, left, right, err)) return false;
        ExprNode& lnode = arena.nodes[n.lhs];
        ExprNode& rnode = arena.nodes[n.rhs];
        if ((n.op == ExprOp::StrEq || n.op == ExprOp::StrNe) &&
            lnode.valueType == ColType::Dict && rnode.op == ExprOp::Const &&
            rnode.literal.type == ColType::String) {
            const TableSnapshot& owner = (lnode.tableSide == 1 && right) ? *right : left;
            if (auto id = owner.dict->find(rnode.literal.str)) rnode.cachedDictId = *id;
            else rnode.cachedDictId = kInvalidDictId;
        }
        n.valueType = ColType::Bool;
        return true;
    }

    case ExprOp::Cond: {
        if (!bindExprNode(arena, n.lhs, left, right, err)) return false;
        if (!bindExprNode(arena, n.rhs, left, right, err)) return false;
        if (!bindExprNode(arena, n.third, left, right, err)) return false;
        ColType rt = arena.nodes[n.rhs].valueType;
        ColType et = arena.nodes[n.third].valueType;
        n.valueType = (rt == ColType::String || et == ColType::String)
                          ? ColType::String
                          : (rt == et ? rt : promoteNumericType(rt, et));
        return true;
    }
    }
    return true;
}

bool exprTouchesSide1(const ExprArena& arena, uint32_t id) {
    if (id == kInvalidExpr || id >= arena.nodes.size()) return false;
    const ExprNode& n = arena.nodes[id];
    if (n.op == ExprOp::ColRef) return n.tableSide == 1;
    if (exprTouchesSide1(arena, n.lhs)) return true;
    if (exprTouchesSide1(arena, n.rhs)) return true;
    if (exprTouchesSide1(arena, n.third)) return true;
    return false;
}

void appendExprKey(const ExprArena& arena, uint32_t id, std::ostringstream& os) {
    if (id == kInvalidExpr || id >= arena.nodes.size()) {
        os << "#";
        return;
    }
    const ExprNode& n = arena.nodes[id];
    os << "(" << static_cast<int>(n.op) << ":" << static_cast<int>(n.valueType) << ":";
    if (n.op == ExprOp::ColRef) {
        os << n.tableSide << "." << n.resolvedCol;
    } else if (n.op == ExprOp::Const) {
        os << static_cast<int>(n.literal.type) << ",";
        if (n.literal.type == ColType::F32 || n.literal.type == ColType::F64) {
            uint64_t bits = 0;
            std::memcpy(&bits, &n.literal.f64, 8);
            os << bits;
        } else if (n.literal.type == ColType::String) {
            os << n.cachedDictId << ":" << n.literal.str.size() << ":" << n.literal.str;
        } else {
            os << n.literal.i64 << "/" << n.literal.u64;
        }
    } else {
        appendExprKey(arena, n.lhs, os);
        if (n.rhs != kInvalidExpr) {
            os << ",";
            appendExprKey(arena, n.rhs, os);
        }
        if (n.third != kInvalidExpr) {
            os << ",";
            appendExprKey(arena, n.third, os);
        }
    }
    os << ")";
}

}  // namespace

bool canJitExpr(const ExprArena& arena, uint32_t id, const TableSnapshot& snap) {
    if (id == kInvalidExpr || id >= arena.nodes.size()) return false;
    const ExprNode& n = arena.nodes[id];
    switch (n.op) {
    case ExprOp::ColRef:
        return n.tableSide == 0 && n.resolvedCol >= 0 &&
               n.valueType != ColType::String;
    case ExprOp::Const:
        return n.literal.type != ColType::String;
    case ExprOp::StrEq:
    case ExprOp::StrNe: {
        // Jittable only when Dict ColRef == Const String resolved to dict ID!
        const ExprNode& ln = arena.nodes[n.lhs];
        const ExprNode& rn = arena.nodes[n.rhs];
        return ln.op == ExprOp::ColRef && ln.tableSide == 0 &&
               ln.valueType == ColType::Dict &&
               rn.op == ExprOp::Const && rn.literal.type == ColType::String;
    }
    case ExprOp::StrContains:
    case ExprOp::StrStartsWith:
    case ExprOp::StrEndsWith:
    case ExprOp::StrLen:
        return false;
    case ExprOp::Eq:
    case ExprOp::Ne: {
        const ExprNode& ln = arena.nodes[n.lhs];
        const ExprNode& rn = arena.nodes[n.rhs];
        if (ln.op == ExprOp::ColRef && ln.tableSide == 0 &&
            ln.valueType == ColType::Dict &&
            rn.op == ExprOp::Const && rn.literal.type == ColType::String) {
            return true;
        }
        if (rn.op == ExprOp::ColRef && rn.tableSide == 0 &&
            rn.valueType == ColType::Dict &&
            ln.op == ExprOp::Const && ln.literal.type == ColType::String) {
            return true;
        }
        if (ln.valueType == ColType::String || rn.valueType == ColType::String) {
            return false;
        }
        return canJitExpr(arena, n.lhs, snap) && canJitExpr(arena, n.rhs, snap);
    }
    case ExprOp::Lt:
    case ExprOp::Le:
    case ExprOp::Gt:
    case ExprOp::Ge:
        if (arena.nodes[n.lhs].valueType == ColType::String ||
            arena.nodes[n.rhs].valueType == ColType::String) {
            return false;
        }
        return canJitExpr(arena, n.lhs, snap) && canJitExpr(arena, n.rhs, snap);
    case ExprOp::Neg:
    case ExprOp::Abs:
    case ExprOp::Sqrt:
    case ExprOp::Floor:
    case ExprOp::Ceil:
    case ExprOp::BitNot:
    case ExprOp::Not:
        return canJitExpr(arena, n.lhs, snap);
    case ExprOp::Cond:
        return canJitExpr(arena, n.lhs, snap) &&
               canJitExpr(arena, n.rhs, snap) &&
               canJitExpr(arena, n.third, snap);
    case ExprOp::Mod:
        if (colTypeIsFloat(n.valueType)) return false;
        return canJitExpr(arena, n.lhs, snap) && canJitExpr(arena, n.rhs, snap);
    default:
        return canJitExpr(arena, n.lhs, snap) && canJitExpr(arena, n.rhs, snap);
    }
}

bool bindQueryPlan(QueryPlan& plan, const TableSnapshot& left, std::string& outError) {
    const TableSnapshot* right = (plan.join.kind != JoinSpec::Kind::None)
                                     ? plan.join.rightTable.get()
                                     : nullptr;
    if (plan.join.kind != JoinSpec::Kind::None) {
        if (!right) {
            outError = "join missing right table";
            return false;
        }
        if (plan.join.leftKeys.empty() || plan.join.leftKeys.size() != plan.join.rightKeys.size()) {
            outError = "join key count mismatch";
            return false;
        }
        plan.join.leftKeyCols.clear();
        plan.join.rightKeyCols.clear();
        plan.join.keyTypes.clear();
        for (size_t i = 0; i < plan.join.leftKeys.size(); ++i) {
            int lc = left.findColumn(plan.join.leftKeys[i]);
            int rc = right->findColumn(plan.join.rightKeys[i]);
            if (lc < 0) {
                outError = "unknown left join column: " + plan.join.leftKeys[i];
                return false;
            }
            if (rc < 0) {
                outError = "unknown right join column: " + plan.join.rightKeys[i];
                return false;
            }
            plan.join.leftKeyCols.push_back(static_cast<size_t>(lc));
            plan.join.rightKeyCols.push_back(static_cast<size_t>(rc));
            ColType lt = left.schema.columns[static_cast<size_t>(lc)].type;
            ColType rt = right->schema.columns[static_cast<size_t>(rc)].type;
            plan.join.keyTypes.push_back((lt == ColType::String || rt == ColType::String)
                                             ? ColType::String
                                             : (lt == rt ? lt : promoteNumericType(lt, rt)));
        }
    }

    if (plan.spatialFilter.kind != SpatialFilterSpec::Kind::None) {
        if (!left.findSpatialIndex(plan.spatialFilter.indexName)) {
            outError = "unknown spatial index: " + plan.spatialFilter.indexName;
            return false;
        }
    }

    // Bind filters and route post-join filters touching right table.
    std::vector<uint32_t> allFilters;
    allFilters.reserve(plan.filters.size() + plan.postJoinFilters.size());
    allFilters.insert(allFilters.end(), plan.filters.begin(), plan.filters.end());
    allFilters.insert(allFilters.end(), plan.postJoinFilters.begin(), plan.postJoinFilters.end());
    plan.filters.clear();
    plan.postJoinFilters.clear();

    for (uint32_t f : allFilters) {
        if (!bindExprNode(plan.exprs, f, left, right, outError)) return false;
        if (right && exprTouchesSide1(plan.exprs, f)) {
            plan.postJoinFilters.push_back(f);
        } else {
            plan.filters.push_back(f);
        }
    }

    for (uint32_t gk : plan.groupKeys) {
        if (!bindExprNode(plan.exprs, gk, left, right, outError)) return false;
    }

    for (AggSpec& agg : plan.aggregates) {
        if (agg.kind == AggSpec::Kind::Count && agg.expr == kInvalidExpr) {
            agg.outputType = ColType::I64;
            continue;
        }
        if (!bindExprNode(plan.exprs, agg.expr, left, right, outError)) return false;
        ColType inType = plan.exprs.nodes[agg.expr].valueType;
        switch (agg.kind) {
        case AggSpec::Kind::Count:
            agg.outputType = ColType::I64;
            break;
        case AggSpec::Kind::Sum:
            agg.outputType = colTypeIsFloat(inType) ? ColType::F64 : ColType::I64;
            break;
        case AggSpec::Kind::Avg:
            agg.outputType = ColType::F64;
            break;
        case AggSpec::Kind::Min:
        case AggSpec::Kind::Max:
            agg.outputType = inType;
            break;
        }
    }

    for (SelectItem& item : plan.selectItems) {
        if (!bindExprNode(plan.exprs, item.expr, left, right, outError)) return false;
        item.outputType = plan.exprs.nodes[item.expr].valueType;
    }

    for (OrderByItem& ob : plan.orderBy) {
        const ExprNode& en = plan.exprs.nodes[ob.expr];
        if (en.op == ExprOp::ColRef && en.tableSide == 0) {
            if (!plan.groupKeys.empty() || !plan.aggregates.empty()) {
                bool matchedAlias = false;
                for (size_t gi = 0; gi < plan.groupKeys.size(); ++gi) {
                    const std::string& gName = gi < plan.groupAliases.size()
                                                   ? plan.groupAliases[gi]
                                                   : plan.exprs.nodes[plan.groupKeys[gi]].colName;
                    if (gName == en.colName) {
                        plan.exprs.nodes[ob.expr].resolvedCol = -2;
                        plan.exprs.nodes[ob.expr].valueType =
                            plan.exprs.nodes[plan.groupKeys[gi]].valueType;
                        matchedAlias = true;
                        break;
                    }
                }
                if (!matchedAlias) {
                    for (size_t ai = 0; ai < plan.aggregates.size(); ++ai) {
                        const std::string aName = !plan.aggregates[ai].alias.empty()
                                                      ? plan.aggregates[ai].alias
                                                      : ("agg_" + std::to_string(ai));
                        if (aName == en.colName) {
                            plan.exprs.nodes[ob.expr].resolvedCol = -2;
                            plan.exprs.nodes[ob.expr].valueType =
                                plan.aggregates[ai].outputType;
                            matchedAlias = true;
                            break;
                        }
                    }
                }
                if (matchedAlias) continue;
            } else if (!plan.selectItems.empty()) {
                bool matchedSelect = false;
                for (const SelectItem& si : plan.selectItems) {
                    if (si.alias == en.colName) {
                        ob.expr = si.expr;
                        matchedSelect = true;
                        break;
                    }
                }
                if (matchedSelect) continue;
            }
        }
        if (!bindExprNode(plan.exprs, ob.expr, left, right, outError)) return false;
    }

    for (UpdateAssignment& up : plan.updates) {
        int c = left.findColumn(up.colName);
        if (c < 0) {
            outError = "unknown update column: " + up.colName;
            return false;
        }
        up.colIdx = c;
        if (!bindExprNode(plan.exprs, up.expr, left, nullptr, outError)) return false;
    }

    if (plan.selectItems.empty() && plan.aggregates.empty() && plan.groupKeys.empty() &&
        plan.updates.empty()) {
        for (size_t c = 0; c < left.schema.size(); ++c) {
            const auto& def = left.schema.columns[c];
            uint32_t e = plan.exprs.col(def.name, 0);
            plan.exprs.nodes[e].resolvedCol = static_cast<int32_t>(c);
            plan.exprs.nodes[e].valueType = def.type;
            plan.selectItems.push_back(SelectItem{def.name, e, def.type});
        }
        if (right) {
            for (size_t c = 0; c < right->schema.size(); ++c) {
                const auto& def = right->schema.columns[c];
                std::string alias = def.name;
                if (left.findColumn(alias) >= 0) {
                    alias = "right_" + alias;
                }
                uint32_t e = plan.exprs.col(def.name, 1);
                plan.exprs.nodes[e].resolvedCol = static_cast<int32_t>(c);
                plan.exprs.nodes[e].valueType = def.type;
                plan.selectItems.push_back(SelectItem{std::move(alias), e, def.type});
            }
        }
    }

    plan.bound = true;
    return true;
}

std::string buildPlanCacheKey(const QueryPlan& plan, const TableSnapshot& left,
                              std::string_view modeTag) {
    std::ostringstream os;
    os << modeTag << "|S=" << left.schemaHash << "|";
    for (const auto& c : left.schema.columns) {
        os << static_cast<int>(c.type) << ",";
    }
    os << "|F=";
    for (uint32_t f : plan.filters) {
        appendExprKey(plan.exprs, f, os);
        os << ";";
    }
    if (modeTag == "FP") {
        os << "|P=";
        for (const auto& s : plan.selectItems) {
            os << static_cast<int>(s.outputType) << ":";
            appendExprKey(plan.exprs, s.expr, os);
            os << ";";
        }
    } else if (modeTag == "U") {
        os << "|U=";
        for (const auto& u : plan.updates) {
            os << u.colIdx << ":";
            appendExprKey(plan.exprs, u.expr, os);
            os << ";";
        }
    } else if (modeTag == "A" || modeTag == "SA") {
        os << "|A=";
        for (const auto& a : plan.aggregates) {
            os << static_cast<int>(a.kind) << ":" << static_cast<int>(a.outputType) << ":";
            appendExprKey(plan.exprs, a.expr, os);
            os << ";";
        }
    }
    return os.str();
}

}  // namespace bro::table
