#include "table/table_io.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bro::table {

namespace {

constexpr char kMagicV1[8] = {'B', 'R', 'O', 'T', 'B', 'L', '0', '1'};
constexpr char kMagicV2[8] = {'B', 'R', 'O', 'T', 'B', 'L', '0', '2'};
constexpr uint32_t kFormatVersionV1 = 1;
constexpr uint32_t kFormatVersionV2 = 2;
constexpr uint32_t kMaxCols = 4096;
constexpr uint32_t kMaxChunks = 1048576;
constexpr uint32_t kMaxDictEntries = 16777216;
constexpr size_t kNullBitmapBytes = sizeof(std::array<uint64_t, kChunkAliveWords>);

#pragma pack(push, 1)
struct FileHeaderV1 {
    char magic[8];
    uint32_t version;
    uint32_t colCount;
    uint32_t chunkCount;
    uint32_t reserved;
    uint64_t totalAliveRows;
    uint32_t dictCount;
    uint32_t reserved2;
    uint64_t payloadBytes;
};

struct FileHeaderV2 {
    char magic[8];
    uint32_t version;
    uint32_t colCount;
    uint32_t chunkCount;
    uint32_t hashIndexCount;
    uint64_t totalAliveRows;
    uint32_t dictCount;
    uint32_t rangeIndexCount;
    uint32_t spatialIndexCount;
    uint32_t reserved;
    uint64_t payloadBytes;
    uint64_t checksum;
};
#pragma pack(pop)

struct SpatialMetaSpec {
    std::string name;
    std::vector<size_t> cols;
    double cellSize = 16.0;
};

struct PersistedIndexSpecs {
    std::vector<size_t> hashCols;
    std::vector<size_t> rangeCols;
    std::vector<SpatialMetaSpec> spatials;
};

uint64_t computeRawFnv1a64(const uint8_t* data, size_t size) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

void appendU8(std::vector<uint8_t>& buf, uint8_t v) { buf.push_back(v); }

void appendU32(std::vector<uint8_t>& buf, uint32_t v) {
    const size_t off = buf.size();
    buf.resize(off + sizeof(v));
    std::memcpy(buf.data() + off, &v, sizeof(v));
}

void appendU64(std::vector<uint8_t>& buf, uint64_t v) {
    const size_t off = buf.size();
    buf.resize(off + sizeof(v));
    std::memcpy(buf.data() + off, &v, sizeof(v));
}

void appendF64(std::vector<uint8_t>& buf, double v) {
    const size_t off = buf.size();
    buf.resize(off + sizeof(v));
    std::memcpy(buf.data() + off, &v, sizeof(v));
}

void padToAlign64(std::vector<uint8_t>& buf) {
    const size_t rem = buf.size() & 63u;
    if (rem != 0) buf.resize(buf.size() + (64u - rem), 0);
}

struct MappedFileOwner {
    const uint8_t* data = nullptr;
    size_t size = 0;
#ifdef _WIN32
    HANDLE fileHandle = INVALID_HANDLE_VALUE;
    HANDLE mapHandle = nullptr;
#else
    int fd = -1;
#endif
    ~MappedFileOwner() {
#ifdef _WIN32
        if (data) UnmapViewOfFile(data);
        if (mapHandle) CloseHandle(mapHandle);
        if (fileHandle != INVALID_HANDLE_VALUE) CloseHandle(fileHandle);
#else
        if (data && size > 0) munmap(const_cast<uint8_t*>(data), size);
        if (fd >= 0) close(fd);
#endif
    }
};

struct ByteCursor {
    const uint8_t* ptr = nullptr;
    size_t size = 0;
    size_t pos = 0;

    bool has(size_t n) const { return pos <= size && n <= (size - pos); }

    template <typename T>
    bool read(T& out) {
        if (!has(sizeof(T))) return false;
        std::memcpy(&out, ptr + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }

    bool skipToAlign64() {
        const size_t rem = pos & 63u;
        if (rem == 0) return true;
        const size_t add = 64u - rem;
        if (!has(add)) return false;
        pos += add;
        return true;
    }
};

bool validateAliveWords(const std::array<uint64_t, kChunkAliveWords>& words,
                        uint32_t rowCount, uint32_t aliveCount) {
    if (rowCount > kChunkRows || aliveCount > rowCount) return false;
    uint32_t pop = 0;
    for (uint32_t w = 0; w < kChunkAliveWords; ++w) {
        pop += static_cast<uint32_t>(std::popcount(words[w]));
    }
    if (pop != aliveCount) return false;
    for (uint32_t r = rowCount; r < kChunkRows; ++r) {
        if (((words[r >> 6] >> (r & 63)) & 1ULL) != 0) return false;
    }
    return true;
}

bool validateColumnPayload(ColType type, uint32_t rowCount, const uint8_t* fixedPtr,
                           size_t fixedBytes, size_t strBytes, const uint64_t* nullWords,
                           size_t dictSize, std::string* err) {
    const size_t expectedFixed = static_cast<size_t>(rowCount) * colTypeElemSize(type);
    if (fixedBytes != expectedFixed) {
        if (err) *err = "corrupted .brotable fixed payload size";
        return false;
    }
    if (type == ColType::String) {
        const auto* spans = reinterpret_cast<const StringSpan*>(fixedPtr);
        for (uint32_t r = 0; r < rowCount; ++r) {
            if (nullWords && ((nullWords[r >> 6] >> (r & 63)) & 1ULL) != 0) continue;
            const StringSpan sp = spans[r];
            if (static_cast<uint64_t>(sp.offset) + sp.len > strBytes) {
                if (err) *err = "corrupted .brotable string span out of bounds";
                return false;
            }
        }
    } else if (type == ColType::Dict && dictSize > 0) {
        const auto* ids = reinterpret_cast<const uint32_t*>(fixedPtr);
        for (uint32_t r = 0; r < rowCount; ++r) {
            if (nullWords && ((nullWords[r >> 6] >> (r & 63)) & 1ULL) != 0) continue;
            if (ids[r] >= dictSize) {
                if (err) *err = "corrupted .brotable dictionary ID out of bounds";
                return false;
            }
        }
    }
    return true;
}

bool parseSchemaAndDict(ByteCursor& cur, uint32_t colCount, uint32_t dictCount,
                        std::vector<ColumnDef>& outCols,
                        std::shared_ptr<StringDictionary>& outDict, std::string* err) {
    if (colCount == 0 || colCount > kMaxCols || dictCount > kMaxDictEntries) {
        if (err) *err = "corrupted .brotable counts";
        return false;
    }
    outCols.reserve(colCount);
    std::unordered_set<std::string> seenNames;
    for (uint32_t c = 0; c < colCount; ++c) {
        uint32_t nameLen = 0;
        uint8_t typeByte = 0;
        if (!cur.read(nameLen) || !cur.read(typeByte) || nameLen == 0 || nameLen > 65536 ||
            typeByte > static_cast<uint8_t>(ColType::String) || !cur.has(nameLen)) {
            if (err) *err = "corrupted .brotable column schema";
            return false;
        }
        std::string name(reinterpret_cast<const char*>(cur.ptr + cur.pos), nameLen);
        cur.pos += nameLen;
        if (!seenNames.insert(name).second) {
            if (err) *err = "duplicate column in .brotable schema: " + name;
            return false;
        }
        outCols.push_back(ColumnDef{std::move(name), static_cast<ColType>(typeByte)});
    }
    outDict = std::make_shared<StringDictionary>();
    for (uint32_t i = 0; i < dictCount; ++i) {
        uint32_t len = 0;
        if (!cur.read(len) || len > 16777216u || !cur.has(len)) {
            if (err) *err = "corrupted .brotable dictionary";
            return false;
        }
        std::string_view sv(reinterpret_cast<const char*>(cur.ptr + cur.pos), len);
        cur.pos += len;
        outDict->intern(sv);
    }
    return true;
}

std::shared_ptr<TableSnapshot> parseSnapshotImpl(const uint8_t* data, size_t size,
                                                 std::shared_ptr<const void> owner,
                                                 PersistedIndexSpecs* outIdxSpecs,
                                                 std::string* err) {
    if (!data || size < 8) {
        if (err) *err = "empty or truncated .brotable data";
        return nullptr;
    }

    bool isV2 = false;
    if (std::memcmp(data, kMagicV2, 8) == 0) {
        isV2 = true;
    } else if (std::memcmp(data, kMagicV1, 8) != 0) {
        if (err) *err = "invalid .brotable magic";
        return nullptr;
    }

    ByteCursor cur{data, size, 0};
    uint32_t colCount = 0;
    uint32_t chunkCount = 0;
    uint64_t headerAlive = 0;
    uint32_t dictCount = 0;
    uint32_t hashCount = 0;
    uint32_t rangeCount = 0;
    uint32_t spatialCount = 0;

    if (isV2) {
        FileHeaderV2 hdr{};
        if (!cur.read(hdr) || hdr.version != kFormatVersionV2) {
            if (err) *err = "invalid .brotable v2 header";
            return nullptr;
        }
        if (hdr.payloadBytes != size || size < sizeof(FileHeaderV2)) {
            if (err) *err = "truncated or corrupted .brotable v2 size";
            return nullptr;
        }
        const uint64_t actualCsum =
            computeRawFnv1a64(data + sizeof(FileHeaderV2), size - sizeof(FileHeaderV2));
        if (actualCsum != hdr.checksum) {
            if (err) *err = ".brotable v2 checksum mismatch";
            return nullptr;
        }
        colCount = hdr.colCount;
        chunkCount = hdr.chunkCount;
        headerAlive = hdr.totalAliveRows;
        dictCount = hdr.dictCount;
        hashCount = hdr.hashIndexCount;
        rangeCount = hdr.rangeIndexCount;
        spatialCount = hdr.spatialIndexCount;
    } else {
        FileHeaderV1 hdr{};
        if (!cur.read(hdr) || hdr.version != kFormatVersionV1) {
            if (err) *err = "invalid .brotable v1 header";
            return nullptr;
        }
        if (hdr.payloadBytes != size) {
            if (err) *err = "truncated or corrupted .brotable v1 size";
            return nullptr;
        }
        colCount = hdr.colCount;
        chunkCount = hdr.chunkCount;
        headerAlive = hdr.totalAliveRows;
        dictCount = hdr.dictCount;
    }

    if (chunkCount > kMaxChunks || hashCount > colCount || rangeCount > colCount || spatialCount > 4096) {
        if (err) *err = "corrupted .brotable header bounds";
        return nullptr;
    }

    std::vector<ColumnDef> cols;
    std::shared_ptr<StringDictionary> dict;
    if (!parseSchemaAndDict(cur, colCount, dictCount, cols, dict, err)) return nullptr;

    PersistedIndexSpecs idxSpecs;
    if (isV2) {
        for (uint32_t i = 0; i < hashCount; ++i) {
            uint32_t col = 0;
            if (!cur.read(col) || col >= colCount) {
                if (err) *err = "corrupted .brotable hash index spec";
                return nullptr;
            }
            idxSpecs.hashCols.push_back(col);
        }
        for (uint32_t i = 0; i < rangeCount; ++i) {
            uint32_t col = 0;
            if (!cur.read(col) || col >= colCount) {
                if (err) *err = "corrupted .brotable range index spec";
                return nullptr;
            }
            idxSpecs.rangeCols.push_back(col);
        }
        for (uint32_t i = 0; i < spatialCount; ++i) {
            uint32_t nameLen = 0;
            uint8_t dims = 0;
            double cellSize = 0.0;
            if (!cur.read(nameLen) || nameLen == 0 || nameLen > 4096 || !cur.has(nameLen)) {
                if (err) *err = "corrupted .brotable spatial index name";
                return nullptr;
            }
            SpatialMetaSpec sp;
            sp.name.assign(reinterpret_cast<const char*>(cur.ptr + cur.pos), nameLen);
            cur.pos += nameLen;
            if (!cur.read(dims) || !cur.read(cellSize) || (dims != 2 && dims != 3) ||
                !std::isfinite(cellSize) || cellSize <= 0.0) {
                if (err) *err = "corrupted .brotable spatial index dimensions";
                return nullptr;
            }
            sp.cellSize = cellSize;
            sp.cols.resize(dims);
            for (uint8_t d = 0; d < dims; ++d) {
                uint32_t col = 0;
                if (!cur.read(col) || col >= colCount) {
                    if (err) *err = "corrupted .brotable spatial index column";
                    return nullptr;
                }
                const ColType ct = cols[col].type;
                if (ct == ColType::String || ct == ColType::Dict || ct == ColType::Bool) {
                    if (err) *err = "invalid non-numeric spatial index column";
                    return nullptr;
                }
                sp.cols[d] = col;
            }
            idxSpecs.spatials.push_back(std::move(sp));
        }
    }

    auto snap = std::make_shared<TableSnapshot>();
    for (auto& cd : cols) snap->schema.addColumn(std::move(cd.name), cd.type);
    snap->schemaHash = snap->schema.computeHash();
    snap->dict = std::move(dict);
    snap->totalAliveRows = 0;
    snap->version = 1;
    snap->structureVersion = 1;
    snap->chunks.reserve(chunkCount);

    for (uint32_t ci = 0; ci < chunkCount; ++ci) {
        uint32_t rowCount = 0;
        uint32_t aliveCount = 0;
        std::array<uint64_t, kChunkAliveWords> aliveWords{};
        if (!cur.read(rowCount) || !cur.read(aliveCount) || !cur.read(aliveWords) ||
            !validateAliveWords(aliveWords, rowCount, aliveCount)) {
            if (err) *err = "corrupted .brotable chunk metadata";
            return nullptr;
        }
        TableSnapshot::FrozenChunk fc;
        fc.chunkId = ci + 1;
        fc.dirtyVersion = 1;
        fc.rowCount = rowCount;
        fc.aliveCount = aliveCount;
        fc.aliveWords = aliveWords;
        fc.columns.reserve(cols.size());
        snap->totalAliveRows += aliveCount;

        for (size_t c = 0; c < cols.size(); ++c) {
            uint64_t fixedBytes = 0;
            uint64_t strBytes = 0;
            uint8_t colFlags = 0;
            if (!cur.read(fixedBytes) || !cur.read(strBytes)) {
                if (err) *err = "corrupted .brotable column header";
                return nullptr;
            }
            if (isV2 && !cur.read(colFlags)) {
                if (err) *err = "corrupted .brotable column flags";
                return nullptr;
            }
            if (!cur.skipToAlign64() || !cur.has(static_cast<size_t>(fixedBytes))) {
                if (err) *err = "corrupted .brotable column payload";
                return nullptr;
            }
            const uint8_t* fixedPtr = cur.ptr + cur.pos;
            cur.pos += static_cast<size_t>(fixedBytes);
            if (!cur.has(static_cast<size_t>(strBytes))) {
                if (err) *err = "corrupted .brotable string arena";
                return nullptr;
            }
            const uint8_t* strPtr = cur.ptr + cur.pos;
            cur.pos += static_cast<size_t>(strBytes);

            const uint64_t* nullPtr = nullptr;
            if (isV2 && (colFlags & 1u) != 0) {
                if (!cur.skipToAlign64() || !cur.has(kNullBitmapBytes)) {
                    if (err) *err = "corrupted .brotable null bitmap";
                    return nullptr;
                }
                nullPtr = reinterpret_cast<const uint64_t*>(cur.ptr + cur.pos);
                cur.pos += kNullBitmapBytes;
            }

            if (!validateColumnPayload(cols[c].type, rowCount, fixedPtr,
                                       static_cast<size_t>(fixedBytes),
                                       static_cast<size_t>(strBytes),
                                       nullPtr, snap->dict->size(), err)) {
                return nullptr;
            }

            if (owner) {
                fc.columns.push_back(ColumnChunk::createMapped(
                    cols[c].type, fixedPtr, static_cast<size_t>(fixedBytes),
                    strPtr, static_cast<size_t>(strBytes), owner, nullPtr));
            } else {
                auto nonOwning =
                    std::shared_ptr<const void>(fixedPtr ? fixedPtr : data, [](const void*) {});
                auto owned = ColumnChunk::createMapped(
                                 cols[c].type, fixedPtr, static_cast<size_t>(fixedBytes),
                                 strPtr, static_cast<size_t>(strBytes), std::move(nonOwning), nullPtr)
                                 ->clone();
                fc.columns.push_back(std::move(owned));
            }
        }
        snap->chunks.push_back(std::move(fc));
    }

    if (snap->totalAliveRows != headerAlive) {
        if (err) *err = "corrupted .brotable alive row total mismatch";
        return nullptr;
    }

    if (outIdxSpecs) *outIdxSpecs = std::move(idxSpecs);
    return snap;
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

std::shared_ptr<MappedFileOwner> mapReadOnlyFile(const std::string& path, std::string* outError) {
    auto owner = std::make_shared<MappedFileOwner>();
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wlen > 0 ? wlen - 1 : 0, L'\0');
    if (wlen > 1) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
    owner->fileHandle = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (owner->fileHandle == INVALID_HANDLE_VALUE) {
        if (outError) *outError = "failed to open .brotable file: " + path;
        return nullptr;
    }
    LARGE_INTEGER fsz{};
    if (!GetFileSizeEx(owner->fileHandle, &fsz) || fsz.QuadPart <= 0) {
        if (outError) *outError = "empty or unreadable .brotable file: " + path;
        return nullptr;
    }
    owner->size = static_cast<size_t>(fsz.QuadPart);
    owner->mapHandle = CreateFileMappingW(owner->fileHandle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!owner->mapHandle) {
        if (outError) *outError = "CreateFileMappingW failed for: " + path;
        return nullptr;
    }
    void* view = MapViewOfFile(owner->mapHandle, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        if (outError) *outError = "MapViewOfFile failed for: " + path;
        return nullptr;
    }
    owner->data = static_cast<const uint8_t*>(view);
#else
    owner->fd = open(path.c_str(), O_RDONLY);
    if (owner->fd < 0) {
        if (outError) *outError = "failed to open .brotable file: " + path;
        return nullptr;
    }
    struct stat st{};
    if (fstat(owner->fd, &st) != 0 || st.st_size <= 0) {
        if (outError) *outError = "empty or unreadable .brotable file: " + path;
        return nullptr;
    }
    owner->size = static_cast<size_t>(st.st_size);
    void* view = mmap(nullptr, owner->size, PROT_READ, MAP_PRIVATE, owner->fd, 0);
    if (view == MAP_FAILED) {
        if (outError) *outError = "mmap failed for: " + path;
        return nullptr;
    }
    owner->data = static_cast<const uint8_t*>(view);
#endif
    return owner;
}

std::atomic<uint64_t> g_tmpSeq{0};

}  // namespace

std::vector<uint8_t> serializeSnapshotBytes(const TableSnapshot& snap) {
    std::vector<uint8_t> buf;
    buf.reserve(sizeof(FileHeaderV2) + snap.rowCount() * 16 + 512);

    const std::vector<size_t> hashCols = snap.hashIndexCols();
    const std::vector<size_t> rangeCols = snap.rangeIndexCols();
    const std::vector<const SpatialIndex*> spatials = snap.spatialIndexList();

    FileHeaderV2 hdr{};
    std::memcpy(hdr.magic, kMagicV2, 8);
    hdr.version = kFormatVersionV2;
    hdr.colCount = static_cast<uint32_t>(snap.schema.size());
    hdr.chunkCount = static_cast<uint32_t>(snap.chunks.size());
    hdr.hashIndexCount = static_cast<uint32_t>(hashCols.size());
    hdr.totalAliveRows = snap.totalAliveRows;
    hdr.dictCount = snap.dict ? static_cast<uint32_t>(snap.dict->size()) : 0u;
    hdr.rangeIndexCount = static_cast<uint32_t>(rangeCols.size());
    hdr.spatialIndexCount = static_cast<uint32_t>(spatials.size());
    buf.resize(sizeof(FileHeaderV2));

    for (const ColumnDef& cd : snap.schema.columns) {
        appendU32(buf, static_cast<uint32_t>(cd.name.size()));
        appendU8(buf, static_cast<uint8_t>(cd.type));
        buf.insert(buf.end(), cd.name.begin(), cd.name.end());
    }
    if (snap.dict) {
        const uint32_t n = static_cast<uint32_t>(snap.dict->size());
        for (uint32_t id = 0; id < n; ++id) {
            std::string s = snap.dict->get(id);
            appendU32(buf, static_cast<uint32_t>(s.size()));
            buf.insert(buf.end(), s.begin(), s.end());
        }
    }
    for (size_t c : hashCols) appendU32(buf, static_cast<uint32_t>(c));
    for (size_t c : rangeCols) appendU32(buf, static_cast<uint32_t>(c));
    for (const SpatialIndex* si : spatials) {
        appendU32(buf, static_cast<uint32_t>(si->name().size()));
        buf.insert(buf.end(), si->name().begin(), si->name().end());
        appendU8(buf, si->dims());
        appendF64(buf, si->cellSize());
        for (uint8_t d = 0; d < si->dims(); ++d) {
            appendU32(buf, static_cast<uint32_t>(si->cols()[d]));
        }
    }

    for (const auto& fc : snap.chunks) {
        appendU32(buf, fc.rowCount);
        appendU32(buf, fc.aliveCount);
        const size_t off = buf.size();
        buf.resize(off + sizeof(fc.aliveWords));
        std::memcpy(buf.data() + off, fc.aliveWords.data(), sizeof(fc.aliveWords));

        for (size_t c = 0; c < snap.schema.size(); ++c) {
            const ColumnChunk& col = *fc.columns[c];
            const size_t fixedBytes = static_cast<size_t>(fc.rowCount) * colTypeElemSize(col.type());
            const size_t strBytes = (col.type() == ColType::String) ? col.stringArenaSize() : 0u;
            const bool hasNulls = col.hasNulls() && col.nullWordsData() != nullptr;
            appendU64(buf, fixedBytes);
            appendU64(buf, strBytes);
            appendU8(buf, hasNulls ? 1u : 0u);
            padToAlign64(buf);
            if (fixedBytes > 0) {
                const uint8_t* raw = col.rawData();
                buf.insert(buf.end(), raw, raw + fixedBytes);
            }
            if (strBytes > 0) {
                const uint8_t* strRaw = col.stringArenaData();
                buf.insert(buf.end(), strRaw, strRaw + strBytes);
            }
            if (hasNulls) {
                padToAlign64(buf);
                const auto* nw = reinterpret_cast<const uint8_t*>(col.nullWordsData());
                buf.insert(buf.end(), nw, nw + kNullBitmapBytes);
            }
        }
    }

    hdr.payloadBytes = buf.size();
    hdr.checksum = computeRawFnv1a64(buf.data() + sizeof(FileHeaderV2),
                                     buf.size() - sizeof(FileHeaderV2));
    std::memcpy(buf.data(), &hdr, sizeof(FileHeaderV2));
    return buf;
}

std::shared_ptr<Table> deserializeTableBytes(const uint8_t* data, size_t size,
                                             std::string* outError) {
    PersistedIndexSpecs specs;
    auto dummyOwner = std::shared_ptr<const void>(data, [](const void*) {});
    auto snap = parseSnapshotImpl(data, size, std::move(dummyOwner), &specs, outError);
    if (!snap) return nullptr;
    auto tbl = Table::fromSnapshot(*snap);
    applyPersistedIndexes(*tbl, specs);
    return tbl;
}

bool saveSnapshotToFile(const TableSnapshot& snap, const std::string& path,
                        std::string* outError) {
    std::vector<uint8_t> bytes = serializeSnapshotBytes(snap);
    const uint64_t seq = g_tmpSeq.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    const std::string tmpPath =
        path + ".tmp." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(seq);
    auto toWide = [](const std::string& s) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring ws(wlen > 0 ? wlen - 1 : 0, L'\0');
        if (wlen > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), wlen);
        return ws;
    };
    const std::wstring wtmp = toWide(tmpPath);
    const std::wstring wpath = toWide(path);
    HANDLE h = CreateFileW(wtmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (outError) *outError = "failed to open temp file for writing: " + tmpPath;
        return false;
    }
    size_t writtenTotal = 0;
    while (writtenTotal < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(bytes.size() - writtenTotal, 64u * 1024u * 1024u));
        DWORD written = 0;
        if (!WriteFile(h, bytes.data() + writtenTotal, chunk, &written, nullptr) ||
            written != chunk) {
            CloseHandle(h);
            DeleteFileW(wtmp.c_str());
            if (outError) *outError = "failed writing .brotable temp file: " + tmpPath;
            return false;
        }
        writtenTotal += written;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!MoveFileExW(wtmp.c_str(), wpath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(wtmp.c_str());
        if (outError) *outError = "failed atomic replace of .brotable file: " + path;
        return false;
    }
    return true;
#else
    const std::string tmpPath =
        path + ".tmp." + std::to_string(getpid()) + "." + std::to_string(seq);
    int fd = open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        if (outError) *outError = "failed to open temp file for writing: " + tmpPath;
        return false;
    }
    size_t off = 0;
    while (off < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + off, bytes.size() - off);
        if (n <= 0) {
            close(fd);
            unlink(tmpPath.c_str());
            if (outError) *outError = "failed writing .brotable temp file: " + tmpPath;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    fsync(fd);
    close(fd);
    if (rename(tmpPath.c_str(), path.c_str()) != 0) {
        unlink(tmpPath.c_str());
        if (outError) *outError = "failed atomic rename of .brotable file: " + path;
        return false;
    }
    return true;
#endif
}

std::shared_ptr<Table> loadTableFromFile(const std::string& path, std::string* outError) {
    auto owner = mapReadOnlyFile(path, outError);
    if (!owner) return nullptr;
    PersistedIndexSpecs specs;
    auto snap = parseSnapshotImpl(owner->data, owner->size, owner, &specs, outError);
    if (!snap) return nullptr;
    auto tbl = Table::fromSnapshot(*snap);
    applyPersistedIndexes(*tbl, specs);
    return tbl;
}

std::shared_ptr<const TableSnapshot> mmapSnapshotFromFile(const std::string& path,
                                                          std::string* outError) {
    auto owner = mapReadOnlyFile(path, outError);
    if (!owner) return nullptr;
    PersistedIndexSpecs specs;
    auto snap = parseSnapshotImpl(owner->data, owner->size, owner, &specs, outError);
    if (snap) applyPersistedIndexesToSnapshot(*snap, specs);
    return snap;
}

}  // namespace bro::table
