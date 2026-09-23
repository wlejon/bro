// What is left of Document once the four big seams moved out: the document's
// own lifetime (the live-document registry NodeHandle checks against), the
// Selection and live-Range bookkeeping the mutators notify, the single funnel
// every stylesheet reaches the cascade through, the layout pass, and the small
// tree queries — getElementById, querySelector, title, the id map.
//
// The rest is in document_parse.cpp (markup in, nodes out), document_style.cpp
// (the restyle pass), document_gencontent.cpp (::before / ::after) and
// document_nodes.cpp (node ownership, cloning, adoption, deferred free).

#include "dom/document.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "engine/css_transitions.h"
#include "layout/element_ref_adapter.h"
#include "layout/layout_node_adapter.h"
#include "css/parser.h"
#include "css/cascade.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <unordered_set>
#include <vector>

namespace bro::dom {

// Live-document registry backing NodeHandle. Documents are created and
// destroyed on the main thread only (app doc, system panels, HtmlNode inner
// docs); the layout/raster threads never construct or destroy one, so a plain
// set needs no synchronization.
static std::unordered_set<const Document*>& liveDocuments() {
    static std::unordered_set<const Document*> s;
    return s;
}

bool Document::isLiveDocument(const Document* doc) {
    return doc && liveDocuments().count(doc) != 0;
}

Document::Document() {
    liveDocuments().insert(this);
}

// Selection owns a Range whose destructor calls back into unregisterRange().
// Members destroy in reverse declaration order, so if we defaulted this the
// liveRanges_ set would already be gone by the time ~Selection ran. Tear
// selection_ down first, then clear any JS-owned Ranges that outlived us.
Document::~Document() {
    liveDocuments().erase(this);
    // Sever anything still pointing into this document's node storage before
    // that storage goes away.
    for (auto& [n, _] : ownedNodes_) {
        if (n) {
            for (NodeObserver obs : nodeFreedObservers_) obs(this, n);
        }
    }
    // The CSS transition/animation managers outlive individual documents (they
    // are Engine members), and index by raw Element*. Same bypass as above:
    // ownedNodes_ is destroyed without freeNode(), so drop this document's
    // entries here, while the keys are still valid to compare.
    if (transitionManager_) transitionManager_->forgetDocument(this);
    if (animationManager_) animationManager_->forgetDocument(this);
    selection_.reset();
    auto ranges = std::move(liveRanges_);
    for (auto* r : ranges) {
        if (r) r->setDocument(nullptr);
    }
}

// ---------------------------------------------------------------------------
// Selection + live Range registry
// ---------------------------------------------------------------------------

Selection* Document::selection() {
    if (!selection_) selection_ = std::make_unique<Selection>(this);
    return selection_.get();
}

void Document::registerRange(Range* r) {
    if (r) liveRanges_.insert(r);
}

void Document::unregisterRange(Range* r) {
    if (!r) return;
    liveRanges_.erase(r);
    // Range destruction invalidates Selection if this is its backing range.
    if (selection_ && r == selection_->getRangeAt(0)) {
        selection_->removeAllRanges();
    }
}

void Document::notifyNodeRemoved(Node* removed) {
    if (!removed) return;
    if (focusedElement_) {
        bool contains = (focusedElement_ == removed);
        if (!contains) {
            for (Node* p = focusedElement_->parentNode(); p; p = p->parentNode()) {
                if (p == removed) { contains = true; break; }
            }
        }
        if (contains) {
            setActiveElement(nullptr);
        }
    }
    // A top-layer element leaving the tree leaves the top layer with it
    // ("remove an element from the top layer immediately").
    if (!topLayer_.empty()) {
        std::vector<Element*> leaving;
        for (const auto& e : topLayer_) {
            for (const Node* p = e.element; p; p = p->parentNode()) {
                if (p == removed) { leaving.push_back(e.element); break; }
            }
        }
        for (Element* el : leaving) removeFromTopLayer(el);
    }
    Node* parent = removed->parentNode();
    if (!parent) return;
    int idx = -1;
    const auto& kids = parent->childNodes();
    for (size_t i = 0; i < kids.size(); ++i)
        if (kids[i] == removed) { idx = static_cast<int>(i); break; }
    if (idx < 0) return;
    for (auto* r : liveRanges_)
        r->onNodeRemoved(removed, parent, idx);
    if (selection_ && selection_->rangeCount() > 0) {
        selection_->schedulePendingChange();
        selection_->flushPendingChange();
    }
}

void Document::notifyTextDataChanged(Node* node, int offset, int count, int newLen) {
    if (!node) return;
    for (auto* r : liveRanges_)
        r->onTextDataChanged(node, offset, count, newLen);
    if (selection_ && selection_->rangeCount() > 0) {
        selection_->schedulePendingChange();
        selection_->flushPendingChange();
    }
}

void Document::notifyTextSplit(Node* node, int offset, Node* tail) {
    if (!node || !tail) return;
    for (auto* r : liveRanges_)
        r->onTextSplit(node, offset, tail);
}

void Document::notifyChildInserted(Node* parent, int index) {
    if (!parent) return;
    for (auto* r : liveRanges_)
        r->onChildInserted(parent, index);
}

void Document::fireSelectionChange() {
}

// ---------------------------------------------------------------------------
// CSS cascade plumbing
// ---------------------------------------------------------------------------

void Document::addSheetToCascade(htmlayout::css::Stylesheet sheet, void* scope,
                                 htmlayout::css::Origin origin) {
    cascade_.addStylesheet(sheet, scope,
                           hasMediaContext_ ? &mediaContext_ : nullptr, origin);
    retainedSheets_.push_back({std::move(sheet), scope, origin});
}

void Document::setMediaViewport(float w, float h) {
    if (hasMediaContext_ && mediaContext_.viewportWidth == w &&
        mediaContext_.viewportHeight == h) {
        return;
    }
    hasMediaContext_ = true;
    mediaContext_.viewportWidth = w;
    mediaContext_.viewportHeight = h;
    ++mediaGeneration_;
    rebuildCascadeForMediaChange();
}

void Document::setMediaColorScheme(const std::string& scheme) {
    if (hasMediaContext_ && mediaContext_.colorScheme == scheme) return;
    hasMediaContext_ = true;
    mediaContext_.colorScheme = scheme;
    ++mediaGeneration_;
    rebuildCascadeForMediaChange();
}

void Document::rebuildCascadeForMediaChange() {
    if (retainedSheets_.empty()) return;
    // The media context changed after sheets were added: re-evaluate every
    // @media block by rebuilding the cascade from the retained parsed sheets.
    cascade_.clear();
    for (auto& rs : retainedSheets_) {
        cascade_.addStylesheet(rs.sheet, rs.scope, &mediaContext_, rs.origin);
    }
    // The rule set changed but no element was marked dirty (nobody touched
    // them — the @media conditions flipped). Force the next resolveStyles()
    // to re-resolve everything, selector-level, exactly like a new sheet.
    mediaRebuilt_ = true;
    markDirty();
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

// Push this frame's invalidation into the layout tree. Elements that recorded a
// geometry change dirty their own layout node and every ancestor above it, so
// layoutTree() recomputes that chain and hands back the cached geometry of every
// subtree it doesn't reach. A change nobody could pin on an element — a fresh
// tree, a new stylesheet, a bare Document::markDirty() — dirties the whole tree
// instead, which is the unconditional pass bro used to run every frame.
//
// The element walk runs either way: it is what clears the per-element flags, and
// what rebuilds the layout children of any element whose child list moved.
void Document::applyLayoutInvalidation() {
    if (fullLayout_) {
        htmlayout::layout::markSubtreeDirty(layoutRoot_.get());
        fullLayout_ = false;
    }
    perf_.treeRebuilds += layoutRoot_->markDirtyFromElements();
}

void Document::performLayout(float viewportWidth, htmlayout::layout::TextMetrics& metrics) {
    performLayout(viewportWidth, 0.0f, metrics);
}

void Document::performLayout(float viewportWidth, float viewportHeight, htmlayout::layout::TextMetrics& metrics) {
    if (!documentElement_) return;
    using clk = std::chrono::steady_clock;
    const auto ms = [](clk::time_point a, clk::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const uint64_t measures0 = metrics.measureCalls;
    perf_.passes++;

    auto t0 = clk::now();
    if (!layoutRoot_ || rebuildLayoutTree_) {
        layoutRoot_ = layout::LayoutNodeAdapter::buildTree(documentElement_);
        rebuildLayoutTree_ = false;
        perf_.treeRebuilds++;
        fullLayout_ = true;   // a fresh tree has no cached geometry to reuse
    }
    auto t1 = clk::now();
    applyLayoutInvalidation();
    structureDirty_ = false;
    auto t2 = clk::now();
    htmlayout::layout::Viewport vp{viewportWidth, viewportHeight};
    htmlayout::layout::layoutTree(layoutRoot_.get(), vp, metrics);
    auto t3 = clk::now();
    layoutRoot_->syncBoxToElement();
    auto t4 = clk::now();

    perf_.buildMs += ms(t0, t1);
    perf_.invalidateMs += ms(t1, t2);
    perf_.layoutMs += ms(t2, t3);
    perf_.syncMs += ms(t3, t4);
    const auto& ls = htmlayout::layout::lastLayoutStats();
    perf_.nodesLaidOut += ls.laidOut;
    perf_.nodeVisits += ls.visits;
    perf_.nodeRevisitsSkipped += ls.revisitsSkipped;
    perf_.nodesReused += ls.reused;
    perf_.layoutTreeMs += ls.treeMs;
    perf_.layoutAbsMs += ls.absoluteMs;
    perf_.layoutHitMs += ls.hitBoundsMs;
    perf_.measureCalls += metrics.measureCalls - measures0;
    perf_.styleLookups += ls.styleLookups;
    perf_.reuseFailDirty += ls.reuseFailDirty;
    perf_.reuseFailAvailW += ls.reuseFailAvailW;
    perf_.reuseFailAvailH += ls.reuseFailAvailH;
    perf_.reuseFailOverride += ls.reuseFailOverride;

    settleContainerQueries([&] {
        // The re-resolve inside settleContainerQueries marks whatever the
        // container query actually changed; nothing else has to relayout.
        applyLayoutInvalidation();
        htmlayout::layout::layoutTree(layoutRoot_.get(), vp, metrics);
        layoutRoot_->syncBoxToElement();
    });
}

// @container rules match against the container's laid-out size, but styles
// resolve before layout — so the first pass evaluates them against stale (or
// zero) sizes. After layout, re-resolve styles once and re-run layout so
// container-dependent styles see the real container boxes. Single settle pass:
// pathological container cycles don't loop.
void Document::settleContainerQueries(const std::function<void()>& relayout) {
    if (!cascade_.usesContainerQueries() || inContainerSettle_) return;
    inContainerSettle_ = true;
    // Forced re-resolve: the elements aren't dirty (they were just resolved),
    // but @container matching depends on the layout boxes that only now exist.
    layout::ElementRefAdapter::clearCache();
    resolveStylesRecursive(documentElement_, nullptr, /*force=*/true,
                           /*selectorForce=*/true);
    resolveGeneratedContent();
    layout::ElementRefAdapter::clearCache();
    relayout();
    inContainerSettle_ = false;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

// True when `node` hangs off this document's root. Shadow content is excluded
// on purpose: a shadow tree's parent chain stops at its ShadowRoot, so it is
// not part of the document tree and document.getElementById must not see into
// it — ShadowRoot::getElementById is the scoped query for that.
static bool inDocumentTree(const Node* node, const Node* root) {
    for (const Node* n = node; n; n = n->parentNode())
        if (n == root) return true;
    return false;
}

/// The first element in the document with this id, in document order.
///
/// The map is a *cache* of that question and not the answer to it, which is
/// the whole of the design here. It holds every element that has ever claimed
/// the id, because an entry outlives its element leaving the tree (the element
/// may be put back without its id attribute ever being written again, and
/// nothing on the insertion path registers ids) and because two elements can
/// hold one id at the same time — legally, as when a redraw builds its
/// replacement before clearing what it replaces. So a cached candidate is
/// believed only while it still carries the id and is still in the tree, and
/// when no candidate qualifies the tree itself is asked. That fallback is what
/// makes every path that forgets to register self-healing: the worst an
/// unregistered element costs is one walk, once, and then it is cached.
Element* Document::getElementById(const std::string& id) {
    if (id.empty()) return nullptr;

    auto it = idMap_.find(id);
    if (it != idMap_.end()) {
        Element* only = nullptr;
        int live = 0;
        for (Element* elem : it->second) {
            if (elem->id() != id || !inDocumentTree(elem, root_)) continue;
            if (++live > 1) break;
            only = elem;
        }
        // Exactly one element in the tree wears this id: the common case, and
        // the only one the cache can answer on its own. Two or more and it
        // cannot say which comes first in document order — the map has no
        // order — so that falls through to the walk as well.
        if (live == 1) return only;
    }

    std::vector<Element*> all;
    collectElements(root_, all);
    for (Element* elem : all) {
        if (elem->id() != id) continue;
        registerElementId(id, elem);
        return elem;
    }
    return nullptr;
}

// A document's descendants include its root element, which the Element
// search (descendants of the element it is called on) leaves out, so the
// root is tested first — document.querySelector('html') / (':root'). For a
// document, :scope is the root element too.
Element* Document::querySelector(const std::string& selector) {
    if (root_ && root_->nodeType() == NodeType::Element) {
        auto* rootEl = static_cast<Element*>(root_);
        if (rootEl->matchesScoped(selector, rootEl)) return rootEl;
        return rootEl->querySelector(selector);
    }
    return nullptr;
}

std::vector<Element*> Document::querySelectorAll(const std::string& selector) {
    if (root_ && root_->nodeType() == NodeType::Element) {
        auto* rootEl = static_cast<Element*>(root_);
        std::vector<Element*> out;
        if (rootEl->matchesScoped(selector, rootEl)) out.push_back(rootEl);
        auto rest = rootEl->querySelectorAll(selector);
        out.insert(out.end(), rest.begin(), rest.end());
        return out;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Title
// ---------------------------------------------------------------------------

std::string Document::title() const {
    if (!documentElement_) return {};
    std::vector<Element*> allElems;
    const_cast<Document*>(this)->collectElements(root_, allElems);
    for (auto* elem : allElems) {
        if (elem->tagName() == "TITLE") {
            return elem->textContent();
        }
    }
    return {};
}

void Document::setTitle(const std::string& title) {
    if (!documentElement_) return;
    std::vector<Element*> allElems;
    collectElements(root_, allElems);
    for (auto* elem : allElems) {
        if (elem->tagName() == "TITLE") {
            elem->setTextContent(title);
            return;
        }
    }
    for (auto* elem : allElems) {
        if (elem->tagName() == "HEAD") {
            auto* titleElem = createElement("title");
            titleElem->setTextContent(title);
            elem->appendChild(titleElem);
            return;
        }
    }
}

void Document::registerElementId(const std::string& id, Element* elem) {
    if (id.empty() || !elem) return;
    auto& candidates = idMap_[id];
    for (Element* known : candidates)
        if (known == elem) return;
    candidates.push_back(elem);
}

/// Drop one element's claim on an id. Only that element's: an element leaving
/// the tree says nothing about any other element holding the same id, and the
/// erase-by-string this replaced would take the whole entry with it.
void Document::unregisterElementId(const std::string& id, const Element* elem) {
    if (id.empty()) return;
    auto it = idMap_.find(id);
    if (it == idMap_.end()) return;
    auto& candidates = it->second;
    candidates.erase(std::remove(candidates.begin(), candidates.end(), elem),
                     candidates.end());
    if (candidates.empty()) idMap_.erase(it);
}

void Document::collectElements(Node* node, std::vector<Element*>& out) {
    if (!node) return;
    if (node->nodeType() == NodeType::Element) {
        out.push_back(static_cast<Element*>(node));
    }
    for (auto& child : node->childNodes()) {
        collectElements(child, out);
    }
}

// ---------------------------------------------------------------------------
// Shadow DOM CSS
// ---------------------------------------------------------------------------

void Document::addShadowStylesheet(ShadowRoot* sr, const std::string& css) {
    if (!sr || css.empty()) return;
    addSheetToCascade(htmlayout::css::parse(css), static_cast<void*>(sr));
}

} // namespace bro::dom
