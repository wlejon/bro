#include "table/table_ingest_parquet.h"

#include "table/table_parquet_internal.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>

namespace bro::table {

namespace {

using namespace parquet_detail;

struct LeafColInfo {
    std::string name;
    int32_t physType = PQ_BYTE_ARRAY;
    int32_t maxDefLevel = 0;
    int32_t fixedLen = 0;
    int32_t convertedType = -1;
};

void extractLeafSchema(const std::vector<PqSchemaElem>& elems, std::vector<LeafColInfo>& out) {
    if (elems.empty()) return;
    size_t idx = 0;
    std::function<void(int, int)> walk = [&](int remainingChildren, int defLevel) {
        while (remainingChildren > 0 && idx < elems.size()) {
            const PqSchemaElem& e = elems[idx++];
            --remainingChildren;
            const int curDef = (e.hasRep && e.rep != 0) ? (defLevel + 1) : defLevel;
            if (e.numChildren > 0 && !e.hasType) {
                walk(e.numChildren, curDef);
            } else {
                LeafColInfo lc;
                lc.name = e.name;
                lc.physType = e.type;
                lc.maxDefLevel = curDef;
                lc.fixedLen = e.typeLen;
                lc.convertedType = e.hasConverted ? e.converted : -1;
                out.push_back(std::move(lc));
            }
        }
    };
    const PqSchemaElem& root = elems[idx++];
    walk(root.numChildren, 0);
}

ColType parquetPhysToBroType(const LeafColInfo& lc, bool stringAsDict) {
    switch (lc.physType) {
        case PQ_BOOLEAN: return ColType::Bool;
        case PQ_INT32: return ColType::I32;
        case PQ_INT64: return ColType::I64;
        case PQ_FLOAT: return ColType::F32;
        case PQ_DOUBLE: return ColType::F64;
        case PQ_BYTE_ARRAY:
        case PQ_FIXED_LEN_BYTE_ARRAY:
            return stringAsDict ? ColType::Dict : ColType::String;
        default: return ColType::String;
    }
}

struct DecodedDict {
    std::vector<uint8_t> scalars;
    std::vector<std::string> strings;
};

bool decodeDictionaryPage(int32_t physType, int32_t fixedLen, int32_t numValues,
                          const uint8_t* ptr, size_t len, DecodedDict& dict, std::string* err) {
    dict.scalars.clear();
    dict.strings.clear();
    if (numValues < 0) return false;
    const size_t n = static_cast<size_t>(numValues);
    if (physType == PQ_INT32 || physType == PQ_FLOAT) {
        if (n * 4 > len) { if (err) *err = "parquet: truncated 4-byte dict"; return false; }
        dict.scalars.assign(ptr, ptr + n * 4);
        return true;
    }
    if (physType == PQ_INT64 || physType == PQ_DOUBLE) {
        if (n * 8 > len) { if (err) *err = "parquet: truncated 8-byte dict"; return false; }
        dict.scalars.assign(ptr, ptr + n * 8);
        return true;
    }
    if (physType == PQ_BYTE_ARRAY) {
        size_t pos = 0;
        dict.strings.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            if (len - pos < 4) { if (err) *err = "parquet: truncated BYTE_ARRAY dict"; return false; }
            uint32_t sl = 0;
            std::memcpy(&sl, ptr + pos, 4);
            pos += 4;
            if (sl > len - pos) { if (err) *err = "parquet: BYTE_ARRAY dict length out of bounds"; return false; }
            dict.strings.emplace_back(reinterpret_cast<const char*>(ptr + pos), sl);
            pos += sl;
        }
        return true;
    }
    if (physType == PQ_FIXED_LEN_BYTE_ARRAY && fixedLen > 0) {
        const size_t fl = static_cast<size_t>(fixedLen);
        if (n * fl > len) { if (err) *err = "parquet: truncated FIXED dict"; return false; }
        dict.strings.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            dict.strings.emplace_back(reinterpret_cast<const char*>(ptr + i * fl), fl);
        }
        return true;
    }
    if (err) *err = "parquet: unsupported dictionary physical type";
    return false;
}

template <typename ByteSliceProvider>
bool ingestParquetFromProvider(const PqFileMeta& meta, const ParquetIngestOptions& opts,
                               ByteSliceProvider&& readRange,
                               std::shared_ptr<Table>& outTable, std::string* err) {
    std::vector<LeafColInfo> allCols;
    extractLeafSchema(meta.schema, allCols);
    if (allCols.empty()) {
        outTable = std::make_shared<Table>();
        return true;
    }

    std::vector<size_t> selectedSrcCols;
    if (opts.columns.empty()) {
        selectedSrcCols.resize(allCols.size());
        for (size_t i = 0; i < allCols.size(); ++i) selectedSrcCols[i] = i;
    } else {
        for (const std::string& want : opts.columns) {
            auto it = std::find_if(allCols.begin(), allCols.end(),
                                   [&](const LeafColInfo& c) { return c.name == want; });
            if (it == allCols.end()) {
                if (err) *err = "parquet: column not found: " + want;
                return false;
            }
            selectedSrcCols.push_back(static_cast<size_t>(it - allCols.begin()));
        }
    }

    std::vector<ColumnDef> defs;
    defs.reserve(selectedSrcCols.size());
    for (size_t idx : selectedSrcCols) {
        defs.push_back(ColumnDef{allCols[idx].name, parquetPhysToBroType(allCols[idx], opts.stringAsDict)});
    }
    outTable = std::make_shared<Table>(defs);

    const size_t maxRows = (opts.maxRows > 0) ? opts.maxRows : static_cast<size_t>(meta.numRows);
    size_t rowsIngested = 0;

    for (const PqRowGroup& rg : meta.rowGroups) {
        if (rowsIngested >= maxRows || rg.numRows <= 0) break;
        const size_t rgTake = std::min(static_cast<size_t>(rg.numRows), maxRows - rowsIngested);

        const size_t nOutCols = selectedSrcCols.size();
        std::vector<std::vector<uint8_t>> colFixed(nOutCols);
        std::vector<std::vector<uint8_t>> colNulls(nOutCols);
        std::vector<std::vector<std::string>> colStrings(nOutCols);

        for (size_t oc = 0; oc < nOutCols; ++oc) {
            const size_t sc = selectedSrcCols[oc];
            if (sc >= rg.columns.size()) {
                if (err) *err = "parquet: row group column count mismatch";
                return false;
            }
            const LeafColInfo& lc = allCols[sc];
            const ColType dstType = defs[oc].type;
            if (dstType == ColType::String) colStrings[oc].resize(rgTake);
            else colFixed[oc].assign(rgTake * colTypeElemSize(dstType), 0);
            colNulls[oc].assign(rgTake, 0);

            const PqColChunk& cc = rg.columns[sc];
            int64_t startOff = cc.meta.dataPageOffset;
            if (cc.meta.hasDictOffset && cc.meta.dictPageOffset > 0 && cc.meta.dictPageOffset < startOff) {
                startOff = cc.meta.dictPageOffset;
            }
            const int64_t chunkBytes = cc.meta.totalCompressed;
            if (startOff < 0 || chunkBytes < 0) {
                if (err) *err = "parquet: invalid column chunk offsets";
                return false;
            }

            std::vector<uint8_t> rawChunk;
            if (!readRange(static_cast<size_t>(startOff), static_cast<size_t>(chunkBytes), rawChunk)) {
                if (err && err->empty()) *err = "parquet: failed reading column chunk bytes";
                return false;
            }

            DecodedDict dict;
            bool hasDict = false;
            size_t curPos = 0;
            size_t rowOut = 0;

            while (curPos < rawChunk.size() && rowOut < rgTake) {
                PqPageHeader ph{};
                size_t hdrLen = 0;
                if (!readPageHeader(rawChunk.data() + curPos, rawChunk.size() - curPos, ph, hdrLen)) {
                    if (err) *err = "parquet: corrupted page header";
                    return false;
                }
                curPos += hdrLen;
                if (ph.compressedSize < 0 || static_cast<size_t>(ph.compressedSize) > rawChunk.size() - curPos) {
                    if (err) *err = "parquet: truncated page payload";
                    return false;
                }
                const uint8_t* compPtr = rawChunk.data() + curPos;
                curPos += static_cast<size_t>(ph.compressedSize);

                if (ph.type == PQ_DICTIONARY_PAGE) {
                    std::vector<uint8_t> dictBuf;
                    if (!decompressPage(cc.meta.codec, compPtr, static_cast<size_t>(ph.compressedSize),
                                        static_cast<size_t>(ph.uncompressedSize), dictBuf, err)) {
                        return false;
                    }
                    if (!decodeDictionaryPage(lc.physType, lc.fixedLen, ph.numValues,
                                              dictBuf.data(), dictBuf.size(), dict, err)) {
                        return false;
                    }
                    hasDict = true;
                    continue;
                }

                if (ph.type != PQ_DATA_PAGE && ph.type != PQ_DATA_PAGE_V2) continue;

                const uint32_t pageVals = static_cast<uint32_t>(std::max(0, ph.numValues));
                std::vector<uint32_t> defLevels;
                const uint8_t* valPtr = nullptr;
                size_t valBytes = 0;
                std::vector<uint8_t> pageBuf;

                if (ph.type == PQ_DATA_PAGE) {
                    if (!decompressPage(cc.meta.codec, compPtr, static_cast<size_t>(ph.compressedSize),
                                        static_cast<size_t>(ph.uncompressedSize), pageBuf, err)) {
                        return false;
                    }
                    size_t off = 0;
                    if (lc.maxDefLevel > 0) {
                        if (pageBuf.size() - off < 4) { if (err) *err = "parquet: truncated def levels"; return false; }
                        uint32_t defLen = 0;
                        std::memcpy(&defLen, pageBuf.data() + off, 4);
                        off += 4;
                        if (defLen > pageBuf.size() - off) { if (err) *err = "parquet: def len OOB"; return false; }
                        const uint32_t bw = (lc.maxDefLevel <= 1) ? 1u : 2u;
                        if (!decodeRleHybrid(pageBuf.data() + off, defLen, bw, pageVals, defLevels, err)) {
                            return false;
                        }
                        off += defLen;
                    }
                    valPtr = pageBuf.data() + off;
                    valBytes = pageBuf.size() - off;
                } else {
                    const size_t repLen = static_cast<size_t>(std::max(0, ph.repBytesLen));
                    const size_t defLen = static_cast<size_t>(std::max(0, ph.defBytesLen));
                    if (repLen + defLen > static_cast<size_t>(ph.compressedSize)) {
                        if (err) *err = "parquet: v2 level length OOB";
                        return false;
                    }
                    if (lc.maxDefLevel > 0 && defLen > 0) {
                        const uint32_t bw = (lc.maxDefLevel <= 1) ? 1u : 2u;
                        if (!decodeRleHybrid(compPtr + repLen, defLen, bw, pageVals, defLevels, err)) {
                            return false;
                        }
                    }
                    const uint8_t* cPtr = compPtr + repLen + defLen;
                    const size_t cLen = static_cast<size_t>(ph.compressedSize) - (repLen + defLen);
                    const size_t uLen = static_cast<size_t>(std::max<int64_t>(0, int64_t(ph.uncompressedSize) - int64_t(repLen) - int64_t(defLen)));
                    const int32_t codec = ph.v2Compressed ? cc.meta.codec : PQ_UNCOMPRESSED;
                    if (!decompressPage(codec, cPtr, cLen, uLen, pageBuf, err)) return false;
                    valPtr = pageBuf.data();
                    valBytes = pageBuf.size();
                }

                uint32_t nonNullCount = pageVals;
                if (!defLevels.empty()) {
                    nonNullCount = 0;
                    for (uint32_t d : defLevels) {
                        if (static_cast<int32_t>(d) == lc.maxDefLevel) ++nonNullCount;
                    }
                }

                std::vector<uint32_t> dictIndices;
                size_t valPos = 0;
                uint32_t boolBitIdx = 0;
                const bool useDict =
                    hasDict && (ph.encoding == PQ_RLE_DICT || ph.encoding == PQ_PLAIN_DICT);
                if (useDict) {
                    if (valBytes < 1) { if (err) *err = "parquet: missing dict bitWidth"; return false; }
                    const uint32_t bw = valPtr[0];
                    if (!decodeRleHybrid(valPtr + 1, valBytes - 1, bw, nonNullCount, dictIndices, err)) {
                        return false;
                    }
                } else if (ph.encoding != PQ_PLAIN) {
                    if (err) *err = "parquet: unsupported encoding (" + std::to_string(ph.encoding) + ")";
                    return false;
                }

                uint32_t nnCursor = 0;
                for (uint32_t pv = 0; pv < pageVals && rowOut < rgTake; ++pv) {
                    const bool isNull = !defLevels.empty() &&
                                        static_cast<int32_t>(defLevels[pv]) < lc.maxDefLevel;
                    if (isNull) {
                        colNulls[oc][rowOut++] = 1;
                        continue;
                    }
                    if (useDict) {
                        if (nnCursor >= dictIndices.size()) break;
                        const uint32_t dIdx = dictIndices[nnCursor++];
                        if (lc.physType == PQ_INT32 || lc.physType == PQ_FLOAT) {
                            if (dIdx * 4 + 4 > dict.scalars.size()) { if (err) *err = "parquet: dict idx OOB"; return false; }
                            std::memcpy(colFixed[oc].data() + rowOut * 4, dict.scalars.data() + dIdx * 4, 4);
                        } else if (lc.physType == PQ_INT64 || lc.physType == PQ_DOUBLE) {
                            if (dIdx * 8 + 8 > dict.scalars.size()) { if (err) *err = "parquet: dict idx OOB"; return false; }
                            std::memcpy(colFixed[oc].data() + rowOut * 8, dict.scalars.data() + dIdx * 8, 8);
                        } else {
                            if (dIdx >= dict.strings.size()) { if (err) *err = "parquet: string dict idx OOB"; return false; }
                            const std::string& s = dict.strings[dIdx];
                            if (dstType == ColType::Dict) {
                                uint32_t tid = outTable->internString(s);
                                std::memcpy(colFixed[oc].data() + rowOut * 4, &tid, 4);
                            } else {
                                colStrings[oc][rowOut] = s;
                            }
                        }
                    } else {
                        if (lc.physType == PQ_BOOLEAN) {
                            const size_t bIdx = boolBitIdx >> 3;
                            if (bIdx >= valBytes) { if (err) *err = "parquet: PLAIN bool OOB"; return false; }
                            colFixed[oc][rowOut] = ((valPtr[bIdx] >> (boolBitIdx & 7u)) & 1u) ? 1 : 0;
                            ++boolBitIdx;
                        } else if (lc.physType == PQ_INT32 || lc.physType == PQ_FLOAT) {
                            if (valPos + 4 > valBytes) { if (err) *err = "parquet: PLAIN 4B OOB"; return false; }
                            std::memcpy(colFixed[oc].data() + rowOut * 4, valPtr + valPos, 4);
                            valPos += 4;
                        } else if (lc.physType == PQ_INT64 || lc.physType == PQ_DOUBLE) {
                            if (valPos + 8 > valBytes) { if (err) *err = "parquet: PLAIN 8B OOB"; return false; }
                            std::memcpy(colFixed[oc].data() + rowOut * 8, valPtr + valPos, 8);
                            valPos += 8;
                        } else if (lc.physType == PQ_BYTE_ARRAY) {
                            if (valPos + 4 > valBytes) { if (err) *err = "parquet: PLAIN BYTE_ARRAY len OOB"; return false; }
                            uint32_t sl = 0;
                            std::memcpy(&sl, valPtr + valPos, 4);
                            valPos += 4;
                            if (sl > valBytes - valPos) { if (err) *err = "parquet: PLAIN BYTE_ARRAY OOB"; return false; }
                            std::string_view sv(reinterpret_cast<const char*>(valPtr + valPos), sl);
                            valPos += sl;
                            if (dstType == ColType::Dict) {
                                uint32_t tid = outTable->internString(sv);
                                std::memcpy(colFixed[oc].data() + rowOut * 4, &tid, 4);
                            } else {
                                colStrings[oc][rowOut].assign(sv);
                            }
                        } else if (lc.physType == PQ_FIXED_LEN_BYTE_ARRAY && lc.fixedLen > 0) {
                            const size_t fl = static_cast<size_t>(lc.fixedLen);
                            if (valPos + fl > valBytes) { if (err) *err = "parquet: PLAIN FIXED OOB"; return false; }
                            std::string_view sv(reinterpret_cast<const char*>(valPtr + valPos), fl);
                            valPos += fl;
                            if (dstType == ColType::Dict) {
                                uint32_t tid = outTable->internString(sv);
                                std::memcpy(colFixed[oc].data() + rowOut * 4, &tid, 4);
                            } else {
                                colStrings[oc][rowOut].assign(sv);
                            }
                        }
                    }
                    ++rowOut;
                }
            }
        }

        std::vector<const void*> ptrs(nOutCols, nullptr);
        size_t rgOff = 0;
        while (rgOff < rgTake) {
            const uint32_t take = static_cast<uint32_t>(std::min<size_t>(kChunkRows, rgTake - rgOff));
            for (size_t oc = 0; oc < nOutCols; ++oc) {
                if (defs[oc].type == ColType::String) ptrs[oc] = nullptr;
                else ptrs[oc] = colFixed[oc].data() + rgOff * colTypeElemSize(defs[oc].type);
            }
            const size_t startChunk = outTable->chunkCount() > 0 ? outTable->chunkCount() - 1 : 0;
            const uint32_t startRow =
                (outTable->chunkCount() > 0) ? outTable->chunk(startChunk).rowCount : 0;

            outTable->appendTypedBatch(take, ptrs.data());

            uint32_t local = 0;
            size_t ci = startChunk;
            uint32_t rCur = startRow;
            while (local < take && ci < outTable->chunkCount()) {
                TableChunk& ch = outTable->chunkMut(ci);
                if (rCur >= ch.rowCount) { ++ci; rCur = 0; continue; }
                const uint32_t inCh = std::min(take - local, ch.rowCount - rCur);
                for (size_t oc = 0; oc < nOutCols; ++oc) {
                    auto& colPtr = ch.mutableColumn(oc);
                    const bool isStr = (defs[oc].type == ColType::String);
                    for (uint32_t i = 0; i < inCh; ++i) {
                        const size_t srcIdx = rgOff + local + i;
                        const uint32_t dstRow = rCur + i;
                        if (colNulls[oc][srcIdx]) {
                            colPtr->setNull(dstRow, true);
                        } else if (isStr) {
                            colPtr->setNull(dstRow, false);
                            colPtr->setString(dstRow, colStrings[oc][srcIdx]);
                        }
                    }
                }
                local += inCh;
                ++ci;
                rCur = 0;
            }
            rgOff += take;
        }
        rowsIngested += rgTake;
    }

    return true;
}

}  // namespace

std::shared_ptr<Table> ingestParquetFromMemory(const uint8_t* data, size_t size,
                                               const ParquetIngestOptions& opts,
                                               std::string* outError) {
    if (!data || size < 12 || std::memcmp(data, kParquetMagic, 4) != 0 ||
        std::memcmp(data + size - 4, kParquetMagic, 4) != 0) {
        if (outError) *outError = "parquet: missing or invalid PAR1 magic";
        return nullptr;
    }
    uint32_t footerLen = 0;
    std::memcpy(&footerLen, data + size - 8, 4);
    if (footerLen == 0 || static_cast<size_t>(footerLen) > size - 12) {
        if (outError) *outError = "parquet: invalid footer length";
        return nullptr;
    }
    PqFileMeta meta;
    if (!readFileMetaData(data + size - 8 - footerLen, footerLen, meta)) {
        if (outError) *outError = "parquet: corrupted Thrift FileMetaData";
        return nullptr;
    }
    std::shared_ptr<Table> tbl;
    if (!ingestParquetFromProvider(meta, opts,
            [&](size_t off, size_t len, std::vector<uint8_t>& dst) -> bool {
                if (off > size || len > size - off) return false;
                dst.assign(data + off, data + off + len);
                return true;
            },
            tbl, outError)) {
        return nullptr;
    }
    return tbl;
}

std::shared_ptr<Table> ingestParquetFromFile(const std::string& path,
                                             const ParquetIngestOptions& opts,
                                             std::string* outError) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        if (outError) *outError = "parquet: failed to open file: " + path;
        return nullptr;
    }
    const auto szPos = in.tellg();
    if (szPos < 12) {
        if (outError) *outError = "parquet: file too small to be valid Parquet";
        return nullptr;
    }
    const size_t fileSize = static_cast<size_t>(szPos);
    uint8_t trailer[8]{};
    in.seekg(static_cast<std::streamoff>(fileSize - 8), std::ios::beg);
    in.read(reinterpret_cast<char*>(trailer), 8);
    if (!in || std::memcmp(trailer + 4, kParquetMagic, 4) != 0) {
        if (outError) *outError = "parquet: invalid PAR1 trailer magic";
        return nullptr;
    }
    uint8_t head[4]{};
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(head), 4);
    if (!in || std::memcmp(head, kParquetMagic, 4) != 0) {
        if (outError) *outError = "parquet: invalid PAR1 header magic";
        return nullptr;
    }
    uint32_t footerLen = 0;
    std::memcpy(&footerLen, trailer, 4);
    if (footerLen == 0 || static_cast<size_t>(footerLen) > fileSize - 12) {
        if (outError) *outError = "parquet: invalid footer length";
        return nullptr;
    }
    std::vector<uint8_t> footerBuf(footerLen);
    in.seekg(static_cast<std::streamoff>(fileSize - 8 - footerLen), std::ios::beg);
    in.read(reinterpret_cast<char*>(footerBuf.data()), static_cast<std::streamsize>(footerLen));
    if (!in) {
        if (outError) *outError = "parquet: failed reading footer";
        return nullptr;
    }

    PqFileMeta meta;
    if (!readFileMetaData(footerBuf.data(), footerBuf.size(), meta)) {
        if (outError) *outError = "parquet: corrupted Thrift FileMetaData";
        return nullptr;
    }

    std::shared_ptr<Table> tbl;
    if (!ingestParquetFromProvider(meta, opts,
            [&](size_t off, size_t len, std::vector<uint8_t>& dst) -> bool {
                if (off > fileSize || len > fileSize - off) return false;
                dst.resize(len);
                if (len == 0) return true;
                in.seekg(static_cast<std::streamoff>(off), std::ios::beg);
                in.read(reinterpret_cast<char*>(dst.data()), static_cast<std::streamsize>(len));
                return static_cast<bool>(in);
            },
            tbl, outError)) {
        return nullptr;
    }
    return tbl;
}

}  // namespace bro::table
