#pragma once

#include "table/table_chunk.h"
#include "table/table_types.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace bro::table {

class TableSnapshot;

/// Open-addressed hash index from canonical column value key -> list of RowLoc.
/// Uses an inline head + flat linked pool to avoid per-key heap allocations.
class HashIndex {
public:
    static constexpr uint32_t kInvalidNode = 0xFFFFFFFFu;

    HashIndex() = default;
    explicit HashIndex(size_t colIdx, ColType colType);

    std::shared_ptr<HashIndex> clone() const;

    void clear(size_t expectedKeys = 0);
    void insert(uint64_t key, RowLoc loc);
    bool erase(uint64_t key, RowLoc loc);

    /// Appends candidate RowLocs matching `key` to `out`.
    void lookupKey(uint64_t key, std::vector<RowLoc>& out) const;

    size_t colIdx() const { return colIdx_; }
    ColType colType() const { return colType_; }
    size_t entryCount() const { return entryCount_; }
    size_t nodeCapacity() const { return nodes_.size(); }

    /// Extracts canonical uint64 key from a ScalarValue or column chunk cell.
    static bool scalarToKey(ColType colType, const ScalarValue& v,
                            const StringDictionary* dict, uint64_t& outKey);
    static uint64_t cellKey(const ColumnChunk& chunk, uint32_t row);

private:
    struct Bucket {
        uint64_t key = 0;
        uint32_t headNode = kInvalidNode;
        bool occupied = false;
    };

    struct Node {
        RowLoc loc{};
        uint32_t next = kInvalidNode;
    };

    void rehash(size_t newCap);
    size_t findSlot(uint64_t key) const;

    size_t colIdx_ = 0;
    ColType colType_ = ColType::I32;
    std::vector<Bucket> buckets_;
    std::vector<Node> nodes_;
    uint32_t freeHead_ = kInvalidNode;
    size_t usedBuckets_ = 0;
    size_t entryCount_ = 0;
};

}  // namespace bro::table
