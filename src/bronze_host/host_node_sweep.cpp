// The detached-tree sweep. host_node_sweep.h carries the design; this is the
// per-frame slice, the groups it demotes, the touch that brings one back, and
// the policy for when the sweep may ask the heap to collect.

#include "bronze_host/host_node_sweep.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_document.h"
#include "bronze_host/host_gc.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_brokit.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/node.h"
#include "dom/shadow_root.h"
#include "layout/el_video.h"
#include "layout/el_terminal.h"
#include "util/log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>
#elif defined(__linux__)
#  include <unistd.h>
#endif

namespace bro::bronze_host {

// One demotion: the trees whose wrappers moved into a group array, the entries
// and Persistents they moved out of (in array order), and the WeakRef the
// array is read back through. Each member is retained (jsRefs) while it is
// here, so the Persistent pointers stay valid; a member whose node was freed
// meanwhile is skipped on the way back.
struct SweepGroup {
    struct Tree {
        dom::Document* doc = nullptr;
        dom::Node* root = nullptr;
    };
    struct Slot {
        ev::Persistent* p = nullptr;
        uint32_t member = 0;
    };
    struct DocSlot {
        ev::Persistent* p = nullptr;
        dom::Document* doc = nullptr;
    };
    std::vector<Tree> trees;
    std::vector<dom::Document*> docs;
    std::vector<HostNodeState*> members;
    std::vector<Slot> slots;
    std::vector<DocSlot> docSlots;
    std::vector<std::shared_ptr<ListenerRef>> refs;
    ev::Persistent weak;
    uint64_t fullAtDemote = 0;
    uint64_t fullSeen = 0;
    double demotedAtMs = 0.0;
    // Lived through a full collection: only a later full one can kill it.
    bool old = false;
    // Decided by a collection, taken apart in a later slice's budget.
    enum class Pending : uint8_t { None, Died, Split } pending = Pending::None;
    size_t nodes = 0;
    size_t index = 0;
};

namespace {

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

// A frame's slice: a tenth of the frame, one to three milliseconds at display
// rates. A headless step of a second or more is not a frame anyone watches,
// and gets up to fifty so a test's few long steps finish the work.
constexpr double kBudgetFraction = 0.1;
constexpr double kMinBudgetMs = 1.0;
constexpr double kMaxFrameBudgetMs = 3.0;
constexpr double kLongStepMs = 100.0;
constexpr double kMaxLongStepBudgetMs = 50.0;
// Fresh trees share a group up to this many.
constexpr size_t kBatchTrees = 256;
// A tree something native holds, or one a touch just brought back, is looked
// at again after this long rather than on every frame.
constexpr double kDeferMs = 2000.0;
constexpr double kLaterScanMs = 500.0;
// Demoted trees wait for the heap's own full collection, which a program that
// keeps allocating reaches on its own. The sweep asks for one itself — the one
// hitch it may cause — only when the nodes waiting are many and have waited a
// while, or have waited very long.
constexpr double kForceCollectAfterMs = 10000.0;
constexpr size_t kForceCollectNodes = 50000;
constexpr double kForceCollectAnywayMs = 60000.0;
// A quiet host — no rAF, microtask or brokit work pending, and no trees
// detached — is not drawing anything, so a collection costs it nothing anyone
// sees. After this long
// quiet the sweep collects for trees demoted since its last quiet collection.
// It is the idle GC's rule, and stands in for it where the idle GC cannot run:
// inside a host eval, which is where a headless script steps time.
constexpr double kQuietCollectMs = 1000.0;
// A host that detaches trees without ever pausing (a timer rebuilding a panel
// every frame, a list re-filled as it scrolls) is never quiet, and the heap may
// not reach a full collection of its own for a long while: once this many
// nodes have waited this long, the sweep collects, at most this often.
constexpr double kBusyCollectMs = 2000.0;
constexpr size_t kBusyCollectNodes = 2000;

// ---------------------------------------------------------------------------
// The pieces of JavaScript the sweep calls
// ---------------------------------------------------------------------------

// Builtins only — Object.defineProperty, Symbol, WeakRef and its deref — so
// that no program code runs while the sweep moves roots. Leaked on purpose,
// like every process-lived Persistent in this layer.
struct Intrinsics {
    ev::Persistent defineProperty;
    ev::Persistent linkKey;
    ev::Persistent weakRefCtor;
    ev::Persistent deref;
    bool ok = false;
};

Intrinsics* g_js = nullptr;

Intrinsics* intrinsics() {
    if (g_js) return g_js->ok ? g_js : nullptr;
    g_js = new Intrinsics();
    ev::GlobalValue object = ev::globalValue("Object");
    if (!object.found || !ev::isObject(object.value)) return nullptr;
    g_js->defineProperty.set(ev::getProperty(object.value, "defineProperty"));
    ev::GlobalValue symbol = ev::globalValue("Symbol");
    if (!symbol.found || !ev::isFunction(symbol.value)) return nullptr;
    ev::Persistent symbolFn(symbol.value);
    ev::Persistent desc(ev::fromUtf8("bro.detachedGroup"));
    const Value descArg = desc.get();
    ev::CallResult sym = ev::call(symbolFn.get(), ev::undefined(), std::span<const Value>(&descArg, 1));
    if (sym.thrown) return nullptr;
    g_js->linkKey.set(sym.value);
    ev::GlobalValue weakRef = ev::globalValue("WeakRef");
    if (!weakRef.found || !ev::isFunction(weakRef.value)) return nullptr;
    g_js->weakRefCtor.set(weakRef.value);
    ev::Persistent proto(ev::getProperty(g_js->weakRefCtor.get(), "prototype"));
    g_js->deref.set(ev::getProperty(proto.get(), "deref"));
    if (!ev::isFunction(g_js->defineProperty.get()) || !ev::isFunction(g_js->deref.get()))
        return nullptr;
    g_js->ok = true;
    return g_js;
}

bool defineLink(Value obj, Value desc) {
    Intrinsics* js = intrinsics();
    ev::Persistent target(obj);
    ev::Persistent d(desc);
    const Value args[3] = {target.get(), js->linkKey.get(), d.get()};
    ev::CallResult r = ev::call(js->defineProperty.get(), ev::undefined(), args);
    return !r.thrown;
}

// The group array, or undefined once it has been collected.
Value derefGroup(SweepGroup* g) {
    Intrinsics* js = intrinsics();
    ev::CallResult got = ev::call(js->deref.get(), g->weak.get(), {});
    return got.thrown ? ev::undefined() : got.value;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct Candidate {
    dom::Document* doc = nullptr;
    dom::Node* node = nullptr;
};

struct Deferred {
    dom::Document* doc = nullptr;
    dom::Node* node = nullptr;
    double dueMs = 0.0;
};

struct SweepState {
    std::vector<Candidate> queue;
    size_t head = 0;
    std::vector<Deferred> later;
    std::unordered_set<const void*> laterSet;
    double nextLaterScanMs = 0.0;
    std::vector<std::unique_ptr<SweepGroup>> groups;
    // Made this slice, stamped with the collection count once the slice has
    // let go of its WeakRefs' targets.
    std::vector<SweepGroup*> fresh;
    std::unique_ptr<SweepGroup> batch;
    std::unordered_set<const void*> batchRoots;
    std::vector<dom::Node*> scratch;
    std::vector<dom::Document*> scratchDocs;
    size_t demotedTrees = 0;
    size_t demotedNodes = 0;
    double clockMs = 0.0;
    double quietMs = 0.0;
    double quietCollectAtMs = -1.0;
    double collectAtMs = -1e9;  // the sweep's last collection of its own
    uint64_t seenCollections = 0;
    uint64_t seenFull = 0;
    // A WeakRef was made or read since the kept list was last cleared.
    bool keptDirty = false;
    bool running = false;
    DomSweepStats stats;
};

SweepState& sweep() {
    static SweepState* s = new SweepState();
    return *s;
}

// ---------------------------------------------------------------------------
// Trees
// ---------------------------------------------------------------------------

// A node's subtree as the sweep sees it: its children, and the template
// content and shadow root its elements own by pointer.
template <typename Fn>
void walkTree(dom::Node* root, Fn&& fn) {
    std::vector<dom::Node*> stack{root};
    while (!stack.empty()) {
        dom::Node* n = stack.back();
        stack.pop_back();
        fn(n);
        for (dom::Node* c : n->childNodes()) stack.push_back(c);
        if (auto* el = dynamic_cast<dom::Element*>(n)) {
            if (dom::Element* content = el->templateContent()) stack.push_back(content);
            if (dom::ShadowRoot* sr = el->shadowRoot()) stack.push_back(sr);
        }
    }
}

dom::Node* topOf(dom::Node* n) {
    while (n->parentNode()) n = n->parentNode();
    return n;
}

// Something outside JavaScript still needs this tree: a MutationObserver
// watching or reporting a node of it, a <video>/<audio> that is playing, or
// a <terminal> whose child is running.
bool heldNatively(dom::Node* n, HostNodeState* st) {
    if (st && hostObserversHold(st)) return true;
    if (auto* el = dynamic_cast<dom::Element*>(n)) {
        if (layout::ElVideo* v = el->videoControl()) {
            if (v->isPlaying()) return true;
        }
        // A <terminal> whose child still runs: its `exit` event is still due.
        if (layout::ElTerminal* t = el->terminalControl()) {
            if (t->running()) return true;
        }
    }
    return false;
}

const void* candidateKey(const dom::Document* doc, const dom::Node* node) {
    return node ? static_cast<const void*>(node) : static_cast<const void*>(doc);
}

void enqueue(dom::Document* doc, dom::Node* node) {
    if (doc) sweep().queue.push_back({doc, node});
}

void defer(dom::Document* doc, dom::Node* node) {
    SweepState& s = sweep();
    if (!doc || !s.laterSet.insert(candidateKey(doc, node)).second) return;
    s.later.push_back({doc, node, s.clockMs + kDeferMs});
}

// ---------------------------------------------------------------------------
// Groups
// ---------------------------------------------------------------------------

void addGroup(std::unique_ptr<SweepGroup> g) {
    SweepState& s = sweep();
    g->index = s.groups.size();
    s.demotedTrees += g->trees.size();
    s.demotedNodes += g->nodes;
    s.fresh.push_back(g.get());
    s.groups.push_back(std::move(g));
}

std::unique_ptr<SweepGroup> removeGroup(SweepGroup* g) {
    SweepState& s = sweep();
    const size_t i = g->index;
    std::unique_ptr<SweepGroup> owned = std::move(s.groups[i]);
    if (i + 1 != s.groups.size()) {
        s.groups[i] = std::move(s.groups.back());
        s.groups[i]->index = i;
    }
    s.groups.pop_back();
    s.demotedTrees -= owned->trees.size();
    s.demotedNodes -= owned->nodes;
    s.fresh.erase(std::remove(s.fresh.begin(), s.fresh.end(), g), s.fresh.end());
    return owned;
}

// Move the trees' wrappers and listener roots into a new array, link every
// wrapper to it, and make the WeakRef. False, with nothing moved and the array
// emptied, when a link cannot be made (a frozen wrapper).
bool demote(SweepGroup& g) {
    Intrinsics* js = intrinsics();
    if (!js) return false;
    ev::Persistent k(ev::makeArray(0));
    ev::Persistent desc(ev::createObject());
    desc.set(ev::setProperty(desc.get(), "value", k.get()));
    desc.set(ev::setProperty(desc.get(), "writable", ev::fromBool(true)));
    desc.set(ev::setProperty(desc.get(), "configurable", ev::fromBool(true)));

    for (uint32_t m = 0; m < g.members.size(); ++m) g.slots.push_back({&g.members[m]->jsObj, m});
    for (uint32_t m = 0; m < g.members.size(); ++m) {
        HostNodeState* st = g.members[m];
        for (ev::Persistent* p : {&st->styleObj, &st->classListObj, &st->datasetObj,
                                  &st->computedObj}) {
            if (!ev::isUndefined(p->get())) g.slots.push_back({p, m});
        }
        if (!st->el) continue;
        forEachElementListenerRef(st->el, [&](const std::shared_ptr<ListenerRef>& ref) {
            if (!ref) return;
            g.refs.push_back(ref);
            g.slots.push_back({&ref->fn, m});
            g.slots.push_back({&ref->self, m});
        });
    }
    for (dom::Document* doc : g.docs) {
        ev::Persistent* ds = hostDocumentWrapperSlot(doc);
        if (ds && !ev::isUndefined(ds->get())) g.docSlots.push_back({ds, doc});
    }
    uint32_t i = 0;
    for (const SweepGroup::Slot& slot : g.slots) k.set(ev::setElement(k.get(), i++, slot.p->get()));
    for (const SweepGroup::DocSlot& ds : g.docSlots) k.set(ev::setElement(k.get(), i++, ds.p->get()));
    bool ok = true;
    for (HostNodeState* st : g.members) {
        if (!defineLink(st->jsObj.get(), desc.get())) {
            ok = false;
            break;
        }
    }
    if (ok) {
        for (const SweepGroup::DocSlot& ds : g.docSlots) {
            if (!defineLink(ds.p->get(), desc.get())) {
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        const Value kArg = k.get();
        ev::CallResult made = ev::construct(js->weakRefCtor.get(), std::span<const Value>(&kArg, 1));
        if (made.thrown) ok = false;
        else g.weak.set(made.value);
    }
    if (!ok) {
        const uint32_t total = static_cast<uint32_t>(g.slots.size() + g.docSlots.size());
        for (uint32_t j = 0; j < total; ++j) k.set(ev::setElement(k.get(), j, ev::undefined()));
        g.slots.clear();
        g.docSlots.clear();
        g.refs.clear();
        return false;
    }
    for (const SweepGroup::Slot& slot : g.slots) slot.p->set(ev::undefined());
    for (const SweepGroup::DocSlot& ds : g.docSlots) ds.p->set(ev::undefined());
    for (HostNodeState* st : g.members) {
        st->sweepGroup = &g;
        retainNodeState(st);
    }
    for (dom::Document* doc : g.docs) setParsedDocumentSweepGroup(doc, &g);
    g.demotedAtMs = sweep().clockMs;
    sweep().keptDirty = true;
    return true;
}

// Everything back from the array, which is left empty so a link that outlives
// this group holds nothing.
void restore(SweepGroup& g, Value kIn) {
    ev::Persistent k(kIn);
    for (uint32_t i = 0; i < g.slots.size(); ++i) {
        const SweepGroup::Slot& slot = g.slots[i];
        if (g.members[slot.member]->node) slot.p->set(ev::getElement(k.get(), i));
        k.set(ev::setElement(k.get(), i, ev::undefined()));
    }
    const uint32_t base = static_cast<uint32_t>(g.slots.size());
    for (uint32_t j = 0; j < g.docSlots.size(); ++j) {
        const SweepGroup::DocSlot& ds = g.docSlots[j];
        if (ds.doc && ds.p && dom::Document::isLiveDocument(ds.doc)) {
            ds.p->set(ev::getElement(k.get(), base + j));
        }
        k.set(ev::setElement(k.get(), base + j, ev::undefined()));
    }
}

enum class GroupEnd { Died, Split, Touched };

// Take a group apart after it died, was split, or was touched. Every tree its
// members now sit in is looked at again: a node may have been moved out of a
// demoted tree, or a wrapped node moved in, while it waited.
void endGroup(SweepGroup* g, GroupEnd how, const dom::Node* touched,
              const dom::Document* touchedDocIn = nullptr) {
    std::unique_ptr<SweepGroup> owned = removeGroup(g);
    std::unordered_set<const dom::Node*> seen;
    std::unordered_set<const dom::Document*> seenDocs;
    const dom::Document* touchedDoc = touchedDocIn;
    if (!touchedDoc && touched && isParsedDocument(touched->document())) {
        touchedDoc = touched->document();
    }
    const dom::Node* touchedTop =
        (touched && !touchedDoc) ? topOf(const_cast<dom::Node*>(touched)) : nullptr;
    auto requeue = [&](dom::Node* top) {
        if (!seen.insert(top).second) return;
        if (top == touchedTop) defer(top->document(), top);
        else enqueue(top->document(), top);
    };
    auto requeueDoc = [&](dom::Document* d) {
        if (!seenDocs.insert(d).second) return;
        if (d == touchedDoc) defer(d, nullptr);
        else enqueue(d, nullptr);
    };
    for (dom::Document* d : owned->docs) setParsedDocumentSweepGroup(d, nullptr);
    for (HostNodeState* st : owned->members) {
        if (!st->node) continue;
        dom::Document* d = st->node->document();
        if (isParsedDocument(d)) {
            if (how == GroupEnd::Split || d == touchedDoc) setParsedDocumentSweepAlone(d, true);
            requeueDoc(d);
            continue;
        }
        dom::Node* top = topOf(st->node);
        if (how == GroupEnd::Split || top == touchedTop) st->sweepAlone = true;
        requeue(top);
    }
    for (const SweepGroup::Tree& t : owned->trees) {
        if (!dom::Document::isLiveDocument(t.doc)) continue;
        if (!t.root) {
            if (isParsedDocument(t.doc)) {
                if (how == GroupEnd::Split || t.doc == touchedDoc)
                    setParsedDocumentSweepAlone(t.doc, true);
                requeueDoc(t.doc);
            }
        } else if (t.doc->ownsNode(t.root)) {
            requeue(topOf(t.root));
        }
    }
    for (HostNodeState* st : owned->members) {
        st->sweepGroup = nullptr;
        if (how == GroupEnd::Died) {
            st->pinned = false;
            hostForgetNodeState(st);
        }
        releaseNodeStateRef(st);
    }
    if (how == GroupEnd::Died) {
        for (const auto& ref : owned->refs) {
            ref->fn.set(ev::undefined());
            ref->self.set(ev::undefined());
        }
    }
    owned->weak.set(ev::undefined());
}

// A group that could not be demoted: its trees are looked at again alone.
void demoteFailed(SweepGroup& g) {
    for (HostNodeState* st : g.members) st->sweepAlone = true;
    for (dom::Document* d : g.docs) setParsedDocumentSweepAlone(d, true);
    for (const SweepGroup::Tree& t : g.trees) {
        if (g.trees.size() > 1) enqueue(t.doc, t.root);
        else defer(t.doc, t.root);
    }
}

void demoteOrDefer(std::unique_ptr<SweepGroup> g) {
    if (demote(*g)) addGroup(std::move(g));
    else demoteFailed(*g);
}

void flushBatch() {
    SweepState& s = sweep();
    s.batchRoots.clear();
    if (!s.batch || s.batch->trees.empty()) return;
    demoteOrDefer(std::move(s.batch));
    s.batch.reset();
}

// ---------------------------------------------------------------------------
// Candidates
// ---------------------------------------------------------------------------

void examineParsedDoc(dom::Document* doc) {
    SweepState& s = sweep();
    if (!dom::Document::isLiveDocument(doc) || !isParsedDocument(doc)) return;
    if (parsedDocumentSweepGroup(doc) || s.batchRoots.count(doc)) return;
    std::vector<HostNodeState*> members;
    bool held = false;
    bool inGroup = false;
    bool alone = parsedDocumentSweepAlone(doc);
    size_t nodes = 1;
    auto inspectSubtree = [&](dom::Node* subRoot, bool isDetachedSubtree) {
        std::vector<HostNodeState*> subMembers;
        bool subHeld = false;
        bool subInGroup = false;
        bool subAlone = false;
        size_t subNodes = 0;
        walkTree(subRoot, [&](dom::Node* n) {
            ++subNodes;
            HostNodeState* st = hostNodeStateIfAny(n);
            if (st && st->sweepGroup) subInGroup = true;
            if (heldNatively(n, st)) subHeld = true;
            if (st && !ev::isUndefined(st->jsObj.get())) {
                subMembers.push_back(st);
                if (st->sweepAlone) subAlone = true;
            }
        });
        if (isDetachedSubtree && !subInGroup && !subHeld && subMembers.empty()) {
            doc->freeDetachedTree(subRoot);
            ++s.stats.treesFreed;
            return;
        }
        nodes += subNodes;
        if (subInGroup) inGroup = true;
        if (subHeld) held = true;
        if (subAlone) alone = true;
        members.insert(members.end(), subMembers.begin(), subMembers.end());
    };
    if (dom::Element* docEl = doc->documentElement()) inspectSubtree(docEl, false);
    std::vector<dom::Node*> detached;
    doc->collectDetachedRoots(detached);
    for (dom::Node* r : detached) inspectSubtree(r, true);

    ev::Persistent* docSlot = hostDocumentWrapperSlot(doc);
    const bool hasDocWrapper = docSlot && !ev::isUndefined(docSlot->get());
    if (inGroup) return;
    if (held) {
        defer(doc, nullptr);
        return;
    }
    if (members.empty() && !hasDocWrapper) {
        destroyParsedDocument(doc);
        ++s.stats.treesFreed;
        return;
    }
    if (alone) {
        auto g = std::make_unique<SweepGroup>();
        g->trees.push_back({doc, nullptr});
        g->docs.push_back(doc);
        g->members = std::move(members);
        g->nodes = nodes;
        demoteOrDefer(std::move(g));
        return;
    }
    if (!s.batch) s.batch = std::make_unique<SweepGroup>();
    s.batch->trees.push_back({doc, nullptr});
    s.batch->docs.push_back(doc);
    s.batch->nodes += nodes;
    s.batch->members.insert(s.batch->members.end(), members.begin(), members.end());
    s.batchRoots.insert(doc);
    if (s.batch->trees.size() >= kBatchTrees) flushBatch();
}

void examine(dom::Document* doc, dom::Node* root) {
    if (!root) {
        examineParsedDoc(doc);
        return;
    }
    SweepState& s = sweep();
    if (!dom::Document::isLiveDocument(doc) || !doc->ownsNode(root)) return;
    if (!doc->isDetachedRoot(root) || s.batchRoots.count(root)) return;
    std::vector<HostNodeState*> members;
    bool held = false;
    bool inGroup = false;
    bool alone = false;
    size_t nodes = 0;
    walkTree(root, [&](dom::Node* n) {
        ++nodes;
        HostNodeState* st = hostNodeStateIfAny(n);
        if (st && st->sweepGroup) inGroup = true;
        if (heldNatively(n, st)) held = true;
        if (st && !ev::isUndefined(st->jsObj.get())) {
            members.push_back(st);
            if (st->sweepAlone) alone = true;
        }
    });
    // Part of a demoted tree: its group looks at it again when it ends.
    if (inGroup) return;
    if (held) {
        defer(doc, root);
        return;
    }
    if (members.empty()) {
        doc->freeDetachedTree(root);
        ++s.stats.treesFreed;
        return;
    }
    if (alone) {
        auto g = std::make_unique<SweepGroup>();
        g->trees.push_back({doc, root});
        g->members = std::move(members);
        g->nodes = nodes;
        demoteOrDefer(std::move(g));
        return;
    }
    if (!s.batch) s.batch = std::make_unique<SweepGroup>();
    s.batch->trees.push_back({doc, root});
    s.batch->nodes += nodes;
    s.batch->members.insert(s.batch->members.end(), members.begin(), members.end());
    s.batchRoots.insert(root);
    if (s.batch->trees.size() >= kBatchTrees) flushBatch();
}

struct Deadline {
    std::chrono::steady_clock::time_point end;
    bool unlimited = false;
    bool over() const { return !unlimited && std::chrono::steady_clock::now() >= end; }
};

// The documents' new candidates, a newly wrapped document's existing detached
// trees, and the deferred trees now due.
void gather() {
    SweepState& s = sweep();
    for (dom::Document* doc : hostObservedDocuments()) {
        if (isParsedDocument(doc)) continue;
        if (!doc->detachTracking()) {
            doc->setDetachTracking(true);
            s.scratch.clear();
            doc->collectDetachedRoots(s.scratch);
            for (dom::Node* n : s.scratch) enqueue(doc, n);
            continue;
        }
        s.scratch.clear();
        doc->takeDetachCandidates(s.scratch);
        for (dom::Node* n : s.scratch) enqueue(doc, n);
    }
    s.scratchDocs.clear();
    takeNewParsedDocuments(s.scratchDocs);
    for (dom::Document* doc : s.scratchDocs) enqueue(doc, nullptr);
    if (s.clockMs < s.nextLaterScanMs || s.later.empty()) return;
    s.nextLaterScanMs = s.clockMs + kLaterScanMs;
    size_t kept = 0;
    for (size_t i = 0; i < s.later.size(); ++i) {
        const Deferred d = s.later[i];
        if (d.dueMs <= s.clockMs) {
            s.laterSet.erase(candidateKey(d.doc, d.node));
            enqueue(d.doc, d.node);
        } else {
            s.later[kept++] = d;
        }
    }
    s.later.resize(kept);
}

void processQueue(const Deadline& dl) {
    SweepState& s = sweep();
    while (s.head < s.queue.size()) {
        if (dl.over()) break;
        const Candidate c = s.queue[s.head++];
        examine(c.doc, c.node);
    }
    if (s.head == s.queue.size()) {
        s.queue.clear();
        s.head = 0;
    } else if (s.head > 4096 && s.head * 2 > s.queue.size()) {
        s.queue.erase(s.queue.begin(), s.queue.begin() + static_cast<std::ptrdiff_t>(s.head));
        s.head = 0;
    }
}

// Read back the groups a collection since the last look may have decided.
void checkGroups(bool always) {
    SweepState& s = sweep();
    const ev::RuntimeTelemetry tel = ev::getRuntimeTelemetry();
    if (!always && tel.gcCollections == s.seenCollections) return;
    s.seenCollections = tel.gcCollections;
    s.seenFull = tel.gcFullCollections;
    for (const auto& owned : s.groups) {
        SweepGroup* g = owned.get();
        if (g->pending != SweepGroup::Pending::None) continue;
        if (g->old && tel.gcFullCollections == g->fullSeen) continue;
        const Value k = derefGroup(g);
        s.keptDirty = true;
        if (ev::isUndefined(k)) {
            s.stats.treesFreed += g->trees.size();
            ++s.stats.groupsDied;
            g->pending = SweepGroup::Pending::Died;
            continue;
        }
        if (tel.gcFullCollections <= g->fullAtDemote) continue;
        if (!g->old) ++s.stats.groupsSurvived;
        if (g->trees.size() > 1) {
            g->pending = SweepGroup::Pending::Split;
            continue;
        }
        g->old = true;
        g->fullSeen = tel.gcFullCollections;
    }
}

// Take apart the groups a collection decided, as many as the slice has time
// for and at least one.
void endPending(const Deadline& dl) {
    SweepState& s = sweep();
    bool first = true;
    for (size_t i = s.groups.size(); i-- > 0;) {
        if (i >= s.groups.size()) continue;
        SweepGroup* g = s.groups[i].get();
        if (g->pending == SweepGroup::Pending::None) continue;
        if (!first && dl.over()) return;
        first = false;
        if (g->pending == SweepGroup::Pending::Died) {
            endGroup(g, GroupEnd::Died, nullptr);
            continue;
        }
        const Value k = derefGroup(g);
        s.keptDirty = true;
        if (ev::isUndefined(k)) {
            s.stats.treesFreed += g->trees.size();
            ++s.stats.groupsDied;
            endGroup(g, GroupEnd::Died, nullptr);
            continue;
        }
        restore(*g, k);
        endGroup(g, GroupEnd::Split, nullptr);
    }
}

// The slice's WeakRefs let go of their targets, so the next collection can
// decide them, and the groups made in it learn which full collection they
// wait for. Clearing the kept list is what the microtask checkpoint does; the
// frame seam calls the sweep just after one.
void finishSlice() {
    SweepState& s = sweep();
    if (s.keptDirty) {
        ev::clearKeptObjects();
        s.keptDirty = false;
    }
    if (s.fresh.empty()) return;
    // A marking cycle open now (host_gc.cpp) counts as a full collection at
    // its finish, but it kept what was live at its start, these groups'
    // arrays among them: the one that decides them is the next.
    const uint64_t full = ev::getRuntimeTelemetry().gcFullCollections + (ev::heapPressure().marking ? 1 : 0);
    for (SweepGroup* g : s.fresh) g->fullAtDemote = full;
    s.fresh.clear();
}

double oldestYoungMs() {
    double oldest = -1.0;
    for (const auto& g : sweep().groups) {
        if (g->old || g->pending != SweepGroup::Pending::None) continue;
        if (oldest < 0.0 || g->demotedAtMs < oldest) oldest = g->demotedAtMs;
    }
    return oldest;
}

void collectForSweep() {
    SweepState& s = sweep();
    const auto c0 = std::chrono::steady_clock::now();
    // Outside an eval, through the idle GC's door, so it counts this as the
    // idle spell's collection rather than making another one.
    if (isHostEvaluating()) ev::collectGarbage();
    else hostCollectGarbage();
    s.stats.lastCollectMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
    ++s.stats.collections;
    if (hostGcLogEnabled())
        LOG_INFO("gc: DOM sweep collection in %.1f ms (quiet %.0f ms)", s.stats.lastCollectMs, s.quietMs);
}

}  // namespace

void hostSweepPromote(HostNodeState* st) {
    SweepGroup* g = st ? st->sweepGroup : nullptr;
    if (!g) return;
    SweepState& s = sweep();
    if (g->pending == SweepGroup::Pending::Died) {
        endGroup(g, GroupEnd::Died, nullptr);
        return;
    }
    const Value k = derefGroup(g);
    s.keptDirty = true;
    if (ev::isUndefined(k)) {
        s.stats.treesFreed += g->trees.size();
        ++s.stats.groupsDied;
        endGroup(g, GroupEnd::Died, nullptr);
        return;
    }
    const dom::Node* touched = st->node;
    restore(*g, k);
    ++s.stats.groupsPromoted;
    endGroup(g, GroupEnd::Touched, touched);
}

void hostSweepTouchNode(const dom::Node* node) {
    hostSweepTouch(hostNodeStateIfAny(node));
}

void hostSweepPromoteDocument(dom::Document* doc) {
    SweepGroup* g = parsedDocumentSweepGroup(doc);
    if (!g) return;
    SweepState& s = sweep();
    if (g->pending == SweepGroup::Pending::Died) {
        endGroup(g, GroupEnd::Died, nullptr, doc);
        return;
    }
    const Value k = derefGroup(g);
    s.keptDirty = true;
    if (ev::isUndefined(k)) {
        s.stats.treesFreed += g->trees.size();
        ++s.stats.groupsDied;
        endGroup(g, GroupEnd::Died, nullptr, doc);
        return;
    }
    restore(*g, k);
    ++s.stats.groupsPromoted;
    endGroup(g, GroupEnd::Touched, nullptr, doc);
}

void hostSweepTouchDocument(dom::Document* doc) {
    if (doc && parsedDocumentSweepGroup(doc)) hostSweepPromoteDocument(doc);
}

void hostSweepForgetDocument(dom::Document* doc) {
    if (!doc) return;
    SweepState& s = sweep();
    s.laterSet.erase(candidateKey(doc, nullptr));
    s.batchRoots.erase(candidateKey(doc, nullptr));
    if (SweepGroup* g = parsedDocumentSweepGroup(doc)) {
        setParsedDocumentSweepGroup(doc, nullptr);
        for (auto& ds : g->docSlots) {
            if (ds.doc == doc) {
                ds.doc = nullptr;
                ds.p = nullptr;
            }
        }
        g->docs.erase(std::remove(g->docs.begin(), g->docs.end(), doc), g->docs.end());
        g->trees.erase(std::remove_if(g->trees.begin(), g->trees.end(),
                                      [doc](const SweepGroup::Tree& t) { return t.doc == doc; }),
                       g->trees.end());
    }
    if (s.batch) {
        for (auto& ds : s.batch->docSlots) {
            if (ds.doc == doc) {
                ds.doc = nullptr;
                ds.p = nullptr;
            }
        }
        s.batch->docs.erase(std::remove(s.batch->docs.begin(), s.batch->docs.end(), doc),
                            s.batch->docs.end());
        s.batch->trees.erase(
            std::remove_if(s.batch->trees.begin(), s.batch->trees.end(),
                           [doc](const SweepGroup::Tree& t) { return t.doc == doc; }),
            s.batch->trees.end());
    }
}

void hostDomSweepFrame(double dtMs) {
    SweepState& s = sweep();
    if (s.running || !intrinsics()) return;
    s.running = true;
    const auto t0 = std::chrono::steady_clock::now();
    const double dt = dtMs > 0.0 ? dtMs : 16.67;
    s.clockMs += dt;
    const double budget =
        dt < kLongStepMs ? std::clamp(dt * kBudgetFraction, kMinBudgetMs, kMaxFrameBudgetMs)
                         : std::min(dt * kBudgetFraction, kMaxLongStepBudgetMs);
    Deadline dl{t0 + std::chrono::microseconds(static_cast<int64_t>(budget * 1000.0)), false};

    auto elapsedMs = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    // The reaper frees the entries the last slice let go of; it gets half the
    // budget first and whatever is left last.
    hostReapNodeStates(budget * 0.5);
    gather();
    const size_t groupsBefore = s.groups.size();
    const bool hadWork = s.head < s.queue.size();
    checkGroups(false);
    endPending(dl);
    processQueue(dl);
    flushBatch();
    finishSlice();
    hostReapNodeStates(std::max(0.0, budget - elapsedMs()));

    const double oldest = oldestYoungMs();
    const double waited = oldest >= 0.0 ? s.clockMs - oldest : 0.0;
    // A frame that detached trees is not quiet either: an app with no rAF
    // that rebuilds its DOM from input or a timer (a view switch) would
    // otherwise collect in the very next frame, every time. The collection
    // waits for a second without any; an app that detaches every frame is
    // collected by the heap and the memory-pressure GC, as one with a rAF is.
    const bool quiet = !hadWork && s.groups.size() == groupsBefore && !ev::microtasksPending() &&
                       !hasPendingAnimationFrames() && !brokitHasWorkInFlight();
    s.quietMs = quiet ? s.quietMs + dt : 0.0;
    const bool quietDue = s.quietMs >= kQuietCollectMs && oldest > s.quietCollectAtMs;
    const bool busyDue = waited >= kBusyCollectMs && s.demotedNodes >= kBusyCollectNodes &&
                         s.clockMs - s.collectAtMs >= kBusyCollectMs;
    bool forced = false;
    if (oldest >= 0.0 && (quietDue || busyDue ||
                         (waited >= kForceCollectAfterMs && s.demotedNodes >= kForceCollectNodes) ||
                         waited >= kForceCollectAnywayMs)) {
        collectForSweep();
        s.quietCollectAtMs = s.clockMs;
        s.collectAtMs = s.clockMs;
        forced = true;
    }

    if (hadWork || forced || s.groups.size() != groupsBefore) {
        ++s.stats.passes;
        s.stats.lastPassMs = elapsedMs();
        s.stats.maxPassMs = std::max(s.stats.maxPassMs, s.stats.lastPassMs);
    }
    s.running = false;
}

void hostDomSweepNow() {
    SweepState& s = sweep();
    if (s.running || !intrinsics()) return;
    s.running = true;
    const auto t0 = std::chrono::steady_clock::now();
    const Deadline all{t0, true};
    hostReapNodeStates();
    gather();
    for (dom::Document* doc : hostObservedDocuments()) {
        if (isParsedDocument(doc)) continue;
        s.scratch.clear();
        doc->collectDetachedRoots(s.scratch);
        for (dom::Node* n : s.scratch) enqueue(doc, n);
    }
    s.scratchDocs.clear();
    collectAllParsedDocuments(s.scratchDocs);
    for (dom::Document* doc : s.scratchDocs) enqueue(doc, nullptr);
    // Three rounds: a batch that lives is split, its trees are tested alone,
    // and what died in either is freed.
    for (int round = 0; round < 3; ++round) {
        processQueue(all);
        flushBatch();
        finishSlice();
        if (s.groups.empty()) break;
        collectForSweep();
        checkGroups(true);
        endPending(all);
    }
    processQueue(all);
    flushBatch();
    finishSlice();
    hostReapNodeStates();
    ++s.stats.passes;
    s.stats.lastPassMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    s.running = false;
}


DomSweepStats hostDomSweepStats() {
    SweepState& s = sweep();
    DomSweepStats out = s.stats;
    s.stats.maxPassMs = 0.0;
    out.demotedTrees = s.demotedTrees;
    out.groups = s.groups.size();
    out.queued = s.queue.size() - s.head;
    std::unordered_set<const dom::Document*> counted;
    for (dom::Document* doc : hostObservedDocuments()) {
        if (counted.insert(doc).second) out.nodes += doc->ownedNodeCount();
    }
    s.scratchDocs.clear();
    collectAllParsedDocuments(s.scratchDocs);
    for (dom::Document* doc : s.scratchDocs) {
        if (counted.insert(doc).second) out.nodes += doc->ownedNodeCount();
    }
    out.processBytes = hostProcessPrivateBytes();
    return out;
}

}  // namespace bro::bronze_host
