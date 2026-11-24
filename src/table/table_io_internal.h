#pragma once

#include "table/table.h"
#include "table/table_io.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace bro::table::io_internal {

inline constexpr char kMagicV1[8] = {'B', 'R', 'O', 'T', 'B', 'L', '0', '1'};
inline constexpr char kMagicV2[8] = {'B', 'R', 'O', 'T', 'B', 'L', '0', '2'};
inline constexpr char kMagicV3[8] = {'B', 'R', 'O', 'T', 'B', 'L', '0', '3'};
inline constexpr char kMagicV4[8] = {'B', 'R', 'O', 'T', 'B', 'L', '0', '4'};
inline constexpr uint32_t kFormatVersionV1 = 1;
inline constexpr uint32_t kFormatVersionV2 = 2;
inline constexpr uint32_t kFormatVersionV3 = 3;
inline constexpr uint32_t kFormatVersionV4 = 4;
inline constexpr uint32_t kFlagLz4Compressed = 1u;
inline constexpr uint8_t kColFlagHasNulls = 1u;
inline constexpr uint8_t kColFlagFixedLz4 = 2u;
inline constexpr uint8_t kColFlagStrLz4 = 4u;
inline constexpr uint32_t kMaxCols = 4096;
inline constexpr uint32_t kMaxChunks = 1048576;
inline constexpr uint32_t kMaxDictEntries = 16777216;
inline constexpr uint64_t kMaxIndexElements = 1ULL << 31;
inline constexpr size_t kNullBitmapBytes = sizeof(std::array<uint64_t, kChunkAliveWords>);

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

struct FileHeaderV3 {
    char magic[8];
    uint32_t version;
    uint32_t colCount;
    uint32_t chunkCount;
    uint32_t hashIndexCount;
    uint64_t totalAliveRows;
    uint32_t dictCount;
    uint32_t rangeIndexCount;
    uint32_t spatialIndexCount;
    uint32_t flags;
    uint64_t payloadBytes;
    uint64_t indexSectionOffset;
    uint64_t metaChecksum;
    uint64_t payloadChecksum;
};
#pragma pack(pop)

static_assert(sizeof(FileHeaderV3) == 80, "FileHeaderV3 must be 80 bytes");

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

struct IncrementalFnv1a64 {
    uint64_t h = 14695981039346656037ULL;

    void reset() { h = 14695981039346656037ULL; }

    void update(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
    }

    uint64_t finish() const { return h; }
};

class ByteSink {
public:
    virtual ~ByteSink() = default;

    void beginChecksums() {
        metaHasher_.reset();
        payloadHasher_.reset();
        hashing_ = true;
    }

    void write(const void* data, size_t size) {
        if (!ok_ || size == 0) return;
        if (hashing_) payloadHasher_.update(data, size);
        bytesWritten_ += size;
        writeBytesRaw(data, size);
    }

    void writeMeta(const void* data, size_t size) {
        if (!ok_ || size == 0) return;
        if (hashing_) {
            metaHasher_.update(data, size);
            payloadHasher_.update(data, size);
        }
        bytesWritten_ += size;
        writeBytesRaw(data, size);
    }

    template <typename T>
    void writeMetaVal(const T& v) { writeMeta(&v, sizeof(T)); }

    void writeU8(uint8_t v) { write(&v, sizeof(v)); }
    void writeMetaU8(uint8_t v) { writeMeta(&v, sizeof(v)); }
    void writeU32(uint32_t v) { write(&v, sizeof(v)); }
    void writeMetaU32(uint32_t v) { writeMeta(&v, sizeof(v)); }
    void writeU64(uint64_t v) { write(&v, sizeof(v)); }
    void writeMetaU64(uint64_t v) { writeMeta(&v, sizeof(v)); }
    void writeMetaF64(double v) { writeMeta(&v, sizeof(v)); }

    void padToAlign64() {
        const size_t rem = static_cast<size_t>(bytesWritten_ & 63ULL);
        if (rem == 0) return;
        static constexpr uint8_t kZeros[64]{};
        write(kZeros, 64u - rem);
    }

    void fail(std::string msg) {
        if (ok_) error_ = std::move(msg);
        ok_ = false;
    }

    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }
    uint64_t bytesWritten() const { return bytesWritten_; }
    uint64_t metaChecksum() const { return metaHasher_.finish(); }
    uint64_t payloadChecksum() const { return payloadHasher_.finish(); }

protected:
    virtual void writeBytesRaw(const void* data, size_t size) = 0;

    bool ok_ = true;
    std::string error_;
    uint64_t bytesWritten_ = 0;
    bool hashing_ = false;
    IncrementalFnv1a64 metaHasher_;
    IncrementalFnv1a64 payloadHasher_;
};

struct ByteCursor {
    const uint8_t* ptr = nullptr;
    size_t size = 0;
    size_t pos = 0;
    IncrementalFnv1a64* metaHasher = nullptr;

    bool has(size_t n) const { return pos <= size && n <= (size - pos); }

    template <typename T>
    bool read(T& out) {
        if (!has(sizeof(T))) return false;
        std::memcpy(&out, ptr + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }

    template <typename T>
    bool readMeta(T& out) {
        if (!has(sizeof(T))) return false;
        std::memcpy(&out, ptr + pos, sizeof(T));
        if (metaHasher) metaHasher->update(&out, sizeof(T));
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

void writeIndexSectionV3(const TableSnapshot& snap, ByteSink& sink);

bool parseIndexSectionV3(ByteCursor& cur,
                         uint32_t hashCount, uint32_t rangeCount, uint32_t spatialCount,
                         TableSnapshot& snap, const std::shared_ptr<const void>& owner,
                         std::string* err);

void applyPersistedIndexes(Table& tbl, const PersistedIndexSpecs& specs);
void applyPersistedIndexesToSnapshot(TableSnapshot& snap, const PersistedIndexSpecs& specs);

std::shared_ptr<const void> mapReadOnlyFileShared(const std::string& path,
                                                  const uint8_t*& outData,
                                                  size_t& outSize,
                                                  std::string* outError);

bool writeSnapshotStream(const TableSnapshot& snap, ByteSink& sink,
                         const SaveOptions& opts, FileHeaderV3& outHdr);

std::shared_ptr<TableSnapshot> parseSnapshotRange(const uint8_t* data, size_t size,
                                                  std::shared_ptr<const void> owner,
                                                  PersistedIndexSpecs* outIdxSpecs,
                                                  bool verifyFullPayload,
                                                  bool allowCompressed,
                                                  uint32_t* outFormatVersion,
                                                  std::string* err);

}  // namespace bro::table::io_internal
