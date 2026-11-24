#include "bronze_host/host_table.h"

#include "bronze_host/host_access.h"
#include "bronze_host/host_builder.h"
#include "embed/embed_typed_array.h"
#include "table/table_ingest_csv.h"
#include "table/table_ingest_parquet.h"
#include "table/table_io_bundle.h"
#include "util/asset_path.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

using namespace bro::table;

Value ioBytesToUint8Array(const std::vector<uint8_t>& bytes) {
    Value arr = ev::createTypedArray(ev::elements::Uint8, static_cast<uint32_t>(bytes.size()));
    if (!bytes.empty()) {
        if (auto ta = ev::typedArrayInfo(arr); ta && ta.data && ta.byteLength >= bytes.size()) {
            std::memcpy(ta.data, bytes.data(), bytes.size());
        }
    }
    return arr;
}

std::vector<std::string> ownObjectKeys(Value objIn) {
    std::vector<std::string> keys;
    ev::Persistent obj(objIn);
    ev::Persistent objCtor(ev::globalValue("Object").value);
    ev::Persistent keysFn(ev::getProperty(objCtor.get(), "keys"));
    Value arg = obj.get();
    ev::CallResult r = ev::call(keysFn.get(), objCtor.get(), std::span<const Value>(&arg, 1));
    if (r.thrown) return keys;
    ev::Persistent arr(r.value);
    Value lenVal = ev::getProperty(arr.get(), "length");
    if (!lenVal.isNumber() || lenVal.asNumber() < 0.0) return keys;
    const uint32_t len = static_cast<uint32_t>(lenVal.asNumber());
    keys.reserve(len);
    for (uint32_t k = 0; k < len; ++k) {
        keys.push_back(ev::toUtf8(ev::getElement(arr.get(), k)));
    }
    return keys;
}

bool parseCompressFlag(Value v) {
    if (v.isUndefined() || v.isNull()) return false;
    if (ev::isString(v)) {
        const std::string s = ev::toUtf8(v);
        return s == "lz4" || s == "LZ4" || s == "true";
    }
    return ev::toBool(v);
}

CsvIngestOptions parseCsvOptions(Value optsVal) {
    CsvIngestOptions opts;
    if (!ev::isObject(optsVal)) return opts;
    ev::Persistent optsObj(optsVal);

    Value dVal = ev::getProperty(optsObj.get(), "delimiter");
    if (ev::isString(dVal)) {
        const std::string s = ev::toUtf8(dVal);
        if (s == "\\t" || s == "tab") opts.delimiter = '\t';
        else if (!s.empty()) opts.delimiter = s[0];
    }
    Value qVal = ev::getProperty(optsObj.get(), "quote");
    if (ev::isString(qVal)) {
        const std::string s = ev::toUtf8(qVal);
        if (!s.empty()) opts.quote = s[0];
    }
    Value hVal = ev::getProperty(optsObj.get(), "header");
    if (hVal.isUndefined()) hVal = ev::getProperty(optsObj.get(), "hasHeader");
    if (!hVal.isUndefined()) opts.hasHeader = ev::toBool(hVal);

    Value skipVal = ev::getProperty(optsObj.get(), "skipRows");
    if (skipVal.isNumber()) opts.skipRows = static_cast<size_t>(std::max(0.0, skipVal.asNumber()));

    Value maxVal = ev::getProperty(optsObj.get(), "maxRows");
    if (maxVal.isNumber()) opts.maxRows = static_cast<size_t>(std::max(0.0, maxVal.asNumber()));

    Value sampleVal = ev::getProperty(optsObj.get(), "inferSampleRows");
    if (sampleVal.isNumber()) {
        opts.inferSampleRows = static_cast<size_t>(std::max(1.0, sampleVal.asNumber()));
    }

    Value dictVal = ev::getProperty(optsObj.get(), "dictMaxCardinality");
    if (dictVal.isNumber()) {
        opts.dictMaxCardinality = static_cast<size_t>(std::max(0.0, dictVal.asNumber()));
    }

    Value schemaVal = ev::getProperty(optsObj.get(), "schema");
    if (ev::isObject(schemaVal)) {
        ev::Persistent sObj(schemaVal);
        std::vector<std::string> keys = ownObjectKeys(sObj.get());
        for (const std::string& k : keys) {
            Value tv = ev::getProperty(sObj.get(), k.c_str());
            if (!ev::isString(tv)) continue;
            ColType ct = ColType::String;
            if (!parseColType(ev::toUtf8(tv), ct)) {
                ev::throwTypeError("fromCSV: invalid schema column type for '" + k + "'");
                return opts;
            }
            opts.explicitSchema.push_back(ColumnDef{k, ct});
        }
    }
    return opts;
}

ParquetIngestOptions parseParquetIngestOptions(Value optsVal) {
    ParquetIngestOptions opts;
    if (!ev::isObject(optsVal)) return opts;
    ev::Persistent optsObj(optsVal);

    Value colsVal = ev::getProperty(optsObj.get(), "columns");
    if (isJsArray(colsVal)) {
        ev::Persistent arr(colsVal);
        const uint32_t n = static_cast<uint32_t>(std::max(0.0, ev::toDouble(ev::getProperty(arr.get(), "length"))));
        opts.columns.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            Value cv = ev::getElement(arr.get(), i);
            if (ev::isString(cv)) opts.columns.push_back(ev::toUtf8(cv));
        }
    }
    Value maxVal = ev::getProperty(optsObj.get(), "maxRows");
    if (maxVal.isNumber()) opts.maxRows = static_cast<size_t>(std::max(0.0, maxVal.asNumber()));
    Value dictVal = ev::getProperty(optsObj.get(), "stringAsDict");
    if (!dictVal.isUndefined()) opts.stringAsDict = ev::toBool(dictVal);
    return opts;
}

ParquetSaveOptions parseParquetSaveOptions(Value optsVal) {
    ParquetSaveOptions opts;
    if (!ev::isObject(optsVal)) return opts;
    ev::Persistent optsObj(optsVal);
    Value cVal = ev::getProperty(optsObj.get(), "compress");
    if (!cVal.isUndefined()) opts.compressLz4 = parseCompressFlag(cVal);
    Value rgVal = ev::getProperty(optsObj.get(), "rowGroupRows");
    if (rgVal.isNumber()) opts.rowGroupRows = static_cast<uint32_t>(std::max(1.0, rgVal.asNumber()));
    return opts;
}

}  // namespace

void attachTableIoExtProto(ev::ObjectBuilder& proto) {
    proto.def("saveParquet", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.saveParquet: invalid receiver");
        if (!hasArg(args, 0) || !ev::isString(args[0])) {
            return ev::throwTypeError("Table.saveParquet: path must be a string");
        }
        std::string resolved = util::resolveAssetWritePath(ev::toUtf8(args[0]));
        requirePathAccess(resolved, true);
        ParquetSaveOptions opts = hasArg(args, 1) ? parseParquetSaveOptions(args[1]) : ParquetSaveOptions{};
        std::string err;
        if (!saveSnapshotToParquetFile(*tbl->snapshot(), resolved, opts, &err)) {
            return ev::throwError("Table.saveParquet: " + err);
        }
        return ev::fromBool(true);
    });

    proto.def("toParquetBytes", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.toParquetBytes: invalid receiver");
        ParquetSaveOptions opts = hasArg(args, 0) ? parseParquetSaveOptions(args[0]) : ParquetSaveOptions{};
        std::string err;
        std::vector<uint8_t> bytes = serializeSnapshotToParquetBytes(*tbl->snapshot(), opts, &err);
        if (bytes.empty()) return ev::throwError("Table.toParquetBytes: " + err);
        return ioBytesToUint8Array(bytes);
    });
}

void attachSnapshotIoExtProto(ev::ObjectBuilder& proto) {
    proto.def("saveParquet", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.saveParquet: invalid receiver");
        if (!hasArg(args, 0) || !ev::isString(args[0])) {
            return ev::throwTypeError("TableSnapshot.saveParquet: path must be a string");
        }
        std::string resolved = util::resolveAssetWritePath(ev::toUtf8(args[0]));
        requirePathAccess(resolved, true);
        ParquetSaveOptions opts = hasArg(args, 1) ? parseParquetSaveOptions(args[1]) : ParquetSaveOptions{};
        std::string err;
        if (!saveSnapshotToParquetFile(*snap, resolved, opts, &err)) {
            return ev::throwError("TableSnapshot.saveParquet: " + err);
        }
        return ev::fromBool(true);
    });

    proto.def("toParquetBytes", 1, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.toParquetBytes: invalid receiver");
        ParquetSaveOptions opts = hasArg(args, 0) ? parseParquetSaveOptions(args[0]) : ParquetSaveOptions{};
        std::string err;
        std::vector<uint8_t> bytes = serializeSnapshotToParquetBytes(*snap, opts, &err);
        if (bytes.empty()) return ev::throwError("TableSnapshot.toParquetBytes: " + err);
        return ioBytesToUint8Array(bytes);
    });
}

void populateTableIoExtNamespace(ev::ObjectBuilder& ns) {
    ns.def("fromCSV", 2, [](Value, std::span<const Value> args) -> Value {
        if (!hasArg(args, 0)) return ev::throwTypeError("bro.table.fromCSV: expects (csvStringOrBytes, opts?)");
        CsvIngestOptions opts = hasArg(args, 1) ? parseCsvOptions(args[1]) : CsvIngestOptions{};
        std::string err;
        std::shared_ptr<Table> tbl;
        if (ev::isString(args[0])) {
            std::string s = ev::toUtf8(args[0]);
            tbl = ingestCsvFromMemory(s.data(), s.size(), opts, &err);
        } else if (auto ta = ev::typedArrayInfo(args[0]); ta && ta.data) {
            tbl = ingestCsvFromMemory(reinterpret_cast<const char*>(ta.data), ta.byteLength, opts, &err);
        } else {
            return ev::throwTypeError("bro.table.fromCSV: expected string or TypedArray");
        }
        if (!tbl) return ev::throwError("bro.table.fromCSV: " + err);
        return wrapHostTable(std::move(tbl));
    });

    ns.def("fromCSVFile", 2, [](Value, std::span<const Value> args) -> Value {
        if (!hasArg(args, 0) || !ev::isString(args[0])) {
            return ev::throwTypeError("bro.table.fromCSVFile: path must be a string");
        }
        std::string resolved = readableAssetPath(ev::toUtf8(args[0]));
        CsvIngestOptions opts = hasArg(args, 1) ? parseCsvOptions(args[1]) : CsvIngestOptions{};
        std::string err;
        auto tbl = ingestCsvFromFile(resolved, opts, &err);
        if (!tbl) return ev::throwError("bro.table.fromCSVFile: " + err);
        return wrapHostTable(std::move(tbl));
    });

    ns.def("fromParquet", 2, [](Value, std::span<const Value> args) -> Value {
        if (!hasArg(args, 0)) return ev::throwTypeError("bro.table.fromParquet: expects (Uint8Array, opts?)");
        auto ta = ev::typedArrayInfo(args[0]);
        if (!ta || !ta.data) return ev::throwTypeError("bro.table.fromParquet: expected TypedArray");
        ParquetIngestOptions opts = hasArg(args, 1) ? parseParquetIngestOptions(args[1]) : ParquetIngestOptions{};
        std::vector<uint8_t> copy(static_cast<const uint8_t*>(ta.data),
                                  static_cast<const uint8_t*>(ta.data) + ta.byteLength);
        std::string err;
        auto tbl = ingestParquetFromMemory(copy.data(), copy.size(), opts, &err);
        if (!tbl) return ev::throwError("bro.table.fromParquet: " + err);
        return wrapHostTable(std::move(tbl));
    });

    ns.def("fromParquetFile", 2, [](Value, std::span<const Value> args) -> Value {
        if (!hasArg(args, 0) || !ev::isString(args[0])) {
            return ev::throwTypeError("bro.table.fromParquetFile: path must be a string");
        }
        std::string resolved = readableAssetPath(ev::toUtf8(args[0]));
        ParquetIngestOptions opts = hasArg(args, 1) ? parseParquetIngestOptions(args[1]) : ParquetIngestOptions{};
        std::string err;
        auto tbl = ingestParquetFromFile(resolved, opts, &err);
        if (!tbl) return ev::throwError("bro.table.fromParquetFile: " + err);
        return wrapHostTable(std::move(tbl));
    });

    ns.def("saveBundle", 3, [](Value, std::span<const Value> args) -> Value {
        if (!hasArg(args, 0) || !ev::isString(args[0]) || !hasArg(args, 1) || !ev::isObject(args[1])) {
            return ev::throwTypeError("bro.table.saveBundle: expects (path, { name: tableOrSnapshot }, opts?)");
        }
        std::string resolved = util::resolveAssetWritePath(ev::toUtf8(args[0]));
        requirePathAccess(resolved, true);

        ev::Persistent mapObj(args[1]);
        std::vector<std::string> keys = ownObjectKeys(mapObj.get());
        std::vector<BundleEntryInput> entries;
        entries.reserve(keys.size());
        for (const std::string& k : keys) {
            Value tv = ev::getProperty(mapObj.get(), k.c_str());
            auto snap = snapshotOrTableOf(tv);
            if (!snap) {
                return ev::throwTypeError("bro.table.saveBundle: entry '" + k + "' is not a Table or TableSnapshot");
            }
            entries.push_back(BundleEntryInput{k, std::move(snap)});
        }

        SaveOptions opts;
        if (hasArg(args, 2) && ev::isObject(args[2])) {
            opts.compress = parseCompressFlag(ev::getProperty(args[2], "compress"));
        }

        std::string err;
        if (!saveBundleToFile(entries, resolved, opts, &err)) {
            return ev::throwError("bro.table.saveBundle: " + err);
        }
        return ev::fromBool(true);
    });

    ns.def("loadBundle", 1, [](Value, std::span<const Value> args) -> Value {
        if (!hasArg(args, 0) || !ev::isString(args[0])) {
            return ev::throwTypeError("bro.table.loadBundle: path must be a string");
        }
        std::string resolved = readableAssetPath(ev::toUtf8(args[0]));
        std::vector<LoadedBundleEntry> entries;
        std::string err;
        if (!loadBundleFromFile(resolved, entries, &err)) {
            return ev::throwError("bro.table.loadBundle: " + err);
        }
        ObjectBuilder out;
        for (auto& e : entries) {
            ev::Persistent tv(wrapHostTable(std::move(e.table)));
            out.set(e.name.c_str(), tv.get());
        }
        return out.get();
    });

    ns.def("mmapBundle", 2, [](Value, std::span<const Value> args) -> Value {
        if (!hasArg(args, 0) || !ev::isString(args[0])) {
            return ev::throwTypeError("bro.table.mmapBundle: path must be a string");
        }
        bool verify = false;
        if (hasArg(args, 1) && ev::isObject(args[1])) {
            Value v = ev::getProperty(args[1], "verify");
            if (!v.isUndefined()) verify = ev::toBool(v);
        }
        std::string resolved = readableAssetPath(ev::toUtf8(args[0]));
        std::vector<MappedBundleEntry> entries;
        std::string err;
        if (!mmapBundleFromFile(resolved, entries, &err, verify)) {
            return ev::throwError("bro.table.mmapBundle: " + err);
        }
        ObjectBuilder out;
        for (auto& e : entries) {
            ev::Persistent sv(wrapHostTableSnapshot(std::move(e.snapshot)));
            out.set(e.name.c_str(), sv.get());
        }
        return out.get();
    });
}

}  // namespace bro::bronze_host
