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
// ONE STATE PER COMPARTMENT (host_compartment.h). Each realm runs its own
// observers.js, which registers its own callbacks and takes its own records,
// so the watches, the queue and the callbacks are the calling compartment's:
// a frame's observers never take the page's records, and the page's keep
// firing while a frame lives. A compartment's state goes at its teardown.
//
// LIFETIME. The table is process-lived, never freed: its states hold
// Persistents (the two JS callbacks), and a static destructor would release
// root slots against a runtime whose statics may already be gone — the same
// convention the timer table and the host classes follow.

#include "bronze_host/host_compartment.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_js_modules.h"
#include "bronze_host/host_builder.h"  // ObjectBuilder, argAt, boolAt
#include "bronze_host/host_node_sweep.h"

#include <algorithm>
#include <memory>

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
    std::vector<HostNodeState*> added;
    std::vector<HostNodeState*> removed;
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

// By compartment id. unique_ptr so a state keeps its address while the map
// grows; the map itself is never destroyed (LIFETIME above).
std::unordered_map<uint64_t, std::unique_ptr<HookState>>* g_states = nullptr;

std::unordered_map<uint64_t, std::unique_ptr<HookState>>& states() {
    if (!g_states) g_states = new std::unordered_map<uint64_t, std::unique_ptr<HookState>>();
    return *g_states;
}

// The calling compartment's.
HookState& state() {
    auto& slot = states()[currentCompartmentId()];
    if (!slot) slot = std::make_unique<HookState>();
    return *slot;
}

HookState* existingState() {
    if (!g_states) return nullptr;
    auto it = g_states->find(currentCompartmentId());
    return it != g_states->end() ? it->second.get() : nullptr;
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

void noticeFor(HookState& s, const dom::Document::MutationNotice& notice);

void onDomMutation(dom::Document*, const dom::Document::MutationNotice& notice) {
    if (!g_states || !notice.target) return;
    // Copied out first: a callback may make another compartment's state.
    std::vector<std::pair<uint64_t, HookState*>> watching;
    for (auto& [id, s] : *g_states)
        if (s && !s->watches.empty()) watching.emplace_back(id, s.get());
    for (auto& [id, s] : watching) {
        Compartment* c = compartmentById(id);
        if (!c || c->dying) continue;
        noticeFor(*s, notice);
    }
}

void noticeFor(HookState& s, const dom::Document::MutationNotice& notice) {
    std::vector<HostNodeState*> matched;
    for (auto& [st, w] : s.watches) {
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
    // Registering the added / removed nodes is also what keeps a removed
    // text node alive past a replace-all (Document::setNodeRetainQuery).
    if (notice.added) rec.added.push_back(hostNodeStateFor(notice.added));
    if (notice.addedList)
        for (dom::Node* n : *notice.addedList) rec.added.push_back(hostNodeStateFor(n));
    if (notice.removed) rec.removed.push_back(hostNodeStateFor(notice.removed));
    if (notice.removedList)
        for (dom::Node* n : *notice.removedList) rec.removed.push_back(hostNodeStateFor(n));
    rec.previousSibling =
        notice.previousSibling ? hostNodeStateFor(notice.previousSibling) : nullptr;
    rec.nextSibling = notice.nextSibling ? hostNodeStateFor(notice.nextSibling) : nullptr;
    if (notice.attributeName) rec.attributeName = *notice.attributeName;
    if (notice.oldValue) {
        rec.oldValue = *notice.oldValue;
        rec.hasOldValue = true;
    }
    rec.nodes = std::move(matched);
    s.queue.push_back(std::move(rec));

    if (s.signalled) return;
    Value fn = s.onMutation.get();
    if (!ev::isFunction(fn)) return;
    s.signalled = true;
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
    // Arrays: a replace-all (innerHTML =, textContent =) is one record naming
    // every node it added and removed.
    const std::vector<HostNodeState*>& added = rec.added;
    b.set("added", hostArrayOf(added.size(), [&added](size_t i) { return nodeSlot(added[i]); }));
    const std::vector<HostNodeState*>& removed = rec.removed;
    b.set("removed",
          hostArrayOf(removed.size(), [&removed](size_t i) { return nodeSlot(removed[i]); }));
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
    HookState* s = existingState();
    if (!node || !s) return ev::undefined();
    const bool subtree = boolAt(a, 1);
    auto it = s->watches.find(hostNodeStateFor(node));
    if (it == s->watches.end()) return ev::undefined();
    Watch& w = it->second;
    if (subtree) { if (w.subtree > 0) --w.subtree; }
    else { if (w.direct > 0) --w.direct; }
    if (w.direct == 0 && w.subtree == 0) s->watches.erase(it);
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
    dom::Document::setNodeRetainQuery(&hostHasNodeState);
    ObjectBuilder b;
    b.def("observeMutations", 2, observeMutations);
    b.def("unobserveMutations", 2, unobserveMutations);
    b.def("takeMutations", 0, takeMutations);
    b.def("onMutation", 1, onMutation);
    b.def("onFrame", 1, onFrame);
    // The builder roots the object; registerGlobal and globalValue both
    // allocate, so each use re-reads it.
    ev::registerGlobal("__bro_observers", b.get());
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) ev::setProperty(gt.value, "__bro_observers", b.get());
}

bool hostObserversHold(const HostNodeState* st) {
    if (!g_states || !st) return false;
    auto* key = const_cast<HostNodeState*>(st);
    auto names = [key](const std::vector<HostNodeState*>& v) {
        return std::find(v.begin(), v.end(), key) != v.end();
    };
    for (auto& [id, s] : *g_states) {
        if (!s) continue;
        if (s->watches.count(key)) return true;
        for (const RawRecord& rec : s->queue) {
            if (rec.target == key || rec.previousSibling == key || rec.nextSibling == key) return true;
            if (names(rec.added) || names(rec.removed) || names(rec.nodes)) return true;
        }
    }
    return false;
}

void fireHostObserverFrame() {
    if (!g_states) return;
    // Each compartment's tick, in it; the list copied first, since a tick
    // may make another compartment's state or tear one down.
    std::vector<std::pair<uint64_t, ev::Persistent>> ticks;
    for (auto& [id, s] : *g_states)
        if (s && ev::isFunction(s->onFrame.get())) ticks.emplace_back(id, ev::Persistent(s->onFrame.get()));
    for (auto& [id, fn] : ticks) {
        Compartment* c = compartmentById(id);
        if (!c) continue;
        runEntry(c, EntryKind::Async, [fn = std::move(fn)] {
            ev::CallResult r = ev::call(fn.get(), ev::undefined(), {});
            if (r.thrown) reportBronzeError("__bro_observers.onFrame", r.value);
        });
    }
}

void dropObserverState(uint64_t compartmentId) {
    if (g_states) g_states->erase(compartmentId);
}

}  // namespace bro::bronze_host
