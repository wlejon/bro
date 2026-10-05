// The node registry: one HostNodeState per DOM node this layer has named, the
// identity map that makes `el === el`, and the lifetime rules for both halves
// of an entry — the node going away (Document::addNodeFreedObserver) and the
// last JS object that reaches the entry going away (jsRefs, host_node_sweep.h).

#include "bronze_host/host_node_sweep.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_builder.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/node.h"
#include "dom/shadow_root.h"

#include <chrono>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bro::bronze_host {

namespace {

struct Registry {
    // Owning, keyed by the entry's own address: every accessor on a wrapper
    // captures its HostNodeState*, so an entry never moves, and the reaper
    // finds the one it is freeing without a scan.
    std::unordered_map<HostNodeState*, std::unique_ptr<HostNodeState>> entries;
    std::unordered_map<const dom::Node*, HostNodeState*> live;
    // Which documents we have asked to warn us. A set rather than a single
    // bool: the warning is per-document, and DOMParser makes a second document
    // reachable. With a bool, whichever document owned the first node this
    // layer ever wrapped was the only one being watched, and if that was a
    // parsed document the LIVE document's wrappers could outlive their nodes.
    std::unordered_set<const dom::Document*> observed;
    // Entries whose count reached zero, queued by a finalizer (which may only
    // count) or by a node going away, and freed by hostReapNodeStates.
    std::vector<HostNodeState*> reapQueue;
};

Registry& registry() {
    static Registry* r = new Registry();
    return *r;
}

// A doomed node's wrapper must stop answering BEFORE the storage goes away.
//
// The entry leaves the live map — so nothing can reach the dead Element*
// through us again, and an element later allocated at the same address gets a
// fresh entry rather than inheriting this one — and its Persistents are
// released here, a normal call site that may make embed calls. The entry
// itself waits for the reaper: a wrapper the program still holds is a handle
// pointing at it, and that pointer has to stay valid and inert.
void clearNodeState(HostNodeState* st) {
    if (!st) return;
    st->node = nullptr;
    st->el = nullptr;
    st->shadowRoot = nullptr;
    st->jsObj.set(ev::undefined());
    st->styleObj.set(ev::undefined());
    st->classListObj.set(ev::undefined());
    st->computedObj.set(ev::undefined());
    st->datasetObj.set(ev::undefined());
    st->inlineHandles.clear();
    st->inlineFns.clear();
    st->hasStyle = false;
    st->hasClassList = false;
    st->hasComputed = false;
    st->hasDataset = false;
    registry().reapQueue.push_back(st);
}

void onNodeFreed(dom::Document*, dom::Node* node) {
    if (hostFullscreenElement() == node) setHostFullscreenElement(nullptr);
    if (node && node->nodeType() == dom::NodeType::Element) {
        auto* el = static_cast<dom::Element*>(node);
        cleanupCanvasForElement(el);
        clearElementListeners(el);
    }
    Registry& r = registry();
    auto it = r.live.find(node);
    if (it == r.live.end()) return;
    HostNodeState* st = it->second;
    r.live.erase(it);
    clearNodeState(st);
}

// Takes a Node rather than an Element so text nodes, comments and fragments
// land in the SAME map as elements. One registry is what keeps identity a
// property of the node rather than of the kind of node: `parent.childNodes[0]
// === textNode` has to hold for the same reason `=== element` does.
HostNodeState* stateFor(dom::Node* node) {
    if (!node) return nullptr;
    Registry& r = registry();
    auto it = r.live.find(node);
    if (it != r.live.end()) {
        HostNodeState* st = it->second;
        if (!st->sweepGroup) return st;
        // Demoted: bring the group back first, and ask again, since a group
        // found dead leaves this node without an entry.
        hostSweepTouch(st);
        return stateFor(node);
    }
    if (dom::Document* doc = node->document()) {
        if (r.observed.insert(doc).second)
            doc->addNodeFreedObserver(&onNodeFreed);
    }
    auto owned = std::make_unique<HostNodeState>();
    HostNodeState* st = owned.get();
    st->node = node;
    st->el = node->nodeType() == dom::NodeType::Element
                 ? static_cast<dom::Element*>(node) : nullptr;
    if (node->nodeName() == "#shadow-root") {
        st->shadowRoot = static_cast<dom::ShadowRoot*>(node);
    }
    r.entries.emplace(st, std::move(owned));
    r.live.emplace(node, st);
    return st;
}

}  // namespace

void clearHostElementsForDocument(dom::Document* doc) {
    if (!doc) return;
    clearElementListenersForDocument(doc);
    Registry& r = registry();
    r.observed.erase(doc);
    std::vector<HostNodeState*> doomed;
    for (auto it = r.live.begin(); it != r.live.end(); ) {
        HostNodeState* st = it->second;
        if (st && st->node && st->node->document() == doc) {
            if (st->el) cleanupCanvasForElement(st->el);
            doomed.push_back(st);
            it = r.live.erase(it);
        } else {
            ++it;
        }
    }
    for (HostNodeState* st : doomed) clearNodeState(st);
}

HostNodeState* hostNodeStateFor(dom::Node* node) { return stateFor(node); }

bool hostHasNodeState(const dom::Node* node) {
    return node && registry().live.count(node) != 0;
}

HostNodeState* hostNodeStateIfAny(const dom::Node* node) {
    if (!node) return nullptr;
    Registry& r = registry();
    auto it = r.live.find(node);
    return it == r.live.end() ? nullptr : it->second;
}

void hostForgetNodeState(HostNodeState* st) {
    if (!st) return;
    Registry& r = registry();
    if (st->node) {
        auto it = r.live.find(st->node);
        if (it != r.live.end() && it->second == st) r.live.erase(it);
    }
    clearNodeState(st);
}

void retainNodeState(HostNodeState* st) {
    ++st->jsRefs;
}

void releaseNodeStateRef(void* p) {
    auto* st = static_cast<HostNodeState*>(p);
    if (!st || st->jsRefs == 0) return;
    if (--st->jsRefs == 0) registry().reapQueue.push_back(st);
}

ev::HandleDestructor stateHandleDtor(HostNodeState* st) {
    if (st) retainNodeState(st);
    return &releaseNodeStateRef;
}

void hostReapNodeStates(double budgetMs) {
    Registry& r = registry();
    if (r.reapQueue.empty()) return;
    std::vector<HostNodeState*> queue;
    queue.swap(r.reapQueue);
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::microseconds(static_cast<int64_t>(budgetMs * 1000.0));
    for (size_t i = 0; i < queue.size(); ++i) {
        // Out of time: the rest wait for the next call, in order.
        if (budgetMs >= 0.0 && (i & 63) == 63 && std::chrono::steady_clock::now() >= end) {
            r.reapQueue.insert(r.reapQueue.end(), queue.begin() + static_cast<std::ptrdiff_t>(i),
                               queue.end());
            break;
        }
        HostNodeState* st = queue[i];
        auto it = r.entries.find(st);
        if (it == r.entries.end()) continue;
        if (st->node || st->jsRefs != 0 || st->pinned) continue;
        if (hostObserversHold(st)) {
            r.reapQueue.push_back(st);
            continue;
        }
        r.entries.erase(it);
    }
}

std::vector<dom::Document*> hostObservedDocuments() {
    std::vector<dom::Document*> out;
    for (const dom::Document* d : registry().observed) {
        if (dom::Document::isLiveDocument(d)) out.push_back(const_cast<dom::Document*>(d));
    }
    return out;
}

NodeRegistryStats hostNodeRegistryStats() {
    Registry& r = registry();
    NodeRegistryStats s;
    s.entries = r.entries.size();
    s.live = r.live.size();
    for (const auto& [node, st] : r.live) {
        if (!ev::isUndefined(st->jsObj.get())) ++s.wrapped;
    }
    return s;
}

}  // namespace bro::bronze_host
