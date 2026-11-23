#pragma once

#include "table/table_chunk.h"
#include "table/table_types.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bro::table {

/// Ordered secondary range index supporting `<`, `<=`, `>`, `>=`, `=`, and top-K scans.
class RangeIndex {
public:
    RangeIndex() = default;
    RangeIndex(size_t colIdx, ColType colType);

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
    size_t size() const {
        return isStringOrdered() ? strEntries_.size() : numEntries_.size();
    }

private:
    struct NumEntry {
        int64_t iKey = 0;
        uint64_t uKey = 0;
        double fKey = 0.0;
        RowLoc loc{};
    };

    struct StrEntry {
        std::string key;
        RowLoc loc{};
    };

    bool isStringOrdered() const {
        return colType_ == ColType::String || colType_ == ColType::Dict;
    }

    static bool makeNumEntry(ColType colType, const ScalarValue& v, RowLoc loc, NumEntry& out);
    int compareNum(const NumEntry& a, const NumEntry& b) const;

    size_t colIdx_ = 0;
    ColType colType_ = ColType::I32;
    mutable std::vector<NumEntry> numEntries_;
    mutable std::vector<StrEntry> strEntries_;
    mutable bool sorted_ = true;
};

/// 2D or 3D uniform-grid hashed spatial index supporting AABB, radius, and K-nearest queries.
class SpatialIndex {
public:
    static constexpr uint32_t kInvalidNode = 0xFFFFFFFFu;

    SpatialIndex() = default;
    SpatialIndex(std::string name, std::vector<size_t> cols, double cellSize = 16.0);

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
    size_t entryCount() const { return entryCount_; }

private:
    struct Bucket {
        int32_t cx = 0;
        int32_t cy = 0;
        int32_t cz = 0;
        uint32_t headNode = kInvalidNode;
        bool occupied = false;
    };

    struct Node {
        RowLoc loc{};
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        uint32_t next = kInvalidNode;
    };

    int32_t cellCoord(double v) const;
    static uint64_t hashCell(int32_t cx, int32_t cy, int32_t cz);
    void rehash(size_t newCap);
    size_t findSlot(int32_t cx, int32_t cy, int32_t cz) const;
    uint32_t findOccupiedHead(int32_t cx, int32_t cy, int32_t cz) const;

    std::string name_;
    std::array<size_t, 3> cols_{};
    uint8_t dims_ = 2;
    double cellSize_ = 16.0;
    std::vector<Bucket> buckets_;
    std::vector<Node> nodes_;
    size_t usedBuckets_ = 0;
    size_t entryCount_ = 0;
};

}  // namespace bro::table
