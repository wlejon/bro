// The detached-tree sweep. host_node_sweep.h carries the design; this is the
// pass, the pool of weak references it reads its groups back through, and the
// policy that decides how often a pass may collect.

#include "bronze_host/host_node_sweep.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/node.h"
#include "dom/shadow_root.h"
#include "layout/el_video.h"
#include "util/log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
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

namespace {

// The process's private committed memory — what a leak grows — or 0 where
// there is no cheap way to ask. Linux answers resident pages instead.
uint64_t processPrivateBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        return static_cast<uint64_t>(pmc.PrivateUsage);
    return 0;
#elif defined(__linux__)
    unsigned long size = 0, resident = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    const int got = std::fscanf(f, "%lu %lu", &size, &resident);
    std::fclose(f);
    if (got != 2) return 0;
    return static_cast<uint64_t>(resident) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#else
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

// A pass scans every node the documents own, so it runs at most once a second
// and only when a document has allocated since the last one; a tree detached
// without anything being built is found by the slow pass.
constexpr double kPassIntervalMs = 1000.0;
constexpr double kSlowPassIntervalMs = 5000.0;
// A pass that has groups to test collects the whole heap. It may do so once a
// second, or less often when a collection costs more than 2% of the time
// between them.
constexpr double kMinCollectIntervalMs = 1000.0;
constexpr double kCollectCostFactor = 50.0;
constexpr uint32_t kRetestEveryPasses = 30;
constexpr size_t kInitialPool = 64;
constexpr size_t kMaxPool = 8192;

// ---------------------------------------------------------------------------
// The pieces of JavaScript a pass calls
// ---------------------------------------------------------------------------

// Builtins only — Object.defineProperty, Symbol, WeakRef and its deref — so
// that no program code can run while a group is off the roots. Leaked on
// purpose, like every process-lived Persistent in this layer.
struct Intrinsics {
    ev::Persistent defineProperty;
    ev::Persistent linkKey;
    ev::Persistent weakRefCtor;
    ev::Persistent deref;
    ev::Persistent unlinkDesc;
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
    ObjectBuilder unlink;
    unlink.set("value", ev::undefined());
    unlink.set("writable", ev::fromBool(true));
    unlink.set("configurable", ev::fromBool(true));
    g_js->unlinkDesc.set(unlink.get());
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

// ---------------------------------------------------------------------------
// The pool
// ---------------------------------------------------------------------------

// A group array and the WeakRef that will read it back. Made at the end of a
// pass and used by a later one: `new WeakRef(k)` keeps `k` alive until the
// next microtask checkpoint, so a WeakRef made inside the pass that tests it
// would always report the group alive. The frame seam drains microtasks every
// frame, so an entry made on an earlier frame is clear of that.
struct PoolEntry {
    ev::Persistent k;
    ev::Persistent w;
    uint64_t frame = 0;
};

struct SweepState {
    std::vector<std::unique_ptr<PoolEntry>> pool;
    size_t poolTarget = kInitialPool;
    uint64_t frame = 0;
    uint32_t pass = 0;
    uint64_t lastAllocs = 0;
    double sincePassMs = 0.0;
    double sinceCollectMs = 1e9;
    // The last pass left fresh trees untested — the pool ran short or the
    // collection was throttled — so the next pass is due on the clock alone.
    bool pending = false;
    bool running = false;
    DomSweepStats stats;
};

SweepState& sweep() {
    static SweepState* s = new SweepState();
    return *s;
}

void topUpPool() {
    SweepState& s = sweep();
    Intrinsics* js = intrinsics();
    if (!js) return;
    while (s.pool.size() < s.poolTarget) {
        auto e = std::make_unique<PoolEntry>();
        e->k.set(ev::makeArray(0));
        const Value kArg = e->k.get();
        ev::CallResult made = ev::construct(js->weakRefCtor.get(), std::span<const Value>(&kArg, 1));
        if (made.thrown) return;
        e->w.set(made.value);
        e->frame = s.frame;
        s.pool.push_back(std::move(e));
    }
}

// Entries are appended in frame order, so when the newest is usable every one
// is; taking from the back keeps a pass that uses thousands of them linear.
std::unique_ptr<PoolEntry> takePoolEntry() {
    SweepState& s = sweep();
    if (s.pool.empty() || s.pool.back()->frame >= s.frame) return nullptr;
    std::unique_ptr<PoolEntry> e = std::move(s.pool.back());
    s.pool.pop_back();
    return e;
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

struct Candidate {
    dom::Document* doc = nullptr;
    dom::Node* root = nullptr;
    std::vector<HostNodeState*> members;
    // Some wrapper of it has survived a test before, so it is likely held and
    // is tested in a group of its own rather than sinking a batch.
    bool alone = false;
};

// Every root a group takes off the root set, in the order it was stored in the
// group array: the wrappers first, then the style / classList / dataset /
// computed objects cached on the entries, then each listener's function and
// receiver. The ListenerRefs are held so the Persistents stay where they are.
//
// A group is one tree, or a batch of trees never tested before: a rebuild
// detaches hundreds of small trees at once and nearly all of them are garbage,
// so one array and one weak reference answer for the batch. A batch that
// survives is not split in the pass; its trees are marked suspect and each is
// tested alone on the next.
struct Group {
    std::vector<Candidate> trees;
    std::unique_ptr<PoolEntry> entry;
    std::vector<ev::Persistent*> moved;
    std::vector<std::shared_ptr<ListenerRef>> refs;
    bool batch = false;
};

// A wrapper whose batch survived: fresh, but tested alone.
constexpr uint32_t kSuspect = UINT32_MAX;
constexpr size_t kBatchTrees = 256;

template <typename Fn>
void forEachMember(Group& g, Fn&& fn) {
    for (Candidate& c : g.trees)
        for (HostNodeState* st : c.members) fn(st);
}

// Something outside JavaScript still needs this tree: a MutationObserver
// watching or reporting a node of it, or a <video>/<audio> that is playing.
bool heldNatively(dom::Node* n, HostNodeState* st) {
    if (st && hostObserversHold(st)) return true;
    if (auto* el = dynamic_cast<dom::Element*>(n)) {
        if (layout::ElVideo* v = el->videoControl()) {
            if (v->isPlaying()) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Demote, and back
// ---------------------------------------------------------------------------

// Move the tree's wrappers and listener roots into the entry's array and link
// every wrapper to it. False when a link cannot be made (a frozen wrapper):
// the group is then promoted straight back and the tree stays as it was.
bool demote(Group& g) {
    ev::Persistent k(g.entry->k.get());
    ev::Persistent desc(ev::createObject());
    desc.set(ev::setProperty(desc.get(), "value", k.get()));
    desc.set(ev::setProperty(desc.get(), "writable", ev::fromBool(true)));
    desc.set(ev::setProperty(desc.get(), "configurable", ev::fromBool(true)));

    forEachMember(g, [&](HostNodeState* st) { g.moved.push_back(&st->jsObj); });
    forEachMember(g, [&](HostNodeState* st) {
        for (ev::Persistent* p : {&st->styleObj, &st->classListObj, &st->datasetObj,
                                  &st->computedObj}) {
            if (!ev::isUndefined(p->get())) g.moved.push_back(p);
        }
        if (!st->el) return;
        forEachElementListenerRef(st->el, [&](const std::shared_ptr<ListenerRef>& ref) {
            if (!ref) return;
            g.refs.push_back(ref);
            g.moved.push_back(&ref->fn);
            g.moved.push_back(&ref->self);
        });
    });
    uint32_t slot = 0;
    for (ev::Persistent* p : g.moved) k.set(ev::setElement(k.get(), slot++, p->get()));
    bool linked = true;
    forEachMember(g, [&](HostNodeState* st) {
        if (linked && !defineLink(st->jsObj.get(), desc.get())) linked = false;
    });
    if (!linked) return false;
    for (ev::Persistent* p : g.moved) p->set(ev::undefined());
    g.entry->k.set(ev::undefined());
    return true;
}

// Put everything back from `k`, and clear the links so the array can go.
void promote(Group& g, Value kIn) {
    Intrinsics* js = intrinsics();
    ev::Persistent k(kIn);
    uint32_t slot = 0;
    for (ev::Persistent* p : g.moved) p->set(ev::getElement(k.get(), slot++));
    forEachMember(g, [&](HostNodeState* st) {
        if (!ev::isUndefined(st->jsObj.get())) defineLink(st->jsObj.get(), js->unlinkDesc.get());
    });
}

// The whole group is garbage: its wrappers, its listeners' functions, all of
// it. Forget the entries and give the trees back.
void release(Group& g) {
    forEachMember(g, [](HostNodeState* st) {
        st->pinned = false;
        hostForgetNodeState(st);
    });
    for (const auto& ref : g.refs) {
        ref->fn.set(ev::undefined());
        ref->self.set(ev::undefined());
    }
    for (Candidate& c : g.trees) c.doc->freeDetachedTree(c.root);
}

// ---------------------------------------------------------------------------
// The pass
// ---------------------------------------------------------------------------

void runPass(bool collectAllowed) {
    SweepState& s = sweep();
    if (s.running || !intrinsics()) return;
    s.running = true;
    const auto t0 = std::chrono::steady_clock::now();
    ++s.pass;
    ++s.stats.passes;
    hostReapNodeStates();

    std::vector<Candidate> candidates;
    bool fresh = false;
    for (dom::Document* doc : hostObservedDocuments()) {
        std::vector<dom::Node*> roots;
        doc->collectDetachedRoots(roots);
        for (dom::Node* root : roots) {
            Candidate c;
            c.doc = doc;
            c.root = root;
            bool held = false;
            walkTree(root, [&](dom::Node* n) {
                HostNodeState* st = hostNodeStateIfAny(n);
                if (heldNatively(n, st)) held = true;
                if (st && !ev::isUndefined(st->jsObj.get())) c.members.push_back(st);
            });
            if (held) {
                for (HostNodeState* st : c.members) st->survivedPass = s.pass;
                continue;
            }
            if (c.members.empty()) {
                doc->freeDetachedTree(root);
                ++s.stats.treesFreed;
                continue;
            }
            // A tree whose every wrapper survived the previous pass was tested
            // a second ago; it waits for a pass with new garbage, or for the
            // periodic retest, rather than buying a collection of its own.
            for (HostNodeState* st : c.members) {
                if (st->survivedPass == 0 || st->survivedPass + 1 != s.pass) fresh = true;
                if (st->survivedPass != 0) c.alone = true;
            }
            if (s.pass % kRetestEveryPasses == 0) fresh = true;
            candidates.push_back(std::move(c));
        }
    }

    s.stats.lastScanMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const bool collect = collectAllowed && fresh && !candidates.empty() &&
                         s.sinceCollectMs >= std::max(kMinCollectIntervalMs,
                                                      kCollectCostFactor * s.stats.lastCollectMs);
    std::vector<Group> groups;
    s.pending = fresh && !candidates.empty() && !collect;
    const auto d0 = std::chrono::steady_clock::now();
    if (collect) {
        std::vector<Group> planned;
        Group batch;
        batch.batch = true;
        for (Candidate& c : candidates) {
            if (c.alone) {
                Group g;
                g.trees.push_back(std::move(c));
                planned.push_back(std::move(g));
                continue;
            }
            batch.trees.push_back(std::move(c));
            if (batch.trees.size() == kBatchTrees) {
                planned.push_back(std::move(batch));
                batch = Group();
                batch.batch = true;
            }
        }
        if (!batch.trees.empty()) planned.push_back(std::move(batch));

        size_t wanted = 0;
        for (Group& g : planned) {
            g.entry = takePoolEntry();
            if (!g.entry) {
                ++wanted;
                continue;
            }
            if (!demote(g)) {
                promote(g, g.entry->k.get());
                forEachMember(g, [&](HostNodeState* st) { st->survivedPass = s.pass; });
                continue;
            }
            groups.push_back(std::move(g));
        }
        if (wanted) {
            s.poolTarget = std::min(kMaxPool, std::max(s.poolTarget, s.pool.size() + wanted) * 2);
            s.pending = true;
        }
    } else if (!fresh) {
        // Every one of them survived the last test; carry that forward. A
        // pass that could not collect for any other reason leaves them fresh.
        for (Candidate& c : candidates)
            for (HostNodeState* st : c.members) st->survivedPass = s.pass;
    }

    if (!groups.empty()) {
        const auto c0 = std::chrono::steady_clock::now();
        s.stats.lastDemoteMs = std::chrono::duration<double, std::milli>(c0 - d0).count();
        ev::collectGarbage();
        s.stats.lastCollectMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        s.sinceCollectMs = 0.0;
        ++s.stats.collections;

        Intrinsics* js = intrinsics();
        const auto f0 = std::chrono::steady_clock::now();
        s.stats.lastGroups = groups.size();
        for (Group& g : groups) {
            const Value w = g.entry->w.get();
            ev::CallResult got = ev::call(js->deref.get(), w, {});
            if (got.thrown || ev::isUndefined(got.value)) {
                release(g);
                ++s.stats.groupsDied;
                s.stats.treesFreed += g.trees.size();
            } else {
                promote(g, got.value);
                const uint32_t mark = g.batch && g.trees.size() > 1 ? kSuspect : s.pass;
                forEachMember(g, [&](HostNodeState* st) { st->survivedPass = mark; });
                if (mark == kSuspect) s.pending = true;
                ++s.stats.groupsSurvived;
            }
        }
        s.stats.lastFreeMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
    }

    hostReapNodeStates();
    topUpPool();
    s.stats.lastPassMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    s.running = false;
}

}  // namespace

void hostDomSweepFrame(double dtMs) {
    // BRO_DOM_SWEEP=0 turns the passes off: the before half of a leak
    // measurement, and the switch to reach for if a pass is ever suspected.
    static const bool enabled = [] {
        const char* v = std::getenv("BRO_DOM_SWEEP");
        return !(v && v[0] == '0');
    }();
    if (!enabled) return;
    SweepState& s = sweep();
    ++s.frame;
    const double dt = dtMs > 0.0 ? dtMs : 16.67;
    s.sincePassMs += dt;
    s.sinceCollectMs += dt;
    hostReapNodeStates();
    if (s.frame == 1) topUpPool();
    const uint64_t allocs = dom::Document::nodeAllocations();
    const bool due = (s.sincePassMs >= kPassIntervalMs && (allocs != s.lastAllocs || s.pending)) ||
                     s.sincePassMs >= kSlowPassIntervalMs;
    if (!due) return;
    s.sincePassMs = 0.0;
    s.lastAllocs = allocs;
    runPass(true);
}

void hostDomSweepNow() {
    SweepState& s = sweep();
    s.sinceCollectMs = 1e9;
    runPass(true);
}

DomSweepStats hostDomSweepStats() {
    DomSweepStats out = sweep().stats;
    for (dom::Document* doc : hostObservedDocuments()) out.nodes += doc->ownedNodeCount();
    out.processBytes = processPrivateBytes();
    return out;
}

}  // namespace bro::bronze_host
