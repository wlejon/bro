#pragma once

#include "table/table_chunk.h"
#include "table/table_index.h"
#include "table/table_types.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bro::table {

struct Schema {
    std::vector<ColumnDef> columns;
    std::unordered_map<std::string, size_t> nameToIndex;

    void addColumn(std::string name, ColType type);
    int findColumn(std::string_view name) const;
    size_t size() const { return columns.size(); }
    uint64_t computeHash() const;
};

/// Immutable COW snapshot of a Table. Safe to share across threads / Workers.
class TableSnapshot {
public:
    struct FrozenChunk {
        uint32_t rowCount = 0;
        uint32_t aliveCount = 0;
        std::array<uint64_t, kChunkAliveWords> aliveWords{};
        std::vector<std::shared_ptr<const ColumnChunk>> columns;

        bool isAlive(uint32_t row) const {
            return row < rowCount && ((aliveWords[row >> 6] >> (row & 63)) & 1ULL) != 0;
        }
        bool allAlive() const { return aliveCount == rowCount; }
    };

    Schema schema;
    std::shared_ptr<StringDictionary> dict;
    std::vector<FrozenChunk> chunks;
    uint64_t totalAliveRows = 0;
    uint64_t schemaHash = 0;
    uint64_t version = 0;
    std::unordered_map<size_t, std::shared_ptr<const HashIndex>> indexes;

    size_t rowCount() const { return static_cast<size_t>(totalAliveRows); }
    size_t columnCount() const { return schema.size(); }
    size_t chunkCount() const { return chunks.size(); }
    int findColumn(std::string_view name) const { return schema.findColumn(name); }

    ScalarValue getCell(size_t colIdx, uint32_t chunkIdx, uint32_t rowIdx) const;
    const HashIndex* findIndex(size_t colIdx) const;
};

/// Mutable off-heap chunked columnar table with COW snapshots and HashIndexes.
class Table {
public:
    explicit Table(std::vector<ColumnDef> columns = {});

    const Schema& schema() const { return schema_; }
    size_t columnCount() const { return schema_.size(); }
    size_t rowCount() const { return static_cast<size_t>(totalAliveRows_); }
    size_t chunkCount() const { return chunks_.size(); }
    uint64_t version() const { return version_; }
    const std::shared_ptr<StringDictionary>& dict() const { return dict_; }
    void setSharedDict(std::shared_ptr<StringDictionary> dict) {
        if (dict) dict_ = std::move(dict);
    }

    int findColumn(std::string_view name) const { return schema_.findColumn(name); }
    std::string internDecode(uint32_t id) const { return dict_->get(id); }
    uint32_t internString(std::string_view s) { return dict_->intern(s); }

    /// Inserts one row (`values.size()` must match schema; missing entries zeroed).
    /// Returns its RowLoc.
    RowLoc appendRow(std::span<const ScalarValue> values);

    /// Bulk-inserts `count` rows from raw typed column pointers (`colData[c]` may
    /// be nullptr for zero-init; Dict takes `const uint32_t*` or strings via appendRow).
    void appendTypedBatch(uint32_t count, const void* const* colData);

    /// Updates a single cell at `loc` (maintaining COW and secondary indexes).
    bool updateCell(RowLoc loc, size_t colIdx, const ScalarValue& v);

    /// Tombstones a row at `loc` (returns false if already dead/invalid).
    bool removeRow(RowLoc loc);

    /// Resolves Nth alive row (0-based) to physical RowLoc.
    bool resolveAliveRow(size_t aliveOrdinal, RowLoc& out) const;

    /// Reads cell at `loc`.
    ScalarValue getCell(RowLoc loc, size_t colIdx) const;

    /// Packs out tombstones, reclaims dead string bytes, and rebuilds indexes.
    void compact();

    /// Creates or returns existing secondary HashIndex on column `colIdx`.
    bool createIndex(size_t colIdx);
    bool createIndex(std::string_view name);
    bool hasIndex(size_t colIdx) const;
    bool dropIndex(size_t colIdx);

    /// Returns a thread-safe immutable COW snapshot of current table state.
    std::shared_ptr<const TableSnapshot> snapshot() const;

    /// Provides access to underlying chunk for zero-copy views & query execution.
    const TableChunk& chunk(size_t idx) const { return chunks_[idx]; }
    TableChunk& chunkMut(size_t idx) { return chunks_[idx]; }

    /// Detaches any shared chunk columns on `chunkIdx` prior to in-place writes.
    void ensureChunkUnique(size_t chunkIdx);

    /// Marks table mutated directly by bulk kernel update and maintains indexes.
    void notifyColumnUpdated(size_t colIdx);

private:
    TableChunk& ensureTailChunkForAppend();
    void rebuildIndex(size_t colIdx);
    std::shared_ptr<HashIndex>& mutableIndex(size_t colIdx);

    Schema schema_;
    std::shared_ptr<StringDictionary> dict_;
    std::vector<TableChunk> chunks_;
    std::unordered_map<size_t, std::shared_ptr<HashIndex>> indexes_;
    uint64_t totalAliveRows_ = 0;
    uint64_t schemaHash_ = 0;
    uint64_t version_ = 1;
};

}  // namespace bro::table
