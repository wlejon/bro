#pragma once

// The node registry's lifetime half, and the sweep that gives detached DOM
// back.
//
// WHAT IS RECLAIMED. A tree a script has taken out of its document — a view
// replaced by the next one, a list rebuilt, a template clone never appended —
// and that nothing in JavaScript can reach any more: its dom::Nodes, the
// registry entries behind their wrappers, the listeners and inline handlers
// registered on it (and the closures they hold), and for a <canvas> its scene
// and drawing buffer, which go with the element. What JavaScript can still
// reach is untouched and stays re-insertable, whether it is the root of the
// tree or a node deep inside it.
//
// HOW REACHABILITY IS ASKED. A connected node's wrapper is rooted by the
// registry (HostNodeState::jsObj), which is what keeps the expandos a program
// puts on it. A detached tree cannot be treated that way — the root is exactly
// what keeps it alive — so the sweep, once a second at most, and only when the
// DOM has changed:
//
//   1. frees every detached tree that has no wrapper at all;
//   2. for each other detached tree, DEMOTES its wrappers: they, and the
//      functions and receivers of the listeners on its elements, move out of
//      the root set into one plain array (the group). Every wrapper in the
//      tree gets a symbol-keyed link to the array, so holding ANY node of the
//      tree holds the whole group — a child kept across a rebuild keeps its
//      parent's wrapper, and the expandos on it, as the web does. Trees never
//      tested before share a group, up to 256 to a batch, since a rebuild
//      detaches hundreds at once and nearly all are garbage; a tree that has
//      survived a test, or whose batch did, gets a group of its own;
//   3. collects;
//   4. reads each group back through a WeakRef made on an earlier frame (a
//      WeakRef keeps its target for the job it was made in, so one made in
//      this pass could never report a death): a group that died is freed, its
//      entries forgotten and its trees handed to Document::freeDetachedTree; a
//      group that survived is promoted back, links cleared, exactly as it was.
//
// No JavaScript runs between 2 and 4, which is what makes the demotion safe:
// nothing can insert a demoted node into the document, or drop the last
// reference to one that was, while its wrapper is off the roots.
//
// ENTRY LIFETIME. A registry entry is freed once its node is gone and no JS
// object reaches it natively: `jsRefs` counts the wrapper handles (released by
// their finalizer) and the style / classList / dataset objects' closures
// (StateRef). The finalizer runs mid-collection and may only count, so the
// delete happens at the next sweep, on a plain host stack.

#include "bronze_host/host_internal.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace bro::bronze_host {

// ---- entry lifetime (host_node_registry.cpp) -------------------------------

// One more JS object reaches `st`. The handle destructor below is the release.
void retainNodeState(HostNodeState* st);
// A HandleDestructor, safe mid-collection: it only counts and queues.
void releaseNodeStateRef(void* st);
// For a handle born with `st` as its data: counts it and answers the
// destructor that uncounts it — `X.make(st, stateHandleDtor(st))`.
ev::HandleDestructor stateHandleDtor(HostNodeState* st);

// A captured entry that keeps the entry alive for as long as the closure
// holding it lives — what style, classList and dataset capture instead of a
// raw HostNodeState*.
class StateRef {
public:
    explicit StateRef(HostNodeState* st) : st_(st) { if (st_) retainNodeState(st_); }
    StateRef(const StateRef& o) : st_(o.st_) { if (st_) retainNodeState(st_); }
    StateRef& operator=(const StateRef&) = delete;
    ~StateRef() { if (st_) releaseNodeStateRef(st_); }
    HostNodeState* operator->() const { return st_; }
    HostNodeState* get() const { return st_; }

private:
    HostNodeState* st_;
};

// The entry for `node` if there is one; never creates.
HostNodeState* hostNodeStateIfAny(const dom::Node* node);
// Drop the entry's node and everything it roots, and take it out of the live
// map. The entry itself is freed by the reaper once nothing reaches it.
void hostForgetNodeState(HostNodeState* st);
// Free the entries that are finished: node gone, jsRefs zero, not pinned, not
// named by a pending MutationObserver record.
void hostReapNodeStates();
// The documents this layer has wrapped a node of, still alive.
std::vector<dom::Document*> hostObservedDocuments();

struct NodeRegistryStats {
    size_t entries = 0;   // allocated HostNodeStates
    size_t live = 0;      // entries whose node is alive
    size_t wrapped = 0;   // live entries with a wrapper rooted
};
NodeRegistryStats hostNodeRegistryStats();

// ---- listeners (host_dom_events.cpp, host_element.cpp) ---------------------

// Every ListenerRef on `el`: addEventListener registrations first, then the
// inline on<type> handlers.
void forEachElementListenerRef(dom::Element* el,
                               const std::function<void(const std::shared_ptr<ListenerRef>&)>& fn);

// ---- MutationObserver (host_observer_hooks.cpp) ----------------------------

// True while a watch or a queued record names `st`: its node is observable and
// its entry may not go.
bool hostObserversHold(const HostNodeState* st);

// ---- the sweep (host_node_sweep.cpp) ---------------------------------------

// Once per frame from the frame seam. Decides whether a pass is due.
void hostDomSweepFrame(double dtMs);
// A pass now, collection included (tests, idle GC).
void hostDomSweepNow();

struct DomSweepStats {
    uint64_t passes = 0;
    uint64_t collections = 0;
    uint64_t treesFreed = 0;
    uint64_t groupsDied = 0;
    uint64_t groupsSurvived = 0;
    double lastPassMs = 0.0;
    double lastCollectMs = 0.0;
    double lastScanMs = 0.0;     // finding the detached trees, freeing the unwrapped
    double lastDemoteMs = 0.0;   // taking the groups off the roots
    double lastFreeMs = 0.0;     // giving back the groups that died
    uint64_t lastGroups = 0;     // groups the last collecting pass tested
    size_t nodes = 0;           // nodes the wrapped documents own
    uint64_t processBytes = 0;   // private committed memory, where the OS says
};
DomSweepStats hostDomSweepStats();

}  // namespace bro::bronze_host
