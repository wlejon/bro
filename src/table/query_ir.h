#pragma once

#include "table/table.h"
#include "table/table_types.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace bro::table {

inline constexpr uint32_t kInvalidExpr = 0xFFFFFFFFu;

enum class ExprOp : uint8_t {
    ColRef = 0,
    Const,
    // Unary
    Neg,
    Abs,
    Sqrt,
    Floor,
    Ceil,
    BitNot,
    Not,
    StrLen,
    // Binary numeric / bitwise
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
    UShr,
    // Comparisons (numeric/bool/dict)
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    // Logical
    And,
    Or,
    // String comparisons / predicates
    StrEq,
    StrNe,
    StrContains,
    StrStartsWith,
    StrEndsWith,
    // Ternary conditional
    Cond,
};

struct ExprNode {
    ExprOp op = ExprOp::Const;
    uint32_t lhs = kInvalidExpr;
    uint32_t rhs = kInvalidExpr;
    uint32_t third = kInvalidExpr;
    uint32_t tableSide = 0;  // 0 = primary/left, 1 = right join table
    std::string colName;
    int32_t resolvedCol = -1;
    ColType valueType = ColType::F64;
    ScalarValue literal;
    uint32_t cachedDictId = kInvalidDictId;
};

struct ExprArena {
    std::vector<ExprNode> nodes;

    uint32_t col(std::string name, uint32_t side = 0);
    uint32_t lit(ScalarValue v);
    uint32_t un(ExprOp op, uint32_t a);
    uint32_t bin(ExprOp op, uint32_t a, uint32_t b);
    uint32_t cond(uint32_t c, uint32_t t, uint32_t e);
};

struct JoinSpec {
    enum class Kind : uint8_t { None = 0, Inner, Left };

    Kind kind = Kind::None;
    std::shared_ptr<const TableSnapshot> rightTable;
    std::vector<std::string> leftKeys;
    std::vector<std::string> rightKeys;

    // Resolved by bindQueryPlan:
    std::vector<size_t> leftKeyCols;
    std::vector<size_t> rightKeyCols;
    std::vector<ColType> keyTypes;
};

struct AggSpec {
    enum class Kind : uint8_t { Count = 0, Sum, Min, Max, Avg };

    Kind kind = Kind::Count;
    std::string alias;
    uint32_t expr = kInvalidExpr;  // kInvalidExpr allowed for Count(*)
    ColType outputType = ColType::I64;
};

struct SelectItem {
    std::string alias;
    uint32_t expr = kInvalidExpr;
    ColType outputType = ColType::F64;
};

struct OrderByItem {
    uint32_t expr = kInvalidExpr;
    bool descending = false;
};

struct UpdateAssignment {
    std::string colName;
    int32_t colIdx = -1;
    uint32_t expr = kInvalidExpr;
};

struct QueryPlan {
    ExprArena exprs;
    std::vector<uint32_t> filters;          // pre-join (side 0) or all filters if no join
    std::vector<uint32_t> postJoinFilters;  // populated by bind() for predicates touching side 1
    JoinSpec join;
    std::vector<uint32_t> groupKeys;
    std::vector<std::string> groupAliases;
    std::vector<AggSpec> aggregates;
    std::vector<SelectItem> selectItems;
    std::vector<OrderByItem> orderBy;
    int64_t limit = -1;
    int64_t offset = 0;
    std::vector<UpdateAssignment> updates;
    bool bound = false;
};

/// Shared numeric promotion rules used identically by binder, reference evaluator, and JIT.
ColType promoteNumericType(ColType a, ColType b);

/// Checks if an expression subtree contains only ops compilable to register-pure brass MIR.
bool canJitExpr(const ExprArena& arena, uint32_t exprId, const TableSnapshot& snap);

/// Resolves column names, side ownership, types, Dict IDs, and default projections.
bool bindQueryPlan(QueryPlan& plan, const TableSnapshot& left, std::string& outError);

/// Builds a collision-free canonical key forCompiledPlanCache.
std::string buildPlanCacheKey(const QueryPlan& plan, const TableSnapshot& left,
                              std::string_view modeTag);

}  // namespace bro::table
