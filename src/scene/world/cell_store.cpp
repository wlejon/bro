#include "scene/world/cell_store.h"

#include <algorithm>
#include <cstring>

namespace bro::scene {

CellStore::CellStore(size_t prefixFloats, int recordFloats)
    : prefixFloats_(prefixFloats), recordFloats_(recordFloats), data_(prefixFloats, 0.0f) {}

void CellStore::prefixChanged() {
    // Frames in flight read the prefix: a new copy, whole.
    ++layout_;
    ++serial_;
    log_.clear();
    logFloor_ = serial_;
}

void CellStore::put(const CellKey& key, const float* records, uint32_t count) {
    drop(key);
    if (count == 0) return;
    uint32_t first = 0;
    if (!allocate(count, first)) {
        grow(count);
        allocate(count, first);
    }
    const size_t at = prefixFloats_ + size_t{first} * recordFloats_;
    std::memcpy(data_.data() + at, records, size_t{count} * recordFloats_ * sizeof(float));
    changed(at, size_t{count} * recordFloats_);
    runs_[key.packed()] = Run{key, first, count};
    live_ += count;
}

void CellStore::drop(const CellKey& key) {
    const auto it = runs_.find(key.packed());
    if (it == runs_.end()) return;
    held_.push_back({it->second.first, it->second.count, tick_});
    live_ -= it->second.count;
    runs_.erase(it);
    ++serial_;
}

void CellStore::clear() {
    for (const auto& [k, r] : runs_) held_.push_back({r.first, r.count, tick_});
    runs_.clear();
    live_ = 0;
    ++serial_;
}

void CellStore::tick() {
    ++tick_;
    for (auto it = held_.begin(); it != held_.end();) {
        if (it->tick + kReuseTicks <= tick_) {
            release(it->first, it->count);
            it = held_.erase(it);
        } else {
            ++it;
        }
    }
    const auto old = std::find_if(log_.begin(), log_.end(), [&](const Change& c) { return c.tick + kLogTicks > tick_; });
    if (old != log_.begin()) {
        logFloor_ = std::prev(old)->serial;
        log_.erase(log_.begin(), old);
    }
}

bool CellStore::changesSince(uint64_t serial, std::vector<std::pair<size_t, size_t>>& out) const {
    out.clear();
    if (serial < logFloor_) return false;
    for (const Change& c : log_)
        if (c.serial > serial) out.emplace_back(c.offset, c.count);
    return true;
}

bool CellStore::allocate(uint32_t count, uint32_t& first) {
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->second < count) continue;
        first = it->first;
        it->first += count;
        it->second -= count;
        if (it->second == 0) free_.erase(it);
        return true;
    }
    return false;
}

void CellStore::release(uint32_t first, uint32_t count) {
    auto it = std::lower_bound(free_.begin(), free_.end(), std::make_pair(first, 0u));
    it = free_.insert(it, {first, count});
    // Coalesce with the next, then the previous.
    if (std::next(it) != free_.end() && it->first + it->second == std::next(it)->first) {
        it->second += std::next(it)->second;
        free_.erase(std::next(it));
    }
    if (it != free_.begin() && std::prev(it)->first + std::prev(it)->second == it->first) {
        std::prev(it)->second += it->second;
        free_.erase(it);
    }
}

void CellStore::grow(uint32_t atLeast) {
    const uint32_t old = capacity_;
    capacity_ = std::max({capacity_ * 2, capacity_ + atLeast, 4096u});
    data_.resize(prefixFloats_ + size_t{capacity_} * recordFloats_, 0.0f);
    release(old, capacity_ - old);
    ++layout_;
    ++serial_;
    log_.clear();
    logFloor_ = serial_;
}

void CellStore::changed(size_t offset, size_t count) {
    ++serial_;
    log_.push_back({serial_, tick_, offset, count});
}

}  // namespace bro::scene
