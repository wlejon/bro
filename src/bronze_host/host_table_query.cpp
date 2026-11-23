#include "bronze_host/host_table.h"

#include "bronze_host/host_builder.h"
#include "embed/embed_host_class.h"
#include "table/query_exec.h"

#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

using namespace bro::table;

ev::HostClass g_exprClass;
ev::HostClass g_aggClass;
ev::HostClass g_queryBuilderClass;

std::shared_ptr<const JsExprNode> makeColNode(std::string name, uint32_t side = 0) {
    auto n = std::make_shared<JsExprNode>();
    n->op = ExprOp::ColRef;
    n->colName = std::move(name);
    n->tableSide = side;
    return n;
}

std::shared_ptr<const JsExprNode> makeLitNode(ScalarValue v) {
    auto n = std::make_shared<JsExprNode>();
    n->op = ExprOp::Const;
    n->literal = std::move(v);
    return n;
}

std::shared_ptr<const JsExprNode> makeUnNode(ExprOp op, std::shared_ptr<const JsExprNode> a) {
    auto n = std::make_shared<JsExprNode>();
    n->op = op;
    n->lhs = std::move(a);
    return n;
}

std::shared_ptr<const JsExprNode> makeBinNode(ExprOp op,
                                              std::shared_ptr<const JsExprNode> a,
                                              std::shared_ptr<const JsExprNode> b) {
    auto n = std::make_shared<JsExprNode>();
    n->op = op;
    n->lhs = std::move(a);
    n->rhs = std::move(b);
    return n;
}

std::shared_ptr<const JsExprNode> makeCondNode(std::shared_ptr<const JsExprNode> c,
                                               std::shared_ptr<const JsExprNode> t,
                                               std::shared_ptr<const JsExprNode> e) {
    auto n = std::make_shared<JsExprNode>();
    n->op = ExprOp::Cond;
    n->lhs = std::move(c);
    n->rhs = std::move(t);
    n->third = std::move(e);
    return n;
}

Value wrapExpr(std::shared_ptr<const JsExprNode> expr) {
    auto h = std::make_unique<HostExprHandle>();
    h->expr = std::move(expr);
    return g_exprClass.createInstance(std::move(h));
}

Value wrapAgg(JsAggNode agg) {
    auto h = std::make_unique<HostAggHandle>();
    h->agg = std::move(agg);
    return g_aggClass.createInstance(std::move(h));
}

std::shared_ptr<const JsExprNode> unwrapExprOr(Value v, bool stringIsColumnName) {
    if (g_exprClass.installed()) {
        if (auto* h = static_cast<HostExprHandle*>(g_exprClass.unwrap(v))) {
            return h->expr;
        }
    }
    if (stringIsColumnName && ev::isString(v)) {
        return makeColNode(ev::toUtf8(v), 0);
    }
    return makeLitNode(jsValueToScalar(v, ColType::F64, /*hasTarget=*/false));
}

HostQueryBuilder* queryOf(Value self) {
    return g_queryBuilderClass.installed()
               ? static_cast<HostQueryBuilder*>(g_queryBuilderClass.unwrap(self))
               : nullptr;
}

Value cloneQueryAnd(Value self, const std::function<void(HostQueryBuilder&)>& fn) {
    HostQueryBuilder* src = queryOf(self);
    if (!src) return ev::throwTypeError("QueryBuilder: invalid receiver");
    auto next = std::make_unique<HostQueryBuilder>(*src);
    fn(*next);
    return g_queryBuilderClass.createInstance(std::move(next));
}

uint32_t lowerExprTree(ExprArena& arena, const std::shared_ptr<const JsExprNode>& node) {
    if (!node) return kInvalidExpr;
    if (node->op == ExprOp::ColRef) return arena.col(node->colName, node->tableSide);
    if (node->op == ExprOp::Const) return arena.lit(node->literal);
    if (node->op == ExprOp::Cond) {
        return arena.cond(lowerExprTree(arena, node->lhs),
                          lowerExprTree(arena, node->rhs),
                          lowerExprTree(arena, node->third));
    }
    if (node->rhs) {
        return arena.bin(node->op, lowerExprTree(arena, node->lhs),
                         lowerExprTree(arena, node->rhs));
    }
    return arena.un(node->op, lowerExprTree(arena, node->lhs));
}

QueryPlan buildPlanFromJs(const HostQueryBuilder& qb) {
    QueryPlan plan;
    for (const auto& f : qb.filters) {
        plan.filters.push_back(lowerExprTree(plan.exprs, f));
    }
    if (qb.join.kind != JoinSpec::Kind::None && qb.join.rightSnap) {
        plan.join.kind = qb.join.kind;
        plan.join.rightTable = qb.join.rightSnap;
        plan.join.leftKeys = qb.join.leftKeys;
        plan.join.rightKeys = qb.join.rightKeys;
    }
    for (const auto& gc : qb.groupCols) {
        plan.groupKeys.push_back(plan.exprs.col(gc, 0));
        plan.groupAliases.push_back(gc);
    }
    for (const auto& a : qb.aggs) {
        AggSpec spec;
        spec.kind = a.kind;
        spec.alias = a.alias;
        spec.expr = a.expr ? lowerExprTree(plan.exprs, a.expr) : kInvalidExpr;
        plan.aggregates.push_back(std::move(spec));
    }
    for (const auto& s : qb.selects) {
        plan.selectItems.push_back(
            SelectItem{s.alias, lowerExprTree(plan.exprs, s.expr), ColType::F64});
    }
    for (const auto& ob : qb.orderBy) {
        plan.orderBy.push_back(OrderByItem{lowerExprTree(plan.exprs, ob.expr), ob.descending});
    }
    plan.spatialFilter = qb.spatial;
    plan.limit = qb.limit;
    plan.offset = qb.offset;
    return plan;
}

std::vector<std::string> getOwnObjectKeys(Value objVal) {
    std::vector<std::string> out;
    auto gObj = ev::globalValue("Object");
    if (!gObj.found) return out;
    ev::Persistent root(objVal);
    ev::Persistent keysFn(ev::getProperty(gObj.value, "keys"));
    Value arg = root.get();
    ev::CallResult res = ev::call(keysFn.get(), ev::undefined(), std::span<const Value>(&arg, 1));
    if (res.thrown || !ev::isObject(res.value)) return out;
    ev::Persistent arr(res.value);
    const uint32_t n = static_cast<uint32_t>(ev::toDouble(ev::getProperty(arr.get(), "length")));
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        out.push_back(ev::toUtf8(ev::getElement(arr.get(), i)));
    }
    return out;
}

void parseStringOrArrayKeys(Value v, std::vector<std::string>& out) {
    if (isJsArray(v)) {
        ev::Persistent arr(v);
        const uint32_t n = static_cast<uint32_t>(ev::toDouble(ev::getProperty(arr.get(), "length")));
        for (uint32_t i = 0; i < n; ++i) out.push_back(ev::toUtf8(ev::getElement(arr.get(), i)));
    } else if (ev::isString(v)) {
        out.push_back(ev::toUtf8(v));
    }
}

ExecOptions parseExecOptions(std::span<const Value> args, size_t idx) {
    ExecOptions opts;
    if (!hasArg(args, idx) || !ev::isObject(args[idx])) return opts;
    ev::Persistent o(args[idx]);
    Value j = ev::getProperty(o.get(), "jit");
    if (!j.isUndefined()) opts.useJit = ev::toBool(j);
    Value ix = ev::getProperty(o.get(), "index");
    if (!ix.isUndefined()) opts.useIndexes = ev::toBool(ix);
    Value par = ev::getProperty(o.get(), "parallel");
    if (!par.isUndefined()) opts.useParallel = ev::toBool(par);
    return opts;
}

void decorateExprProto(ev::ObjectBuilder& proto) {
    auto binMethod = [](const char* name, ExprOp op) {
        return std::pair<const char*, ev::NativeFn>{
            name, [op](Value thisVal, std::span<const Value> args) -> Value {
                auto lhs = unwrapExprOr(thisVal, false);
                auto rhs = unwrapExprOr(argAt(args, 0), false);
                return wrapExpr(makeBinNode(op, std::move(lhs), std::move(rhs)));
            }};
    };
    auto addBin = [&](const char* name, ExprOp op) {
        proto.def(name, 1, binMethod(name, op).second);
    };
    addBin("add", ExprOp::Add);
    addBin("sub", ExprOp::Sub);
    addBin("mul", ExprOp::Mul);
    addBin("div", ExprOp::Div);
    addBin("mod", ExprOp::Mod);
    addBin("bitAnd", ExprOp::BitAnd);
    addBin("bitOr", ExprOp::BitOr);
    addBin("bitXor", ExprOp::BitXor);
    addBin("shl", ExprOp::Shl);
    addBin("shr", ExprOp::Shr);
    addBin("ushr", ExprOp::UShr);
    addBin("eq", ExprOp::Eq);
    addBin("ne", ExprOp::Ne);
    addBin("lt", ExprOp::Lt);
    addBin("le", ExprOp::Le);
    addBin("gt", ExprOp::Gt);
    addBin("ge", ExprOp::Ge);
    addBin("and", ExprOp::And);
    addBin("or", ExprOp::Or);
    addBin("strEq", ExprOp::StrEq);
    addBin("strNe", ExprOp::StrNe);
    addBin("contains", ExprOp::StrContains);
    addBin("startsWith", ExprOp::StrStartsWith);
    addBin("endsWith", ExprOp::StrEndsWith);

    auto addUn = [&](const char* name, ExprOp op) {
        proto.def(name, 0, [op](Value thisVal, std::span<const Value>) -> Value {
            return wrapExpr(makeUnNode(op, unwrapExprOr(thisVal, false)));
        });
    };
    addUn("neg", ExprOp::Neg);
    addUn("abs", ExprOp::Abs);
    addUn("sqrt", ExprOp::Sqrt);
    addUn("floor", ExprOp::Floor);
    addUn("ceil", ExprOp::Ceil);
    addUn("bitNot", ExprOp::BitNot);
    addUn("not", ExprOp::Not);
    addUn("len", ExprOp::StrLen);

    proto.def("selectAs", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        // Convenience object descriptor { alias, expr } for select() arrays.
        ObjectBuilder obj;
        obj.set("alias", ev::fromUtf8(ev::toUtf8(argAt(args, 0))));
        obj.set("expr", thisVal);
        return obj.get();
    });
}

void decorateQueryBuilderProto(ev::ObjectBuilder& proto) {
    proto.def("where", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        if (!hasArg(args, 0)) return thisVal;
        HostQueryBuilder* src = queryOf(thisVal);
        if (!src) return ev::throwTypeError("Query.where: invalid receiver");
        std::vector<std::shared_ptr<const JsExprNode>> added;
        std::string err;
        if (!appendWhereCondition(args[0], added, err)) {
            return ev::throwTypeError("Query.where: " + err);
        }
        auto next = std::make_unique<HostQueryBuilder>(*src);
        next->filters.insert(next->filters.end(), added.begin(), added.end());
        return g_queryBuilderClass.createInstance(std::move(next));
    });

    proto.def("select", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        return cloneQueryAnd(thisVal, [&](HostQueryBuilder& next) {
            next.selects.clear();
            auto pushEntry = [&](const std::string& alias, Value exprVal) {
                auto ex = unwrapExprOr(exprVal, true);
                std::string a = alias;
                if (a.empty() && ex->op == ExprOp::ColRef) a = ex->colName;
                if (a.empty()) a = "col_" + std::to_string(next.selects.size());
                next.selects.push_back(HostQueryBuilder::SelectCfg{std::move(a), std::move(ex)});
            };
            if (args.size() == 1 && ev::isObject(args[0]) &&
                !g_exprClass.unwrap(args[0])) {
                ev::Persistent root(args[0]);
                if (isJsArray(root.get())) {
                    const uint32_t n =
                        static_cast<uint32_t>(ev::toDouble(ev::getProperty(root.get(), "length")));
                    for (uint32_t i = 0; i < n; ++i) {
                        Value el = ev::getElement(root.get(), i);
                        if (ev::isObject(el) && !g_exprClass.unwrap(el)) {
                            std::string alias = ev::toUtf8(ev::getProperty(el, "alias"));
                            pushEntry(alias, ev::getProperty(el, "expr"));
                        } else {
                            pushEntry("", el);
                        }
                    }
                } else {
                    for (const std::string& k : getOwnObjectKeys(root.get())) {
                        pushEntry(k, ev::getProperty(root.get(), k));
                    }
                }
            } else {
                for (Value a : args) {
                    if (!a.isUndefined()) pushEntry("", a);
                }
            }
        });
    });

    proto.def("groupBy", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        return cloneQueryAnd(thisVal, [&](HostQueryBuilder& next) {
            next.groupCols.clear();
            for (Value a : args) {
                if (!a.isUndefined()) parseStringOrArrayKeys(a, next.groupCols);
            }
        });
    });

    proto.def("agg", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        return cloneQueryAnd(thisVal, [&](HostQueryBuilder& next) {
            next.aggs.clear();
            auto pushAgg = [&](const std::string& aliasOverride, Value item) {
                auto* ah = g_aggClass.installed()
                               ? static_cast<HostAggHandle*>(g_aggClass.unwrap(item))
                               : nullptr;
                if (!ah) return;
                JsAggNode copy = ah->agg;
                if (!aliasOverride.empty()) copy.alias = aliasOverride;
                next.aggs.push_back(std::move(copy));
            };
            if (args.size() == 1 && ev::isObject(args[0]) && !g_aggClass.unwrap(args[0])) {
                ev::Persistent root(args[0]);
                if (isJsArray(root.get())) {
                    const uint32_t n =
                        static_cast<uint32_t>(ev::toDouble(ev::getProperty(root.get(), "length")));
                    for (uint32_t i = 0; i < n; ++i) {
                        pushAgg("", ev::getElement(root.get(), i));
                    }
                } else {
                    for (const std::string& k : getOwnObjectKeys(root.get())) {
                        pushAgg(k, ev::getProperty(root.get(), k));
                    }
                }
            } else {
                for (Value a : args) {
                    if (!a.isUndefined()) pushAgg("", a);
                }
            }
        });
    });

    auto makeJoinFn = [](JoinSpec::Kind jkind) {
        return [jkind](Value thisVal, std::span<const Value> args) -> Value {
            auto rightSnap = snapshotOrTableOf(argAt(args, 0));
            if (!rightSnap) return ev::throwTypeError("Query.join: arg 0 must be Table or TableSnapshot");
            std::vector<std::string> lk, rk;
            parseStringOrArrayKeys(argAt(args, 1), lk);
            parseStringOrArrayKeys(hasArg(args, 2) ? argAt(args, 2) : argAt(args, 1), rk);
            if (lk.empty() || lk.size() != rk.size()) {
                return ev::throwTypeError("Query.join: mismatched left/right join keys");
            }
            return cloneQueryAnd(thisVal, [&](HostQueryBuilder& next) {
                next.join.kind = jkind;
                next.join.rightSnap = rightSnap;
                next.join.leftKeys = std::move(lk);
                next.join.rightKeys = std::move(rk);
            });
        };
    };
    proto.def("join", 3, makeJoinFn(JoinSpec::Kind::Inner));
    proto.def("leftJoin", 3, makeJoinFn(JoinSpec::Kind::Left));

    proto.def("orderBy", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        return cloneQueryAnd(thisVal, [&](HostQueryBuilder& next) {
            if (hasArg(args, 0) && isJsArray(args[0])) {
                ev::Persistent arr(args[0]);
                const uint32_t n =
                    static_cast<uint32_t>(ev::toDouble(ev::getProperty(arr.get(), "length")));
                for (uint32_t i = 0; i < n; ++i) {
                    ev::Persistent spec(ev::getElement(arr.get(), i));
                    if (ev::isObject(spec.get()) && !g_exprClass.unwrap(spec.get())) {
                        auto ex = unwrapExprOr(ev::getProperty(spec.get(), "by"), true);
                        bool desc = ev::toBool(ev::getProperty(spec.get(), "desc"));
                        next.orderBy.push_back(HostQueryBuilder::OrderCfg{std::move(ex), desc});
                    } else {
                        next.orderBy.push_back(
                            HostQueryBuilder::OrderCfg{unwrapExprOr(spec.get(), true), false});
                    }
                }
            } else {
                auto ex = unwrapExprOr(argAt(args, 0), true);
                bool desc = false;
                if (hasArg(args, 1)) {
                    Value d = args[1];
                    if (ev::isString(d)) desc = (ev::toUtf8(d) == "desc" || ev::toUtf8(d) == "DESC");
                    else desc = ev::toBool(d);
                }
                next.orderBy.push_back(HostQueryBuilder::OrderCfg{std::move(ex), desc});
            }
        });
    });

    proto.def("limit", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        return cloneQueryAnd(thisVal, [&](HostQueryBuilder& next) {
            next.limit = std::max<int64_t>(0, i64At(args, 0));
        });
    });

    proto.def("offset", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        return cloneQueryAnd(thisVal, [&](HostQueryBuilder& next) {
            next.offset = std::max<int64_t>(0, i64At(args, 0));
        });
    });

    auto execFn = [](Value thisVal, std::span<const Value> args) -> Value {
        HostQueryBuilder* qb = queryOf(thisVal);
        if (!qb) return ev::throwTypeError("Query.run: invalid receiver");
        auto snap = qb->snapshotSource ? qb->snapshotSource
                                       : (qb->mutableSource ? qb->mutableSource->snapshot() : nullptr);
        if (!snap) return ev::throwTypeError("Query.run: no table source");
        QueryPlan plan = buildPlanFromJs(*qb);
        QueryExecResult res = executeQuery(*snap, std::move(plan), parseExecOptions(args, 0));
        if (!res.ok) return ev::throwError("Query.run: " + res.error);
        return wrapHostTable(std::move(res.table));
    };
    proto.def("run", 1, execFn);
    proto.def("toTable", 1, execFn);

    proto.def("toArray", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        HostQueryBuilder* qb = queryOf(thisVal);
        if (!qb) return ev::throwTypeError("Query.toArray: invalid receiver");
        auto snap = qb->snapshotSource ? qb->snapshotSource
                                       : (qb->mutableSource ? qb->mutableSource->snapshot() : nullptr);
        if (!snap) return ev::throwTypeError("Query.toArray: no table source");
        QueryPlan plan = buildPlanFromJs(*qb);
        QueryExecResult res = executeQuery(*snap, std::move(plan), parseExecOptions(args, 0));
        if (!res.ok) return ev::throwError("Query.toArray: " + res.error);
        auto outSnap = res.table->snapshot();
        return tableRowsToJsArray(*outSnap);
    });

    proto.def("count", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        HostQueryBuilder* qb = queryOf(thisVal);
        if (!qb) return ev::throwTypeError("Query.count: invalid receiver");
        auto snap = qb->snapshotSource ? qb->snapshotSource
                                       : (qb->mutableSource ? qb->mutableSource->snapshot() : nullptr);
        if (!snap) return ev::fromDouble(0);
        HostQueryBuilder copy = *qb;
        copy.selects.clear();
        copy.orderBy.clear();
        copy.aggs.clear();
        copy.groupCols.clear();
        copy.aggs.push_back(JsAggNode{AggSpec::Kind::Count, "cnt", nullptr});
        QueryExecResult res = executeQuery(*snap, buildPlanFromJs(copy), parseExecOptions(args, 0));
        if (!res.ok) return ev::throwError("Query.count: " + res.error);
        RowLoc loc{};
        if (!res.table->resolveAliveRow(0, loc)) return ev::fromDouble(0);
        return ev::fromDouble(static_cast<double>(res.table->getCell(loc, 0).i64));
    });

    proto.def("update", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        HostQueryBuilder* qb = queryOf(thisVal);
        if (!qb || !qb->mutableSource) {
            return ev::throwTypeError("Query.update: requires mutable Table query");
        }
        return applyTableUpdateDirect(qb->mutableSource, qb->filters, argAt(args, 0));
    });

    proto.def("remove", 1, [](Value thisVal, std::span<const Value>) -> Value {
        HostQueryBuilder* qb = queryOf(thisVal);
        if (!qb || !qb->mutableSource) {
            return ev::throwTypeError("Query.remove: requires mutable Table query");
        }
        return applyTableDeleteDirect(qb->mutableSource, qb->filters);
    });
    attachQueryExtProto(proto);
}

}  // namespace

bool appendWhereCondition(Value condVal, std::vector<std::shared_ptr<const JsExprNode>>& out,
                          std::string& err) {
    if (g_exprClass.installed()) {
        if (auto* h = static_cast<HostExprHandle*>(g_exprClass.unwrap(condVal))) {
            out.push_back(h->expr);
            return true;
        }
    }
    if (ev::isObject(condVal)) {
        ev::Persistent root(condVal);
        for (const std::string& k : getOwnObjectKeys(root.get())) {
            Value v = ev::getProperty(root.get(), k);
            out.push_back(makeBinNode(ExprOp::Eq, makeColNode(k),
                                      makeLitNode(jsValueToScalar(v, ColType::F64, false))));
        }
        return true;
    }
    err = "where condition must be Expr or equality object";
    return false;
}

Value applyTableUpdateDirect(const std::shared_ptr<bro::table::Table>& tbl,
                             const std::vector<std::shared_ptr<const JsExprNode>>& filters,
                             Value patchObj) {
    if (!ev::isObject(patchObj)) return ev::throwTypeError("update: expected column map object");
    QueryPlan plan;
    for (const auto& f : filters) plan.filters.push_back(lowerExprTree(plan.exprs, f));
    ev::Persistent patch(patchObj);
    for (const std::string& col : getOwnObjectKeys(patch.get())) {
        Value rhsVal = ev::getProperty(patch.get(), col);
        auto rhsExpr = unwrapExprOr(rhsVal, false);
        plan.updates.push_back(UpdateAssignment{col, -1, lowerExprTree(plan.exprs, rhsExpr)});
    }
    MutationExecResult res = executeUpdate(*tbl, std::move(plan));
    if (!res.ok) return ev::throwTypeError("update: " + res.error);
    ObjectBuilder obj;
    obj.set("affectedRows", ev::fromDouble(static_cast<double>(res.affectedRows)));
    obj.set("usedJit", ev::fromBool(res.usedJit));
    return obj.get();
}

Value applyTableDeleteDirect(const std::shared_ptr<bro::table::Table>& tbl,
                             const std::vector<std::shared_ptr<const JsExprNode>>& filters) {
    QueryPlan plan;
    for (const auto& f : filters) plan.filters.push_back(lowerExprTree(plan.exprs, f));
    MutationExecResult res = executeDelete(*tbl, std::move(plan));
    if (!res.ok) return ev::throwTypeError("deleteWhere: " + res.error);
    ObjectBuilder obj;
    obj.set("affectedRows", ev::fromDouble(static_cast<double>(res.affectedRows)));
    obj.set("usedJit", ev::fromBool(res.usedJit));
    return obj.get();
}

Value createQueryBuilderValue(std::shared_ptr<bro::table::Table> mutableTbl,
                              std::shared_ptr<const bro::table::TableSnapshot> frozenSnap) {
    ensureTableClassesInstalled();
    auto qb = std::make_unique<HostQueryBuilder>();
    qb->mutableSource = std::move(mutableTbl);
    qb->snapshotSource = std::move(frozenSnap);
    return g_queryBuilderClass.createInstance(std::move(qb));
}

void populateQueryNamespace(ev::ObjectBuilder& ns) {
    if (!g_exprClass.installed()) {
        g_exprClass.install("TableExpr", 0, nullptr, decorateExprProto, /*global=*/false);
    }
    if (!g_aggClass.installed()) {
        g_aggClass.install("TableAgg", 0, nullptr, [](ev::ObjectBuilder&) {}, /*global=*/false);
    }
    if (!g_queryBuilderClass.installed()) {
        g_queryBuilderClass.install("TableQuery", 0, nullptr, decorateQueryBuilderProto,
                                    /*global=*/false);
    }

    ns.def("col", 2, [](Value, std::span<const Value> args) -> Value {
        uint32_t side = hasArg(args, 1) ? u32At(args, 1) : 0;
        return wrapExpr(makeColNode(ev::toUtf8(argAt(args, 0)), side));
    });
    ns.def("lit", 1, [](Value, std::span<const Value> args) -> Value {
        return wrapExpr(makeLitNode(jsValueToScalar(argAt(args, 0), ColType::F64, false)));
    });
    ns.def("cond", 3, [](Value, std::span<const Value> args) -> Value {
        return wrapExpr(makeCondNode(unwrapExprOr(argAt(args, 0), true),
                                     unwrapExprOr(argAt(args, 1), false),
                                     unwrapExprOr(argAt(args, 2), false)));
    });

    auto regUn = [&](const char* name, ExprOp op) {
        ns.def(name, 1, [op](Value, std::span<const Value> args) -> Value {
            return wrapExpr(makeUnNode(op, unwrapExprOr(argAt(args, 0), true)));
        });
    };
    regUn("neg", ExprOp::Neg);
    regUn("abs", ExprOp::Abs);
    regUn("sqrt", ExprOp::Sqrt);
    regUn("floor", ExprOp::Floor);
    regUn("ceil", ExprOp::Ceil);
    regUn("bitNot", ExprOp::BitNot);
    regUn("not", ExprOp::Not);
    regUn("strLen", ExprOp::StrLen);

    auto regBin = [&](const char* name, ExprOp op) {
        ns.def(name, 2, [op](Value, std::span<const Value> args) -> Value {
            return wrapExpr(makeBinNode(op, unwrapExprOr(argAt(args, 0), true),
                                        unwrapExprOr(argAt(args, 1), false)));
        });
    };
    regBin("add", ExprOp::Add);
    regBin("sub", ExprOp::Sub);
    regBin("mul", ExprOp::Mul);
    regBin("div", ExprOp::Div);
    regBin("mod", ExprOp::Mod);
    regBin("bitAnd", ExprOp::BitAnd);
    regBin("bitOr", ExprOp::BitOr);
    regBin("bitXor", ExprOp::BitXor);
    regBin("shl", ExprOp::Shl);
    regBin("shr", ExprOp::Shr);
    regBin("ushr", ExprOp::UShr);
    regBin("eq", ExprOp::Eq);
    regBin("ne", ExprOp::Ne);
    regBin("lt", ExprOp::Lt);
    regBin("le", ExprOp::Le);
    regBin("gt", ExprOp::Gt);
    regBin("ge", ExprOp::Ge);
    regBin("strEq", ExprOp::StrEq);
    regBin("strNe", ExprOp::StrNe);
    regBin("contains", ExprOp::StrContains);
    regBin("startsWith", ExprOp::StrStartsWith);
    regBin("endsWith", ExprOp::StrEndsWith);

    auto regVariadicLogic = [&](const char* name, ExprOp op) {
        ns.def(name, 2, [op](Value, std::span<const Value> args) -> Value {
            std::shared_ptr<const JsExprNode> acc;
            for (Value a : args) {
                if (a.isUndefined()) continue;
                auto item = unwrapExprOr(a, true);
                acc = acc ? makeBinNode(op, acc, item) : item;
            }
            return wrapExpr(acc ? acc : makeLitNode(ScalarValue::makeBool(op == ExprOp::And)));
        });
    };
    regVariadicLogic("and", ExprOp::And);
    regVariadicLogic("or", ExprOp::Or);

    ns.def("count", 1, [](Value, std::span<const Value> args) -> Value {
        std::string alias = hasArg(args, 0) && ev::isString(args[0]) ? ev::toUtf8(args[0]) : "count";
        return wrapAgg(JsAggNode{AggSpec::Kind::Count, std::move(alias), nullptr});
    });

    auto regAgg = [&](const char* name, AggSpec::Kind kind, const char* defPrefix) {
        ns.def(name, 2, [kind, defPrefix](Value, std::span<const Value> args) -> Value {
            auto ex = unwrapExprOr(argAt(args, 0), true);
            std::string alias = hasArg(args, 1) ? ev::toUtf8(args[1]) : "";
            if (alias.empty() && ex->op == ExprOp::ColRef) {
                alias = std::string(defPrefix) + "_" + ex->colName;
            }
            return wrapAgg(JsAggNode{kind, std::move(alias), std::move(ex)});
        });
    };
    regAgg("sum", AggSpec::Kind::Sum, "sum");
    regAgg("min", AggSpec::Kind::Min, "min");
    regAgg("max", AggSpec::Kind::Max, "max");
    regAgg("avg", AggSpec::Kind::Avg, "avg");
}

HostQueryBuilder* hostQueryBuilderOf(Value val) {
    return queryOf(val);
}

Value cloneQueryBuilderAnd(Value self, const std::function<void(HostQueryBuilder&)>& fn) {
    return cloneQueryAnd(self, fn);
}

bro::table::QueryPlan buildJsQueryPlan(const HostQueryBuilder& qb) {
    return buildPlanFromJs(qb);
}

bro::table::ExecOptions parseQueryExecOptions(std::span<const Value> args, size_t idx) {
    return parseExecOptions(args, idx);
}

}  // namespace bro::bronze_host
