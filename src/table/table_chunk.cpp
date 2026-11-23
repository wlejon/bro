#include "table/table_chunk.h"

#include <mutex>
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>

namespace bro::table {

namespace {

uint8_t* allocAlignedZeroed(size_t bytes) {
    if (bytes == 0) return nullptr;
    constexpr size_t kAlign = 64;
    void* ptr = ::operator new(bytes, std::align_val_t{kAlign});
    std::memset(ptr, 0, bytes);
    return static_cast<uint8_t*>(ptr);
}

void freeAligned(uint8_t* ptr) {
    if (!ptr) return;
    ::operator delete(ptr, std::align_val_t{64});
}

}  // namespace

StringDictionary::StringDictionary() {
    // Reserve ID 0 for empty string so zero-initialized cells decode predictably.
    entries_.emplace_back("");
    index_.emplace(std::string_view(entries_.back()), 0u);
}

uint32_t StringDictionary::intern(std::string_view s) {
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = index_.find(s);
        if (it != index_.end()) return it->second;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = index_.find(s);
    if (it != index_.end()) return it->second;
    const uint32_t id = static_cast<uint32_t>(entries_.size());
    entries_.emplace_back(s);
    index_.emplace(std::string_view(entries_.back()), id);
    return id;
}

std::optional<uint32_t> StringDictionary::find(std::string_view s) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = index_.find(s);
    if (it == index_.end()) return std::nullopt;
    return it->second;
}

std::string StringDictionary::get(uint32_t id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (id >= entries_.size()) return {};
    return entries_[id];
}

size_t StringDictionary::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return entries_.size();
}

ColumnChunk::ColumnChunk(ColType type) : type_(type) {
    fixedByteCapacity_ = static_cast<size_t>(kChunkRows) * colTypeElemSize(type);
    fixedData_ = allocAlignedZeroed(fixedByteCapacity_);
}

ColumnChunk::ColumnChunk(ColType type, const uint8_t* fixedData, size_t fixedBytes,
                         const uint8_t* strArena, size_t strArenaBytes,
                         std::shared_ptr<const void> owner,
                         const uint64_t* nullWords)
    : type_(type),
      fixedData_(const_cast<uint8_t*>(fixedData)),
      fixedByteCapacity_(fixedBytes),
      mappedOwner_(std::move(owner)),
      mappedStringArena_(strArena),
      mappedStringArenaSize_(strArenaBytes),
      mappedNullWords_(nullWords) {
    if (mappedNullWords_) {
        for (uint32_t w = 0; w < kChunkAliveWords; ++w) {
            nullCount_ += static_cast<uint32_t>(std::popcount(mappedNullWords_[w]));
        }
    }
}

ColumnChunk::~ColumnChunk() {
    if (!mappedOwner_) {
        freeAligned(fixedData_);
    }
    fixedData_ = nullptr;
}

std::shared_ptr<ColumnChunk> ColumnChunk::create(ColType type) {
    return std::shared_ptr<ColumnChunk>(new ColumnChunk(type));
}

std::shared_ptr<ColumnChunk> ColumnChunk::createMapped(
    ColType type, const uint8_t* fixedData, size_t fixedBytes,
    const uint8_t* strArena, size_t strArenaBytes,
    std::shared_ptr<const void> owner,
    const uint64_t* nullWords) {
    return std::shared_ptr<ColumnChunk>(
        new ColumnChunk(type, fixedData, fixedBytes, strArena, strArenaBytes,
                        std::move(owner), nullWords));
}

std::shared_ptr<ColumnChunk> ColumnChunk::clone() const {
    auto copy = std::shared_ptr<ColumnChunk>(new ColumnChunk(type_));
    if (fixedData_ && copy->fixedData_) {
        const size_t n = std::min(fixedByteCapacity_, copy->fixedByteCapacity_);
        std::memcpy(copy->fixedData_, fixedData_, n);
    }
    if (hasNulls()) {
        const uint64_t* srcNw = nullWordsData();
        copy->nullWords_ = std::make_unique<std::array<uint64_t, kChunkAliveWords>>();
        if (srcNw) {
            std::memcpy(copy->nullWords_->data(), srcNw, sizeof(*copy->nullWords_));
        } else {
            copy->nullWords_->fill(0);
        }
        copy->nullCount_ = nullCount_;
    }
    if (mappedOwner_) {
        if (mappedStringArena_ && mappedStringArenaSize_ > 0) {
            copy->stringArena_.assign(
                reinterpret_cast<const char*>(mappedStringArena_),
                reinterpret_cast<const char*>(mappedStringArena_ + mappedStringArenaSize_));
        }
    } else {
        copy->stringArena_ = stringArena_;
    }
    return copy;
}

void ColumnChunk::setNull(uint32_t row, bool isNullVal) {
    if (row >= kChunkRows) return;
    if (!isNullVal && !nullWords_) return;
    if (!nullWords_) {
        nullWords_ = std::make_unique<std::array<uint64_t, kChunkAliveWords>>();
        nullWords_->fill(0);
    }
    const uint64_t mask = 1ULL << (row & 63);
    uint64_t& word = (*nullWords_)[row >> 6];
    const bool wasNull = (word & mask) != 0;
    if (isNullVal && !wasNull) {
        word |= mask;
        ++nullCount_;
    } else if (!isNullVal && wasNull) {
        word &= ~mask;
        --nullCount_;
    }
}

std::string_view ColumnChunk::getString(uint32_t row) const {
    if (type_ != ColType::String) return {};
    const size_t spanCap = fixedByteCapacity_ / sizeof(StringSpan);
    if (row >= spanCap) return {};
    const StringSpan span = stringSpans()[row];
    if (span.len == 0) return {};
    const uint8_t* arenaPtr = stringArenaData();
    const size_t arenaLen = stringArenaSize();
    if (!arenaPtr || static_cast<size_t>(span.offset) + span.len > arenaLen) return {};
    return std::string_view(reinterpret_cast<const char*>(arenaPtr) + span.offset, span.len);
}

void ColumnChunk::setString(uint32_t row, std::string_view s) {
    if (type_ != ColType::String || row >= kChunkRows) return;
    StringSpan& span = stringSpans()[row];
    if (s.empty()) {
        span = {0, 0};
        return;
    }
    if (s.size() <= span.len &&
        static_cast<size_t>(span.offset) + s.size() <= stringArena_.size()) {
        std::memcpy(stringArena_.data() + span.offset, s.data(), s.size());
        span.len = static_cast<uint32_t>(s.size());
        return;
    }
    const uint32_t off = static_cast<uint32_t>(stringArena_.size());
    stringArena_.insert(stringArena_.end(), s.begin(), s.end());
    span = {off, static_cast<uint32_t>(s.size())};
}

void ColumnChunk::compactStrings(const uint32_t* keptRows, uint32_t keptCount,
                                 const ColumnChunk& src, std::vector<uint32_t>*) {
    if (type_ != ColType::String || src.type_ != ColType::String) return;
    stringArena_.clear();
    std::memset(fixedData_, 0, fixedByteCapacity_);
    nullWords_.reset();
    nullCount_ = 0;
    for (uint32_t dst = 0; dst < keptCount; ++dst) {
        if (src.isNull(keptRows[dst])) {
            setNull(dst, true);
        } else {
            setString(dst, src.getString(keptRows[dst]));
        }
    }
}

ScalarValue ColumnChunk::readCell(uint32_t row, const StringDictionary* dict) const {
    if (isNull(row)) return ScalarValue::makeNull();
    switch (type_) {
    case ColType::I32: return ScalarValue::makeI32(as<int32_t>()[row]);
    case ColType::U32: return ScalarValue::makeU32(as<uint32_t>()[row]);
    case ColType::I64: return ScalarValue::makeI64(as<int64_t>()[row]);
    case ColType::F32: return ScalarValue::makeF32(as<float>()[row]);
    case ColType::F64: return ScalarValue::makeF64(as<double>()[row]);
    case ColType::Bool: return ScalarValue::makeBool(as<uint8_t>()[row] != 0);
    case ColType::Dict: {
        const uint32_t id = as<uint32_t>()[row];
        return ScalarValue::makeDict(id, dict ? dict->get(id) : std::string{});
    }
    case ColType::String: return ScalarValue::makeString(std::string(getString(row)));
    }
    return ScalarValue::makeNull();
}

void ColumnChunk::writeCell(uint32_t row, const ScalarValue& v, StringDictionary* dict) {
    if (v.isNull) {
        setNull(row, true);
        if (fixedData_ && row < kChunkRows) {
            const size_t elemBytes = colTypeElemSize(type_);
            std::memset(fixedData_ + static_cast<size_t>(row) * elemBytes, 0, elemBytes);
        }
        return;
    }
    setNull(row, false);
    switch (type_) {
    case ColType::I32:
        as<int32_t>()[row] = (v.type == ColType::F32 || v.type == ColType::F64)
                                 ? static_cast<int32_t>(v.f64)
                                 : static_cast<int32_t>(v.i64);
        break;
    case ColType::U32:
        as<uint32_t>()[row] = (v.type == ColType::F32 || v.type == ColType::F64)
                                  ? static_cast<uint32_t>(static_cast<int64_t>(v.f64))
                                  : static_cast<uint32_t>(v.u64);
        break;
    case ColType::I64:
        as<int64_t>()[row] = (v.type == ColType::F32 || v.type == ColType::F64)
                                 ? static_cast<int64_t>(v.f64)
                                 : v.i64;
        break;
    case ColType::F32:
        as<float>()[row] = static_cast<float>(v.f64);
        break;
    case ColType::F64:
        as<double>()[row] = v.f64;
        break;
    case ColType::Bool:
        as<uint8_t>()[row] = (v.i64 != 0 || v.u64 != 0 || v.f64 != 0.0 || !v.str.empty()) ? 1 : 0;
        break;
    case ColType::Dict:
        if (v.type == ColType::Dict && v.u64 != kInvalidDictId && v.str.empty()) {
            as<uint32_t>()[row] = static_cast<uint32_t>(v.u64);
        } else if (dict) {
            as<uint32_t>()[row] = dict->intern(v.str);
        } else {
            as<uint32_t>()[row] = static_cast<uint32_t>(v.u64);
        }
        break;
    case ColType::String:
        setString(row, v.str);
        break;
    }
}

void ColumnChunk::copyRowFrom(uint32_t dstRow, const ColumnChunk& src, uint32_t srcRow) {
    if (src.isNull(srcRow)) {
        setNull(dstRow, true);
        if (fixedData_ && dstRow < kChunkRows) {
            const size_t elemBytes = colTypeElemSize(type_);
            std::memset(fixedData_ + static_cast<size_t>(dstRow) * elemBytes, 0, elemBytes);
        }
        return;
    }
    setNull(dstRow, false);
    if (type_ == ColType::String) {
        setString(dstRow, src.getString(srcRow));
        return;
    }
    const size_t elemBytes = colTypeElemSize(type_);
    std::memcpy(fixedData_ + static_cast<size_t>(dstRow) * elemBytes,
                src.fixedData_ + static_cast<size_t>(srcRow) * elemBytes,
                elemBytes);
}

void TableChunk::setAlive(uint32_t row, bool alive) {
    if (row >= kChunkRows) return;
    const uint64_t mask = 1ULL << (row & 63);
    uint64_t& word = aliveWords[row >> 6];
    const bool wasAlive = (word & mask) != 0;
    if (alive && !wasAlive) {
        word |= mask;
        ++aliveCount;
    } else if (!alive && wasAlive) {
        word &= ~mask;
        --aliveCount;
    }
}

std::shared_ptr<ColumnChunk>& TableChunk::mutableColumn(size_t colIdx) {
    auto& ptr = columns[colIdx];
    if (ptr.use_count() > 1) {
        ptr = ptr->clone();
    }
    return ptr;
}

}  // namespace bro::table
