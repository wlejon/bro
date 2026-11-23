#include "bronze_host/host_table.h"

#include "bronze_host/host_bro_namespaces.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_values.h"
#include "embed/embed_host_class.h"
#include "embed/embed_typed_array.h"
#include "table/query_exec.h"
#include "table/query_jit.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

ev::HostClass g_tableClass;
ev::HostClass g_snapshotClass;

void releaseChunkPin(void* user, uint8_t*) {
    delete static_cast<std::shared_ptr<const bro::table::ColumnChunk>*>(user);
}

int resolveColumnArg(const bro::table::Schema& schema, Value arg) {
    if (arg.isNumber()) {
        int idx = static_cast<int>(arg.asNumber());
        if (idx >= 0 && static_cast<size_t>(idx) < schema.size()) return idx;
        return -1;
    }
    if (ev::isString(arg)) {
        return schema.findColumn(ev::toUtf8(arg));
    }
    return -1;
}

bool parseSchemaSpec(Value arg, std::vector<bro::table::ColumnDef>& outDefs, std::string& err) {
    if (!ev::isObject(arg)) {
        err = "schema must be an object or array";
        return false;
    }
    ev::Persistent root(arg);
    Value lenVal = ev::getProperty(root.get(), "length");
    if (isJsArray(root.get()) && !lenVal.isUndefined()) {
        const uint32_t n = static_cast<uint32_t>(ev::toDouble(lenVal));
        outDefs.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            ev::Persistent item(ev::getElement(root.get(), i));
            std::string name;
            std::string typeStr;
            if (isJsArray(item.get())) {
                name = ev::toUtf8(ev::getElement(item.get(), 0));
                typeStr = ev::toUtf8(ev::getElement(item.get(), 1));
            } else if (ev::isObject(item.get())) {
                name = ev::toUtf8(ev::getProperty(item.get(), "name"));
                typeStr = ev::toUtf8(ev::getProperty(item.get(), "type"));
            } else {
                err = "invalid array schema entry";
                return false;
            }
            bro::table::ColType ct{};
            if (name.empty() || !bro::table::parseColType(typeStr, ct)) {
                err = "invalid column schema entry: " + name + ":" + typeStr;
                return false;
            }
            outDefs.push_back(bro::table::ColumnDef{std::move(name), ct});
        }
        return !outDefs.empty();
    }

    auto gObj = ev::globalValue("Object");
    if (!gObj.found) {
        err = "Object global unavailable";
        return false;
    }
    ev::Persistent keysFn(ev::getProperty(gObj.value, "keys"));
    Value objArg = root.get();
    ev::CallResult keysRes = ev::call(keysFn.get(), ev::undefined(), std::span<const Value>(&objArg, 1));
    if (keysRes.thrown || !ev::isObject(keysRes.value)) {
        err = "failed to inspect schema keys";
        return false;
    }
    ev::Persistent keysArr(keysRes.value);
    const uint32_t n = static_cast<uint32_t>(ev::toDouble(ev::getProperty(keysArr.get(), "length")));
    outDefs.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        std::string name = ev::toUtf8(ev::getElement(keysArr.get(), i));
        std::string typeStr = ev::toUtf8(ev::getProperty(root.get(), name));
        bro::table::ColType ct{};
        if (!bro::table::parseColType(typeStr, ct)) {
            err = "unknown column type '" + typeStr + "' for column '" + name + "'";
            return false;
        }
        outDefs.push_back(bro::table::ColumnDef{std::move(name), ct});
    }
    if (outDefs.empty()) {
        err = "schema must define at least one column";
        return false;
    }
    return true;
}

Value schemaToJsArray(const bro::table::Schema& schema) {
    return ev::hostArrayOf(schema.columns.size(), [&schema](size_t i) -> Value {
        ObjectBuilder item;
        item.set("name", ev::fromUtf8(schema.columns[i].name));
        item.set("type", ev::fromUtf8(bro::table::colTypeName(schema.columns[i].type)));
        return item.get();
    });
}

bool readRowFromJs(const bro::table::Table& tbl, Value rowVal,
                   std::vector<bro::table::ScalarValue>& out) {
    const auto& schema = tbl.schema();
    const size_t nCols = schema.size();
    out.assign(nCols, bro::table::ScalarValue::makeNull());
    if (!ev::isObject(rowVal)) return false;

    ev::Persistent root(rowVal);
    if (isJsArray(root.get())) {
        const uint32_t arrLen =
            static_cast<uint32_t>(ev::toDouble(ev::getProperty(root.get(), "length")));
        const size_t take = std::min<size_t>(nCols, arrLen);
        for (size_t c = 0; c < take; ++c) {
            Value v = ev::getElement(root.get(), static_cast<uint32_t>(c));
            if (!v.isUndefined() && !v.isNull()) {
                out[c] = jsValueToScalar(v, schema.columns[c].type);
            }
        }
        return true;
    }

    for (size_t c = 0; c < nCols; ++c) {
        Value v = ev::getProperty(root.get(), schema.columns[c].name);
        if (!v.isUndefined() && !v.isNull()) {
            out[c] = jsValueToScalar(v, schema.columns[c].type);
        }
    }
    return true;
}

template <bool MutableTable>
std::shared_ptr<const bro::table::TableSnapshot> getSnapshotForQuery(Value self) {
    if constexpr (MutableTable) {
        auto tbl = hostMutableTableOf(self);
        return tbl ? tbl->snapshot() : nullptr;
    } else {
        return hostTableSnapshotOf(self);
    }
}

void decorateTableProto(ev::ObjectBuilder& proto) {
    proto.accessor("rowCount", [](Value thisVal, std::span<const Value>) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        return tbl ? ev::fromDouble(static_cast<double>(tbl->rowCount())) : ev::fromDouble(0);
    }, nullptr);

    proto.accessor("columnCount", [](Value thisVal, std::span<const Value>) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        return tbl ? ev::fromDouble(static_cast<double>(tbl->columnCount())) : ev::fromDouble(0);
    }, nullptr);

    proto.accessor("chunkCount", [](Value thisVal, std::span<const Value>) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        return tbl ? ev::fromDouble(static_cast<double>(tbl->chunkCount())) : ev::fromDouble(0);
    }, nullptr);

    proto.accessor("version", [](Value thisVal, std::span<const Value>) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        return tbl ? ev::fromDouble(static_cast<double>(tbl->version())) : ev::fromDouble(0);
    }, nullptr);

    proto.accessor("schema", [](Value thisVal, std::span<const Value>) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        return tbl ? schemaToJsArray(tbl->schema()) : makeEmptyArray();
    }, nullptr);

    proto.def("insert", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.insert: invalid receiver");
        std::vector<bro::table::ScalarValue> row;
        if (!hasArg(args, 0) || !readRowFromJs(*tbl, args[0], row)) {
            return ev::throwTypeError("Table.insert: expected row object or array");
        }
        bro::table::RowLoc loc = tbl->appendRow(row);
        ObjectBuilder ret;
        ret.set("chunk", ev::fromDouble(loc.chunkIdx));
        ret.set("row", ev::fromDouble(loc.rowIdx));
        return ret.get();
    });

    proto.def("insertMany", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.insertMany: invalid receiver");
        if (!hasArg(args, 0) || !ev::isObject(args[0])) {
            return ev::throwTypeError("Table.insertMany: expected array or columnar map");
        }
        ev::Persistent root(args[0]);
        if (isJsArray(root.get())) {
            const uint32_t count =
                static_cast<uint32_t>(ev::toDouble(ev::getProperty(root.get(), "length")));
            std::vector<bro::table::ScalarValue> row;
            for (uint32_t i = 0; i < count; ++i) {
                Value rv = ev::getElement(root.get(), i);
                if (readRowFromJs(*tbl, rv, row)) tbl->appendRow(row);
            }
            return ev::fromDouble(static_cast<double>(count));
        }

        // Columnar object: { colName: TypedArray | Array, ... }
        // Pass 1: root every column Value first (ev::getProperty allocates on moving GC).
        const size_t nCols = tbl->columnCount();
        std::vector<ev::Persistent> colVals;
        colVals.reserve(nCols);
        uint32_t batchLen = 0;
        bool foundAny = false;
        bool allTypedNumeric = true;

        auto expectedElementKind = [](bro::table::ColType ct, bronze::ElementKind& outKind) -> bool {
            switch (ct) {
            case bro::table::ColType::I32: outKind = ev::elements::Int32; return true;
            case bro::table::ColType::U32: outKind = ev::elements::Uint32; return true;
            case bro::table::ColType::I64: outKind = ev::elements::BigInt64; return true;
            case bro::table::ColType::F32: outKind = ev::elements::Float32; return true;
            case bro::table::ColType::F64: outKind = ev::elements::Float64; return true;
            case bro::table::ColType::Bool: outKind = ev::elements::Uint8; return true;
            case bro::table::ColType::Dict:
            case bro::table::ColType::String:
                return false;
            }
            return false;
        };

        for (size_t c = 0; c < nCols; ++c) {
            Value cv = ev::getProperty(root.get(), tbl->schema().columns[c].name);
            colVals.emplace_back(cv);
            if (!cv.isUndefined() && !cv.isNull()) {
                auto ta = ev::typedArrayInfo(cv);
                if (ta) {
                    if (!foundAny) { batchLen = ta.elementCount; foundAny = true; }
                    else batchLen = std::min(batchLen, ta.elementCount);
                    bronze::ElementKind expected{};
                    if (!expectedElementKind(tbl->schema().columns[c].type, expected) ||
                        ta.elementKind != expected) {
                        allTypedNumeric = false;
                    }
                } else if (ev::isObject(cv)) {
                    allTypedNumeric = false;
                    uint32_t l = static_cast<uint32_t>(ev::toDouble(ev::getProperty(cv, "length")));
                    if (!foundAny) { batchLen = l; foundAny = true; }
                    else batchLen = std::min(batchLen, l);
                }
            }
        }
        if (!foundAny || batchLen == 0) return ev::fromDouble(0);
        if (allTypedNumeric) {
            // Pass 2: ZERO allocating embed calls while holding raw typed-array pointers.
            std::vector<const void*> rawPtrs(nCols, nullptr);
            for (size_t c = 0; c < nCols; ++c) {
                Value cv = colVals[c].get();
                if (!cv.isUndefined() && !cv.isNull()) {
                    if (auto ta = ev::typedArrayInfo(cv)) {
                        rawPtrs[c] = ta.data;
                    }
                }
            }
            tbl->appendTypedBatch(batchLen, rawPtrs.data());
            return ev::fromDouble(static_cast<double>(batchLen));
        }
        std::vector<bro::table::ScalarValue> row(nCols);
        for (uint32_t i = 0; i < batchLen; ++i) {
            for (size_t c = 0; c < nCols; ++c) {
                Value cv = colVals[c].get();
                if (cv.isUndefined() || cv.isNull()) {
                    row[c] = bro::table::ScalarValue::makeNull();
                } else {
                    Value cell = ev::getElement(cv, i);
                    row[c] = jsValueToScalar(cell, tbl->schema().columns[c].type);
                }
            }
            tbl->appendRow(row);
        }
        return ev::fromDouble(static_cast<double>(batchLen));
    });

    proto.def("get", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.get: invalid receiver");
        const int colIdx = resolveColumnArg(tbl->schema(), argAt(args, 1));
        if (colIdx < 0) return ev::undefined();
        bro::table::RowLoc loc{};
        if (!tbl->resolveAliveRow(static_cast<size_t>(numAt(args, 0)), loc)) return ev::undefined();
        return scalarToJsValue(tbl->getCell(loc, static_cast<size_t>(colIdx)));
    });

    proto.def("row", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.row: invalid receiver");
        bro::table::RowLoc loc{};
        if (!tbl->resolveAliveRow(static_cast<size_t>(numAt(args, 0)), loc)) return ev::undefined();
        auto snap = tbl->snapshot();
        return tableRowToJsObject(*snap, loc.chunkIdx, loc.rowIdx);
    });

    proto.def("updateRow", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.updateRow: invalid receiver");
        bro::table::RowLoc loc{};
        if (!tbl->resolveAliveRow(static_cast<size_t>(numAt(args, 0)), loc)) return ev::fromBool(false);
        if (!hasArg(args, 1) || !ev::isObject(args[1])) return ev::fromBool(false);
        ev::Persistent patch(args[1]);
        bool any = false;
        for (size_t c = 0; c < tbl->columnCount(); ++c) {
            Value v = ev::getProperty(patch.get(), tbl->schema().columns[c].name);
            if (!v.isUndefined()) {
                any |= tbl->updateCell(loc, c, jsValueToScalar(v, tbl->schema().columns[c].type));
            }
        }
        return ev::fromBool(any);
    });

    proto.def("removeRow", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.removeRow: invalid receiver");
        bro::table::RowLoc loc{};
        if (!tbl->resolveAliveRow(static_cast<size_t>(numAt(args, 0)), loc)) return ev::fromBool(false);
        return ev::fromBool(tbl->removeRow(loc));
    });

    proto.def("compact", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.compact: invalid receiver");
        tbl->compact();
        return thisVal;
    });

    proto.def("createIndex", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.createIndex: invalid receiver");
        const int colIdx = resolveColumnArg(tbl->schema(), argAt(args, 0));
        return ev::fromBool(colIdx >= 0 && tbl->createIndex(static_cast<size_t>(colIdx)));
    });

    proto.def("hasIndex", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.hasIndex: invalid receiver");
        const int colIdx = resolveColumnArg(tbl->schema(), argAt(args, 0));
        return ev::fromBool(colIdx >= 0 && tbl->hasIndex(static_cast<size_t>(colIdx)));
    });

    proto.def("dropIndex", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.dropIndex: invalid receiver");
        const int colIdx = resolveColumnArg(tbl->schema(), argAt(args, 0));
        return ev::fromBool(colIdx >= 0 && tbl->dropIndex(static_cast<size_t>(colIdx)));
    });

    proto.def("snapshot", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.snapshot: invalid receiver");
        return wrapHostTableSnapshot(tbl->snapshot());
    });

    proto.def("chunkRowCount", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::fromDouble(0);
        const size_t ci = static_cast<size_t>(numAt(args, 0));
        if (ci >= tbl->chunkCount()) return ev::fromDouble(0);
        return ev::fromDouble(tbl->chunk(ci).rowCount);
    });

    proto.def("chunkColumnView", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.chunkColumnView: invalid receiver");
        const size_t ci = static_cast<size_t>(numAt(args, 0));
        if (ci >= tbl->chunkCount()) return ev::undefined();
        const int colIdx = resolveColumnArg(tbl->schema(), argAt(args, 1));
        if (colIdx < 0) return ev::undefined();
        const auto& ch = tbl->chunk(ci);
        return chunkColumnTypedView(*ch.columns[static_cast<size_t>(colIdx)],
                                    ch.columns[static_cast<size_t>(colIdx)],
                                    ch.rowCount);
    });

    proto.def("decodeDict", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::fromUtf8("");
        return ev::fromUtf8(tbl->internDecode(u32At(args, 0)));
    });

    proto.def("query", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.query: invalid receiver");
        return createQueryBuilderValue(tbl, nullptr);
    });

    proto.def("where", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.where: invalid receiver");
        ev::Persistent qb(createQueryBuilderValue(tbl, nullptr));
        Value fn = ev::getProperty(qb.get(), "where");
        Value arg = argAt(args, 0);
        ev::CallResult res = ev::call(fn, qb.get(), std::span<const Value>(&arg, 1));
        return res.thrown ? ev::undefined() : res.value;
    });

    proto.def("select", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.select: invalid receiver");
        ev::Persistent qb(createQueryBuilderValue(tbl, nullptr));
        Value fn = ev::getProperty(qb.get(), "select");
        ev::CallResult res = ev::call(fn, qb.get(), args);
        return res.thrown ? ev::undefined() : res.value;
    });

    proto.def("updateWhere", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.updateWhere: invalid receiver");
        std::vector<std::shared_ptr<const JsExprNode>> filters;
        std::string err;
        if (hasArg(args, 0) && !appendWhereCondition(args[0], filters, err)) {
            return ev::throwTypeError("Table.updateWhere: " + err);
        }
        return applyTableUpdateDirect(tbl, filters, argAt(args, 1));
    });

    proto.def("deleteWhere", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.deleteWhere: invalid receiver");
        std::vector<std::shared_ptr<const JsExprNode>> filters;
        std::string err;
        if (hasArg(args, 0) && !appendWhereCondition(args[0], filters, err)) {
            return ev::throwTypeError("Table.deleteWhere: " + err);
        }
        return applyTableDeleteDirect(tbl, filters);
    });

    proto.def("toArray", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.toArray: invalid receiver");
        auto snap = tbl->snapshot();
        return tableRowsToJsArray(*snap);
    });
}

void decorateSnapshotProto(ev::ObjectBuilder& proto) {
    proto.accessor("rowCount", [](Value thisVal, std::span<const Value>) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        return snap ? ev::fromDouble(static_cast<double>(snap->rowCount())) : ev::fromDouble(0);
    }, nullptr);

    proto.accessor("columnCount", [](Value thisVal, std::span<const Value>) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        return snap ? ev::fromDouble(static_cast<double>(snap->columnCount())) : ev::fromDouble(0);
    }, nullptr);

    proto.accessor("chunkCount", [](Value thisVal, std::span<const Value>) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        return snap ? ev::fromDouble(static_cast<double>(snap->chunkCount())) : ev::fromDouble(0);
    }, nullptr);

    proto.accessor("version", [](Value thisVal, std::span<const Value>) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        return snap ? ev::fromDouble(static_cast<double>(snap->version)) : ev::fromDouble(0);
    }, nullptr);

    proto.accessor("schema", [](Value thisVal, std::span<const Value>) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        return snap ? schemaToJsArray(snap->schema) : makeEmptyArray();
    }, nullptr);

    proto.def("get", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.get: invalid receiver");
        const int colIdx = resolveColumnArg(snap->schema, argAt(args, 1));
        if (colIdx < 0) return ev::undefined();
        size_t target = static_cast<size_t>(numAt(args, 0));
        size_t acc = 0;
        for (uint32_t ci = 0; ci < snap->chunks.size(); ++ci) {
            const auto& fc = snap->chunks[ci];
            if (acc + fc.aliveCount <= target) {
                acc += fc.aliveCount;
                continue;
            }
            for (uint32_t r = 0; r < fc.rowCount; ++r) {
                if (fc.isAlive(r)) {
                    if (acc == target) return scalarToJsValue(snap->getCell(colIdx, ci, r));
                    ++acc;
                }
            }
        }
        return ev::undefined();
    });

    proto.def("row", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.row: invalid receiver");
        size_t target = static_cast<size_t>(numAt(args, 0));
        size_t acc = 0;
        for (uint32_t ci = 0; ci < snap->chunks.size(); ++ci) {
            const auto& fc = snap->chunks[ci];
            if (acc + fc.aliveCount <= target) {
                acc += fc.aliveCount;
                continue;
            }
            for (uint32_t r = 0; r < fc.rowCount; ++r) {
                if (fc.isAlive(r)) {
                    if (acc == target) return tableRowToJsObject(*snap, ci, r);
                    ++acc;
                }
            }
        }
        return ev::undefined();
    });

    proto.def("chunkRowCount", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::fromDouble(0);
        const size_t ci = static_cast<size_t>(numAt(args, 0));
        if (ci >= snap->chunks.size()) return ev::fromDouble(0);
        return ev::fromDouble(snap->chunks[ci].rowCount);
    });

    proto.def("chunkColumnView", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.chunkColumnView: invalid receiver");
        const size_t ci = static_cast<size_t>(numAt(args, 0));
        if (ci >= snap->chunks.size()) return ev::undefined();
        const int colIdx = resolveColumnArg(snap->schema, argAt(args, 1));
        if (colIdx < 0) return ev::undefined();
        const auto& fc = snap->chunks[ci];
        return chunkColumnTypedView(*fc.columns[static_cast<size_t>(colIdx)],
                                    fc.columns[static_cast<size_t>(colIdx)],
                                    fc.rowCount);
    });

    proto.def("decodeDict", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap || !snap->dict) return ev::fromUtf8("");
        return ev::fromUtf8(snap->dict->get(u32At(args, 0)));
    });

    proto.def("query", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.query: invalid receiver");
        return createQueryBuilderValue(nullptr, snap);
    });

    proto.def("where", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.where: invalid receiver");
        ev::Persistent qb(createQueryBuilderValue(nullptr, snap));
        Value fn = ev::getProperty(qb.get(), "where");
        Value arg = argAt(args, 0);
        ev::CallResult res = ev::call(fn, qb.get(), std::span<const Value>(&arg, 1));
        return res.thrown ? ev::undefined() : res.value;
    });

    proto.def("select", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.select: invalid receiver");
        ev::Persistent qb(createQueryBuilderValue(nullptr, snap));
        Value fn = ev::getProperty(qb.get(), "select");
        ev::CallResult res = ev::call(fn, qb.get(), args);
        return res.thrown ? ev::undefined() : res.value;
    });

    proto.def("toArray", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.toArray: invalid receiver");
        return tableRowsToJsArray(*snap);
    });
}

}  // namespace

void ensureTableClassesInstalled() {
    if (!g_tableClass.installed()) {
        g_tableClass.install("Table", 0, nullptr, decorateTableProto, /*global=*/false);
    }
    if (!g_snapshotClass.installed()) {
        g_snapshotClass.install("TableSnapshot", 0, nullptr, decorateSnapshotProto, /*global=*/false);
    }
}

std::shared_ptr<bro::table::Table> hostMutableTableOf(Value val) {
    if (!g_tableClass.installed()) return nullptr;
    auto* h = static_cast<HostTableHandle*>(g_tableClass.unwrap(val));
    return h ? h->table : nullptr;
}

std::shared_ptr<const bro::table::TableSnapshot> hostTableSnapshotOf(Value val) {
    if (!g_snapshotClass.installed()) return nullptr;
    auto* h = static_cast<HostTableSnapshotHandle*>(g_snapshotClass.unwrap(val));
    return h ? h->snapshot : nullptr;
}

std::shared_ptr<const bro::table::TableSnapshot> snapshotOrTableOf(Value val) {
    if (auto snap = hostTableSnapshotOf(val)) return snap;
    if (auto tbl = hostMutableTableOf(val)) return tbl->snapshot();
    return nullptr;
}

Value wrapHostTable(std::shared_ptr<bro::table::Table> table) {
    ensureTableClassesInstalled();
    auto handle = std::make_unique<HostTableHandle>();
    handle->table = std::move(table);
    return g_tableClass.createInstance(std::move(handle));
}

Value wrapHostTableSnapshot(std::shared_ptr<const bro::table::TableSnapshot> snapshot) {
    ensureTableClassesInstalled();
    auto handle = std::make_unique<HostTableSnapshotHandle>();
    handle->snapshot = std::move(snapshot);
    return g_snapshotClass.createInstance(std::move(handle));
}

bro::table::ScalarValue jsValueToScalar(Value v, bro::table::ColType targetType, bool hasTarget) {
    using namespace bro::table;
    if (v.isUndefined() || v.isNull()) return ScalarValue::makeNull();
    if (!hasTarget) {
        if (v.isBool()) return ScalarValue::makeBool(v.asBool());
        if (v.isNumber()) {
            const double d = v.asNumber();
            if (std::isfinite(d) && std::trunc(d) == d &&
                d >= -2147483648.0 && d <= 2147483647.0 && !std::signbit(d)) {
                return ScalarValue::makeI32(static_cast<int32_t>(d));
            }
            if (std::isfinite(d) && std::trunc(d) == d &&
                d >= -2147483648.0 && d <= 2147483647.0 && d != 0.0) {
                return ScalarValue::makeI32(static_cast<int32_t>(d));
            }
            if (std::isfinite(d) && std::trunc(d) == d &&
                d >= -9007199254740991.0 && d <= 9007199254740991.0 && d != 0.0) {
                return ScalarValue::makeI64(static_cast<int64_t>(d));
            }
            return ScalarValue::makeF64(d);
        }
        if (ev::isString(v)) return ScalarValue::makeString(ev::toUtf8(v));
        return ScalarValue::makeI64(ev::toInt64(v));
    }

    switch (targetType) {
    case ColType::I32:
        return ScalarValue::makeI32(jsToInt32(v.isNumber() ? v.asNumber() : ev::toDouble(v)));
    case ColType::U32:
        return ScalarValue::makeU32(jsToUint32(v.isNumber() ? v.asNumber() : ev::toDouble(v)));
    case ColType::I64:
        if (v.isNumber()) return ScalarValue::makeI64(static_cast<int64_t>(v.asNumber()));
        return ScalarValue::makeI64(ev::toInt64(v));
    case ColType::F32:
        return ScalarValue::makeF32(static_cast<float>(v.isNumber() ? v.asNumber() : ev::toDouble(v)));
    case ColType::F64:
        return ScalarValue::makeF64(v.isNumber() ? v.asNumber() : ev::toDouble(v));
    case ColType::Bool:
        return ScalarValue::makeBool(v.isBool() ? v.asBool() : ev::toBool(v));
    case ColType::Dict:
        if (v.isNumber()) return ScalarValue::makeDict(jsToUint32(v.asNumber()));
        return ScalarValue::makeString(ev::toUtf8(v));
    case ColType::String:
        return ScalarValue::makeString(ev::toUtf8(v));
    }
    return ScalarValue::makeNull();
}

Value scalarToJsValue(const bro::table::ScalarValue& sv) {
    using namespace bro::table;
    if (sv.isNull) return ev::null();
    switch (sv.type) {
    case ColType::I32: return ev::fromDouble(static_cast<double>(sv.i64));
    case ColType::U32: return ev::fromDouble(static_cast<double>(sv.u64));
    case ColType::I64: return ev::fromDouble(static_cast<double>(sv.i64));
    case ColType::F32: return ev::fromDouble(sv.f64);
    case ColType::F64: return ev::fromDouble(sv.f64);
    case ColType::Bool: return ev::fromBool(sv.i64 != 0);
    case ColType::Dict:
    case ColType::String:
        return ev::fromUtf8(sv.str);
    }
    return ev::undefined();
}

Value tableRowToJsObject(const bro::table::TableSnapshot& snap, uint32_t chunkIdx, uint32_t rowIdx) {
    ObjectBuilder obj;
    for (size_t c = 0; c < snap.columnCount(); ++c) {
        obj.set(snap.schema.columns[c].name.c_str(),
                scalarToJsValue(snap.getCell(c, chunkIdx, rowIdx)));
    }
    return obj.get();
}

Value tableRowsToJsArray(const bro::table::TableSnapshot& snap) {
    std::vector<bro::table::RowLoc> aliveLocs;
    aliveLocs.reserve(snap.rowCount());
    for (uint32_t ci = 0; ci < snap.chunks.size(); ++ci) {
        const auto& fc = snap.chunks[ci];
        for (uint32_t r = 0; r < fc.rowCount; ++r) {
            if (fc.isAlive(r)) aliveLocs.push_back(bro::table::RowLoc{ci, r});
        }
    }
    return ev::hostArrayOf(aliveLocs.size(), [&snap, &aliveLocs](size_t i) -> Value {
        return tableRowToJsObject(snap, aliveLocs[i].chunkIdx, aliveLocs[i].rowIdx);
    });
}

Value chunkColumnTypedView(const bro::table::ColumnChunk& colChunk,
                           const std::shared_ptr<const bro::table::ColumnChunk>& keepAlive,
                           uint32_t rowCount) {
    using namespace bro::table;
    bronze::ElementKind kind{};
    switch (colChunk.type()) {
    case ColType::I32: kind = ev::elements::Int32; break;
    case ColType::U32:
    case ColType::Dict:
        kind = ev::elements::Uint32;
        break;
    case ColType::I64: kind = ev::elements::BigInt64; break;
    case ColType::F32: kind = ev::elements::Float32; break;
    case ColType::F64: kind = ev::elements::Float64; break;
    case ColType::Bool: kind = ev::elements::Uint8; break;
    case ColType::String: return ev::undefined();
    }
    if (rowCount == 0) {
        return ev::createTypedArray(kind, 0);
    }
    const uint32_t byteLen =
        rowCount * static_cast<uint32_t>(colTypeElemSize(colChunk.type()));
    auto* pin = new std::shared_ptr<const ColumnChunk>(keepAlive);
    uint8_t* ptr = const_cast<uint8_t*>(colChunk.rawData());
    ev::Persistent ab(ev::createExternalArrayBuffer(ptr, byteLen, releaseChunkPin, pin));
    return ev::createTypedArrayView(kind, ab.get(), 0, rowCount);
}

Value makeBroTableValue() {
    ensureTableClassesInstalled();
    ev::ObjectBuilder ns;
    ns.def("create", 1, [](Value, std::span<const Value> args) -> Value {
        std::vector<bro::table::ColumnDef> defs;
        std::string err;
        if (!hasArg(args, 0) || !parseSchemaSpec(args[0], defs, err)) {
            return ev::throwTypeError("bro.table.create: " + err);
        }
        return wrapHostTable(std::make_shared<bro::table::Table>(std::move(defs)));
    });
    ns.def("jitCacheStats", 0, [](Value, std::span<const Value>) -> Value {
        auto st = bro::table::CompiledPlanCache::instance().stats();
        ObjectBuilder obj;
        obj.set("hits", ev::fromDouble(static_cast<double>(st.hits)));
        obj.set("misses", ev::fromDouble(static_cast<double>(st.misses)));
        obj.set("entries", ev::fromDouble(static_cast<double>(st.entries)));
        return obj.get();
    });
    ns.def("clearJitCache", 0, [](Value, std::span<const Value>) -> Value {
        bro::table::CompiledPlanCache::instance().clear();
        return ev::undefined();
    });
    ns.set("CHUNK_ROWS", static_cast<uint32_t>(bro::table::kChunkRows));
    populateQueryNamespace(ns);
    return ns.obj.get();
}

}  // namespace bro::bronze_host
