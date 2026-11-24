#include "table/table_io.h"

#include "table/table_codec.h"
#include "table/table_io_internal.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <unordered_set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bro::table {

namespace {

using namespace io_internal;

constexpr size_t kStreamBufBytes = 4u * 1024u * 1024u;

uint64_t computeRawFnv1a64(const uint8_t* data, size_t size) {
    IncrementalFnv1a64 hasher;
    hasher.update(data, size);
    return hasher.finish();
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

class VectorByteSink final : public ByteSink {
public:
    explicit VectorByteSink(std::vector<uint8_t>& buf) : buf_(buf) {}

    bool finalizeHeader(const FileHeaderV3& hdr) {
        if (!ok_ || buf_.size() < sizeof(FileHeaderV3)) return false;
        std::memcpy(buf_.data(), &hdr, sizeof(FileHeaderV3));
        return true;
    }

protected:
    void writeBytesRaw(const void* data, size_t size) override {
        const size_t off = buf_.size();
        buf_.resize(off + size);
        std::memcpy(buf_.data() + off, data, size);
    }

private:
    std::vector<uint8_t>& buf_;
};

#ifdef _WIN32
std::wstring utf8ToWide(const std::string& s) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(wlen > 0 ? wlen - 1 : 0, L'\0');
    if (wlen > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), wlen);
    return ws;
}
#endif

class FileByteSink final : public ByteSink {
public:
    FileByteSink(const std::string& finalPath, const std::string& tmpPath)
        : finalPath_(finalPath), tmpPath_(tmpPath), buf_(kStreamBufBytes) {
#ifdef _WIN32
        wtmp_ = utf8ToWide(tmpPath_);
        wfinal_ = utf8ToWide(finalPath_);
        h_ = CreateFileW(wtmp_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            fail("failed to open temp file for writing: " + tmpPath_);
        }
#else
        fd_ = open(tmpPath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) {
            fail("failed to open temp file for writing: " + tmpPath_);
        }
#endif
    }

    ~FileByteSink() override { abortCleanup(); }

    bool finalize(const FileHeaderV3& hdr) {
        if (!ok_) {
            abortCleanup();
            return false;
        }
        flushBuffer();
        if (!ok_) {
            abortCleanup();
            return false;
        }
#ifdef _WIN32
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(h_, zero, nullptr, FILE_BEGIN)) {
            fail("failed seeking to .brotable header: " + tmpPath_);
            abortCleanup();
            return false;
        }
        if (!writeAllRaw(&hdr, sizeof(hdr))) {
            abortCleanup();
            return false;
        }
        FlushFileBuffers(h_);
        CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(wtmp_.c_str(), wfinal_.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(wtmp_.c_str());
            fail("failed atomic replace of .brotable file: " + finalPath_);
            return false;
        }
        committed_ = true;
        return true;
#else
        if (lseek(fd_, 0, SEEK_SET) != 0 || !writeAllRaw(&hdr, sizeof(hdr))) {
            abortCleanup();
            return false;
        }
        fsync(fd_);
        close(fd_);
        fd_ = -1;
        if (rename(tmpPath_.c_str(), finalPath_.c_str()) != 0) {
            unlink(tmpPath_.c_str());
            fail("failed atomic rename of .brotable file: " + finalPath_);
            return false;
        }
        committed_ = true;
        return true;
#endif
    }

protected:
    void writeBytesRaw(const void* data, size_t size) override {
        const auto* src = static_cast<const uint8_t*>(data);
        size_t remaining = size;
        while (remaining > 0 && ok_) {
            const size_t space = buf_.size() - bufPos_;
            if (space == 0) {
                flushBuffer();
                continue;
            }
            if (bufPos_ == 0 && remaining >= buf_.size()) {
                writeAllRaw(src, remaining);
                return;
            }
            const size_t take = std::min(space, remaining);
            std::memcpy(buf_.data() + bufPos_, src, take);
            bufPos_ += take;
            src += take;
            remaining -= take;
        }
    }

private:
    bool writeAllRaw(const void* data, size_t size) {
        const auto* src = static_cast<const uint8_t*>(data);
        size_t off = 0;
        while (off < size) {
#ifdef _WIN32
            const DWORD chunk =
                static_cast<DWORD>(std::min<size_t>(size - off, 64u * 1024u * 1024u));
            DWORD written = 0;
            if (!WriteFile(h_, src + off, chunk, &written, nullptr) || written != chunk) {
                fail("failed writing .brotable temp file: " + tmpPath_);
                return false;
            }
            off += written;
#else
            ssize_t n = ::write(fd_, src + off, size - off);
            if (n <= 0) {
                fail("failed writing .brotable temp file: " + tmpPath_);
                return false;
            }
            off += static_cast<size_t>(n);
#endif
        }
        return true;
    }

    void flushBuffer() {
        if (!ok_ || bufPos_ == 0) return;
        writeAllRaw(buf_.data(), bufPos_);
        bufPos_ = 0;
    }

    void abortCleanup() {
        if (committed_) return;
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
            DeleteFileW(wtmp_.c_str());
        }
#else
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
            unlink(tmpPath_.c_str());
        }
#endif
    }

    std::string finalPath_;
    std::string tmpPath_;
    std::vector<uint8_t> buf_;
    size_t bufPos_ = 0;
    bool committed_ = false;
#ifdef _WIN32
    std::wstring wtmp_;
    std::wstring wfinal_;
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

bool writeSnapshotStreamImpl(const TableSnapshot& snap, ByteSink& sink,
                             const SaveOptions& opts, FileHeaderV3& outHdr) {
    const std::vector<size_t> hashCols = snap.hashIndexCols();
    const std::vector<size_t> rangeCols = snap.rangeIndexCols();
    const std::vector<const SpatialIndex*> spatials = snap.spatialIndexList();

    outHdr = FileHeaderV3{};
    std::memcpy(outHdr.magic, opts.compress ? kMagicV4 : kMagicV3, 8);
    outHdr.version = opts.compress ? kFormatVersionV4 : kFormatVersionV3;
    outHdr.colCount = static_cast<uint32_t>(snap.schema.size());
    outHdr.chunkCount = static_cast<uint32_t>(snap.chunks.size());
    outHdr.hashIndexCount = static_cast<uint32_t>(hashCols.size());
    outHdr.totalAliveRows = snap.totalAliveRows;
    outHdr.dictCount = snap.dict ? static_cast<uint32_t>(snap.dict->size()) : 0u;
    outHdr.rangeIndexCount = static_cast<uint32_t>(rangeCols.size());
    outHdr.spatialIndexCount = static_cast<uint32_t>(spatials.size());
    outHdr.flags = opts.compress ? kFlagLz4Compressed : 0u;

    sink.write(&outHdr, sizeof(FileHeaderV3));
    sink.beginChecksums();

    for (const ColumnDef& cd : snap.schema.columns) {
        sink.writeMetaU32(static_cast<uint32_t>(cd.name.size()));
        sink.writeMetaU8(static_cast<uint8_t>(cd.type));
        sink.writeMeta(cd.name.data(), cd.name.size());
    }
    if (snap.dict) {
        const uint32_t n = static_cast<uint32_t>(snap.dict->size());
        for (uint32_t id = 0; id < n; ++id) {
            std::string s = snap.dict->get(id);
            sink.writeMetaU32(static_cast<uint32_t>(s.size()));
            sink.writeMeta(s.data(), s.size());
        }
    }

    std::vector<uint8_t> lz4Fixed, lz4Str;
    for (const auto& fc : snap.chunks) {
        sink.writeMetaU32(fc.rowCount);
        sink.writeMetaU32(fc.aliveCount);
        sink.writeMeta(fc.aliveWords.data(), sizeof(fc.aliveWords));

        for (size_t c = 0; c < snap.schema.size(); ++c) {
            const ColumnChunk& col = *fc.columns[c];
            const size_t fixedBytes = static_cast<size_t>(fc.rowCount) * colTypeElemSize(col.type());
            const size_t strBytes = (col.type() == ColType::String) ? col.stringArenaSize() : 0u;
            const bool hasNulls = col.hasNulls() && col.nullWordsData() != nullptr;
            uint8_t colFlags = hasNulls ? kColFlagHasNulls : 0u;

            if (!opts.compress) {
                sink.writeMetaU64(fixedBytes);
                sink.writeMetaU64(strBytes);
                sink.writeMetaU8(colFlags);
                sink.padToAlign64();
                if (fixedBytes > 0) sink.write(col.rawData(), fixedBytes);
                if (strBytes > 0) sink.write(col.stringArenaData(), strBytes);
            } else {
                const uint8_t* fixedSrc = col.rawData();
                size_t storedFixed = fixedBytes;
                if (fixedBytes >= 64 && lz4BlockCompress(col.rawData(), fixedBytes, lz4Fixed) &&
                    lz4Fixed.size() < fixedBytes) {
                    colFlags |= kColFlagFixedLz4;
                    fixedSrc = lz4Fixed.data();
                    storedFixed = lz4Fixed.size();
                }
                const uint8_t* strSrc = col.stringArenaData();
                size_t storedStr = strBytes;
                if (strBytes >= 64 && lz4BlockCompress(col.stringArenaData(), strBytes, lz4Str) &&
                    lz4Str.size() < strBytes) {
                    colFlags |= kColFlagStrLz4;
                    strSrc = lz4Str.data();
                    storedStr = lz4Str.size();
                }
                sink.writeMetaU64(fixedBytes);
                sink.writeMetaU64(storedFixed);
                sink.writeMetaU64(strBytes);
                sink.writeMetaU64(storedStr);
                sink.writeMetaU8(colFlags);
                sink.padToAlign64();
                if (storedFixed > 0) sink.write(fixedSrc, storedFixed);
                if (storedStr > 0) sink.write(strSrc, storedStr);
            }
            if (hasNulls) {
                sink.padToAlign64();
                sink.write(col.nullWordsData(), kNullBitmapBytes);
            }
            if (!sink.ok()) return false;
        }
    }

    sink.padToAlign64();
    outHdr.indexSectionOffset = sink.bytesWritten();
    writeIndexSectionV3(snap, sink);
    if (!sink.ok()) return false;

    outHdr.payloadBytes = sink.bytesWritten();
    outHdr.metaChecksum = sink.metaChecksum();
    outHdr.payloadChecksum = sink.payloadChecksum();
    return true;
}

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
                           size_t dictSize, bool fullScan, std::string* err) {
    const size_t expectedFixed = static_cast<size_t>(rowCount) * colTypeElemSize(type);
    if (fixedBytes != expectedFixed) {
        if (err) *err = "corrupted .brotable fixed payload size";
        return false;
    }
    if (!fullScan) return true;
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
        if (!cur.readMeta(nameLen) || !cur.readMeta(typeByte) || nameLen == 0 || nameLen > 65536 ||
            typeByte > static_cast<uint8_t>(ColType::String) || !cur.has(nameLen)) {
            if (err) *err = "corrupted .brotable column schema";
            return false;
        }
        std::string name(reinterpret_cast<const char*>(cur.ptr + cur.pos), nameLen);
        if (cur.metaHasher) cur.metaHasher->update(name.data(), nameLen);
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
        if (!cur.readMeta(len) || len > 16777216u || !cur.has(len)) {
            if (err) *err = "corrupted .brotable dictionary";
            return false;
        }
        std::string_view sv(reinterpret_cast<const char*>(cur.ptr + cur.pos), len);
        if (cur.metaHasher) cur.metaHasher->update(sv.data(), len);
        cur.pos += len;
        outDict->intern(sv);
    }
    return true;
}

bool parseLegacyV2Specs(ByteCursor& cur, uint32_t colCount, const std::vector<ColumnDef>& cols,
                        uint32_t hashCount, uint32_t rangeCount, uint32_t spatialCount,
                        PersistedIndexSpecs& outSpecs, std::string* err) {
    for (uint32_t i = 0; i < hashCount; ++i) {
        uint32_t col = 0;
        if (!cur.read(col) || col >= colCount) {
            if (err) *err = "corrupted .brotable hash index spec";
            return false;
        }
        outSpecs.hashCols.push_back(col);
    }
    for (uint32_t i = 0; i < rangeCount; ++i) {
        uint32_t col = 0;
        if (!cur.read(col) || col >= colCount) {
            if (err) *err = "corrupted .brotable range index spec";
            return false;
        }
        outSpecs.rangeCols.push_back(col);
    }
    for (uint32_t i = 0; i < spatialCount; ++i) {
        uint32_t nameLen = 0;
        uint8_t dims = 0;
        double cellSize = 0.0;
        if (!cur.read(nameLen) || nameLen == 0 || nameLen > 4096 || !cur.has(nameLen)) {
            if (err) *err = "corrupted .brotable spatial index name";
            return false;
        }
        SpatialMetaSpec sp;
        sp.name.assign(reinterpret_cast<const char*>(cur.ptr + cur.pos), nameLen);
        cur.pos += nameLen;
        if (!cur.read(dims) || !cur.read(cellSize) || (dims != 2 && dims != 3) ||
            !std::isfinite(cellSize) || cellSize <= 0.0) {
            if (err) *err = "corrupted .brotable spatial index dimensions";
            return false;
        }
        sp.cellSize = cellSize;
        sp.cols.resize(dims);
        for (uint8_t d = 0; d < dims; ++d) {
            uint32_t col = 0;
            if (!cur.read(col) || col >= colCount) {
                if (err) *err = "corrupted .brotable spatial index column";
                return false;
            }
            const ColType ct = cols[col].type;
            if (ct == ColType::String || ct == ColType::Dict || ct == ColType::Bool) {
                if (err) *err = "invalid non-numeric spatial index column";
                return false;
            }
            sp.cols[d] = col;
        }
        outSpecs.spatials.push_back(std::move(sp));
    }
    return true;
}

std::shared_ptr<TableSnapshot> parseSnapshotImpl(const uint8_t* data, size_t size,
                                                 std::shared_ptr<const void> owner,
                                                 PersistedIndexSpecs* outIdxSpecs,
                                                 bool verifyFullPayload,
                                                 bool allowCompressed,
                                                 uint32_t* outFormatVersion,
                                                 std::string* err) {
    if (!data || size < 8) {
        if (err) *err = "empty or truncated .brotable data";
        return nullptr;
    }

    uint32_t fmtVer = 0;
    if (std::memcmp(data, kMagicV4, 8) == 0) fmtVer = kFormatVersionV4;
    else if (std::memcmp(data, kMagicV3, 8) == 0) fmtVer = kFormatVersionV3;
    else if (std::memcmp(data, kMagicV2, 8) == 0) fmtVer = kFormatVersionV2;
    else if (std::memcmp(data, kMagicV1, 8) == 0) fmtVer = kFormatVersionV1;
    else {
        if (err) *err = "invalid .brotable magic";
        return nullptr;
    }
    if (fmtVer == kFormatVersionV4 && !allowCompressed) {
        if (err) *err = ".brotable v4 compressed files cannot be zero-copy mmapped; use load() instead";
        return nullptr;
    }
    if (outFormatVersion) *outFormatVersion = fmtVer;

    IncrementalFnv1a64 metaHasher;
    ByteCursor cur{data, size, 0, (fmtVer >= kFormatVersionV3) ? &metaHasher : nullptr};
    uint32_t colCount = 0, chunkCount = 0, dictCount = 0;
    uint32_t hashCount = 0, rangeCount = 0, spatialCount = 0;
    uint64_t headerAlive = 0, expectedIdxOff = 0, expectedMetaCsum = 0;

    if (fmtVer >= kFormatVersionV3) {
        FileHeaderV3 hdr{};
        if (!cur.read(hdr) || hdr.version != fmtVer) {
            if (err) *err = "invalid .brotable v3/v4 header";
            return nullptr;
        }
        if (hdr.payloadBytes != size || size < sizeof(FileHeaderV3) ||
            hdr.indexSectionOffset < sizeof(FileHeaderV3) || hdr.indexSectionOffset > size) {
            if (err) *err = "truncated or corrupted .brotable v3/v4 size";
            return nullptr;
        }
        if (verifyFullPayload) {
            const uint64_t actual =
                computeRawFnv1a64(data + sizeof(FileHeaderV3), size - sizeof(FileHeaderV3));
            if (actual != hdr.payloadChecksum) {
                if (err) *err = ".brotable v3/v4 payload checksum mismatch";
                return nullptr;
            }
        }
        colCount = hdr.colCount;
        chunkCount = hdr.chunkCount;
        headerAlive = hdr.totalAliveRows;
        dictCount = hdr.dictCount;
        hashCount = hdr.hashIndexCount;
        rangeCount = hdr.rangeIndexCount;
        spatialCount = hdr.spatialIndexCount;
        expectedIdxOff = hdr.indexSectionOffset;
        expectedMetaCsum = hdr.metaChecksum;
    } else if (fmtVer == kFormatVersionV2) {
        FileHeaderV2 hdr{};
        if (!cur.read(hdr) || hdr.version != kFormatVersionV2 ||
            hdr.payloadBytes != size || size < sizeof(FileHeaderV2)) {
            if (err) *err = "invalid or truncated .brotable v2 header";
            return nullptr;
        }
        if (verifyFullPayload) {
            const uint64_t actual =
                computeRawFnv1a64(data + sizeof(FileHeaderV2), size - sizeof(FileHeaderV2));
            if (actual != hdr.checksum) {
                if (err) *err = ".brotable v2 checksum mismatch";
                return nullptr;
            }
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
        if (!cur.read(hdr) || hdr.version != kFormatVersionV1 || hdr.payloadBytes != size) {
            if (err) *err = "invalid or truncated .brotable v1 header";
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

    PersistedIndexSpecs legacySpecs;
    if (fmtVer == kFormatVersionV2) {
        if (!parseLegacyV2Specs(cur, colCount, cols, hashCount, rangeCount, spatialCount, legacySpecs, err)) {
            return nullptr;
        }
    }

    auto snap = std::make_shared<TableSnapshot>();
    for (auto& cd : cols) snap->schema.addColumn(std::move(cd.name), cd.type);
    snap->schemaHash = snap->schema.computeHash();
    snap->dict = std::move(dict);
    snap->version = 1;
    snap->structureVersion = 1;
    snap->chunks.reserve(chunkCount);

    std::shared_ptr<const void> chunkOwner =
        owner ? owner : std::shared_ptr<const void>(data, [](const void*) {});

    for (uint32_t ci = 0; ci < chunkCount; ++ci) {
        uint32_t rowCount = 0, aliveCount = 0;
        std::array<uint64_t, kChunkAliveWords> aliveWords{};
        if (!cur.readMeta(rowCount) || !cur.readMeta(aliveCount) || !cur.readMeta(aliveWords) ||
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
            uint64_t fixedBytes = 0, storedFixed = 0, strBytes = 0, storedStr = 0;
            uint8_t colFlags = 0;
            if (fmtVer == kFormatVersionV4) {
                if (!cur.readMeta(fixedBytes) || !cur.readMeta(storedFixed) ||
                    !cur.readMeta(strBytes) || !cur.readMeta(storedStr) || !cur.readMeta(colFlags)) {
                    if (err) *err = "corrupted .brotable v4 column header";
                    return nullptr;
                }
            } else {
                if (!cur.readMeta(fixedBytes) || !cur.readMeta(strBytes)) {
                    if (err) *err = "corrupted .brotable column header";
                    return nullptr;
                }
                if (fmtVer >= kFormatVersionV2 && !cur.readMeta(colFlags)) {
                    if (err) *err = "corrupted .brotable column flags";
                    return nullptr;
                }
                storedFixed = fixedBytes;
                storedStr = strBytes;
            }
            if (!cur.skipToAlign64() || !cur.has(static_cast<size_t>(storedFixed))) {
                if (err) *err = "corrupted .brotable column payload";
                return nullptr;
            }
            const uint8_t* storedFixedPtr = cur.ptr + cur.pos;
            cur.pos += static_cast<size_t>(storedFixed);
            if (!cur.has(static_cast<size_t>(storedStr))) {
                if (err) *err = "corrupted .brotable string arena";
                return nullptr;
            }
            const uint8_t* storedStrPtr = cur.ptr + cur.pos;
            cur.pos += static_cast<size_t>(storedStr);

            const uint64_t* nullPtr = nullptr;
            if (fmtVer >= kFormatVersionV2 && (colFlags & kColFlagHasNulls) != 0) {
                if (!cur.skipToAlign64() || !cur.has(kNullBitmapBytes)) {
                    if (err) *err = "corrupted .brotable null bitmap";
                    return nullptr;
                }
                nullPtr = reinterpret_cast<const uint64_t*>(cur.ptr + cur.pos);
                cur.pos += kNullBitmapBytes;
            }

            std::vector<uint8_t> decompFixed, decompStr;
            const uint8_t* fixedPtr = storedFixedPtr;
            const uint8_t* strPtr = storedStrPtr;
            if (fmtVer == kFormatVersionV4 && (colFlags & kColFlagFixedLz4) != 0) {
                decompFixed.resize(static_cast<size_t>(fixedBytes));
                if (!lz4BlockDecompress(storedFixedPtr, static_cast<size_t>(storedFixed),
                                        decompFixed.data(), static_cast<size_t>(fixedBytes), err)) {
                    return nullptr;
                }
                fixedPtr = decompFixed.data();
            }
            if (fmtVer == kFormatVersionV4 && (colFlags & kColFlagStrLz4) != 0) {
                decompStr.resize(static_cast<size_t>(strBytes));
                if (!lz4BlockDecompress(storedStrPtr, static_cast<size_t>(storedStr),
                                        decompStr.data(), static_cast<size_t>(strBytes), err)) {
                    return nullptr;
                }
                strPtr = decompStr.data();
            }

            if (!validateColumnPayload(cols[c].type, rowCount, fixedPtr,
                                       static_cast<size_t>(fixedBytes),
                                       static_cast<size_t>(strBytes),
                                       nullPtr, snap->dict->size(), verifyFullPayload, err)) {
                return nullptr;
            }

            if (owner && fmtVer != kFormatVersionV4) {
                fc.columns.push_back(ColumnChunk::createMapped(
                    cols[c].type, fixedPtr, static_cast<size_t>(fixedBytes),
                    strPtr, static_cast<size_t>(strBytes), owner, nullPtr));
            } else {
                auto owned = ColumnChunk::createMapped(
                                 cols[c].type, fixedPtr, static_cast<size_t>(fixedBytes),
                                 strPtr, static_cast<size_t>(strBytes), chunkOwner, nullPtr)
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

    if (fmtVer >= kFormatVersionV3) {
        if (!cur.skipToAlign64() || cur.pos != expectedIdxOff) {
            if (err) *err = "corrupted .brotable v3/v4 index section offset";
            return nullptr;
        }
        std::shared_ptr<const void> idxOwner =
            (owner && fmtVer != kFormatVersionV4) ? chunkOwner : std::shared_ptr<const void>{};
        if (!parseIndexSectionV3(cur, hashCount, rangeCount, spatialCount, *snap, idxOwner, err)) {
            return nullptr;
        }
        if (cur.pos != size || metaHasher.finish() != expectedMetaCsum) {
            if (err) *err = ".brotable v3/v4 metadata checksum mismatch";
            return nullptr;
        }
    } else if (outIdxSpecs) {
        *outIdxSpecs = std::move(legacySpecs);
    }

    return snap;
}

std::shared_ptr<MappedFileOwner> mapReadOnlyFile(const std::string& path, std::string* outError) {
    auto owner = std::make_shared<MappedFileOwner>();
#ifdef _WIN32
    const std::wstring wpath = utf8ToWide(path);
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

std::vector<uint8_t> serializeSnapshotBytes(const TableSnapshot& snap, const SaveOptions& opts) {
    std::vector<uint8_t> buf;
    buf.reserve(sizeof(FileHeaderV3) + snap.rowCount() * 16 + 512);
    VectorByteSink sink(buf);
    FileHeaderV3 hdr{};
    if (writeSnapshotStreamImpl(snap, sink, opts, hdr)) {
        sink.finalizeHeader(hdr);
    } else {
        buf.clear();
    }
    return buf;
}

std::shared_ptr<Table> deserializeTableBytes(const uint8_t* data, size_t size,
                                             std::string* outError) {
    PersistedIndexSpecs specs;
    uint32_t fmtVer = 0;
    auto snap = parseSnapshotImpl(data, size, nullptr, &specs, true, true, &fmtVer, outError);
    if (!snap) return nullptr;
    auto tbl = Table::fromSnapshot(*snap);
    if (fmtVer < kFormatVersionV3) applyPersistedIndexes(*tbl, specs);
    return tbl;
}

bool saveSnapshotToFile(const TableSnapshot& snap, const std::string& path,
                        const SaveOptions& opts, std::string* outError) {
    const uint64_t seq = g_tmpSeq.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    const uint64_t pid = static_cast<uint64_t>(GetCurrentProcessId());
#else
    const uint64_t pid = static_cast<uint64_t>(getpid());
#endif
    const std::string tmpPath =
        path + ".tmp." + std::to_string(pid) + "." + std::to_string(seq);
    FileByteSink sink(path, tmpPath);
    FileHeaderV3 hdr{};
    if (!writeSnapshotStreamImpl(snap, sink, opts, hdr) || !sink.finalize(hdr)) {
        if (outError) *outError = sink.error();
        return false;
    }
    return true;
}

std::shared_ptr<Table> loadTableFromFile(const std::string& path, std::string* outError) {
    auto owner = mapReadOnlyFile(path, outError);
    if (!owner) return nullptr;
    PersistedIndexSpecs specs;
    uint32_t fmtVer = 0;
    auto snap = parseSnapshotImpl(owner->data, owner->size, owner, &specs, true, true, &fmtVer, outError);
    if (!snap) return nullptr;
    auto tbl = Table::fromSnapshot(*snap);
    if (fmtVer < kFormatVersionV3) applyPersistedIndexes(*tbl, specs);
    return tbl;
}

std::shared_ptr<const TableSnapshot> mmapSnapshotFromFile(const std::string& path,
                                                          std::string* outError,
                                                          bool verifyChecksum) {
    auto owner = mapReadOnlyFile(path, outError);
    if (!owner) return nullptr;
    PersistedIndexSpecs specs;
    uint32_t fmtVer = 0;
    auto snap = parseSnapshotImpl(
        owner->data, owner->size, owner, &specs, verifyChecksum, false, &fmtVer, outError);
    if (snap && fmtVer < kFormatVersionV3) {
        applyPersistedIndexesToSnapshot(*snap, specs);
    }
    return snap;
}

namespace io_internal {

std::shared_ptr<const void> mapReadOnlyFileShared(const std::string& path,
                                                  const uint8_t*& outData,
                                                  size_t& outSize,
                                                  std::string* outError) {
    auto owner = mapReadOnlyFile(path, outError);
    if (!owner) return nullptr;
    outData = owner->data;
    outSize = owner->size;
    return owner;
}

bool writeSnapshotStream(const TableSnapshot& snap, ByteSink& sink,
                         const SaveOptions& opts, FileHeaderV3& outHdr) {
    return writeSnapshotStreamImpl(snap, sink, opts, outHdr);
}

std::shared_ptr<TableSnapshot> parseSnapshotRange(const uint8_t* data, size_t size,
                                                  std::shared_ptr<const void> owner,
                                                  PersistedIndexSpecs* outIdxSpecs,
                                                  bool verifyFullPayload,
                                                  bool allowCompressed,
                                                  uint32_t* outFormatVersion,
                                                  std::string* err) {
    return parseSnapshotImpl(data, size, std::move(owner), outIdxSpecs, verifyFullPayload,
                             allowCompressed, outFormatVersion, err);
}

}  // namespace io_internal

}  // namespace bro::table
