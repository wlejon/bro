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
// what keeps it alive — so the sweep, a little every frame:
//
//   1. takes the nodes that lost a parent or were made since the last frame
//      (Document::takeDetachCandidates) and keeps those that are detached
//      roots, so it never walks the connected document;
//   2. frees every such tree that has no wrapper at all, there and then;
//   3. DEMOTES the wrappers of every other tree: they, the style / classList /
//      dataset / computed objects cached on their entries, and the functions
//      and receivers of the listeners on its elements, move out of the root set
//      into one plain array (the group), read back later through a WeakRef.
//      Every wrapper in the tree gets a symbol-keyed link to the array, so
//      holding ANY node of the tree holds the whole group — a child kept
//      across a rebuild keeps its parent's wrapper, and the expandos on it, as
//      the web does. Trees never tested share a group, up to 256 to a batch,
//      since a rebuild detaches hundreds at once and nearly all are garbage; a
//      tree that has been found held gets a group of its own;
//   4. after the heap next collects — on its own schedule, not the sweep's —
//      reads each group back: a group that died has its entries forgotten and
//      its trees queued again, now wrapper-less, for step 2; a batch that
//      lived through a full collection is split into one group per tree; a
//      single tree that lived stays demoted for as long as the program holds
//      it, at the cost of one weak read per full collection.
//
// PROMOTE ON TOUCH. A demoted group stays off the roots across frames, while
// the program runs. Everything native that reads a wrapper, a cached object or
// a listener goes through a touch first — the registry lookup (stateFor), the
// unwrap of a receiver or argument (nodeStateOf), getComputedStyle, and a
// listener being called — and a touch puts the whole group back exactly as it
// was before anything reads it. So a detached node JavaScript still uses is a
// promoted one, and a demoted one is never read. A group found dead at a touch
// is released there, and the lookup answers a fresh entry.
//
// COST. Steps 1 to 4, taking apart the groups a collection decided, and the
// reaper freeing the entries they let go of share a budget of a tenth of the
// frame (one to three milliseconds at display rates; more for the long steps
// of a headless run) and pick up where they stopped. Old wrappers can only be
// proven dead by a full collection, which costs tens of milliseconds, so the
// sweep does not ask for one on the frame path: it waits for the heap's own,
// which a program that keeps rebuilding its DOM reaches by allocating. It
// collects itself only once the host has been quiet for a second (no rAF,
// microtask or brokit work pending — the idle GC's rule, which it also applies
// inside a host eval, where a headless script steps time and the idle GC does
// not run), or when tens of thousands of nodes have waited ten seconds, or any
// a minute.
//
// ENTRY LIFETIME. A registry entry is freed once its node is gone and no JS
// object reaches it natively: `jsRefs` counts the wrapper handles (released by
// their finalizer), the style / classList / dataset objects' closures
// (StateRef), and a demoted group holding it. The finalizer runs
// mid-collection and may only count, so the delete happens at the next sweep,
// on a plain host stack.

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

// The entry for `node` if there is one; never creates, never promotes.
HostNodeState* hostNodeStateIfAny(const dom::Node* node);
// Drop the entry's node and everything it roots, and take it out of the live
// map. The entry itself is freed by the reaper once nothing reaches it.
void hostForgetNodeState(HostNodeState* st);
// Free the entries that are finished: node gone, jsRefs zero, not pinned, not
// named by a pending MutationObserver record. With a budget, stops when it is
// spent and leaves the rest queued.
void hostReapNodeStates(double budgetMs = -1.0);
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

// Put back the group `st` was demoted with, or release it if it died. Always
// leaves st->sweepGroup null.
void hostSweepPromote(HostNodeState* st);
// The touch: every native read of a node's wrapper, cached objects or
// listeners comes through here first.
inline void hostSweepTouch(HostNodeState* st) {
    if (st && st->sweepGroup) hostSweepPromote(st);
}
void hostSweepTouchNode(const dom::Node* node);

// Once per frame from the frame seam: a budgeted slice of the work.
void hostDomSweepFrame(double dtMs);
// All of it now, collections included (tests).
void hostDomSweepNow();

struct DomSweepStats {
    uint64_t passes = 0;          // frames the sweep had work on
    uint64_t collections = 0;     // collections the sweep asked for itself
    uint64_t treesFreed = 0;
    uint64_t groupsDied = 0;
    uint64_t groupsSurvived = 0;  // lived through a full collection
    uint64_t groupsPromoted = 0;  // brought back by a touch
    double lastPassMs = 0.0;      // the last frame's slice
    double maxPassMs = 0.0;       // the longest slice since the stats were last read
    double lastCollectMs = 0.0;
    size_t demotedTrees = 0;      // trees in groups now
    size_t groups = 0;            // groups now
    size_t queued = 0;            // candidates not yet looked at
    size_t nodes = 0;             // nodes the wrapped documents own
    uint64_t processBytes = 0;    // private committed memory, where the OS says
};
DomSweepStats hostDomSweepStats();

}  // namespace bro::bronze_host
