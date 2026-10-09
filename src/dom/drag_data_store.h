#pragma once

// The drag data store of the drag in progress (HTML's "drag data store"):
// what dragstart's handlers put in with dataTransfer.setData, the allowed
// effects and the drop effect. The DataTransfer the page sees reads and
// writes it; when the drag leaves the window, the engine offers its items to
// other applications (Engine::beginNativeDrag) and reports the effect they
// took back through dropEffect. One per process: there is one pointer.

#include <string>
#include <unordered_map>

namespace bro::dom {

struct DragDataStore {
    std::unordered_map<std::string, std::string> data;  // format (lower-case MIME type) -> value
    std::string effectAllowed = "all";
    std::string dropEffect = "none";

    void reset() {
        data.clear();
        effectAllowed = "all";
        dropEffect = "none";
    }
};

inline DragDataStore& dragDataStore() {
    static DragDataStore store;
    return store;
}

}  // namespace bro::dom
