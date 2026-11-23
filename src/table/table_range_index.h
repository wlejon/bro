#pragma once

#include "table/table_chunk.h"
#include "table/table_types.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bro::table {

/// Ordered secondary range index supporting `<`, `<=`, `>`, `>=`, `=`, and top-K scans.
class RangeIndex {
public:
    struct NumEntry {
        union {
            int64_t iKey;
            uint64_t uKey;
            double fKey;
        };
        RowLoc loc{};

        NumEntry() : uKey(0), loc{} {}
    };

    struct MappedStrEntry {
        uint32_t offset = 0;
        uint32_t len = 0;
        RowLoc loc{};
    };

    struct StrEntry {
        std::string key;
        RowLoc loc{};
    };

    static_assert(sizeof(NumEntry) == 16, "RangeIndex::NumEntry must be 16 bytes");
    static_assert(sizeof(MappedStrEntry) == 16, "RangeIndex::MappedStrEntry must be 16 bytes");

    RangeIndex() = default;
    RangeIndex(size_t colIdx, ColType colType);

    static std::shared_ptr<RangeIndex> createMappedNumeric(
        size_t colIdx, ColType colType,
        const NumEntry* entries, size_t count,
        std::shared_ptr<const void> owner);

    static std::shared_ptr<RangeIndex> createMappedString(
        size_t colIdx, ColType colType,
        const MappedStrEntry* entries, size_t count,
        const char* blob, size_t blobBytes,
        std::shared_ptr<const void> owner);

    std::shared_ptr<RangeIndex> clone() const;
    void clear();

    void insert(const ColumnChunk& chunk, uint32_t row, RowLoc loc,
                const StringDictionary* dict);
    bool erase(const ColumnChunk& chunk, uint32_t row, RowLoc loc,
               const StringDictionary* dict);

    /// Ensures internal entries are sorted (const/mutable internal state; call before freeze).
    void sortIfNeeded() const;

    /// Appends candidate RowLocs in `[lo, hi]` (either bound may be nullptr for open range).
    void lookupRange(const ScalarValue* lo, bool incLo,
                     const ScalarValue* hi, bool incHi,
                     const StringDictionary* dict,
                     std::vector<RowLoc>& out) const;

    /// Iterates entries in ascending or descending key order; stops if `visit` returns false.
    void forEachOrdered(bool descending,
                        const std::function<bool(RowLoc)>& visit) const;

    size_t colIdx() const { return colIdx_; }
    ColType colType() const { return colType_; }
    bool isMapped() const { return mappedOwner_ != nullptr; }
    bool isStringOrdered() const {
        return colType_ == ColType::String || colType_ == ColType::Dict;
    }
    size_t size() const {
        if (isStringOrdered()) {
            return mappedOwner_ ? mappedStrCount_ : strEntries_.size();
        }
        return mappedOwner_ ? mappedNumCount_ : numEntries_.size();
    }

    const NumEntry* numData() const {
        return mappedOwner_ ? mappedNum_ : numEntries_.data();
    }
    size_t numCount() const {
        return mappedOwner_ ? mappedNumCount_ : numEntries_.size();
    }
    const std::vector<StrEntry>& strEntries() const { return strEntries_; }
    const MappedStrEntry* mappedStrData() const { return mappedStr_; }
    size_t mappedStrCount() const { return mappedStrCount_; }
    const char* mappedStrBlob() const { return mappedStrBlob_; }
    size_t mappedStrBlobBytes() const { return mappedStrBlobBytes_; }

private:
    static bool makeNumEntry(ColType colType, const ScalarValue& v, RowLoc loc, NumEntry& out);
    static void convertBound(ColType colType, const ScalarValue& v, bool isLo, bool& inc,
                             NumEntry& out, bool& alwaysEmpty, bool& unbounded);
    int compareNum(const NumEntry& a, const NumEntry& b) const;
    void ensureOwned();
    std::string_view strViewAt(size_t i) const;
    RowLoc strLocAt(size_t i) const;
    size_t strCount() const { return mappedOwner_ ? mappedStrCount_ : strEntries_.size(); }

    size_t colIdx_ = 0;
    ColType colType_ = ColType::I32;
    mutable std::vector<NumEntry> numEntries_;
    mutable std::vector<StrEntry> strEntries_;
    mutable bool sorted_ = true;
    std::shared_ptr<const void> mappedOwner_;
    const NumEntry* mappedNum_ = nullptr;
    size_t mappedNumCount_ = 0;
    const MappedStrEntry* mappedStr_ = nullptr;
    size_t mappedStrCount_ = 0;
    const char* mappedStrBlob_ = nullptr;
    size_t mappedStrBlobBytes_ = 0;
};

/// 2D or 3D uniform-grid hashed spatial index supporting AABB, radius, and K-nearest queries.
class SpatialIndex {
public:
    static constexpr uint32_t kInvalidNode = 0xFFFFFFFFu;

    struct Bucket {
        int32_t cx = 0;
        int32_t cy = 0;
        int32_t cz = 0;
        uint32_t headNode = kInvalidNode;
        uint32_t occupied = 0;
    };

    struct Node {
        RowLoc loc{};
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        uint32_t next = kInvalidNode;
        uint32_t pad = 0;
    };

    static_assert(sizeof(Bucket) == 20, "SpatialIndex::Bucket must be tightly packed 20 bytes");
    static_assert(sizeof(Node) == 40, "SpatialIndex::Node must be 40 bytes with explicit pad");

    SpatialIndex() = default;
    SpatialIndex(std::string name, std::vector<size_t> cols, double cellSize = 16.0);

    static std::shared_ptr<SpatialIndex> createMapped(
        std::string name, std::vector<size_t> cols, double cellSize,
        const Bucket* buckets, size_t bucketCount,
        const Node* nodes, size_t nodeCount,
        size_t usedBuckets, size_t entryCount,
        std::shared_ptr<const void> owner);

    std::shared_ptr<SpatialIndex> clone() const;
    void clear(size_t expectedItems = 0);

    void insert(double x, double y, double z, RowLoc loc);
    bool erase(double x, double y, double z, RowLoc loc);

    void queryAABB(double minX, double minY, double minZ,
                   double maxX, double maxY, double maxZ,
                   std::vector<RowLoc>& out) const;

    void queryRadius(double cx, double cy, double cz, double radius,
                     std::vector<RowLoc>& out) const;

    void queryNearest(double cx, double cy, double cz, size_t k, double maxRadius,
                      std::vector<std::pair<RowLoc, double>>& out) const;

    const std::string& name() const { return name_; }
    const std::array<size_t, 3>& cols() const { return cols_; }
    uint8_t dims() const { return dims_; }
    double cellSize() const { return cellSize_; }
    bool isMapped() const { return mappedOwner_ != nullptr; }
    size_t usedBuckets() const { return usedBuckets_; }
    size_t entryCount() const { return entryCount_; }
    size_t nodeCapacity() const { return mappedOwner_ ? mappedNodeCount_ : nodes_.size(); }

    const Bucket* bucketData() const {
        return mappedOwner_ ? mappedBuckets_ : buckets_.data();
    }
    size_t bucketCount() const {
        return mappedOwner_ ? mappedBucketCount_ : buckets_.size();
    }
    const Node* nodeData() const {
        return mappedOwner_ ? mappedNodes_ : nodes_.data();
    }
    size_t nodeCount() const {
        return mappedOwner_ ? mappedNodeCount_ : nodes_.size();
    }

private:
    int32_t cellCoord(double v) const;
    static uint64_t hashCell(int32_t cx, int32_t cy, int32_t cz);
    void ensureOwned();
    void rehash(size_t newCap);
    size_t findSlot(int32_t cx, int32_t cy, int32_t cz) const;
    uint32_t findOccupiedHead(int32_t cx, int32_t cy, int32_t cz) const;

    std::string name_;
    std::array<size_t, 3> cols_{};
    uint8_t dims_ = 2;
    double cellSize_ = 16.0;
    std::vector<Bucket> buckets_;
    std::vector<Node> nodes_;
    std::shared_ptr<const void> mappedOwner_;
    const Bucket* mappedBuckets_ = nullptr;
    size_t mappedBucketCount_ = 0;
    const Node* mappedNodes_ = nullptr;
    size_t mappedNodeCount_ = 0;
    uint32_t freeHead_ = kInvalidNode;
    size_t usedBuckets_ = 0;
    size_t entryCount_ = 0;
};

}  // namespace bro::table
