#include "table/table_ingest_parquet.h"

#include "table/table_codec.h"
#include "table/table_parquet_internal.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>

namespace bro::table {

using namespace parquet_detail;

std::vector<uint8_t> serializeSnapshotToParquetBytes(const TableSnapshot& snap,
                                                     const ParquetSaveOptions& opts,
                                                     std::string* outError) {
    (void)outError;
    std::vector<uint8_t> out;
    out.insert(out.end(), kParquetMagic, kParquetMagic + 4);

    const size_t nCols = snap.schema.size();
    std::vector<bool> colOptional(nCols, false);
    for (const auto& fc : snap.chunks) {
        for (size_t c = 0; c < nCols; ++c) {
            if (fc.columns[c]->hasNulls()) colOptional[c] = true;
        }
    }

    const uint32_t rgCap = std::max<uint32_t>(1, opts.rowGroupRows);
    std::vector<RowLoc> allLocs;
    allLocs.reserve(snap.rowCount());
    for (uint32_t ci = 0; ci < snap.chunks.size(); ++ci) {
        const auto& fc = snap.chunks[ci];
        for (uint32_t r = 0; r < fc.rowCount; ++r) {
            if (fc.isAlive(r)) allLocs.push_back(RowLoc{ci, r});
        }
    }

    std::vector<PqRowGroup> writtenGroups;
    const int32_t codec = opts.compressLz4 ? PQ_LZ4_RAW : PQ_UNCOMPRESSED;

    for (size_t rgStart = 0; rgStart < allLocs.size(); rgStart += rgCap) {
        const size_t rgEnd = std::min(allLocs.size(), rgStart + rgCap);
        const size_t rgRows = rgEnd - rgStart;
        PqRowGroup rg;
        rg.numRows = static_cast<int64_t>(rgRows);
        rg.columns.resize(nCols);

        for (size_t c = 0; c < nCols; ++c) {
            const ColType ct = snap.schema.columns[c].type;
            std::vector<uint8_t> isPresent(rgRows, 1);
            std::vector<uint8_t> plain;

            if (ct == ColType::Bool) {
                uint8_t curByte = 0;
                uint32_t bitIdx = 0;
                for (size_t i = rgStart; i < rgEnd; ++i) {
                    const RowLoc loc = allLocs[i];
                    const ColumnChunk& col = *snap.chunks[loc.chunkIdx].columns[c];
                    if (col.isNull(loc.rowIdx)) {
                        isPresent[i - rgStart] = 0;
                        continue;
                    }
                    if (col.rawData()[loc.rowIdx] != 0) curByte |= static_cast<uint8_t>(1u << bitIdx);
                    if (++bitIdx == 8) { plain.push_back(curByte); curByte = 0; bitIdx = 0; }
                }
                if (bitIdx > 0) plain.push_back(curByte);
            } else {
                for (size_t i = rgStart; i < rgEnd; ++i) {
                    const RowLoc loc = allLocs[i];
                    const ColumnChunk& col = *snap.chunks[loc.chunkIdx].columns[c];
                    if (col.isNull(loc.rowIdx)) {
                        isPresent[i - rgStart] = 0;
                        continue;
                    }
                    if (ct == ColType::I32 || ct == ColType::F32) {
                        const uint8_t* p = col.rawData() + static_cast<size_t>(loc.rowIdx) * 4;
                        plain.insert(plain.end(), p, p + 4);
                    } else if (ct == ColType::U32) {
                        uint32_t u = 0;
                        std::memcpy(&u, col.rawData() + static_cast<size_t>(loc.rowIdx) * 4, 4);
                        int64_t i64 = static_cast<int64_t>(u);
                        const uint8_t* p = reinterpret_cast<const uint8_t*>(&i64);
                        plain.insert(plain.end(), p, p + 8);
                    } else if (ct == ColType::I64 || ct == ColType::F64) {
                        const uint8_t* p = col.rawData() + static_cast<size_t>(loc.rowIdx) * 8;
                        plain.insert(plain.end(), p, p + 8);
                    } else if (ct == ColType::Dict) {
                        uint32_t id = 0;
                        std::memcpy(&id, col.rawData() + static_cast<size_t>(loc.rowIdx) * 4, 4);
                        std::string s = snap.dict ? snap.dict->get(id) : std::string{};
                        const uint32_t sl = static_cast<uint32_t>(s.size());
                        const uint8_t* lp = reinterpret_cast<const uint8_t*>(&sl);
                        plain.insert(plain.end(), lp, lp + 4);
                        plain.insert(plain.end(), s.begin(), s.end());
                    } else if (ct == ColType::String) {
                        std::string_view sv = col.getString(loc.rowIdx);
                        const uint32_t sl = static_cast<uint32_t>(sv.size());
                        const uint8_t* lp = reinterpret_cast<const uint8_t*>(&sl);
                        plain.insert(plain.end(), lp, lp + 4);
                        plain.insert(plain.end(), sv.begin(), sv.end());
                    }
                }
            }

            std::vector<uint8_t> uncompPage;
            if (colOptional[c]) writeDefLevelsV1(uncompPage, isPresent);
            uncompPage.insert(uncompPage.end(), plain.begin(), plain.end());
            const int32_t uSize = static_cast<int32_t>(uncompPage.size());

            std::vector<uint8_t> payload;
            if (codec == PQ_LZ4_RAW) lz4BlockCompress(uncompPage.data(), uncompPage.size(), payload);
            else payload = std::move(uncompPage);

            CompactWriter pw;
            writeDataPageHeader(pw, uSize, static_cast<int32_t>(payload.size()), static_cast<int32_t>(rgRows));

            const int64_t dataOffset = static_cast<int64_t>(out.size());
            out.insert(out.end(), pw.buf.begin(), pw.buf.end());
            out.insert(out.end(), payload.begin(), payload.end());

            PqColChunk& cc = rg.columns[c];
            cc.fileOffset = dataOffset;
            cc.meta.type = broTypeToParquetPhys(ct);
            cc.meta.codec = codec;
            cc.meta.numValues = static_cast<int64_t>(rgRows);
            cc.meta.dataPageOffset = dataOffset;
            cc.meta.totalCompressed = static_cast<int64_t>(pw.buf.size() + payload.size());
            cc.meta.path = {snap.schema.columns[c].name};
        }
        writtenGroups.push_back(std::move(rg));
    }

    CompactWriter fw;
    int16_t fPrev = 0;
    fw.fieldI32(fPrev, 1, 1);
    fw.field(fPrev, 2, kCompactList);
    fw.listHeader(static_cast<uint32_t>(nCols + 1), kCompactStruct);
    {
        int16_t sp = 0;
        fw.fieldBin(sp, 4, "schema");
        fw.fieldI32(sp, 5, static_cast<int32_t>(nCols));
        fw.structEnd();
    }
    for (size_t c = 0; c < nCols; ++c) {
        int16_t sp = 0;
        fw.fieldI32(sp, 1, broTypeToParquetPhys(snap.schema.columns[c].type));
        fw.fieldI32(sp, 3, colOptional[c] ? 1 : 0);
        fw.fieldBin(sp, 4, snap.schema.columns[c].name);
        fw.structEnd();
    }
    fw.fieldI64(fPrev, 3, static_cast<int64_t>(snap.rowCount()));
    fw.field(fPrev, 4, kCompactList);
    fw.listHeader(static_cast<uint32_t>(writtenGroups.size()), kCompactStruct);
    for (const PqRowGroup& rg : writtenGroups) {
        int16_t rp = 0;
        fw.field(rp, 1, kCompactList);
        fw.listHeader(static_cast<uint32_t>(rg.columns.size()), kCompactStruct);
        int64_t rgBytes = 0;
        for (const PqColChunk& cc : rg.columns) {
            rgBytes += cc.meta.totalCompressed;
            int16_t cp = 0;
            fw.fieldI64(cp, 2, cc.fileOffset);
            fw.field(cp, 3, kCompactStruct);
            int16_t mp = 0;
            fw.fieldI32(mp, 1, cc.meta.type);
            fw.field(mp, 2, kCompactList);
            fw.listHeader(1, kCompactI32);
            fw.zigI32(PQ_PLAIN);
            fw.field(mp, 3, kCompactList);
            fw.listHeader(1, kCompactBinary);
            fw.varU64(cc.meta.path[0].size());
            fw.buf.insert(fw.buf.end(), cc.meta.path[0].begin(), cc.meta.path[0].end());
            fw.fieldI32(mp, 4, cc.meta.codec);
            fw.fieldI64(mp, 5, cc.meta.numValues);
            fw.fieldI64(mp, 6, cc.meta.totalCompressed);
            fw.fieldI64(mp, 7, cc.meta.totalCompressed);
            fw.fieldI64(mp, 9, cc.meta.dataPageOffset);
            fw.structEnd();
            fw.structEnd();
        }
        fw.fieldI64(rp, 2, rgBytes);
        fw.fieldI64(rp, 3, rg.numRows);
        fw.structEnd();
    }
    fw.fieldBin(fPrev, 6, "bro.table");
    fw.structEnd();

    const uint32_t footerLen = static_cast<uint32_t>(fw.buf.size());
    out.insert(out.end(), fw.buf.begin(), fw.buf.end());
    const uint8_t* flp = reinterpret_cast<const uint8_t*>(&footerLen);
    out.insert(out.end(), flp, flp + 4);
    out.insert(out.end(), kParquetMagic, kParquetMagic + 4);
    return out;
}

bool saveSnapshotToParquetFile(const TableSnapshot& snap, const std::string& path,
                               const ParquetSaveOptions& opts, std::string* outError) {
    std::vector<uint8_t> bytes = serializeSnapshotToParquetBytes(snap, opts, outError);
    if (bytes.empty()) return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (outError) *outError = "parquet: failed to open output file: " + path;
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        if (outError) *outError = "parquet: failed writing file: " + path;
        return false;
    }
    return true;
}

}  // namespace bro::table
