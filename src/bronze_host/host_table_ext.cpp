#include "bronze_host/host_table.h"

#include "bronze_host/host_access.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_values.h"
#include "embed/embed_host_class.h"
#include "embed/embed_typed_array.h"
#include "table/query_exec.h"
#include "table/table_io.h"
#include "table/table_reactive.h"
#include "util/asset_path.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

using namespace bro::table;

ev::HostClass g_reactiveClass;

struct HostReactiveHandle {
    std::shared_ptr<ReactiveQuery> reactive;
    std::vector<ev::Persistent> listeners;
};

HostReactiveHandle* reactiveOf(Value val) {
    return g_reactiveClass.installed()
               ? static_cast<HostReactiveHandle*>(g_reactiveClass.unwrap(val))
               : nullptr;
}

bool parsePoint3(Value v, double out[3]) {
    out[0] = 0.0;
    out[1] = 0.0;
    out[2] = 0.0;
    if (!ev::isObject(v)) return false;
    ev::Persistent arr(v);
    Value lenVal = ev::getProperty(arr.get(), "length");
    if (!lenVal.isNumber()) return false;
    const uint32_t n = static_cast<uint32_t>(lenVal.asNumber());
    if (n < 2) return false;
    out[0] = ev::toDouble(ev::getElement(arr.get(), 0));
    out[1] = ev::toDouble(ev::getElement(arr.get(), 1));
    if (n >= 3) out[2] = ev::toDouble(ev::getElement(arr.get(), 2));
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

Value bytesToUint8Array(const std::vector<uint8_t>& bytes) {
    Value arr = ev::createTypedArray(ev::elements::Uint8, static_cast<uint32_t>(bytes.size()));
    if (!bytes.empty()) {
        if (auto ta = ev::typedArrayInfo(arr); ta && ta.data && ta.byteLength >= bytes.size()) {
            std::memcpy(ta.data, bytes.data(), bytes.size());
        }
    }
    return arr;
}

Value uint32VectorToJsArray(const std::vector<uint32_t>& vals) {
    return hostArrayOf(vals.size(), [&vals](size_t i) -> Value {
        return ev::fromDouble(static_cast<double>(vals[i]));
    });
}

Value materializeNearestTable(const TableSnapshot& snap, const std::string& indexName,
                              const double pt[3], size_t k, double maxRadius) {
    const SpatialIndex* si = snap.findSpatialIndex(indexName);
    if (!si) {
        return ev::throwError("nearest: unknown spatial index '" + indexName + "'");
    }
    std::vector<std::pair<RowLoc, double>> hits;
    si->queryNearest(pt[0], pt[1], pt[2], k, maxRadius, hits);

    std::vector<ColumnDef> outCols = snap.schema.columns;
    outCols.push_back(ColumnDef{"_distance", ColType::F64});
    auto out = std::make_shared<Table>(std::move(outCols));
    out->setSharedDict(snap.dict);

    const size_t nCols = snap.columnCount();
    std::vector<ScalarValue> row(nCols + 1);
    for (const auto& [loc, dist] : hits) {
        if (loc.chunkIdx >= snap.chunks.size() || !snap.chunks[loc.chunkIdx].isAlive(loc.rowIdx)) {
            continue;
        }
        for (size_t c = 0; c < nCols; ++c) {
            row[c] = snap.getCell(c, loc.chunkIdx, loc.rowIdx);
        }
        row[nCols] = ScalarValue::makeF64(dist);
        out->appendRow(row);
    }
    return wrapHostTable(std::move(out));
}

bool snapshotHasMappedChunk(const TableSnapshot& snap) {
    for (const auto& fc : snap.chunks) {
        for (const auto& col : fc.columns) {
            if (col && col->isMapped()) return true;
        }
    }
    return false;
}

void decorateReactiveProto(ev::ObjectBuilder& proto) {
    proto.accessor("table", [](Value thisVal, std::span<const Value>) -> Value {
        HostReactiveHandle* h = reactiveOf(thisVal);
        if (!h || !h->reactive || !h->reactive->latestResult()) return ev::null();
        return wrapHostTable(h->reactive->latestResult());
    }, nullptr);

    proto.accessor("version", [](Value thisVal, std::span<const Value>) -> Value {
        HostReactiveHandle* h = reactiveOf(thisVal);
        return ev::fromDouble(h && h->reactive ? static_cast<double>(h->reactive->lastVersion()) : 0.0);
    }, nullptr);

    proto.def("dirty", 0, [](Value thisVal, std::span<const Value>) -> Value {
        HostReactiveHandle* h = reactiveOf(thisVal);
        if (!h || !h->reactive) return ev::fromBool(false);
        return ev::fromBool(h->reactive->isDirty());
    });

    proto.def("refresh", 0, [](Value thisVal, std::span<const Value>) -> Value {
        HostReactiveHandle* h = reactiveOf(thisVal);
        if (!h || !h->reactive) return ev::throwTypeError("ReactiveQuery.refresh: disposed or invalid");
        ReactiveRefreshResult rr = h->reactive->refresh();
        ev::Persistent tblVal(rr.table ? wrapHostTable(rr.table) : ev::null());
        ObjectBuilder ob;
        ob.set("table", tblVal.get());
        ob.set("changed", ev::fromBool(rr.changed));
        ob.set("version", ev::fromDouble(static_cast<double>(rr.version)));
        ob.set("recomputedChunks", ev::fromDouble(static_cast<double>(rr.recomputedChunks)));
        ob.set("reusedChunks", ev::fromDouble(static_cast<double>(rr.reusedChunks)));
        ev::Persistent resObj(ob.get());

        if (rr.changed && !h->listeners.empty()) {
            std::vector<ev::Persistent> callbacks;
            callbacks.reserve(h->listeners.size());
            for (const auto& fn : h->listeners) callbacks.emplace_back(fn.get());
            for (const auto& fn : callbacks) {
                Value arg = resObj.get();
                ev::CallResult cr = ev::call(fn.get(), ev::undefined(), std::span<const Value>(&arg, 1));
                if (cr.thrown) return ev::throwValue(cr.value);
            }
        }
        return resObj.get();
    });

    proto.def("onChange", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        HostReactiveHandle* h = reactiveOf(thisVal);
        if (!h || !h->reactive) return ev::throwTypeError("ReactiveQuery.onChange: disposed or invalid");
        if (!hasArg(args, 0) || !ev::isFunction(args[0])) {
            return ev::throwTypeError("ReactiveQuery.onChange: callback must be a function");
        }
        h->listeners.emplace_back(args[0]);
        return thisVal;
    });

    proto.def("dispose", 0, [](Value thisVal, std::span<const Value>) -> Value {
        if (HostReactiveHandle* h = reactiveOf(thisVal)) {
            h->listeners.clear();
            h->reactive.reset();
        }
        return ev::undefined();
    });
}

void ensureReactiveClassInstalled() {
    if (!g_reactiveClass.installed()) {
        g_reactiveClass.install("ReactiveQuery", 0, nullptr, decorateReactiveProto, /*global=*/false);
    }
}

}  // namespace

void attachTableExtProto(ev::ObjectBuilder& proto) {
    proto.def("createRangeIndex", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.createRangeIndex: invalid receiver");
        int col = hasArg(args, 0) ? resolveTableColumnArg(tbl->schema(), args[0]) : -1;
        if (col < 0) return ev::throwRangeError("Table.createRangeIndex: invalid column");
        return ev::fromBool(tbl->createRangeIndex(static_cast<size_t>(col)));
    });

    proto.def("hasRangeIndex", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.hasRangeIndex: invalid receiver");
        int col = hasArg(args, 0) ? resolveTableColumnArg(tbl->schema(), args[0]) : -1;
        return ev::fromBool(col >= 0 && tbl->hasRangeIndex(static_cast<size_t>(col)));
    });

    proto.def("dropRangeIndex", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.dropRangeIndex: invalid receiver");
        int col = hasArg(args, 0) ? resolveTableColumnArg(tbl->schema(), args[0]) : -1;
        return ev::fromBool(col >= 0 && tbl->dropRangeIndex(static_cast<size_t>(col)));
    });

    proto.def("createSpatialIndex", 3, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.createSpatialIndex: invalid receiver");
        if (!hasArg(args, 0) || !ev::isString(args[0]) || !hasArg(args, 1) || !isJsArray(args[1])) {
            return ev::throwTypeError("Table.createSpatialIndex: expects (name, [cols...], cellSize?)");
        }
        std::string name = ev::toUtf8(args[0]);
        ev::Persistent colsArr(args[1]);
        const uint32_t n = static_cast<uint32_t>(ev::toDouble(ev::getProperty(colsArr.get(), "length")));
        if (n != 2 && n != 3) {
            return ev::throwTypeError("Table.createSpatialIndex: cols array must have 2 or 3 entries");
        }
        std::vector<size_t> cols(n);
        for (uint32_t i = 0; i < n; ++i) {
            int idx = resolveTableColumnArg(tbl->schema(), ev::getElement(colsArr.get(), i));
            if (idx < 0) return ev::throwRangeError("Table.createSpatialIndex: invalid column");
            cols[i] = static_cast<size_t>(idx);
        }
        const double cellSize = hasArg(args, 2) && args[2].isNumber() ? args[2].asNumber() : 16.0;
        return ev::fromBool(tbl->createSpatialIndex(std::move(name), std::move(cols), cellSize));
    });

    proto.def("hasSpatialIndex", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.hasSpatialIndex: invalid receiver");
        if (!hasArg(args, 0) || !ev::isString(args[0])) return ev::fromBool(false);
        return ev::fromBool(tbl->hasSpatialIndex(ev::toUtf8(args[0])));
    });

    proto.def("dropSpatialIndex", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.dropSpatialIndex: invalid receiver");
        if (!hasArg(args, 0) || !ev::isString(args[0])) return ev::fromBool(false);
        return ev::fromBool(tbl->dropSpatialIndex(ev::toUtf8(args[0])));
    });

    proto.def("nearest", 4, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.nearest: invalid receiver");
        if (!hasArg(args, 0) || !ev::isString(args[0]) || !hasArg(args, 1)) {
            return ev::throwTypeError("Table.nearest: expects (spatialIndexName, [x,y(,z)], k, maxRadius?)");
        }
        double pt[3]{};
        if (!parsePoint3(args[1], pt)) return ev::throwTypeError("Table.nearest: invalid point array");
        const size_t k = hasArg(args, 2) ? static_cast<size_t>(std::max(0.0, ev::toDouble(args[2]))) : 10;
        const double maxR = (hasArg(args, 3) && args[3].isNumber()) ? args[3].asNumber() : -1.0;
        auto snap = tbl->snapshot();
        return materializeNearestTable(*snap, ev::toUtf8(args[0]), pt, k, maxR);
    });

    proto.def("save", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.save: invalid receiver");
        if (!hasArg(args, 0) || !ev::isString(args[0])) {
            return ev::throwTypeError("Table.save: path must be a string");
        }
        std::string resolved = util::resolveAssetWritePath(ev::toUtf8(args[0]));
        requirePathAccess(resolved, true);
        std::string err;
        if (!saveSnapshotToFile(*tbl->snapshot(), resolved, &err)) {
            return ev::throwError("Table.save: " + err);
        }
        return ev::fromBool(true);
    });

    proto.def("toBytes", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.toBytes: invalid receiver");
        return bytesToUint8Array(serializeSnapshotBytes(*tbl->snapshot()));
    });

    proto.accessor("structureVersion", [](Value thisVal, std::span<const Value>) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        return tbl ? ev::fromDouble(static_cast<double>(tbl->structureVersion())) : ev::fromDouble(0);
    }, nullptr);

    proto.def("dirtyChunksSince", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.dirtyChunksSince: invalid receiver");
        const uint64_t ver = hasArg(args, 0) ? static_cast<uint64_t>(std::max(0.0, ev::toDouble(args[0]))) : 0;
        return uint32VectorToJsArray(tbl->dirtyChunksSince(ver));
    });

    proto.def("chunkDirtyVersion", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.chunkDirtyVersion: invalid receiver");
        const size_t ci = hasArg(args, 0) ? static_cast<size_t>(std::max(0.0, ev::toDouble(args[0]))) : SIZE_MAX;
        return ev::fromDouble(static_cast<double>(tbl->chunkDirtyVersion(ci)));
    });

    proto.def("markChunkDirty", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.markChunkDirty: invalid receiver");
        const size_t ci = hasArg(args, 0) ? static_cast<size_t>(std::max(0.0, ev::toDouble(args[0]))) : SIZE_MAX;
        tbl->markChunkDirty(ci);
        return ev::undefined();
    });

    proto.def("notifyColumnUpdated", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.notifyColumnUpdated: invalid receiver");
        int col = hasArg(args, 0) ? resolveTableColumnArg(tbl->schema(), args[0]) : -1;
        if (col < 0) return ev::throwRangeError("Table.notifyColumnUpdated: invalid column");
        tbl->notifyColumnUpdated(static_cast<size_t>(col));
        return ev::undefined();
    });

    attachTableInteropProto(proto);
}

void attachSnapshotExtProto(ev::ObjectBuilder& proto) {
    proto.accessor("isMapped", [](Value thisVal, std::span<const Value>) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        return ev::fromBool(snap && snapshotHasMappedChunk(*snap));
    }, nullptr);

    proto.accessor("structureVersion", [](Value thisVal, std::span<const Value>) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        return snap ? ev::fromDouble(static_cast<double>(snap->structureVersion)) : ev::fromDouble(0);
    }, nullptr);

    proto.def("hasRangeIndex", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.hasRangeIndex: invalid receiver");
        int col = hasArg(args, 0) ? resolveTableColumnArg(snap->schema, args[0]) : -1;
        return ev::fromBool(col >= 0 && snap->findRangeIndex(static_cast<size_t>(col)) != nullptr);
    });

    proto.def("hasSpatialIndex", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.hasSpatialIndex: invalid receiver");
        if (!hasArg(args, 0) || !ev::isString(args[0])) return ev::fromBool(false);
        return ev::fromBool(snap->findSpatialIndex(ev::toUtf8(args[0])) != nullptr);
    });

    proto.def("nearest", 4, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.nearest: invalid receiver");
        if (!hasArg(args, 0) || !ev::isString(args[0]) || !hasArg(args, 1)) {
            return ev::throwTypeError("TableSnapshot.nearest: expects (spatialIndexName, [x,y(,z)], k, maxRadius?)");
        }
        double pt[3]{};
        if (!parsePoint3(args[1], pt)) return ev::throwTypeError("TableSnapshot.nearest: invalid point array");
        const size_t k = hasArg(args, 2) ? static_cast<size_t>(std::max(0.0, ev::toDouble(args[2]))) : 10;
        const double maxR = (hasArg(args, 3) && args[3].isNumber()) ? args[3].asNumber() : -1.0;
        return materializeNearestTable(*snap, ev::toUtf8(args[0]), pt, k, maxR);
    });

    proto.def("dirtyChunksSince", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.dirtyChunksSince: invalid receiver");
        const uint64_t ver = hasArg(args, 0) ? static_cast<uint64_t>(std::max(0.0, ev::toDouble(args[0]))) : 0;
        return uint32VectorToJsArray(snap->dirtyChunksSince(ver));
    });

    proto.def("chunkDirtyVersion", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.chunkDirtyVersion: invalid receiver");
        const size_t ci = hasArg(args, 0) ? static_cast<size_t>(std::max(0.0, ev::toDouble(args[0]))) : SIZE_MAX;
        if (ci >= snap->chunks.size()) return ev::fromDouble(0);
        return ev::fromDouble(static_cast<double>(snap->chunks[ci].dirtyVersion));
    });

    proto.def("save", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.save: invalid receiver");
        if (!hasArg(args, 0) || !ev::isString(args[0])) {
            return ev::throwTypeError("TableSnapshot.save: path must be a string");
        }
        std::string resolved = util::resolveAssetWritePath(ev::toUtf8(args[0]));
        requirePathAccess(resolved, true);
        std::string err;
        if (!saveSnapshotToFile(*snap, resolved, &err)) {
            return ev::throwError("TableSnapshot.save: " + err);
        }
        return ev::fromBool(true);
    });

    proto.def("toBytes", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.toBytes: invalid receiver");
        return bytesToUint8Array(serializeSnapshotBytes(*snap));
    });

    proto.def("toTable", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.toTable: invalid receiver");
        return wrapHostTable(Table::fromSnapshot(*snap));
    });

    attachSnapshotInteropProto(proto);
}

void attachQueryExtProto(ev::ObjectBuilder& proto) {
    proto.def("whereInAABB", 3, [](Value thisVal, std::span<const Value> args) -> Value {
        if (!hasArg(args, 0) || !ev::isString(args[0]) || !hasArg(args, 1) || !hasArg(args, 2)) {
            return ev::throwTypeError("Query.whereInAABB: expects (spatialIndexName, minPt, maxPt)");
        }
        double minPt[3]{}, maxPt[3]{};
        if (!parsePoint3(args[1], minPt) || !parsePoint3(args[2], maxPt)) {
            return ev::throwTypeError("Query.whereInAABB: minPt and maxPt must be 2D/3D arrays");
        }
        std::string name = ev::toUtf8(args[0]);
        return cloneQueryBuilderAnd(thisVal, [&](HostQueryBuilder& next) {
            next.spatial.kind = SpatialFilterSpec::Kind::AABB;
            next.spatial.indexName = std::move(name);
            for (int i = 0; i < 3; ++i) {
                next.spatial.a[i] = minPt[i];
                next.spatial.b[i] = maxPt[i];
            }
            next.spatial.radius = 0.0;
        });
    });

    proto.def("whereInRadius", 3, [](Value thisVal, std::span<const Value> args) -> Value {
        if (!hasArg(args, 0) || !ev::isString(args[0]) || !hasArg(args, 1) || !hasArg(args, 2)) {
            return ev::throwTypeError("Query.whereInRadius: expects (spatialIndexName, centerPt, radius)");
        }
        double cPt[3]{};
        if (!parsePoint3(args[1], cPt)) {
            return ev::throwTypeError("Query.whereInRadius: centerPt must be a 2D/3D array");
        }
        const double r = ev::toDouble(args[2]);
        if (!std::isfinite(r) || r < 0.0) {
            return ev::throwRangeError("Query.whereInRadius: radius must be non-negative finite number");
        }
        std::string name = ev::toUtf8(args[0]);
        return cloneQueryBuilderAnd(thisVal, [&](HostQueryBuilder& next) {
            next.spatial.kind = SpatialFilterSpec::Kind::Radius;
            next.spatial.indexName = std::move(name);
            for (int i = 0; i < 3; ++i) {
                next.spatial.a[i] = cPt[i];
                next.spatial.b[i] = 0.0;
            }
            next.spatial.radius = r;
        });
    });

    proto.def("runWithStats", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        HostQueryBuilder* qb = hostQueryBuilderOf(thisVal);
        if (!qb) return ev::throwTypeError("Query.runWithStats: invalid receiver");
        auto snap = qb->snapshotSource
                        ? qb->snapshotSource
                        : (qb->mutableSource ? qb->mutableSource->snapshot() : nullptr);
        if (!snap) return ev::throwTypeError("Query.runWithStats: missing table source");
        QueryPlan plan = buildJsQueryPlan(*qb);
        ExecOptions opts = parseQueryExecOptions(args, 0);
        QueryExecResult res = executeQuery(*snap, std::move(plan), opts);
        if (!res.ok) return ev::throwError("Query.runWithStats: " + res.error);
        ev::Persistent tblVal(wrapHostTable(res.table));
        ObjectBuilder ob;
        ob.set("table", tblVal.get());
        ob.set("usedJit", ev::fromBool(res.usedJit));
        ob.set("usedSimd", ev::fromBool(res.usedSimd));
        ob.set("usedJitAgg", ev::fromBool(res.usedJitAgg));
        ob.set("usedIndex", ev::fromBool(res.usedIndex));
        ob.set("usedRangeIndex", ev::fromBool(res.usedRangeIndex));
        ob.set("usedSpatialIndex", ev::fromBool(res.usedSpatialIndex));
        return ob.get();
    });

    proto.def("reactive", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        ensureReactiveClassInstalled();
        HostQueryBuilder* qb = hostQueryBuilderOf(thisVal);
        if (!qb || !qb->mutableSource) {
            return ev::throwTypeError("Query.reactive: requires a mutable Table query source");
        }
        ExecOptions opts = parseQueryExecOptions(args, 0);
        bool defer = false;
        if (hasArg(args, 0) && ev::isObject(args[0])) {
            Value d = ev::getProperty(args[0], "defer");
            if (!d.isUndefined()) defer = ev::toBool(d);
        }
        QueryPlan plan = buildJsQueryPlan(*qb);
        auto rq = std::make_shared<ReactiveQuery>(qb->mutableSource, std::move(plan), opts);
        if (!defer) rq->refresh();
        auto handle = std::make_unique<HostReactiveHandle>();
        handle->reactive = std::move(rq);
        return g_reactiveClass.createInstance(std::move(handle));
    });
}

void populateTableExtNamespace(ev::ObjectBuilder& ns) {
    ensureReactiveClassInstalled();

    ns.def("load", 1, [](Value, std::span<const Value> args) -> Value {
        if (!hasArg(args, 0) || !ev::isString(args[0])) {
            return ev::throwTypeError("bro.table.load: path must be a string");
        }
        std::string resolved = readableAssetPath(ev::toUtf8(args[0]));
        std::string err;
        auto tbl = loadTableFromFile(resolved, &err);
        if (!tbl) return ev::throwError("bro.table.load: " + err);
        return wrapHostTable(std::move(tbl));
    });

    ns.def("mmap", 1, [](Value, std::span<const Value> args) -> Value {
        if (!hasArg(args, 0) || !ev::isString(args[0])) {
            return ev::throwTypeError("bro.table.mmap: path must be a string");
        }
        std::string resolved = readableAssetPath(ev::toUtf8(args[0]));
        std::string err;
        auto snap = mmapSnapshotFromFile(resolved, &err);
        if (!snap) return ev::throwError("bro.table.mmap: " + err);
        return wrapHostTableSnapshot(std::move(snap));
    });

    ns.def("fromBytes", 1, [](Value, std::span<const Value> args) -> Value {
        if (!hasArg(args, 0)) return ev::throwTypeError("bro.table.fromBytes: expects Uint8Array");
        std::vector<uint8_t> copy;
        if (auto ta = ev::typedArrayInfo(args[0]); ta && ta.data && ta.byteLength > 0) {
            const uint8_t* ptr = static_cast<const uint8_t*>(ta.data);
            copy.assign(ptr, ptr + ta.byteLength);
        } else {
            return ev::throwTypeError("bro.table.fromBytes: expects non-empty Uint8Array");
        }
        std::string err;
        auto tbl = deserializeTableBytes(copy.data(), copy.size(), &err);
        if (!tbl) return ev::throwError("bro.table.fromBytes: " + err);
        return wrapHostTable(std::move(tbl));
    });

    populateTableInteropNamespace(ns);
}

}  // namespace bro::bronze_host
