#pragma once

#include "table/table_types.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bro::table {

/// Thread-safe append-only string dictionary mapping strings to dense uint32 IDs.
class StringDictionary {
public:
    StringDictionary();

    /// Returns existing ID or interns the string under exclusive lock.
    uint32_t intern(std::string_view s);

    /// Read-only lookup without inserting a new ID.
    std::optional<uint32_t> find(std::string_view s) const;

    /// Returns the string for an existing ID, or empty string if invalid.
    std::string get(uint32_t id) const;

    size_t size() const;

private:
    mutable std::shared_mutex mutex_;
    std::deque<std::string> entries_;
    std::unordered_map<std::string_view, uint32_t> index_;
};

/// Single column's storage within one chunk (up to kChunkRows = 4096 rows).
/// Fixed-width columns allocate full kChunkRows capacity upfront so raw pointers
/// never move during intra-chunk appends; COW clones whenever use_count() > 1.
class ColumnChunk {
public:
    ~ColumnChunk();

    ColumnChunk(const ColumnChunk&) = delete;
    ColumnChunk& operator=(const ColumnChunk&) = delete;

    static std::shared_ptr<ColumnChunk> create(ColType type);

    /// Clones only the active prefix `[0, rowCount)` (or full capacity for fixed types).
    std::shared_ptr<ColumnChunk> clone() const;

    ColType type() const { return type_; }
    uint8_t* rawData() { return fixedData_; }
    const uint8_t* rawData() const { return fixedData_; }
    size_t fixedByteCapacity() const { return fixedByteCapacity_; }

    StringSpan* stringSpans() {
        return reinterpret_cast<StringSpan*>(fixedData_);
    }
    const StringSpan* stringSpans() const {
        return reinterpret_cast<const StringSpan*>(fixedData_);
    }

    std::string_view getString(uint32_t row) const;
    void setString(uint32_t row, std::string_view s);
    void compactStrings(const uint32_t* keptRows, uint32_t keptCount,
                        const ColumnChunk& src, std::vector<uint32_t>* outNewMap = nullptr);
    const uint8_t* stringArenaData() const {
        return reinterpret_cast<const uint8_t*>(stringArena_.data());
    }
    size_t stringArenaSize() const { return stringArena_.size(); }

    template <typename T>
    T* as() {
        return reinterpret_cast<T*>(fixedData_);
    }

    template <typename T>
    const T* as() const {
        return reinterpret_cast<const T*>(fixedData_);
    }

    ScalarValue readCell(uint32_t row, const StringDictionary* dict = nullptr) const;
    void writeCell(uint32_t row, const ScalarValue& v, StringDictionary* dict = nullptr);
    void copyRowFrom(uint32_t dstRow, const ColumnChunk& src, uint32_t srcRow);

private:
    explicit ColumnChunk(ColType type);

    ColType type_;
    uint8_t* fixedData_ = nullptr;
    size_t fixedByteCapacity_ = 0;
    std::vector<char> stringArena_;
};

/// One chunk across all columns of a Table (max kChunkRows rows).
struct TableChunk {
    uint32_t rowCount = 0;
    uint32_t aliveCount = 0;
    std::array<uint64_t, kChunkAliveWords> aliveWords{};
    std::vector<std::shared_ptr<ColumnChunk>> columns;

    bool isAlive(uint32_t row) const {
        if (row >= rowCount) return false;
        return ((aliveWords[row >> 6] >> (row & 63)) & 1ULL) != 0;
    }

    void setAlive(uint32_t row, bool alive);
    bool allAlive() const { return aliveCount == rowCount; }

    /// Ensures the column buffer is uniquely owned before mutating in place.
    std::shared_ptr<ColumnChunk>& mutableColumn(size_t colIdx);
};

}  // namespace bro::table
