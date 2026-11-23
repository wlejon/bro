#include "table/table_index.h"

#include <cmath>
#include <cstring>

namespace bro::table {

HashIndex::HashIndex(size_t colIdx, ColType colType)
    : colIdx_(colIdx), colType_(colType) {
    clear(64);
}

std::shared_ptr<HashIndex> HashIndex::clone() const {
    auto copy = std::make_shared<HashIndex>();
    copy->colIdx_ = colIdx_;
    copy->colType_ = colType_;
    copy->buckets_ = buckets_;
    copy->nodes_ = nodes_;
    copy->usedBuckets_ = usedBuckets_;
    copy->entryCount_ = entryCount_;
    return copy;
}

void HashIndex::clear(size_t expectedKeys) {
    size_t cap = 64;
    const size_t minCap = (expectedKeys * 10) / 6 + 16;
    while (cap < minCap) cap <<= 1;
    buckets_.assign(cap, Bucket{});
    nodes_.clear();
    usedBuckets_ = 0;
    entryCount_ = 0;
}

void HashIndex::rehash(size_t newCap) {
    std::vector<Bucket> nextBuckets(newCap);
    const size_t mask = newCap - 1;
    for (const Bucket& b : buckets_) {
        if (!b.occupied) continue;
        size_t pos = static_cast<size_t>(mixHash64(b.key)) & mask;
        while (nextBuckets[pos].occupied) {
            pos = (pos + 1) & mask;
        }
        nextBuckets[pos] = b;
    }
    buckets_ = std::move(nextBuckets);
}

size_t HashIndex::findSlot(uint64_t key) const {
    if (buckets_.empty()) return 0;
    const size_t mask = buckets_.size() - 1;
    size_t pos = static_cast<size_t>(mixHash64(key)) & mask;
    while (buckets_[pos].occupied && buckets_[pos].key != key) {
        pos = (pos + 1) & mask;
    }
    return pos;
}

void HashIndex::insert(uint64_t key, RowLoc loc) {
    if (buckets_.empty()) clear(64);
    if ((usedBuckets_ + 1) * 10 >= buckets_.size() * 7) {
        rehash(buckets_.size() * 2);
    }
    const size_t pos = findSlot(key);
    Bucket& b = buckets_[pos];
    const uint32_t nodeIdx = static_cast<uint32_t>(nodes_.size());
    if (!b.occupied) {
        b.occupied = true;
        b.key = key;
        b.headNode = nodeIdx;
        ++usedBuckets_;
        nodes_.push_back(Node{loc, kInvalidNode});
    } else {
        nodes_.push_back(Node{loc, b.headNode});
        b.headNode = nodeIdx;
    }
    ++entryCount_;
}

bool HashIndex::erase(uint64_t key, RowLoc loc) {
    if (buckets_.empty()) return false;
    const size_t pos = findSlot(key);
    Bucket& b = buckets_[pos];
    if (!b.occupied) return false;
    uint32_t prev = kInvalidNode;
    uint32_t cur = b.headNode;
    while (cur != kInvalidNode) {
        if (nodes_[cur].loc == loc) {
            if (prev == kInvalidNode) {
                b.headNode = nodes_[cur].next;
            } else {
                nodes_[prev].next = nodes_[cur].next;
            }
            --entryCount_;
            return true;
        }
        prev = cur;
        cur = nodes_[cur].next;
    }
    return false;
}

void HashIndex::lookupKey(uint64_t key, std::vector<RowLoc>& out) const {
    if (buckets_.empty() || entryCount_ == 0) return;
    const size_t pos = findSlot(key);
    const Bucket& b = buckets_[pos];
    if (!b.occupied) return;
    uint32_t cur = b.headNode;
    while (cur != kInvalidNode) {
        out.push_back(nodes_[cur].loc);
        cur = nodes_[cur].next;
    }
}

bool HashIndex::scalarToKey(ColType colType, const ScalarValue& v,
                            const StringDictionary* dict, uint64_t& outKey) {
    if (v.isNull) return false;
    switch (colType) {
    case ColType::I32: {
        int32_t iv = (v.type == ColType::F32 || v.type == ColType::F64)
                         ? static_cast<int32_t>(v.f64)
                         : static_cast<int32_t>(v.i64);
        outKey = static_cast<uint64_t>(static_cast<uint32_t>(iv));
        return true;
    }
    case ColType::U32: {
        uint32_t uv = (v.type == ColType::F32 || v.type == ColType::F64)
                          ? static_cast<uint32_t>(static_cast<int64_t>(v.f64))
                          : static_cast<uint32_t>(v.u64);
        outKey = uv;
        return true;
    }
    case ColType::I64: {
        int64_t iv = (v.type == ColType::F32 || v.type == ColType::F64)
                         ? static_cast<int64_t>(v.f64)
                         : v.i64;
        outKey = static_cast<uint64_t>(iv);
        return true;
    }
    case ColType::F32: {
        float fv = static_cast<float>(v.f64);
        if (fv == 0.0f) fv = 0.0f;
        uint32_t bits = 0;
        std::memcpy(&bits, &fv, 4);
        outKey = bits;
        return true;
    }
    case ColType::F64:
        outKey = canonicalFloatKey(v.f64);
        return true;
    case ColType::Bool:
        outKey = (v.i64 != 0 || v.u64 != 0 || v.f64 != 0.0) ? 1ULL : 0ULL;
        return true;
    case ColType::Dict: {
        if (v.type == ColType::Dict && v.u64 != kInvalidDictId && v.str.empty()) {
            outKey = static_cast<uint32_t>(v.u64);
            return true;
        }
        if (!dict) return false;
        auto found = dict->find(v.str);
        if (!found) return false;
        outKey = *found;
        return true;
    }
    case ColType::String:
        outKey = hashBytes(v.str.data(), v.str.size());
        return true;
    }
    return false;
}

uint64_t HashIndex::cellKey(const ColumnChunk& chunk, uint32_t row) {
    switch (chunk.type()) {
    case ColType::I32:
    case ColType::U32:
    case ColType::Dict:
        return chunk.as<uint32_t>()[row];
    case ColType::I64:
        return static_cast<uint64_t>(chunk.as<int64_t>()[row]);
    case ColType::F32: {
        float fv = chunk.as<float>()[row];
        if (fv == 0.0f) fv = 0.0f;
        uint32_t bits = 0;
        std::memcpy(&bits, &fv, 4);
        return bits;
    }
    case ColType::F64:
        return canonicalFloatKey(chunk.as<double>()[row]);
    case ColType::Bool:
        return chunk.as<uint8_t>()[row] != 0 ? 1ULL : 0ULL;
    case ColType::String: {
        std::string_view sv = chunk.getString(row);
        return hashBytes(sv.data(), sv.size());
    }
    }
    return 0;
}

}  // namespace bro::table
