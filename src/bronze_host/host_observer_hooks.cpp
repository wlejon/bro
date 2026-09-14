// `__bro_observers` — the host half of MutationObserver, ResizeObserver and
// IntersectionObserver. The classes themselves are JavaScript
// (js/observers.js, compiled by bronze at build time); this file supplies only
// what that JavaScript cannot see for itself:
//
//   observeMutations(node, subtree)    the DOM records notices touching `node`
//   unobserveMutations(node, subtree)  ...until the last interest in it goes
//   takeMutations()                    every notice since the last take, as
//                                      plain records, and the queue emptied
//   onMutation(fn)                     fn() runs at the FIRST notice after each
//                                      take — the JS queues its delivery
//                                      microtask from it
//   onFrame(fn)                        fn() runs once per frame from the frame
//                                      seam (dom_globals.cpp hostFrame), after
//                                      rAF and its microtask checkpoint
//
// WHERE THE NOTICES COME FROM. Not from watching this layer's own mutators:
// the hook registers with the DOM (Document::addMutationObserver) and is told
// about every change to the tree — one made by compiled code, by the page's
// script, or by the engine's C++ — because a mutation is a property of the
// tree and not of who made it. Every mutator funnels through
// Document::notifyMutation.
//
// WHICH WATCHED NODES A NOTICE HITS is decided HERE, at notice time, and
// carried on the record as `nodes`: the target itself, and every
// subtree-watched ancestor it has at that instant. That is the only moment
// the question has a right answer — a removal notice fires before the detach
// and the removed subtree has no ancestors afterwards — and it is what lets
// the JavaScript apply each observer's own options without re-deriving the
// ancestry from a tree that has moved on.
//
// NODES ARE HELD AS REGISTRY ENTRIES (HostNodeState*), never as raw
// dom::Node*: a node removed in one turn can be freed before the record
// naming it is taken, and the registry is the thing that already learns about
// that (Document::addNodeFreedObserver). A slot whose node has since been
// freed reports null rather than a pointer into released storage.
//
// LIFETIME. One process-lived state block, never freed: it holds Persistents
// (the two JS callbacks), and a static destructor would release root slots
// against a runtime whose statics may already be gone — the same convention
// the timer table and the host classes follow.

#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"  // ObjectBuilder, argAt, boolAt

#include "dom/document.h"
#include "dom/node.h"
#include "util/log.h"

#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::bronze_host {

namespace {

// How many JS registrations name this node, split by whether they asked for
// the subtree: a node stays watched while either count is above zero, and
// its subtree matches while the second is.
struct Watch {
    int direct = 0;
    int subtree = 0;
};

// One notice, copied out: MutationNotice borrows its strings for the duration
// of the call and its nodes for no longer than the tree keeps them.
struct RawRecord {
    dom::Document::MutationNotice::Kind kind{};
    HostNodeState* target = nullptr;
    HostNodeState* added = nullptr;
    HostNodeState* removed = nullptr;
    HostNodeState* previousSibling = nullptr;
    HostNodeState* nextSibling = nullptr;
    std::string attributeName;
    std::string oldValue;
    bool hasOldValue = false;
    std::vector<HostNodeState*> nodes;  // the watched nodes this one hit
};

struct HookState {
    std::unordered_map<HostNodeState*, Watch> watches;
    std::vector<RawRecord> queue;
    // Set when onMutation has been called for the current batch; cleared by
    // takeMutations. One JS call per batch rather than one per notice.
    bool signalled = false;
    ev::Persistent onMutation;
    ev::Persistent onFrame;
};

HookState* g_state = nullptr;

HookState& state() {
    if (!g_state) g_state = new HookState();
    return *g_state;
}

bool isAncestorOf(dom::Node* maybeAncestor, dom::Node* node) {
    for (dom::Node* p = node->parentNode(); p; p = p->parentNode()) {
        if (p == maybeAncestor) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The notice hook
// ---------------------------------------------------------------------------

void onDomMutation(dom::Document*, const dom::Document::MutationNotice& notice) {
    if (!g_state || !notice.target || g_state->watches.empty()) return;

    std::vector<HostNodeState*> matched;
    for (auto& [st, w] : g_state->watches) {
        dom::Node* watched = st->node;
        if (!watched) continue;  // freed since observe()
        if (watched == notice.target) {
            matched.push_back(st);
        } else if (w.subtree > 0 && isAncestorOf(watched, notice.target)) {
            matched.push_back(st);
        }
    }
    if (matched.empty()) return;

    RawRecord rec;
    rec.kind = notice.kind;
    rec.target = hostNodeStateFor(notice.target);
    rec.added = notice.added ? hostNodeStateFor(notice.added) : nullptr;
    rec.removed = notice.removed ? hostNodeStateFor(notice.removed) : nullptr;
    rec.previousSibling =
        notice.previousSibling ? hostNodeStateFor(notice.previousSibling) : nullptr;
    rec.nextSibling = notice.nextSibling ? hostNodeStateFor(notice.nextSibling) : nullptr;
    if (notice.attributeName) rec.attributeName = *notice.attributeName;
    if (notice.oldValue) {
        rec.oldValue = *notice.oldValue;
        rec.hasOldValue = true;
    }
    rec.nodes = std::move(matched);
    g_state->queue.push_back(std::move(rec));

    if (g_state->signalled) return;
    Value fn = g_state->onMutation.get();
    if (!ev::isFunction(fn)) return;
    g_state->signalled = true;
    // Calling into JS from inside a notice is allowed: the callback only
    // queues a microtask. It must not mutate the DOM, and it does not.
    ev::CallResult r = ev::call(fn, ev::undefined(), {});
    if (r.thrown) reportBronzeError("__bro_observers.onMutation", r.value);
}

// The notice is per-document — a DOMParser result is a document the engine
// has never heard of — so the hook goes on the document of the node being
// observed, every time: addMutationObserver is idempotent (one function
// pointer, installed once), which is cheaper and safer than a set of
// document pointers that a freed-and-reallocated document could alias.
void ensureHook(dom::Document* doc) {
    if (doc) doc->addMutationObserver(&onDomMutation);
}

// ---------------------------------------------------------------------------
// Records, as plain data
// ---------------------------------------------------------------------------

Value nodeSlot(HostNodeState* st) {
    if (!st || !st->node) return ev::null();
    return hostNodeValue(st->node);
}

const char* kindName(dom::Document::MutationNotice::Kind kind) {
    switch (kind) {
        case dom::Document::MutationNotice::Kind::ChildList: return "childList";
        case dom::Document::MutationNotice::Kind::Attributes: return "attributes";
        case dom::Document::MutationNotice::Kind::CharacterData: return "characterData";
    }
    return "childList";
}

Value makeRawRecordValue(const RawRecord& rec) {
    ObjectBuilder b;
    b.set("type", ev::fromUtf8(kindName(rec.kind)));
    b.set("target", nodeSlot(rec.target));
    b.set("added", nodeSlot(rec.added));
    b.set("removed", nodeSlot(rec.removed));
    b.set("previousSibling", nodeSlot(rec.previousSibling));
    b.set("nextSibling", nodeSlot(rec.nextSibling));
    b.set("attributeName",
          rec.attributeName.empty() ? ev::null() : ev::fromUtf8(rec.attributeName));
    // null rather than "": an attribute that had no previous value and one
    // whose old value nobody asked for read the same on the web, and the
    // JavaScript decides which observers see it at all.
    b.set("oldValue", rec.hasOldValue ? ev::fromUtf8(rec.oldValue) : ev::null());
    const std::vector<HostNodeState*>& nodes = rec.nodes;
    b.set("nodes", hostArrayOf(nodes.size(), [&nodes](size_t i) { return nodeSlot(nodes[i]); }));
    return b.get();
}

// ---------------------------------------------------------------------------
// The hook object
// ---------------------------------------------------------------------------

Value observeMutations(Value, std::span<const Value> a) {
    dom::Node* node = hostNodeOf(argAt(a, 0));
    if (!node) {
        return ev::throwTypeError("__bro_observers.observeMutations: target must be a node");
    }
    const bool subtree = boolAt(a, 1);
    Watch& w = state().watches[hostNodeStateFor(node)];
    if (subtree) ++w.subtree; else ++w.direct;
    ensureHook(node->document());
    return ev::undefined();
}

Value unobserveMutations(Value, std::span<const Value> a) {
    dom::Node* node = hostNodeOf(argAt(a, 0));
    if (!node || !g_state) return ev::undefined();
    const bool subtree = boolAt(a, 1);
    auto it = g_state->watches.find(hostNodeStateFor(node));
    if (it == g_state->watches.end()) return ev::undefined();
    Watch& w = it->second;
    if (subtree) { if (w.subtree > 0) --w.subtree; }
    else { if (w.direct > 0) --w.direct; }
    if (w.direct == 0 && w.subtree == 0) g_state->watches.erase(it);
    return ev::undefined();
}

Value takeMutations(Value, std::span<const Value>) {
    HookState& s = state();
    s.signalled = false;
    // Moved out first: building the values allocates, an allocation can run
    // a collection, and nothing about that should be able to see a queue that
    // is half-consumed — or the notices a callback queues while it runs.
    std::vector<RawRecord> batch;
    batch.swap(s.queue);
    return hostArrayOf(batch.size(),
                       [&batch](size_t i) { return makeRawRecordValue(batch[i]); });
}

Value onMutation(Value, std::span<const Value> a) {
    Value fn = argAt(a, 0);
    if (!ev::isFunction(fn)) {
        return ev::throwTypeError("__bro_observers.onMutation: expected a function");
    }
    state().onMutation.set(fn);
    return ev::undefined();
}

Value onFrame(Value, std::span<const Value> a) {
    Value fn = argAt(a, 0);
    if (!ev::isFunction(fn)) {
        return ev::throwTypeError("__bro_observers.onFrame: expected a function");
    }
    state().onFrame.set(fn);
    return ev::undefined();
}

}  // namespace

void installObserverHooks() {
    state();
    ObjectBuilder b;
    b.def("observeMutations", 2, observeMutations);
    b.def("unobserveMutations", 2, unobserveMutations);
    b.def("takeMutations", 0, takeMutations);
    b.def("onMutation", 1, onMutation);
    b.def("onFrame", 1, onFrame);
    Value hooks = b.get();
    ev::registerGlobal("__bro_observers", hooks);
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) ev::setProperty(gt.value, "__bro_observers", hooks);
}

void fireHostObserverFrame() {
    if (!g_state) return;
    Value fn = g_state->onFrame.get();
    if (!ev::isFunction(fn)) return;
    ev::CallResult r = ev::call(fn, ev::undefined(), {});
    if (r.thrown) reportBronzeError("__bro_observers.onFrame", r.value);
}

}  // namespace bro::bronze_host
