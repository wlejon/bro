#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bro::dom {

class Event;

// ---------------------------------------------------------------------------
// C++-side event listeners.
//
// A host application that has no app JS — an AOT-compiled app, an embedder
// building its page from C++ — still has to answer `window.addEventListener`
// and `element.addEventListener`. This is the C++ half of both: a callback
// plus a token that unregisters it, reaching the *same* dispatch the JS
// listeners reach rather than a parallel simpler path.
//
// Ordering. Every registration — C++ or JS — takes a sequence number from
// nextListenerSeq(). Dispatch merges the two lists on that number, so
// listeners on one target fire in registration order regardless of which side
// registered them. JS registrations are stamped by the bindings
// (js_element_addEventListener) and by the window polyfill, which calls
// __bro_listener_seq() for the same counter.
//
// Threading: main (JS) thread only, like the rest of the DOM.
// ---------------------------------------------------------------------------

/// A C++ event listener. Receives the same dom::Event the JS listeners on the
/// same target receive; preventDefault() / stopPropagation() /
/// stopImmediatePropagation() called on it affect the remainder of dispatch
/// exactly as they would from a JS listener.
using EventCallback = std::function<void(Event&)>;

struct ListenerOptions {
    /// Fire during the capture phase instead of the bubble phase. At the
    /// target itself both kinds fire, as in the DOM.
    bool capture = false;
    /// Unregister automatically after the first invocation.
    bool once = false;
};

/// Opaque token for one registration. A default-constructed handle is "none"
/// and is safe to pass to remove() (it removes nothing and returns false).
struct ListenerHandle {
    uint64_t id = 0;
    explicit operator bool() const { return id != 0; }
    bool operator==(const ListenerHandle& o) const { return id == o.id; }
    bool operator!=(const ListenerHandle& o) const { return id != o.id; }
};

/// The shared registration counter. Monotonic for the life of the process.
uint64_t nextListenerSeq();

/// The C++ listeners registered on one event target (an Element, or a realm's
/// window — see Document::windowListeners()).
class NativeListenerList {
public:
    struct Entry {
        uint64_t seq = 0;
        uint64_t id = 0;
        std::string type;
        EventCallback cb;
        ListenerOptions opts;
        /// Set by remove(). Entries are held by shared_ptr and snapshotted
        /// before dispatch, so a listener that removes another (or itself)
        /// mid-dispatch is honoured without invalidating the iteration.
        bool removed = false;
    };
    using EntryPtr = std::shared_ptr<Entry>;

    ListenerHandle add(const std::string& type, EventCallback cb,
                       ListenerOptions opts = {});
    /// True if `h` named a live registration on this list.
    bool remove(ListenerHandle h);
    void clear();

    bool empty() const { return entries_.empty(); }
    /// Whether any listener for `type` is registered. Cheap gate for dispatch.
    bool hasType(const std::string& type) const;

    /// Live entries for `type` in registration order. A snapshot: entries
    /// removed while it is being walked report removed == true rather than
    /// disappearing.
    std::vector<EntryPtr> snapshot(const std::string& type) const;

private:
    std::vector<EntryPtr> entries_;
};

} // namespace bro::dom
