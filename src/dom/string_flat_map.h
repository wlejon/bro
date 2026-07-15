#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstddef>

namespace bro::dom {

// A tiny string->string map backed by a flat vector of pairs, kept for the two
// per-element collections (an element's attributes and its inline-style
// declarations) that are almost always small — a handful of entries — and were
// paying for a full std::unordered_map each: a 56-byte header plus a separate
// bucket-array allocation and per-node allocations, most of it idle.
//
// A linear scan over a contiguous vector beats hashing at these sizes and costs
// one allocation instead of several, with far better locality across the many
// thousands of elements a document holds. The public surface is the subset of
// std::unordered_map that the DOM and its bindings actually use, so it drops in
// without touching call sites: find/count/operator[]/erase/begin/end/empty/
// size/clear, iterating value_type = pair<string,string>.
class StringFlatMap {
public:
    using value_type = std::pair<std::string, std::string>;
    using storage = std::vector<value_type>;
    using iterator = storage::iterator;
    using const_iterator = storage::const_iterator;

    iterator begin() { return items_.begin(); }
    iterator end() { return items_.end(); }
    const_iterator begin() const { return items_.begin(); }
    const_iterator end() const { return items_.end(); }

    bool empty() const { return items_.empty(); }
    size_t size() const { return items_.size(); }
    void clear() { items_.clear(); }

    iterator find(const std::string& key) {
        for (auto it = items_.begin(); it != items_.end(); ++it)
            if (it->first == key) return it;
        return items_.end();
    }
    const_iterator find(const std::string& key) const {
        for (auto it = items_.begin(); it != items_.end(); ++it)
            if (it->first == key) return it;
        return items_.end();
    }

    size_t count(const std::string& key) const {
        return find(key) != end() ? 1 : 0;
    }

    // Insert-or-return, matching std::unordered_map::operator[].
    std::string& operator[](const std::string& key) {
        for (auto& kv : items_)
            if (kv.first == key) return kv.second;
        items_.emplace_back(key, std::string{});
        return items_.back().second;
    }

    // Erase by key; returns the number removed (0 or 1), like unordered_map.
    size_t erase(const std::string& key) {
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (it->first == key) {
                items_.erase(it);
                return 1;
            }
        }
        return 0;
    }

private:
    storage items_;
};

} // namespace bro::dom
