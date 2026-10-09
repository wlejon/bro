#include "layout/el_remote_view.h"

#include <unordered_map>
#include <vector>

namespace bro::layout {

namespace {

// Main thread.
std::unordered_map<uint64_t, ElRemoteView*>& registry() {
    static std::unordered_map<uint64_t, ElRemoteView*> views;
    return views;
}
uint64_t g_nextViewId = 1;

}  // namespace

ElRemoteView::ElRemoteView() : viewId_(g_nextViewId++) { registry()[viewId_] = this; }

ElRemoteView::~ElRemoteView() { registry().erase(viewId_); }

ElRemoteView* ElRemoteView::byId(uint64_t id) {
    auto it = registry().find(id);
    return it == registry().end() ? nullptr : it->second;
}

void ElRemoteView::forEach(const std::function<void(ElRemoteView&)>& fn) {
    // Over a copy, so fn may add or remove views.
    std::vector<uint64_t> ids;
    ids.reserve(registry().size());
    for (auto& [id, v] : registry()) ids.push_back(id);
    for (uint64_t id : ids)
        if (ElRemoteView* v = byId(id)) fn(*v);
}

bool ElRemoteView::setStreamSize(uint32_t w, uint32_t h) {
    if (streamW_.load(std::memory_order_relaxed) == w && streamH_.load(std::memory_order_relaxed) == h) return false;
    streamW_.store(w, std::memory_order_relaxed);
    streamH_.store(h, std::memory_order_relaxed);
    return true;
}

void ElRemoteView::getContentSize(float& w, float& h) const {
    const uint32_t sw = streamW_.load(std::memory_order_relaxed), sh = streamH_.load(std::memory_order_relaxed);
    if (sw == 0 || sh == 0) {
        w = 300.0f;
        h = 150.0f;
        return;
    }
    w = float(sw);
    h = float(sh);
}

}  // namespace bro::layout
