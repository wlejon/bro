#pragma once

#include "table/query_ir.h"

#include <memory>

namespace bro::table {

/// Evaluates a single IR expression on a single row pair (`left` + optional `right`).
ScalarValue evalExpr(const ExprArena& arena, uint32_t exprId,
                     const TableSnapshot& left, uint32_t lChunk, uint32_t lRow,
                     const TableSnapshot* right = nullptr, uint32_t rChunk = 0,
                     uint32_t rRow = 0);

bool scalarTruthy(const ScalarValue& v);
int compareScalar(const ScalarValue& a, const ScalarValue& b);
uint64_t hashScalarKey(ColType type, const ScalarValue& v);

/// Ground-truth reference implementation of full QueryPlan execution (no JIT).
std::shared_ptr<Table> executePlanReference(const TableSnapshot& left,
                                            const QueryPlan& plan);

/// Ground-truth reference implementation of in-place Table updates.
size_t applyUpdateReference(Table& table, const QueryPlan& plan);

/// Ground-truth reference implementation of in-place Table row deletion.
size_t applyDeleteReference(Table& table, const QueryPlan& plan);

}  // namespace bro::table
