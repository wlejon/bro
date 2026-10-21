#pragma once
// A provider's records kept by world cell for the GPU (docs/scene-world-
// architecture.md §1, §10): a prefix (a header, tables) and then a pool of
// fixed-size records, each leaf's a run anywhere in the pool. Leaves come
// and go as placement streams them, so the pool is never rewritten whole:
// a leaf put takes a free run, one dropped gives its run back, and the GPU
// copy takes only the ranges changed since it last looked (changesSince).
//
// A run given back is not reused for kReuseTicks ticks (a tick a frame), so
// a frame still in flight that reads the old leaf never sees a new one's
// records written under it. The pool grows when no free run fits, which
// moves layout(): the GPU copy is made again, whole.

#include "scene/world/world_cells.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::scene {

class CellStore {
public:
    static constexpr int kReuseTicks = 4;
    static constexpr int kLogTicks = 120;   // how far back changesSince reaches

    CellStore(size_t prefixFloats, int recordFloats);

    /// The prefix, to write; then prefixChanged() (which, as frames in
    /// flight read it, moves layout()).
    float* prefix() { return data_.data(); }
    void prefixChanged();

    /// Leaf `key`'s records, replacing any it had (count 0: dropped).
    void put(const CellKey& key, const float* records, uint32_t count);
    void drop(const CellKey& key);
    void clear();

    /// A leaf's run: its records' first (counted from the pool's start) and
    /// count.
    struct Run {
        CellKey key;
        uint32_t first = 0, count = 0;
    };
    const std::unordered_map<uint64_t, Run>& runs() const { return runs_; }
    const float* record(uint32_t i) const { return data_.data() + prefixFloats_ + size_t{i} * recordFloats_; }
    /// Records in live runs.
    uint32_t live() const { return live_; }
    /// The pool's room, in records.
    uint32_t capacity() const { return capacity_; }

    /// A frame: runs given back kReuseTicks ago are free again, the change
    /// log's old entries go.
    void tick();

    /// The prefix and the pool as the GPU reads them.
    const std::vector<float>& data() const { return data_; }
    /// Moves when data() is resized (the pool grew) or the prefix changed:
    /// its copy is made whole.
    uint64_t layout() const { return layout_; }
    /// Moves with every change.
    uint64_t serial() const { return serial_; }
    /// The float ranges (offset, count) of data() changed after `serial`;
    /// false when the log no longer reaches back that far (copy it whole).
    bool changesSince(uint64_t serial, std::vector<std::pair<size_t, size_t>>& out) const;

private:
    bool allocate(uint32_t count, uint32_t& first);
    void release(uint32_t first, uint32_t count);
    void grow(uint32_t atLeast);
    void changed(size_t offset, size_t count);

    size_t prefixFloats_;
    int recordFloats_;
    std::vector<float> data_;
    uint32_t capacity_ = 0, live_ = 0;
    std::unordered_map<uint64_t, Run> runs_;
    std::vector<std::pair<uint32_t, uint32_t>> free_;   // (first, count), by first, coalesced
    struct Held {
        uint32_t first, count;
        uint64_t tick;
    };
    std::vector<Held> held_;   // given back, not yet free
    struct Change {
        uint64_t serial, tick;
        size_t offset, count;
    };
    std::vector<Change> log_;
    uint64_t logFloor_ = 0;   // changes after this serial are all in log_
    uint64_t tick_ = 0, serial_ = 1, layout_ = 1;
};

}  // namespace bro::scene
