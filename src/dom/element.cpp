#include "dom/element.h"
#include "dom/document.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "dom/shadow_root.h"
#include "dom/text_node.h"
#include "dom/comment_node.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "layout/el_svg.h"
#include "layout/el_video.h"
#include "layout/element_ref_adapter.h"
#include "css/selector.h"
#include "util/log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace bro::dom {

// ---------------------------------------------------------------------------
// Node implementations
// ---------------------------------------------------------------------------

// Walk up from any node to its owning Document (via the nearest Element).
static Document* findDocument(Node* node) {
    for (Node* n = node; n; n = n->parentNode()) {
        if (n->nodeType() == NodeType::Element)
            return static_cast<Element*>(n)->document();
    }
    return nullptr;
}

// A child list changed under `parent` — tell layout.
//
// This lives in the DOM layer on purpose. Layout invalidation is a consequence
// of the tree changing shape, not of *who* changed it, and leaving it to every
// caller made the raw mutators a trap: a node appended through them never got a
// layout adapter and silently never rendered. Only the JS bindings remembered
// to compensate (js_element_appendChild's markChildListChanged), so engine C++
// hit the trap and patched it locally (input_handling.cpp's caret text node),
// and any host calling Node::appendChild from C++ hit it with no clue why.
//
// What deliberately stays OUT of here, because it is realm-scoped rather than
// tree-scoped and needs a JSContext the DOM layer does not have:
//   - custom-element connectedCallback (runs script)
//   - MutationObserver DELIVERY (per-realm observer lists, JS callbacks)
// Those remain in the JS bindings. What the tree does now report is the plain
// NOTICE that a mutation happened — Document::notifyMutation, fired below —
// because what changed is a property of the tree and not of who changed it;
// document.h has the argument.
//
// Attributed to the parent element where there is one: Element::
// markStructureDirty rebuilds that node's children and keeps every other
// subtree's cached geometry, where the document-wide form throws the whole
// layout tree away (~150ms on a few-thousand-element document). Idempotent —
// it sets flags — so a caller that also marks explicitly costs nothing.
static void notifyChildListChanged(Node* parent) {
    if (!parent) return;
    if (parent->nodeType() == NodeType::Element) {
        auto* el = static_cast<Element*>(parent);
        el->markStructureDirty();
        // A SHADOW HOST's child list is its shadow tree's slot input: the
        // light-DOM children are what `distributeSlots` hands to the <slot>
        // elements, so adding or removing one leaves the assignment cache
        // stale. A component that appends into itself after attaching its
        // shadow root — the ordinary build order — otherwise sees the new
        // child rendered nowhere and `assignedSlot` answer null forever.
        if (el->hasShadow()) {
            if (ShadowRoot* sr = el->shadowRoot()) sr->invalidateSlots();
        }
        return;
    }
    // A ShadowRoot's child list is a SCOPE as well as a tree. Two things have
    // to happen that no other parent owes: the slot assignment cache is stale
    // (a <slot> may have appeared, moved or left), and a <style> that just
    // arrived carries rules nothing has put in the cascade yet. Both used to
    // live in the JS binding's own appendChild/insertBefore/removeChild, so a
    // shadow tree built any other way — bronze-compiled code calling
    // Node::appendChild, ShadowRoot::setInnerHTML, engine C++ — got neither,
    // and shadow CSS simply never reached the cascade.
    if (auto* sr = dynamic_cast<ShadowRoot*>(parent)) {
        sr->invalidateSlots();
        Document* doc = sr->document();
        if (!doc && sr->host()) doc = sr->host()->document();
        sr->registerStyleElements(doc);
        if (sr->host()) {
            sr->host()->markDirty();
            sr->host()->markStructureDirty();
        }
        if (doc) doc->markStructureDirty();
        return;
    }
    if (auto* doc = findDocument(parent)) {
        doc->markStructureDirty();
    }
}

// A childList notice, if anyone is listening. `added` and `removed` are what
// this one mutation did; the siblings are read from the tree as it stands,
// which is why the removal path calls this BEFORE it detaches.
static void notifyChildListMutation(Node* parent, Node* added, Node* removed,
                                    Node* prev, Node* next) {
    Document* doc = findDocument(parent);
    if (!doc || !doc->hasMutationObservers()) return;
    Document::MutationNotice notice;
    notice.kind = Document::MutationNotice::Kind::ChildList;
    notice.target = parent;
    notice.added = added;
    notice.removed = removed;
    notice.previousSibling = prev;
    notice.nextSibling = next;
    doc->notifyMutation(notice);
}

// DOM "pre-insert" step 2: a node entering a tree that belongs to a different
// document is adopted into it first.
//
// This is not a notice and could not be one — it is an ACTION that has to
// happen before the tree changes, which is why it sat in the JS bindings under
// the heading of things that "genuinely need a JS realm". It never needed one.
// A document pointer comparison and a call to Document::adoptNode name no
// context and no realm; what actually kept it up there was that the JS
// bindings were the only inserter anyone had written.
//
// They are not any more. A bronze-compiled program appends through
// Node::appendChild directly, and without this its DOMParser nodes stayed
// owned by the parser document while living in the live tree — rendering
// correctly right up until that document was destroyed and took them with it.
// Leaving the step with the callers means every future inserter has to know to
// perform it, and the failure when it doesn't is a use-after-free that looks
// like a rendering bug.
//
// Cost when it does not apply is one pointer comparison, which is what a node
// created by the document it is being appended to always is.
static void adoptIntoParentDocument(Node* parent, Node* node) {
    if (!parent || !node) return;
    // The parent's own document first, and the walk only as a fallback: a
    // DocumentFragment and a TextNode are both legal parents here and neither
    // is an Element, which is all findDocument knows how to read.
    Document* target = parent->document();
    if (!target) target = findDocument(parent);
    // No document to adopt INTO — an offscreen subtree being assembled before
    // it is attached. Insertion into a real tree adopts the whole thing later.
    if (!target || node->document() == target) return;
    // adoptNode detaches from the current parent, which is the same thing the
    // caller below is about to do; doing it here just means it happens once.
    target->adoptNode(node);
}

void Node::appendChild(Node* child) {
    if (!child) return;
    adoptIntoParentDocument(this, child);
    if (child->parent_) {
        child->parent_->removeChild(child);
    }
    Node* prev = children_.empty() ? nullptr : children_.back();
    child->parent_ = this;
    children_.push_back(child);
    notifyChildListChanged(this);
    notifyChildListMutation(this, child, nullptr, prev, nullptr);
}

void Node::removeChild(Node* child) {
    if (!child) return;
    auto it = std::find(children_.begin(), children_.end(), child);
    if (it != children_.end()) {
        // Notify live ranges BEFORE detaching so they can still see the
        // child's former parent/index.
        if (auto* doc = findDocument(this)) {
            doc->notifyNodeRemoved(child);
        }
        // Same reason, one step further: the record names the siblings the
        // node HAD, and a moment from now it has none.
        Node* prev = it == children_.begin() ? nullptr : *(it - 1);
        Node* next = (it + 1) == children_.end() ? nullptr : *(it + 1);
        notifyChildListMutation(this, nullptr, child, prev, next);
        (*it)->parent_ = nullptr;
        children_.erase(it);
        notifyChildListChanged(this);
    }
}

void Node::releaseChildren() {
    for (auto* child : children_) child->setParent(nullptr);
    children_.clear();
}

void Node::insertBefore(Node* newChild, Node* refChild) {
    if (!newChild) return;
    if (!refChild) {
        appendChild(newChild);  // invalidates, adopts, and fires its own notice
        return;
    }
    adoptIntoParentDocument(this, newChild);
    if (newChild->parent_) {
        newChild->parent_->removeChild(newChild);
    }
    auto it = std::find(children_.begin(), children_.end(), refChild);
    if (it != children_.end()) {
        Node* prev = it == children_.begin() ? nullptr : *(it - 1);
        newChild->parent_ = this;
        children_.insert(it, newChild);
        notifyChildListChanged(this);
        notifyChildListMutation(this, newChild, nullptr, prev, refChild);
    }
}

// ---------------------------------------------------------------------------
// Element implementations
// ---------------------------------------------------------------------------

#if BRO_DOM_FREED_ELEMENT_GUARD
// See element.h. Function-local static so it outlives every Element (the DOM is
// torn down long before static destruction ordering matters here).
static std::unordered_set<const void*>& freedElements() {
    static std::unordered_set<const void*> s;
    return s;
}

bool wasElementFreed(const void* p) {
    return freedElements().count(p) != 0;
}

void reportFreedElementAccess(const void* p, const char* what) {
    LOG_ERROR("USE-AFTER-FREE: %s dereferenced Element storage at %p that "
              "~Element already destroyed. isAlive()/magic_ cannot catch this "
              "reliably — the probe is itself the use-after-free.",
              what, p);
    std::fflush(nullptr);
    // Not abort(): avoid the Windows error-reporting dialog, which would hang
    // an unattended test run instead of failing it.
    std::_Exit(70);
}
#endif

Element::~Element() {
    // Tell the backing CanvasScene (if any) that its Element is gone, so it
    // drops the layout/detached callbacks that aim at this object before the
    // memory is reclaimed. Without this, a scene whose Element is destroyed via
    // the deferred-free path (drainPendingFrees) — rather than the JS GC
    // finalizer — would dereference freed memory on the next frame's
    // prepareAndSignal()/checkDetached(). The engine clears canvasScene_ when it
    // reclaims a scene first, so this never calls into a freed scene.
    if (canvasScene_ && canvasSceneOnDestroy_) {
        canvasSceneOnDestroy_(canvasScene_);
    }
    magic_ = 0xDEAD;
#if BRO_DOM_FREED_ELEMENT_GUARD
    freedElements().insert(this);
#endif
}

std::string Element::resolveUrl(const std::string& src) const {
    if (src.size() >= 2 && src[1] == ':') return src;                 // Windows absolute
    if (!src.empty() && (src[0] == '/' || src[0] == '\\')) return src;
    if (!document_) return src;
    const std::string& base = document_->basePath();
    if (base.empty()) return src;
    std::string path = base;
    if (path.back() != '/' && path.back() != '\\') path += '/';
    return path + src;
}

Element::Element(const std::string& tag)
    : tag_(tag)
    , style_(this)
{
#if BRO_DOM_FREED_ELEMENT_GUARD
    // This address is an Element again — retire any stale freed-record for it,
    // so recycled storage never produces a false positive.
    freedElements().erase(this);
#endif
    for (auto& c : tag_) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
}

const std::string& Element::id() const {
    return getAttribute("id");
}

void Element::setId(const std::string& val) {
    setAttribute("id", val);
}

const std::string& Element::className() const {
    return getAttribute("class");
}

void Element::setClassName(const std::string& val) {
    setAttribute("class", val);
}

const std::string& Element::getAttribute(const std::string& name) const {
    if (name == "style") return style_.cssText();
    auto it = attributes_.find(name);
    if (it != attributes_.end()) {
        return it->second;
    }
    static const std::string kEmpty;
    return kEmpty;
}

bool Element::hasAttribute(const std::string& name) const {
    // True if the attribute was explicitly set (even to an now-empty
    // declaration block) or if .style has live properties set directly
    // (which implicitly creates the attribute in real DOM too).
    if (name == "style") return hasStyleAttr_ || !style_.empty();
    return attributes_.find(name) != attributes_.end();
}

// Fires an Attributes notice when it goes out of scope. setAttribute has three
// exits and this way it has one notice; the old value is COPIED at
// construction, because the write that follows can move the map's storage out
// from under a reference into it.
//
// Inert unless the document has observers, which is what keeps the copy out of
// a hot path — setAttribute runs on every class toggle in every frame.
namespace {
struct AttributeNoticeGuard {
    Document* doc = nullptr;
    Element* el = nullptr;
    const std::string* name = nullptr;
    std::string oldValue;
    bool hadValue = false;

    AttributeNoticeGuard(Element* element, const std::string& attr,
                         const std::string* previous)
        : el(element), name(&attr) {
        Document* d = element->document();
        if (!d || !d->hasMutationObservers()) return;
        doc = d;
        if (previous) {
            oldValue = *previous;
            hadValue = true;
        }
    }
    ~AttributeNoticeGuard() {
        if (!doc) return;
        Document::MutationNotice notice;
        notice.kind = Document::MutationNotice::Kind::Attributes;
        notice.target = el;
        notice.attributeName = name;
        notice.oldValue = hadValue ? &oldValue : nullptr;
        doc->notifyMutation(notice);
    }
};
}  // namespace

void Element::setAttribute(const std::string& name, const std::string& val) {
    if (name == "style") {
        // The "style" attribute and the .style CSSOM object (StyleProxy) are
        // the same underlying declaration block in real DOM — route through
        // StyleProxy instead of stashing a second, independent copy in
        // attributes_. Two copies naively concatenated for cascade
        // resolution (the old approach) can't express "a property was
        // removed via .style" — the attribute's original text for that
        // property would linger forever. setCssText() replaces the whole
        // block, matching setAttribute('style', ...) semantics.
        style_.setCssText(val);
        hasStyleAttr_ = true;
        return;
    }

    auto existing = attributes_.find(name);
    if (existing != attributes_.end() && existing->second == val) return;

    // setAttribute has three exits from here on (the class branch, the src
    // branch's fallthrough, the plain one), so the notice is fired by a guard
    // on scope exit rather than written out three times. Inert unless someone
    // is observing — which is also what keeps the old-value copy out of the
    // hot path.
    AttributeNoticeGuard notice(this, name,
                                existing != attributes_.end() ? &existing->second
                                                              : nullptr);

    if (name == "id" && document_) {
        std::string oldId = getAttribute("id");
        if (!oldId.empty()) document_->unregisterElementId(oldId, this);
        if (!val.empty()) document_->registerElementId(val, this);
    }

    // "class" and "id" feed the selector match and nothing else, so — like an
    // inline-style write — they are style *inputs*: whether geometry moves is
    // only knowable once the cascade re-runs, and resolveStyles() promotes to a
    // layout then. A class toggle that swaps a colour must not reflow the
    // element's subtree. Every other attribute can change content or intrinsic
    // size without any computed-style diff to detect it (img@src, input@value,
    // td@colspan), so those keep the unconditional layout mark.
    if (name == "class") {
        // And a class only re-matches this element's DESCENDANTS if some rule
        // names it in an ancestor position (`.dark .btn`). The common toggle —
        // `row.classList.toggle('on')`, styled by `.row.on` — cannot, so it
        // restyles one element instead of the subtree under it.
        const std::string oldClass =
            existing != attributes_.end() ? existing->second : std::string{};
        attributes_[name] = val;
        if (document_ && !document_->classChangeAffectsDescendants(oldClass, val))
            markStyleDirty();
        else
            markPaintDirty();
        return;
    }

    attributes_[name] = val;
    if (name == "id") {
        markPaintDirty();
    } else if (name == "src" && (tag_ == "IMG" || tag_ == "img")) {
        // A new src means a new intrinsic size, and the size is only recovered
        // by re-probing the file — which happens in the structure pass
        // (engine::ensureReplacedElements). A plain layout mark would relayout
        // the old dimensions and leave the image sized for its predecessor, so
        // this is one of the few attribute writes that genuinely restructures.
        markStructureDirty();
    } else if (name == "src" && (tag_ == "VIDEO" || tag_ == "video")) {
        // `video.setAttribute('src', url)` is the same instruction as
        // `video.src = url` and has to start the same load. When the control
        // does not exist yet the attribute alone suffices —
        // engine::ensureReplacedElements reads it when it builds the control —
        // but once there IS a control, only telling it loads anything, and
        // without this the element kept showing the previous file.
        if (videoControl_ && !val.empty()) videoControl_->load(val);
        markStructureDirty();
    } else {
        markDirty();
    }
}

void Element::removeAttribute(const std::string& name) {
    if (name == "style") {
        style_.setCssText("");
        hasStyleAttr_ = false;
        return;
    }
    if (name == "id" && document_) {
        std::string oldId = getAttribute("id");
        if (!oldId.empty()) document_->unregisterElementId(oldId, this);
    }
    {
        auto existing = attributes_.find(name);
        AttributeNoticeGuard notice(this, name,
                                    existing != attributes_.end() ? &existing->second
                                                                  : nullptr);
        attributes_.erase(name);
    }
    if (name == "class" || name == "id") markPaintDirty();  // see setAttribute
    else markDirty();
}

// textContent / innerHTML / outerHTML and the SVG serializer live in
// element_serialize.cpp — the string half of an Element, split out to keep
// this file inside the repo's per-file size limit.

void Element::addJsListener(const std::string& type) {
    ++jsListenerCounts_[type];
}

void Element::removeJsListener(const std::string& type) {
    auto it = jsListenerCounts_.find(type);
    if (it == jsListenerCounts_.end()) return;
    // Saturating, not wrapping. The count is kept in step by two independent
    // callers (the removeEventListener binding and dispatch's `once` reap), and
    // a listener that removes another listener mid-dispatch can make them
    // overlap; the gate erring towards "no listener of this type" the moment
    // there are none left is the safe direction, and underflowing a uint32 to
    // four billion would pin the slow dispatch path on forever.
    if (--it->second == 0) jsListenerCounts_.erase(it);
}

bool Element::hasJsListener(const std::string& type) const {
    return jsListenerCounts_.find(type) != jsListenerCounts_.end();
}

ListenerHandle Element::addEventListener(const std::string& type, EventCallback cb,
                                         ListenerOptions opts) {
    if (!cb) return ListenerHandle{};
    if (!nativeListeners_) nativeListeners_ = std::make_unique<NativeListenerList>();
    return nativeListeners_->add(type, std::move(cb), opts);
}

bool Element::removeEventListener(ListenerHandle handle) {
    if (!nativeListeners_) return false;
    return nativeListeners_->remove(handle);
}

std::vector<Element*> Element::children() const {
    std::vector<Element*> result;
    for (const auto& child : children_) {
        if (child->nodeType() == NodeType::Element) {
            result.push_back(static_cast<Element*>(child));
        }
    }
    return result;
}

Element* Node::parentElement() const {
    if (parent_ && parent_->nodeType() == NodeType::Element) {
        return static_cast<Element*>(parent_);
    }
    return nullptr;
}

Element* Element::parentElement() const {
    return Node::parentElement();
}

std::vector<Node*> Element::composedChildNodes() const {
    std::vector<Node*> result;
    std::vector<Node*> childList;
    if (hasShadow()) {
        auto* sr = shadowRoot();
        if (!sr->slotsValid()) sr->distributeSlots();
        childList = sr->composedChildren();
    } else {
        childList = childNodes();
    }
    auto* enclosingSR = containingShadowRoot();
    for (auto* child : childList) {
        if (enclosingSR && child->nodeType() == NodeType::Element) {
            auto* ce = static_cast<Element*>(child);
            if (ce->tagName() == "SLOT") {
                auto assigned = enclosingSR->assignedNodes(ce);
                auto& nodes = assigned.empty() ? ce->childNodes() : assigned;
                for (auto* n : nodes)
                    result.push_back(n);
                continue;
            }
        }
        result.push_back(child);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Selector matching (powered by htmlayout)
// ---------------------------------------------------------------------------

// Parsing a selector string into the compiled Selector list is not free — a
// tokenize + parse per call — and querySelector/matches/closest are hammered in
// tight loops with the same handful of literal selectors. Cache the compiled
// result keyed by the source text. The parsed Selectors are pure value data
// (no element pointers), so the cache is valid for the process; it is bounded so
// a program generating unique selectors can't grow it without limit.
static const std::vector<htmlayout::css::Selector>&
cachedParseSelectorList(const std::string& selector) {
    static thread_local std::unordered_map<std::string,
                                            std::vector<htmlayout::css::Selector>> cache;
    auto it = cache.find(selector);
    if (it != cache.end()) return it->second;
    if (cache.size() >= 1024) cache.clear();   // bound for dynamically-built selectors
    auto res = cache.emplace(selector, htmlayout::css::parseSelectorList(selector));
    return res.first->second;
}

std::vector<Element*> Element::querySelectorAll(const std::string& selector) {
    std::vector<Element*> result;
    const auto& selectors = cachedParseSelectorList(selector);

    std::function<void(Element*)> search = [&](Element* elem) {
        for (auto* child : elem->children()) {
            auto* adapter = layout::ElementRefAdapter::getOrCreate(child);
            for (auto& sel : selectors) {
                if (sel.matches(*adapter)) {
                    result.push_back(child);
                    break;
                }
            }
            search(child);
        }
    };
    search(this);
    layout::ElementRefAdapter::clearCache();
    return result;
}

Element* Element::querySelector(const std::string& selector) {
    const auto& selectors = cachedParseSelectorList(selector);

    std::function<Element*(Element*)> search = [&](Element* elem) -> Element* {
        for (auto* child : elem->children()) {
            auto* adapter = layout::ElementRefAdapter::getOrCreate(child);
            for (auto& sel : selectors) {
                if (sel.matches(*adapter)) {
                    layout::ElementRefAdapter::clearCache();
                    return child;
                }
            }
            auto* found = search(child);
            if (found) return found;
        }
        return nullptr;
    };
    auto* result = search(this);
    layout::ElementRefAdapter::clearCache();
    return result;
}

bool Element::matches(const std::string& selector) const {
    const auto& selectors = cachedParseSelectorList(selector);
    auto* adapter = layout::ElementRefAdapter::getOrCreate(const_cast<Element*>(this));
    bool matched = false;
    for (auto& sel : selectors) {
        if (sel.matches(*adapter)) {
            matched = true;
            break;
        }
    }
    layout::ElementRefAdapter::clearCache();
    return matched;
}

Element* Element::closest(const std::string& selector) {
    Element* current = this;
    while (current) {
        if (current->matches(selector)) return current;
        current = current->parentElement();
    }
    return nullptr;
}

// Simple CSS selector matching for dynamically created elements.
// Supports: tag, .class, #id, tag.class, .class1.class2, tag#id
bool Element::matchesSimple(const std::string& selector) const {
    if (selector.empty()) return false;
    if (selector[0] == ':') return false;

    std::string reqTag, reqId;
    std::vector<std::string> reqClasses;

    std::string current;
    enum Part { TAG, CLASS, ID } part = TAG;

    for (size_t i = 0; i <= selector.size(); ++i) {
        char c = (i < selector.size()) ? selector[i] : '\0';
        if (c == '.' || c == '#' || c == '\0') {
            if (!current.empty()) {
                if (part == TAG) reqTag = current;
                else if (part == CLASS) reqClasses.push_back(current);
                else if (part == ID) reqId = current;
            }
            current.clear();
            if (c == '.') part = CLASS;
            else if (c == '#') part = ID;
        } else {
            current += c;
        }
    }

    if (!reqTag.empty()) {
        std::string upper = reqTag;
        for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (tag_ != upper) return false;
    }

    if (!reqId.empty() && getAttribute("id") != reqId) return false;

    if (!reqClasses.empty()) {
        const std::string& cls = getAttribute("class");   // no copy
        auto isSpace = [](char c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
        };
        // True if `want` appears as a whitespace-delimited token in `cls`.
        auto hasClass = [&](const std::string& want) {
            size_t i = 0, n = cls.size();
            while (i < n) {
                while (i < n && isSpace(cls[i])) ++i;
                size_t start = i;
                while (i < n && !isSpace(cls[i])) ++i;
                if (i - start == want.size() && cls.compare(start, i - start, want) == 0)
                    return true;
            }
            return false;
        };
        for (auto& rc : reqClasses) {
            if (!hasClass(rc)) return false;
        }
    }
    return true;
}

void Element::querySelectorAllSimple(const std::string& selector, std::vector<Element*>& out) {
    std::string simpleSelector = selector;
    while (!simpleSelector.empty() && simpleSelector.front() == ' ') simpleSelector.erase(0, 1);
    while (!simpleSelector.empty() && simpleSelector.back() == ' ') simpleSelector.pop_back();
    auto spacePos = simpleSelector.rfind(' ');
    if (spacePos != std::string::npos) simpleSelector = simpleSelector.substr(spacePos + 1);
    auto gtPos = simpleSelector.rfind('>');
    if (gtPos != std::string::npos) {
        simpleSelector = simpleSelector.substr(gtPos + 1);
        while (!simpleSelector.empty() && simpleSelector.front() == ' ') simpleSelector.erase(0, 1);
    }

    for (auto& child : children_) {
        if (child->nodeType() == NodeType::Element) {
            auto* elem = static_cast<Element*>(child);
            if (elem->matchesSimple(simpleSelector)) {
                out.push_back(elem);
            }
            elem->querySelectorAllSimple(selector, out);
        }
    }
}

Element* Element::querySelectorSimple(const std::string& selector) {
    std::vector<Element*> results;
    querySelectorAllSimple(selector, results);
    return results.empty() ? nullptr : results[0];
}

void Element::setScrollToBottom(bool v) {
    scrollToBottom_ = v;
    if (document_) {
        if (v) document_->addScrollToBottomElement(this);
        else   document_->removeScrollToBottomElement(this);
    }
}

void Element::markDirty() {
    dirty_ = true;
    layoutDirty_ = true;
    selectorDirty_ = true;
    if (document_) {
        // Attributed: the document knows *which* element changed, so the layout
        // pass can recompute this element's chain and reuse the rest of the
        // tree. Document::markDirty() — the same call without an element — has
        // to relayout everything instead.
        document_->markElementDirty();
    }
}

void Element::markPaintDirty() {
    // Element still re-resolves (dirty_), but the document is only paint-dirtied
    // so the layout thread can skip the full layoutTree() pass unless the
    // re-resolve turns up an actual geometry change (promoteLayoutDirty).
    dirty_ = true;
    selectorDirty_ = true;   // class / id — descendant rules may re-match
    if (document_) {
        document_->markPaintDirty();
    }
}

void Element::markHoverScopeDirty() {
    // Not dirty_ itself: the pointer entering a descendant does not change this
    // element's own :hover (it was hovered before and after). It only opens the
    // subtree to the scoped hover re-match — see takeHoverScopeDirty.
    hoverScopeDirty_ = true;
    if (document_) {
        document_->markPaintDirty();
    }
}

void Element::markStyleDirty() {
    // Inline style is not a selector input, so selectorDirty_ stays put: no
    // descendant's rule set can change because of this write, and only the
    // inherited values this element hands down can reach them.
    dirty_ = true;
    if (document_) {
        document_->markPaintDirty();
    }
}

// Layout does not descend into every element. A <select>/<textarea>/<iframe>
// owns its children (the control reads them straight from the DOM), and an <svg>
// subtree is painted straight from the DOM by layout/svg_paint — none of them
// have layout nodes, so a structural mark left on one would never be consumed
// and the change would silently never reach the screen. The element layout
// stops at does have a node, and re-laying it out is what re-measures the
// control, so redirect there.
static bool layoutOwnsChildren(const Element* e) {
    std::string_view tag = e->tagName();
    return tag != "SELECT" && tag != "TEXTAREA" && tag != "IFRAME" && tag != "SVG";
}

void Element::markStructureDirty() {
    Element* target = this;
    // A <slot> is replaced by the nodes assigned to it, so it has no layout node
    // of its own. Its parent's children are what get rebuilt.
    if (tagName() == "SLOT") {
        if (Element* p = parentElement()) target = p;
        else if (ShadowRoot* sr = containingShadowRoot()) target = sr->host();
    }
    if (!target) {
        // Nowhere to pin it — fall back to rebuilding the whole tree, which is
        // slow but always right.
        if (document_) document_->markStructureDirty();
        return;
    }
    for (Element* p = target->parentElement(); p; p = p->parentElement())
        if (!layoutOwnsChildren(p)) target = p;

    target->dirty_ = true;
    target->structureDirty_ = true;
    // The child list is a selector input: inserting an <li> changes which
    // sibling matches :last-child, and :nth-child renumbers from the insertion
    // point on. So the subtree re-matches, exactly as a class change does.
    target->selectorDirty_ = true;
    if (document_) document_->markElementStructureDirty();
}

ShadowRoot* Element::containingShadowRoot() const {
    for (auto* p = parent_; p; p = p->parentNode()) {
        if (p->nodeType() == NodeType::DocumentFragment) {
            auto* sr = dynamic_cast<ShadowRoot*>(p);
            if (sr) return sr;
        }
    }
    return nullptr;
}

Element* Element::layoutParent() const {
    if (!parent_) return nullptr;

    // If parent is a ShadowRoot, layout parent is the host element
    if (parent_->nodeType() == NodeType::DocumentFragment) {
        auto* sr = dynamic_cast<ShadowRoot*>(parent_);
        return sr ? sr->host() : nullptr;
    }

    if (parent_->nodeType() != NodeType::Element) return nullptr;
    auto* parentElem = static_cast<Element*>(parent_);

    // If the parent element has a shadow DOM, this element was distributed
    // to a <slot> in that shadow tree. The layout parent is the element
    // containing the <slot>, not the host element itself. This matches the
    // composed tree that the layout engine and draw traversal walk.
    if (parentElem->hasShadow()) {
        auto* sr = parentElem->shadowRoot();
        if (sr) {
            auto* slot = sr->assignedSlot(const_cast<Element*>(this));
            if (slot) {
                auto* slotParent = slot->parentNode();
                if (slotParent && slotParent->nodeType() == NodeType::Element) {
                    return static_cast<Element*>(slotParent);
                }
                // Slot is a direct child of the shadow root — layout parent
                // is the host element
                return parentElem;
            }
        }
    }

    return parentElem;
}

ShadowRoot* Element::attachShadow(ShadowRoot::Mode mode) {
    if (shadowRoot_) return nullptr;
    if (!document_) return nullptr;
    shadowRoot_ = document_->allocateShadowRoot(this, mode);
    return shadowRoot_;
}

// A replaced-element control is what gives its element an intrinsic size:
// LayoutNodeAdapter::intrinsicSize() answers `false` for an <input>/<select>/
// <textarea>/<svg>/<video> that has none, so layout treats it as an ordinary
// empty box — a text field collapses to its padding and border, losing the
// whole line of content height, and the geometry gets cached that way.
//
// The controls are created lazily, by engine::ensureReplacedElements on the
// structure pass, and normally that runs before the first layout. It does not
// have to: anything that lays the document out earlier — bro.menu.show() at
// startup, which resizes the content area and relayouts, is the case that
// found this — leaves clean, control-less boxes behind, and nothing else marks
// these elements when their control finally arrives. So the arrival is the
// mark: layout-dirty here, and every later pass re-measures the control.
//
// `rebuildChildren` additionally rebuilds the element's layout children. Only
// <svg> needs it: LayoutNodeAdapter::buildChildren decides whether to descend
// by asking for the *control* (unlike <select>/<textarea>, which it recognises
// by tag), so a tree built before the ElSvg existed laid the SVG's children
// out as CSS boxes, and dirtying the geometry alone would keep them.
void Element::noteReplacedControlInstalled(bool rebuildChildren) {
    markDirty();
    if (rebuildChildren) markStructureDirty();
}

void Element::setInputControl(std::unique_ptr<layout::ElInput> ctrl) {
    inputControl_ = std::move(ctrl);
    if (inputControl_) noteReplacedControlInstalled();
}

void Element::setTextareaControl(std::unique_ptr<layout::ElTextarea> ctrl) {
    textareaControl_ = std::move(ctrl);
    if (textareaControl_) noteReplacedControlInstalled();
}

void Element::setSelectControl(std::unique_ptr<layout::ElSelect> ctrl) {
    selectControl_ = std::move(ctrl);
    if (selectControl_) noteReplacedControlInstalled();
}

void Element::setSvgControl(std::unique_ptr<layout::ElSvg> ctrl) {
    svgControl_ = std::move(ctrl);
    if (svgControl_) noteReplacedControlInstalled(/*rebuildChildren=*/true);
}

void Element::setVideoControl(std::unique_ptr<layout::ElVideo> ctrl) {
    videoControl_ = std::move(ctrl);
    if (videoControl_) noteReplacedControlInstalled();
}

} // namespace bro::dom
