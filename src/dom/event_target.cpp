#include "dom/event_target.h"

#include <algorithm>
#include <atomic>

namespace bro::dom {

// One counter for every listener registration in the process, C++ and JS
// alike. Registration is main-thread in practice; the counter is atomic anyway
// because a torn one would show up as listeners running in the wrong order,
// which is exactly the kind of failure that gets blamed on the listeners.
// Starts at 1 so 0 can mean "no handle".
uint64_t nextListenerSeq() {
    static std::atomic<uint64_t> s_seq{0};
    return s_seq.fetch_add(1, std::memory_order_relaxed) + 1;
}

ListenerHandle NativeListenerList::add(const std::string& type, EventCallback cb,
                                       ListenerOptions opts) {
    if (!cb) return ListenerHandle{};
    auto e = std::make_shared<Entry>();
    e->seq = nextListenerSeq();
    e->id = e->seq;   // the sequence doubles as the identity; both are unique
    e->type = type;
    e->cb = std::move(cb);
    e->opts = opts;
    entries_.push_back(std::move(e));
    return ListenerHandle{entries_.back()->id};
}

bool NativeListenerList::remove(ListenerHandle h) {
    if (!h) return false;
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const EntryPtr& e) { return e->id == h.id; });
    if (it == entries_.end()) return false;
    // Mark before erasing: a snapshot taken by an in-flight dispatch still
    // holds this entry, and must not call a listener that has been removed.
    (*it)->removed = true;
    entries_.erase(it);
    return true;
}

void NativeListenerList::clear() {
    for (auto& e : entries_) e->removed = true;
    entries_.clear();
}

bool NativeListenerList::hasType(const std::string& type) const {
    for (const auto& e : entries_)
        if (e->type == type) return true;
    return false;
}

std::vector<NativeListenerList::EntryPtr>
NativeListenerList::snapshot(const std::string& type) const {
    std::vector<EntryPtr> out;
    for (const auto& e : entries_)
        if (e->type == type) out.push_back(e);
    return out;
}

} // namespace bro::dom
