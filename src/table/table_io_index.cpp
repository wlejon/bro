#include "table/table_io_internal.h"

#include <cmath>
#include <limits>

namespace bro::table::io_internal {

namespace {

bool isValidPowerOfTwoOrZero(uint64_t n) {
    return n == 0 || (n & (n - 1)) == 0;
}

}  // namespace

void writeIndexSectionV3(const TableSnapshot& snap, ByteSink& sink) {
    const std::vector<size_t> hashCols = snap.hashIndexCols();
    const std::vector<size_t> rangeCols = snap.rangeIndexCols();
    const std::vector<const SpatialIndex*> spatials = snap.spatialIndexList();

    for (size_t c : hashCols) {
        const HashIndex* idx = snap.findIndex(c);
        if (!idx) continue;
        const uint64_t bCnt = static_cast<uint64_t>(idx->bucketCount());
        const uint64_t nCnt = static_cast<uint64_t>(idx->nodeCount());
        sink.writeMetaU32(static_cast<uint32_t>(c));
        sink.writeMetaU8(static_cast<uint8_t>(idx->colType()));
        sink.writeMetaU64(bCnt);
        sink.writeMetaU64(nCnt);
        sink.writeMetaU64(static_cast<uint64_t>(idx->usedBuckets()));
        sink.writeMetaU64(static_cast<uint64_t>(idx->entryCount()));
        sink.padToAlign64();
        if (bCnt > 0) {
            sink.write(idx->bucketData(), static_cast<size_t>(bCnt) * sizeof(HashIndex::Bucket));
        }
        sink.padToAlign64();
        if (nCnt > 0) {
            sink.write(idx->nodeData(), static_cast<size_t>(nCnt) * sizeof(HashIndex::Node));
        }
        if (!sink.ok()) return;
    }

    for (size_t c : rangeCols) {
        const RangeIndex* idx = snap.findRangeIndex(c);
        if (!idx) continue;
        idx->sortIfNeeded();
        const bool isStr = idx->isStringOrdered();
        uint64_t entryCount = 0;
        uint64_t blobBytes = 0;
        std::vector<RangeIndex::MappedStrEntry> strMeta;

        if (!isStr) {
            entryCount = static_cast<uint64_t>(idx->numCount());
        } else if (idx->isMapped()) {
            entryCount = static_cast<uint64_t>(idx->mappedStrCount());
            blobBytes = static_cast<uint64_t>(idx->mappedStrBlobBytes());
        } else {
            const auto& se = idx->strEntries();
            entryCount = static_cast<uint64_t>(se.size());
            strMeta.resize(se.size());
            uint64_t offAcc = 0;
            for (size_t i = 0; i < se.size(); ++i) {
                const size_t slen = se[i].key.size();
                if (slen > UINT32_MAX || offAcc > static_cast<uint64_t>(UINT32_MAX) - slen) {
                    sink.fail("string range index blob exceeds 4GB limit");
                    return;
                }
                strMeta[i].offset = static_cast<uint32_t>(offAcc);
                strMeta[i].len = static_cast<uint32_t>(slen);
                strMeta[i].loc = se[i].loc;
                offAcc += slen;
            }
            blobBytes = offAcc;
        }

        sink.writeMetaU32(static_cast<uint32_t>(c));
        sink.writeMetaU8(static_cast<uint8_t>(idx->colType()));
        sink.writeMetaU8(isStr ? 1u : 0u);
        sink.writeMetaU64(entryCount);
        sink.writeMetaU64(blobBytes);
        sink.padToAlign64();

        if (!isStr) {
            if (entryCount > 0) {
                sink.write(idx->numData(),
                           static_cast<size_t>(entryCount) * sizeof(RangeIndex::NumEntry));
            }
        } else {
            if (entryCount > 0) {
                const RangeIndex::MappedStrEntry* mp =
                    idx->isMapped() ? idx->mappedStrData() : strMeta.data();
                sink.write(mp, static_cast<size_t>(entryCount) * sizeof(RangeIndex::MappedStrEntry));
            }
            sink.padToAlign64();
            if (blobBytes > 0) {
                if (idx->isMapped()) {
                    sink.write(idx->mappedStrBlob(), static_cast<size_t>(blobBytes));
                } else {
                    for (const auto& s : idx->strEntries()) {
                        if (!s.key.empty()) sink.write(s.key.data(), s.key.size());
                    }
                }
            }
        }
        if (!sink.ok()) return;
    }

    for (const SpatialIndex* si : spatials) {
        if (!si) continue;
        sink.writeMetaU32(static_cast<uint32_t>(si->name().size()));
        sink.writeMeta(si->name().data(), si->name().size());
        sink.writeMetaU8(si->dims());
        sink.writeMetaF64(si->cellSize());
        for (uint8_t d = 0; d < si->dims(); ++d) {
            sink.writeMetaU32(static_cast<uint32_t>(si->cols()[d]));
        }
        const uint64_t bCnt = static_cast<uint64_t>(si->bucketCount());
        const uint64_t nCnt = static_cast<uint64_t>(si->nodeCount());
        sink.writeMetaU64(bCnt);
        sink.writeMetaU64(nCnt);
        sink.writeMetaU64(static_cast<uint64_t>(si->usedBuckets()));
        sink.writeMetaU64(static_cast<uint64_t>(si->entryCount()));
        sink.padToAlign64();
        if (bCnt > 0) {
            sink.write(si->bucketData(), static_cast<size_t>(bCnt) * sizeof(SpatialIndex::Bucket));
        }
        sink.padToAlign64();
        if (nCnt > 0) {
            sink.write(si->nodeData(), static_cast<size_t>(nCnt) * sizeof(SpatialIndex::Node));
        }
        if (!sink.ok()) return;
    }
}

bool parseIndexSectionV3(ByteCursor& cur,
                         uint32_t hashCount, uint32_t rangeCount, uint32_t spatialCount,
                         TableSnapshot& snap, const std::shared_ptr<const void>& owner,
                         std::string* err) {
    const size_t colCount = snap.schema.size();

    for (uint32_t i = 0; i < hashCount; ++i) {
        uint32_t col = 0;
        uint8_t typeByte = 0;
        uint64_t bCnt = 0, nCnt = 0, usedB = 0, entryCnt = 0;
        if (!cur.readMeta(col) || !cur.readMeta(typeByte) || !cur.readMeta(bCnt) ||
            !cur.readMeta(nCnt) || !cur.readMeta(usedB) || !cur.readMeta(entryCnt) ||
            col >= colCount || typeByte != static_cast<uint8_t>(snap.schema.columns[col].type) ||
            !isValidPowerOfTwoOrZero(bCnt) || bCnt > kMaxIndexElements ||
            nCnt > kMaxIndexElements || !cur.skipToAlign64()) {
            if (err) *err = "corrupted .brotable v3 hash index metadata";
            return false;
        }
        const uint64_t bucketBytes = bCnt * sizeof(HashIndex::Bucket);
        const uint64_t nodeBytes = nCnt * sizeof(HashIndex::Node);
        if (bucketBytes > (cur.size - cur.pos)) {
            if (err) *err = "corrupted .brotable v3 hash buckets";
            return false;
        }
        const auto* bucketsPtr = bCnt > 0
                                     ? reinterpret_cast<const HashIndex::Bucket*>(cur.ptr + cur.pos)
                                     : nullptr;
        cur.pos += static_cast<size_t>(bucketBytes);
        if (!cur.skipToAlign64() || nodeBytes > (cur.size - cur.pos)) {
            if (err) *err = "corrupted .brotable v3 hash nodes";
            return false;
        }
        const auto* nodesPtr = nCnt > 0
                                   ? reinterpret_cast<const HashIndex::Node*>(cur.ptr + cur.pos)
                                   : nullptr;
        cur.pos += static_cast<size_t>(nodeBytes);
        snap.indexes[col] = HashIndex::createMapped(
            col, static_cast<ColType>(typeByte),
            bucketsPtr, static_cast<size_t>(bCnt),
            nodesPtr, static_cast<size_t>(nCnt),
            static_cast<size_t>(usedB), static_cast<size_t>(entryCnt), owner);
    }

    for (uint32_t i = 0; i < rangeCount; ++i) {
        uint32_t col = 0;
        uint8_t typeByte = 0, mode = 0;
        uint64_t entryCnt = 0, blobBytes = 0;
        if (!cur.readMeta(col) || !cur.readMeta(typeByte) || !cur.readMeta(mode) ||
            !cur.readMeta(entryCnt) || !cur.readMeta(blobBytes) ||
            col >= colCount || typeByte != static_cast<uint8_t>(snap.schema.columns[col].type) ||
            entryCnt > kMaxIndexElements || !cur.skipToAlign64()) {
            if (err) *err = "corrupted .brotable v3 range index metadata";
            return false;
        }
        const ColType ct = static_cast<ColType>(typeByte);
        const bool expectStr = (ct == ColType::String || ct == ColType::Dict);
        if ((mode != 0) != expectStr) {
            if (err) *err = "corrupted .brotable v3 range index mode";
            return false;
        }
        if (!expectStr) {
            const uint64_t numBytes = entryCnt * sizeof(RangeIndex::NumEntry);
            if (numBytes > (cur.size - cur.pos)) {
                if (err) *err = "corrupted .brotable v3 numeric range index";
                return false;
            }
            const auto* entriesPtr =
                entryCnt > 0 ? reinterpret_cast<const RangeIndex::NumEntry*>(cur.ptr + cur.pos)
                             : nullptr;
            cur.pos += static_cast<size_t>(numBytes);
            snap.rangeIndexes[col] = RangeIndex::createMappedNumeric(
                col, ct, entriesPtr, static_cast<size_t>(entryCnt), owner);
        } else {
            const uint64_t strBytes = entryCnt * sizeof(RangeIndex::MappedStrEntry);
            if (strBytes > (cur.size - cur.pos)) {
                if (err) *err = "corrupted .brotable v3 string range entries";
                return false;
            }
            const auto* strPtr =
                entryCnt > 0
                    ? reinterpret_cast<const RangeIndex::MappedStrEntry*>(cur.ptr + cur.pos)
                    : nullptr;
            cur.pos += static_cast<size_t>(strBytes);
            if (!cur.skipToAlign64() || blobBytes > (cur.size - cur.pos)) {
                if (err) *err = "corrupted .brotable v3 string range blob";
                return false;
            }
            const char* blobPtr =
                blobBytes > 0 ? reinterpret_cast<const char*>(cur.ptr + cur.pos) : nullptr;
            cur.pos += static_cast<size_t>(blobBytes);
            snap.rangeIndexes[col] = RangeIndex::createMappedString(
                col, ct, strPtr, static_cast<size_t>(entryCnt),
                blobPtr, static_cast<size_t>(blobBytes), owner);
        }
    }

    for (uint32_t i = 0; i < spatialCount; ++i) {
        uint32_t nameLen = 0;
        if (!cur.readMeta(nameLen) || nameLen == 0 || nameLen > 4096 || !cur.has(nameLen)) {
            if (err) *err = "corrupted .brotable v3 spatial index name";
            return false;
        }
        std::string name(reinterpret_cast<const char*>(cur.ptr + cur.pos), nameLen);
        if (cur.metaHasher) cur.metaHasher->update(name.data(), nameLen);
        cur.pos += nameLen;

        uint8_t dims = 0;
        double cellSize = 0.0;
        if (!cur.readMeta(dims) || !cur.readMeta(cellSize) || (dims != 2 && dims != 3) ||
            !std::isfinite(cellSize) || cellSize <= 0.0) {
            if (err) *err = "corrupted .brotable v3 spatial dimensions";
            return false;
        }
        std::vector<size_t> cols(dims);
        for (uint8_t d = 0; d < dims; ++d) {
            uint32_t col = 0;
            if (!cur.readMeta(col) || col >= colCount) {
                if (err) *err = "corrupted .brotable v3 spatial column";
                return false;
            }
            const ColType ct = snap.schema.columns[col].type;
            if (ct == ColType::String || ct == ColType::Dict || ct == ColType::Bool) {
                if (err) *err = "invalid non-numeric spatial index column";
                return false;
            }
            cols[d] = col;
        }

        uint64_t bCnt = 0, nCnt = 0, usedB = 0, entryCnt = 0;
        if (!cur.readMeta(bCnt) || !cur.readMeta(nCnt) || !cur.readMeta(usedB) ||
            !cur.readMeta(entryCnt) || !isValidPowerOfTwoOrZero(bCnt) ||
            bCnt > kMaxIndexElements || nCnt > kMaxIndexElements || !cur.skipToAlign64()) {
            if (err) *err = "corrupted .brotable v3 spatial index metadata";
            return false;
        }
        const uint64_t bucketBytes = bCnt * sizeof(SpatialIndex::Bucket);
        const uint64_t nodeBytes = nCnt * sizeof(SpatialIndex::Node);
        if (bucketBytes > (cur.size - cur.pos)) {
            if (err) *err = "corrupted .brotable v3 spatial buckets";
            return false;
        }
        const auto* bucketsPtr =
            bCnt > 0 ? reinterpret_cast<const SpatialIndex::Bucket*>(cur.ptr + cur.pos) : nullptr;
        cur.pos += static_cast<size_t>(bucketBytes);
        if (!cur.skipToAlign64() || nodeBytes > (cur.size - cur.pos)) {
            if (err) *err = "corrupted .brotable v3 spatial nodes";
            return false;
        }
        const auto* nodesPtr =
            nCnt > 0 ? reinterpret_cast<const SpatialIndex::Node*>(cur.ptr + cur.pos) : nullptr;
        cur.pos += static_cast<size_t>(nodeBytes);

        std::string mapKey = name;
        snap.spatialIndexes.emplace(
            std::move(mapKey),
            SpatialIndex::createMapped(
                std::move(name), std::move(cols), cellSize,
                bucketsPtr, static_cast<size_t>(bCnt),
                nodesPtr, static_cast<size_t>(nCnt),
                static_cast<size_t>(usedB), static_cast<size_t>(entryCnt), owner));
    }

    return true;
}

void applyPersistedIndexes(Table& tbl, const PersistedIndexSpecs& specs) {
    for (size_t c : specs.hashCols) tbl.createIndex(c);
    for (size_t c : specs.rangeCols) tbl.createRangeIndex(c);
    for (const auto& sp : specs.spatials) {
        tbl.createSpatialIndex(sp.name, sp.cols, sp.cellSize);
    }
}

void applyPersistedIndexesToSnapshot(TableSnapshot& snap, const PersistedIndexSpecs& specs) {
    for (size_t c : specs.hashCols) {
        if (c >= snap.schema.size()) continue;
        auto idx = std::make_shared<HashIndex>(c, snap.schema.columns[c].type);
        idx->clear(snap.totalAliveRows);
        for (uint32_t ci = 0; ci < snap.chunks.size(); ++ci) {
            const auto& ch = snap.chunks[ci];
            const ColumnChunk& colChunk = *ch.columns[c];
            for (uint32_t r = 0; r < ch.rowCount; ++r) {
                if (ch.isAlive(r) && !colChunk.isNull(r)) {
                    idx->insert(HashIndex::cellKey(colChunk, r), RowLoc{ci, r});
                }
            }
        }
        snap.indexes[c] = std::move(idx);
    }
    for (size_t c : specs.rangeCols) {
        if (c >= snap.schema.size()) continue;
        auto idx = std::make_shared<RangeIndex>(c, snap.schema.columns[c].type);
        for (uint32_t ci = 0; ci < snap.chunks.size(); ++ci) {
            const auto& ch = snap.chunks[ci];
            const ColumnChunk& colChunk = *ch.columns[c];
            for (uint32_t r = 0; r < ch.rowCount; ++r) {
                if (ch.isAlive(r) && !colChunk.isNull(r)) {
                    idx->insert(colChunk, r, RowLoc{ci, r}, snap.dict.get());
                }
            }
        }
        idx->sortIfNeeded();
        snap.rangeIndexes[c] = std::move(idx);
    }
    for (const auto& sp : specs.spatials) {
        auto si = std::make_shared<SpatialIndex>(sp.name, sp.cols, sp.cellSize);
        si->clear(snap.totalAliveRows);
        for (uint32_t ci = 0; ci < snap.chunks.size(); ++ci) {
            const auto& ch = snap.chunks[ci];
            for (uint32_t r = 0; r < ch.rowCount; ++r) {
                if (!ch.isAlive(r)) continue;
                double coords[3] = {0.0, 0.0, 0.0};
                bool ok = true;
                for (uint8_t d = 0; d < si->dims(); ++d) {
                    const size_t col = si->cols()[d];
                    if (col >= ch.columns.size() || ch.columns[col]->isNull(r)) {
                        ok = false;
                        break;
                    }
                    ScalarValue sv = ch.columns[col]->readCell(r, snap.dict.get());
                    if (sv.isNull) { ok = false; break; }
                    if (colTypeIsFloat(sv.type)) coords[d] = sv.f64;
                    else if (colTypeIsUnsignedInt(sv.type)) coords[d] = static_cast<double>(sv.u64);
                    else coords[d] = static_cast<double>(sv.i64);
                }
                if (ok) si->insert(coords[0], coords[1], coords[2], RowLoc{ci, r});
            }
        }
        snap.spatialIndexes[sp.name] = std::move(si);
    }
}

}  // namespace bro::table::io_internal
