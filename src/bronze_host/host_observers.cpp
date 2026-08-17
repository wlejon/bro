// MutationObserver for a bronze-compiled app.
//
// WHERE THE NOTICES COME FROM, and why that is the whole design. This does not
// watch the bronze host's own mutators. It registers with the DOM
// (Document::addMutationObserver) and is told about every change to the tree —
// one made by compiled code, one made by the page's own script, one made by the
// engine's C++ — because a mutation is a property of the tree and not of who
// made it. That hook is new (src/dom/document.h) and it replaces the older
// arrangement where the only mutation notifications in the process were built
// inside the QuickJS bindings and were therefore invisible to everything else.
// The alternative — a second observer system watching only this layer's own
// calls — is exactly the mistake host_dom_events.cpp exists to avoid.
//
// WHEN THEY ARE DELIVERED. The web delivers records at the end of the microtask
// checkpoint that follows the mutation. Here they are delivered once per frame,
// from the frame seam, after requestAnimationFrame and before the closing
// microtask drain (dom_globals.cpp lists the order). So a mutation made in an
// rAF callback or a timer is reported in the same frame, and one made in an
// event handler — which runs outside the seam, inside the engine's input
// pipeline — is reported at the top of the next. That is the same one-frame
// resolution every other asynchronous thing in this layer has, and it is stated
// rather than papered over: there is exactly one host seam per frame.
//
// Records queued DURING a delivery are held for the next one. That is what
// stops an observer whose callback mutates the thing it observes from
// re-entering itself forever, and it is what the web's "queue a mutation
// observer microtask" ends up doing too.
//
// LIFETIME. An observer is rooted from host memory until disconnect(), like an
// uncleared interval (host_timers.cpp says the same thing at more length). Its
// entry is never freed: the entry holds Persistents, and freeing it from the
// handle finalizer is precisely what the GC rule in host_internal.h forbids.
//
// NODES IN A RECORD are held as REGISTRY ENTRIES, not raw dom::Node*. A node
// removed in one frame can be freed before the record naming it is delivered,
// and the registry is the thing that already learns about that
// (Document::addNodeFreedObserver). A slot whose node has since been freed
// reports null rather than a pointer into released storage.

#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"  // ObjectBuilder, argAt

#include "dom/document.h"
#include "dom/element.h"
#include "dom/node.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// One queued record, in host memory, waiting for the frame seam. The strings
// are copies: MutationNotice borrows both of its for the duration of the call.
struct PendingRecord {
    dom::Document::MutationNotice::Kind kind{};
    HostNodeState* target = nullptr;
    HostNodeState* added = nullptr;
    HostNodeState* removed = nullptr;
    HostNodeState* previousSibling = nullptr;
    HostNodeState* nextSibling = nullptr;
    std::string attributeName;
    std::string oldValue;
    bool hasOldValue = false;
};

// What one observe() call asked for.
struct MoTarget {
    HostNodeState* node = nullptr;
    bool subtree = false;
    bool childList = false;
    bool attributes = false;
    bool characterData = false;
    bool attributeOldValue = false;
    bool characterDataOldValue = false;
};

struct MoEntry {
    uint32_t tag = kHostMutationObserverTag;  // must be first — see host_internal.h
    ev::Persistent obj;       // the observer object: the callback's `this` and 2nd argument
    ev::Persistent callback;
    std::vector<MoTarget> targets;
    std::vector<PendingRecord> queue;
};

// Process-lived and never freed, the same convention the timer table follows:
// these hold ev::Persistents, and a static destructor running at process exit
// would release root slots against a runtime whose statics may already be gone.
std::vector<std::unique_ptr<MoEntry>>* g_observers = nullptr;
bool g_hookInstalled = false;
bool g_delivering = false;

std::vector<std::unique_ptr<MoEntry>>& observers() {
    if (!g_observers) g_observers = new std::vector<std::unique_ptr<MoEntry>>();
    return *g_observers;
}

MoEntry* observerOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* mo = static_cast<MoEntry*>(ev::handleData(v));
    if (!mo || mo->tag != kHostMutationObserverTag) return nullptr;
    return mo;
}

bool isAncestorOf(dom::Node* maybeAncestor, dom::Node* node) {
    if (!maybeAncestor || !node) return false;
    for (dom::Node* p = node->parentNode(); p; p = p->parentNode()) {
        if (p == maybeAncestor) return true;
    }
    return false;
}

bool optionOn(Value options, const char* name) {
    if (!ev::isObject(options)) return false;
    return ev::toBool(ev::getProperty(options, name));
}

// ---------------------------------------------------------------------------
// The notice hook
// ---------------------------------------------------------------------------

void onDomMutation(dom::Document*, const dom::Document::MutationNotice& notice) {
    using Kind = dom::Document::MutationNotice::Kind;
    if (!notice.target) return;

    for (std::unique_ptr<MoEntry>& entry : observers()) {
        for (const MoTarget& t : entry->targets) {
            dom::Node* watched = t.node ? t.node->node : nullptr;
            if (!watched) continue;   // the observed node has been freed

            const bool direct = watched == notice.target;
            if (!direct && !(t.subtree && isAncestorOf(watched, notice.target))) continue;

            bool wanted = false;
            bool withOldValue = false;
            switch (notice.kind) {
                case Kind::ChildList:
                    wanted = t.childList;
                    break;
                case Kind::Attributes:
                    wanted = t.attributes;
                    withOldValue = t.attributeOldValue;
                    break;
                case Kind::CharacterData:
                    wanted = t.characterData;
                    withOldValue = t.characterDataOldValue;
                    break;
            }
            if (!wanted) continue;

            PendingRecord rec;
            rec.kind = notice.kind;
            rec.target = hostNodeStateFor(notice.target);
            rec.added = notice.added ? hostNodeStateFor(notice.added) : nullptr;
            rec.removed = notice.removed ? hostNodeStateFor(notice.removed) : nullptr;
            rec.previousSibling =
                notice.previousSibling ? hostNodeStateFor(notice.previousSibling) : nullptr;
            rec.nextSibling =
                notice.nextSibling ? hostNodeStateFor(notice.nextSibling) : nullptr;
            if (notice.attributeName) rec.attributeName = *notice.attributeName;
            if (withOldValue && notice.oldValue) {
                rec.oldValue = *notice.oldValue;
                rec.hasOldValue = true;
            }
            entry->queue.push_back(std::move(rec));

            // One record per mutation per observer, however many of its
            // targets matched — two overlapping observe() calls on the same
            // observer are one registration in the web's model too.
            break;
        }
    }
}

void ensureHook(dom::Document* doc) {
    if (g_hookInstalled || !doc) return;
    doc->addMutationObserver(&onDomMutation);
    g_hookInstalled = true;
}

// ---------------------------------------------------------------------------
// Building the records the callback sees
// ---------------------------------------------------------------------------

// The node a slot names, or null if it has been freed since the record was
// queued. Never a raw pointer read: `st->node` is what the registry's
// freed-node observer nulls.
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

Value makeRecordValue(const PendingRecord& rec) {
    ObjectBuilder b;
    b.set("type", ev::fromUtf8(kindName(rec.kind)));
    b.set("target", nodeSlot(rec.target));

    // Real arrays, for the reason hostArrayOf exists: what an app does with
    // addedNodes is iterate it, and a numeric-keyed object has no iterator.
    // One node per record here, which is what the DOM's mutators do one at a
    // time — the web's arrays are longer only because replaceChildren and
    // innerHTML are single operations, and neither exists in this layer.
    HostNodeState* added = rec.added;
    b.set("addedNodes", hostArrayOf(added ? 1 : 0,
                                    [added](size_t) { return nodeSlot(added); }));
    HostNodeState* removed = rec.removed;
    b.set("removedNodes", hostArrayOf(removed ? 1 : 0,
                                      [removed](size_t) { return nodeSlot(removed); }));

    b.set("previousSibling", nodeSlot(rec.previousSibling));
    b.set("nextSibling", nodeSlot(rec.nextSibling));
    b.set("attributeName", rec.attributeName.empty()
                               ? ev::null()
                               : ev::fromUtf8(rec.attributeName));
    // null rather than absent, and null rather than "": an observer that did
    // not ask for the old value and one whose attribute genuinely had none
    // read the same on the web, and code tests `=== null`.
    b.set("attributeNamespace", ev::null());
    b.set("oldValue", rec.hasOldValue ? ev::fromUtf8(rec.oldValue) : ev::null());
    return b.get();
}

Value makeRecordArray(const std::vector<PendingRecord>& records) {
    return hostArrayOf(records.size(),
                       [&records](size_t i) { return makeRecordValue(records[i]); });
}

// ---------------------------------------------------------------------------
// The JS surface
// ---------------------------------------------------------------------------

Value observerObserve(Value thisValue, std::span<const Value> a) {
    MoEntry* entry = observerOf(thisValue);
    if (!entry) {
        return ev::throwTypeError("MutationObserver.observe: receiver is not an observer");
    }
    dom::Node* node = hostNodeOf(argAt(a, 0));
    if (!node) {
        return ev::throwTypeError("MutationObserver.observe: target must be a node");
    }
    // Rooted: every optionOn below interns a key string, and an allocation can
    // move the object a raw Value still points at.
    ev::Persistent options(argAt(a, 1));

    MoTarget t;
    t.node = hostNodeStateFor(node);
    t.subtree = optionOn(options.get(), "subtree");
    t.childList = optionOn(options.get(), "childList");
    t.attributes = optionOn(options.get(), "attributes");
    t.characterData = optionOn(options.get(), "characterData");
    t.attributeOldValue = optionOn(options.get(), "attributeOldValue");
    t.characterDataOldValue = optionOn(options.get(), "characterDataOldValue");
    // The web's shorthand: asking for old values, or for an attribute filter,
    // turns the corresponding kind on without naming it.
    if (t.attributeOldValue) t.attributes = true;
    if (t.characterDataOldValue) t.characterData = true;
    if (!t.childList && !t.attributes && !t.characterData) {
        return ev::throwTypeError(
            "MutationObserver.observe: one of childList, attributes or "
            "characterData must be true");
    }

    // Re-observing the same node REPLACES its options, as the web says, rather
    // than stacking a second registration that would double every record.
    for (MoTarget& existing : entry->targets) {
        if (existing.node == t.node) {
            existing = t;
            ensureHook(node->document());
            return ev::undefined();
        }
    }
    entry->targets.push_back(t);
    ensureHook(node->document());
    return ev::undefined();
}

Value observerDisconnect(Value thisValue, std::span<const Value>) {
    MoEntry* entry = observerOf(thisValue);
    if (!entry) return ev::undefined();
    entry->targets.clear();
    // The web drops the record queue too: a disconnected observer's pending
    // records are gone, not merely undelivered.
    entry->queue.clear();
    return ev::undefined();
}

Value observerTakeRecords(Value thisValue, std::span<const Value>) {
    MoEntry* entry = observerOf(thisValue);
    if (!entry) return hostArrayOf(0, [](size_t) { return ev::null(); });
    // Move out first: makeRecordArray allocates, and an allocation can run a
    // collection, and nothing about that should be able to see a queue that is
    // half-consumed.
    std::vector<PendingRecord> taken;
    taken.swap(entry->queue);
    return makeRecordArray(taken);
}

Value makeObserverValue(Value callback) {
    auto owned = std::make_unique<MoEntry>();
    MoEntry* entry = owned.get();
    entry->callback = ev::Persistent(callback);
    observers().push_back(std::move(owned));

    // The finalizer is deliberately empty: the entry holds Persistents, and
    // ~Persistent is an embed call, which a finalizer may not make. The entry
    // outlives the value on purpose (see the file header).
    ObjectBuilder b(ev::makeHandle(entry, [](void*) {}));
    b.def("observe", 2, observerObserve);
    b.def("disconnect", 0, observerDisconnect);
    b.def("takeRecords", 0, observerTakeRecords);
    Value obj = b.get();
    entry->obj = ev::Persistent(obj);
    return obj;
}

}  // namespace

// ---------------------------------------------------------------------------
// The frame seam
// ---------------------------------------------------------------------------

void deliverHostObservers() {
    if (!g_observers || g_observers->empty()) return;
    // Re-entrancy is not merely guarded, it is the design: a callback that
    // mutates what it observes queues records that are delivered NEXT frame.
    if (g_delivering) return;
    g_delivering = true;

    // By index, and the bound is read fresh every step: a callback is free to
    // construct another MutationObserver, which push_backs onto this very
    // vector and can move it. `count` is fixed up front because an observer
    // created during this pass has no records to deliver anyway.
    const size_t count = g_observers->size();
    for (size_t i = 0; i < count && i < g_observers->size(); ++i) {
        MoEntry* entry = (*g_observers)[i].get();
        if (entry->queue.empty()) continue;
        // Take the batch before calling, so records queued by the callback
        // itself land in a queue that is already empty rather than in the one
        // being iterated.
        std::vector<PendingRecord> batch;
        batch.swap(entry->queue);

        ev::Persistent records(makeRecordArray(batch));
        Value argv[2] = {records.get(), entry->obj.get()};
        ev::CallResult r = ev::call(entry->callback.get(), entry->obj.get(),
                                    std::span<const Value>(argv, 2));
        // Report and keep going: one broken observer must not silence its
        // siblings, the same stance the rAF and listener paths take.
        if (r.thrown) reportBronzeError("MutationObserver", r.value);
    }

    g_delivering = false;
}

void installObserverGlobals() {
    Value ctor = ev::makeFunction(
        [](Value, std::span<const Value> a) {
            Value cb = argAt(a, 0);
            if (!ev::isFunction(cb)) {
                return ev::throwTypeError(
                    "MutationObserver: the argument must be a function");
            }
            return makeObserverValue(cb);
        },
        1);
    ev::registerGlobal("MutationObserver", ctor);
}

}  // namespace bro::bronze_host
