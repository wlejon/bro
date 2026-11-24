#pragma once

#include "embed/embed.h"
#include "runtime/heap.h"
#include "table/query_exec.h"
#include "table/query_ir.h"
#include "table/table.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

inline bool isJsArray(Value v) {
    return v.isObject() &&
           v.asObject<bronze::HeapObjectHeader>()->flags == bronze::HeapKind::Array;
}

/// Immutable expression AST node held by JS ExprRef handles.
struct JsExprNode {
    bro::table::ExprOp op = bro::table::ExprOp::Const;
    std::shared_ptr<const JsExprNode> lhs;
    std::shared_ptr<const JsExprNode> rhs;
    std::shared_ptr<const JsExprNode> third;
    uint32_t tableSide = 0;
    std::string colName;
    bro::table::ScalarValue literal;
};

struct JsAggNode {
    bro::table::AggSpec::Kind kind = bro::table::AggSpec::Kind::Count;
    std::string alias;
    std::shared_ptr<const JsExprNode> expr;  // nullptr for count()
};

struct HostTableHandle {
    std::shared_ptr<bro::table::Table> table;
};

struct HostTableSnapshotHandle {
    std::shared_ptr<const bro::table::TableSnapshot> snapshot;
};

struct HostExprHandle {
    std::shared_ptr<const JsExprNode> expr;
};

struct HostAggHandle {
    JsAggNode agg;
};

struct HostQueryBuilder {
    std::shared_ptr<bro::table::Table> mutableSource;
    std::shared_ptr<const bro::table::TableSnapshot> snapshotSource;

    std::vector<std::shared_ptr<const JsExprNode>> filters;
    struct JoinCfg {
        bro::table::JoinSpec::Kind kind = bro::table::JoinSpec::Kind::None;
        std::shared_ptr<const bro::table::TableSnapshot> rightSnap;
        std::vector<std::string> leftKeys;
        std::vector<std::string> rightKeys;
    } join;

    struct SelectCfg {
        std::string alias;
        std::shared_ptr<const JsExprNode> expr;
    };
    std::vector<SelectCfg> selects;

    std::vector<std::string> groupCols;
    std::vector<JsAggNode> aggs;

    struct OrderCfg {
        std::shared_ptr<const JsExprNode> expr;
        bool descending = false;
    };
    std::vector<OrderCfg> orderBy;

    bro::table::SpatialFilterSpec spatial;

    int64_t limit = -1;
    int64_t offset = 0;
};

/// Ensures all HostClass prototypes for Table, TableSnapshot, Expr, Agg, QueryBuilder are installed in current realm.
void ensureTableClassesInstalled();

/// Extracts snapshot if `val` is a HostTableSnapshot, or nullptr.
std::shared_ptr<const bro::table::TableSnapshot> hostTableSnapshotOf(Value val);

/// Extracts table OR current snapshot from either Table or TableSnapshot JS handle.
std::shared_ptr<const bro::table::TableSnapshot> snapshotOrTableOf(Value val);

/// Extracts mutable Table pointer (if `val` is a Table) or null.
std::shared_ptr<bro::table::Table> hostMutableTableOf(Value val);

/// Wraps a C++ Table / TableSnapshot into JS host instances.
Value wrapHostTable(std::shared_ptr<bro::table::Table> table);
Value wrapHostTableSnapshot(std::shared_ptr<const bro::table::TableSnapshot> snapshot);

/// Helper conversions between JS Value and ScalarValue.
bro::table::ScalarValue jsValueToScalar(Value v, bro::table::ColType targetType,
                                        bool hasTarget = true);
Value scalarToJsValue(const bro::table::ScalarValue& sv);

/// Attaches Table / Expr / Agg / QueryBuilder HostClass definitions & populates `ns` object.
void populateQueryNamespace(ev::ObjectBuilder& ns);

/// Starts a QueryBuilder targeting a mutable table or frozen snapshot.
Value createQueryBuilderValue(std::shared_ptr<bro::table::Table> mutableTbl,
                              std::shared_ptr<const bro::table::TableSnapshot> frozenSnap);

/// Helper methods called by both Table.prototype and TableSnapshot.prototype.
Value tableRowsToJsArray(const bro::table::TableSnapshot& snap);
Value tableRowToJsObject(const bro::table::TableSnapshot& snap, uint32_t chunkIdx, uint32_t rowIdx);
Value chunkColumnTypedView(const bro::table::ColumnChunk& colChunk,
                           const std::shared_ptr<const bro::table::ColumnChunk>& keepAlive,
                           uint32_t rowCount);

/// Parses `where(cond)` input (ExprRef or plain `{ col: literal }` filter object).
bool appendWhereCondition(Value condVal, std::vector<std::shared_ptr<const JsExprNode>>& out,
                          std::string& err);

/// Executes update/delete shorthand against a mutable Table using filter + patch.
Value applyTableUpdateDirect(const std::shared_ptr<bro::table::Table>& tbl,
                             const std::vector<std::shared_ptr<const JsExprNode>>& filters,
                             Value patchObj,
                             const bro::table::SpatialFilterSpec* spatial = nullptr,
                             bro::table::ExecOptions opts = {});
Value applyTableDeleteDirect(const std::shared_ptr<bro::table::Table>& tbl,
                             const std::vector<std::shared_ptr<const JsExprNode>>& filters,
                             const bro::table::SpatialFilterSpec* spatial = nullptr,
                             bro::table::ExecOptions opts = {});

/// Shared helpers & extension prototype hooks (implemented in host_table_ext.cpp).
int resolveTableColumnArg(const bro::table::Schema& schema, Value arg);
HostQueryBuilder* hostQueryBuilderOf(Value val);
Value cloneQueryBuilderAnd(Value self, const std::function<void(HostQueryBuilder&)>& fn);
bro::table::QueryPlan buildJsQueryPlan(const HostQueryBuilder& qb);
bro::table::ExecOptions parseQueryExecOptions(std::span<const Value> args, size_t idx);

void attachTableExtProto(ev::ObjectBuilder& proto);
void attachSnapshotExtProto(ev::ObjectBuilder& proto);
void attachQueryExtProto(ev::ObjectBuilder& proto);
void populateTableExtNamespace(ev::ObjectBuilder& ns);

/// CSV / Parquet / Bundle IO hooks (implemented in host_table_io_ext.cpp).
void attachTableIoExtProto(ev::ObjectBuilder& proto);
void attachSnapshotIoExtProto(ev::ObjectBuilder& proto);
void populateTableIoExtNamespace(ev::ObjectBuilder& ns);

/// Kernel / Tensor zero-copy & bulk interop hooks (implemented in host_table_interop.cpp).
void attachTableInteropProto(ev::ObjectBuilder& proto);
void attachSnapshotInteropProto(ev::ObjectBuilder& proto);
void populateTableInteropNamespace(ev::ObjectBuilder& ns);

/// Multi-column & 3D instance GPU packing hooks (implemented in host_table_gpu_pack.cpp).
void attachTableGpuPackProto(ev::ObjectBuilder& proto);
void attachSnapshotGpuPackProto(ev::ObjectBuilder& proto);

}  // namespace bro::bronze_host
