#include "dom/document.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "engine/css_transitions.h"
#include "engine/web_animations.h"
#include "layout/element_ref_adapter.h"
#include "layout/layout_node_adapter.h"
#include "css/parser.h"
#include "css/cascade.h"
#include <gumbo.h>
#include <sstream>
#include <functional>
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <chrono>
#include <unordered_set>

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
    // that storage goes away. Destroying ownedNodes_/pendingFrees_ below runs
    // ~Element on every node without going through freeNode(), so the per-node
    // NodeFreedCallback never fires for them — a JS wrapper that outlives the
    // document would keep a raw Element* to freed memory and dereference it
    // when it is finally collected.
    //
    // This is not hypothetical: teardown order for sub-documents, system panels
    // and app reload is cleanup(ctx) -> document.reset() -> JS_FreeContext(ctx),
    // so the wrappers are ALWAYS finalized after their Elements are gone.
    if (nodeDestroyingCb_) {
        forEachLiveElement([this](Element* el) { nodeDestroyingCb_(this, el); });
    }
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
    if (selectionChangeCb_) selectionChangeCb_(this);
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
// Parsing with gumbo
// ---------------------------------------------------------------------------

void Document::parse(const std::string& html, const std::string& authorCss,
                     const std::string& uaCss) {
    // Clear any existing tree
    root_ = nullptr;
    documentElement_ = nullptr;
    body_ = nullptr;
    focusedElement_ = nullptr;   // ownedNodes_.clear() bypasses freeNode's scrub
    idMap_.clear();
    // LayoutRoot points into ownedNodes_ via raw Element*; clear it first so
    // we don't hold dangling pointers when ownedNodes_ drops its unique_ptrs.
    layoutRoot_.reset();
    // Any outstanding Range (including the Selection's backing range) points
    // into the tree we're about to destroy. ownedNodes_.clear() bypasses
    // freeNode, so onNodeDestroyed never fires — null endpoints first.
    if (selection_) selection_->removeAllRanges();
    for (auto* r : liveRanges_) {
        if (r) {
            r->onNodeDestroyed(r->startContainer());
            r->onNodeDestroyed(r->endContainer());
        }
    }
    pendingFrees_.clear();
    pendingSet_.clear();
    ownedNodes_.clear();
    cascade_.clear();
    retainedSheets_.clear();

    // Add UA default styles (lowest priority — author styles always win)
    if (!uaCss.empty()) {
        addSheetToCascade(htmlayout::css::parse(uaCss), nullptr,
                          htmlayout::css::Origin::UserAgent);
    }

    // Add author CSS (app stylesheets)
    if (!authorCss.empty()) {
        addSheetToCascade(htmlayout::css::parse(authorCss));
    }

    // Parse HTML with gumbo
    GumboOutput* output = gumbo_parse(html.c_str());
    if (!output) return;

    // Find the <html> element in gumbo's tree
    GumboNode* htmlNode = output->root;
    if (htmlNode && htmlNode->type == GUMBO_NODE_ELEMENT) {
        const char* rootTag = gumbo_normalized_tagname(htmlNode->v.element.tag);
        auto* rootElem = allocateNode<Element>(rootTag && rootTag[0] ? rootTag : "html");
        rootElem->setDocument(this);

        // Copy attributes from gumbo root
        GumboVector* attrs = &htmlNode->v.element.attributes;
        for (unsigned int i = 0; i < attrs->length; ++i) {
            auto* attr = static_cast<GumboAttribute*>(attrs->data[i]);
            rootElem->setAttribute(attr->name, attr->value ? attr->value : "");
        }

        root_ = rootElem;
        documentElement_ = rootElem;

        // Build children recursively
        buildTreeFromGumbo(htmlNode, rootElem);
    }

    // Find <body> and extract <style> elements
    if (documentElement_) {
        std::vector<Element*> allElems;
        collectElements(root_, allElems);

        for (auto* elem : allElems) {
            // Find body
            if (!body_ && elem->tagName() == "BODY") {
                body_ = elem;
            }

            // Register IDs
            std::string elemId = elem->id();
            if (!elemId.empty()) {
                idMap_[elemId] = elem;
            }

            // Extract <style> elements and add their CSS to the cascade
            if (elem->tagName() == "STYLE") {
                std::string css = elem->textContent();
                if (!css.empty()) {
                    addSheetToCascade(htmlayout::css::parse(css));
                    elem->setStyleSheetAdded(true);
                }
            }
        }
    }
    // The parse pass above added every <style> present in the source, so no
    // reconcile is owed until the DOM next changes.
    styleElsDirty_ = false;

    gumbo_destroy_output(&kGumboDefaultOptions, output);
    dirty_ = false;
}

void Document::buildTreeFromGumbo(::GumboNode* node, Element* parentElem) {
    // GUMBO_NODE_TEMPLATE is a distinct node type carrying a GumboElement, not a
    // GUMBO_NODE_ELEMENT — gumbo splits it out precisely so clients can choose
    // whether to descend (see the comment on the enum in gumbo.h). bro only ever
    // matched GUMBO_NODE_ELEMENT, so every <template> and everything inside it
    // silently vanished from any document parsed straight through gumbo
    // (DOMParser, innerHTML). The app-HTML path only escaped that because
    // extractTemplates() rewrites templates to placeholders before parsing.
    if (!node || (node->type != GUMBO_NODE_ELEMENT &&
                  node->type != GUMBO_NODE_TEMPLATE)) return;
    auto* gumboElem = &node->v.element;

    for (unsigned int i = 0; i < gumboElem->children.length; ++i) {
        auto* child = static_cast<GumboNode*>(gumboElem->children.data[i]);

        if (child->type == GUMBO_NODE_ELEMENT ||
            child->type == GUMBO_NODE_TEMPLATE) {
            const char* tag = gumbo_normalized_tagname(child->v.element.tag);
            std::string tagStr = (tag && tag[0]) ? tag : "";

            // Handle unknown tags (gumbo returns "" for custom elements)
            if (tagStr.empty()) {
                GumboStringPiece original = child->v.element.original_tag;
                if (original.data && original.length > 0) {
                    // Extract tag name from "<tag-name ..." or "<tag-name>"
                    const char* start = original.data + 1; // skip '<'
                    const char* end = start;
                    while (end < original.data + original.length &&
                           *end != ' ' && *end != '>' && *end != '/' && *end != '\t' && *end != '\n') {
                        ++end;
                    }
                    tagStr = std::string(start, end);
                }
                if (tagStr.empty()) tagStr = "div";
            }

            auto* childElem = allocateNode<Element>(tagStr);
            childElem->setDocument(this);

            // Copy attributes
            GumboVector* attrs = &child->v.element.attributes;
            for (unsigned int j = 0; j < attrs->length; ++j) {
                auto* attr = static_cast<GumboAttribute*>(attrs->data[j]);
                std::string attrName = attr->name;
                // Reconstruct namespace prefix for SVG/XML attributes
                switch (attr->attr_namespace) {
                    case GUMBO_ATTR_NAMESPACE_XLINK:
                        attrName = "xlink:" + attrName; break;
                    case GUMBO_ATTR_NAMESPACE_XML:
                        attrName = "xml:" + attrName; break;
                    case GUMBO_ATTR_NAMESPACE_XMLNS:
                        if (attrName != "xmlns") attrName = "xmlns:" + attrName; break;
                    default: break;
                }
                childElem->setAttribute(attrName, attr->value ? attr->value : "");
            }

            parentElem->appendChild(childElem);

            if (child->type == GUMBO_NODE_TEMPLATE) {
                // Template children go into a separate DocumentFragment, never
                // the normal child list — that is what makes them inert: not
                // laid out, not painted, not reachable from a document query.
                auto* frag = allocateNode<Element>("#DOCUMENT-FRAGMENT");
                childElem->setTemplateContent(frag);
                buildTreeFromGumbo(child, frag);
            } else {
                buildTreeFromGumbo(child, childElem);
            }

        } else if (child->type == GUMBO_NODE_TEXT ||
                   child->type == GUMBO_NODE_WHITESPACE) {
            const char* text = child->v.text.text;
            if (text && text[0]) {
                auto* textNode = allocateNode<TextNode>(text);
                parentElem->appendChild(textNode);
            }
        } else if (child->type == GUMBO_NODE_COMMENT) {
            const char* data = child->v.text.text;
            auto* commentNode = allocateNode<CommentNode>(data ? data : "");
            parentElem->appendChild(commentNode);
        }
    }
}

// ---------------------------------------------------------------------------
// Style resolution
// ---------------------------------------------------------------------------

void Document::setActiveElement(Element* el) {
    if (focusedElement_ == el) return;
    // A focus change adds/removes :focus (and :focus-within on ancestors)
    // styling and toggles the native input/textarea caret, all of which live
    // in the cached HTML base. Dirty the outgoing and incoming elements so
    // their styles re-resolve and the base is re-recorded; without this the
    // retained-base cache re-presents the stale frame and the focus change
    // doesn't paint until some unrelated restyle (e.g. a :hover) forces a
    // rebuild. markDirty() propagates to the document, so this also drives the
    // relayout the :focus rules may need.
    if (focusedElement_) focusedElement_->markDirty();
    if (el) el->markDirty();
    focusedElement_ = el;
}

namespace {
// Split a class attribute into its whitespace-separated tokens.
std::vector<std::string> classTokens(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string t;
    while (iss >> t) out.push_back(t);
    return out;
}
}  // namespace

bool Document::classChangeAffectsDescendants(const std::string& oldCls,
                                             const std::string& newCls) const {
    auto before = classTokens(oldCls);
    auto after  = classTokens(newCls);
    auto changedOne = [&](const std::vector<std::string>& a,
                          const std::vector<std::string>& b) {
        for (const auto& t : a) {
            if (std::find(b.begin(), b.end(), t) != b.end()) continue;  // unchanged
            if (cascade_.classAffectsDescendants(t)) return true;
        }
        return false;
    };
    return changedOne(before, after) || changedOne(after, before);
}

void Document::resolveStyles() {
    if (!documentElement_) return;
    auto styleT0 = std::chrono::steady_clock::now();
    // A new stylesheet can restyle anything, and the elements it now matches
    // were never marked dirty (nobody touched them — the *rules* changed). So a
    // sheet arriving forces a full re-resolve; otherwise a runtime
    // document.head.appendChild(styleEl) would only reach elements that some
    // unrelated change happened to dirty later.
    bool sheetAdded = styleElsDirty_ && reconcileStyleElements();
    // A media-context change (viewport resize, color-scheme flip) rebuilt the
    // cascade: the rules changed under every element, same invalidation as a
    // new sheet.
    if (mediaRebuilt_) { sheetAdded = true; mediaRebuilt_ = false; }
    // Sticky: once a sheet declares `border: inherit` (or any other forced
    // inherit of a non-inherited property), the scoped restyle below can no
    // longer prove a clean subtree, for this document, for good.
    if (cascade_.usesForcedInherit()) forcedInherit_ = true;
    layout::ElementRefAdapter::clearCache();
    restyled_.clear();
    // A new sheet can change what matches anywhere, so it is a selector-level
    // invalidation, not just a re-resolve of the values already matched.
    resolveStylesRecursive(documentElement_, nullptr, /*force=*/sheetAdded,
                           /*selectorForce=*/sheetAdded);
    resolveGeneratedContent();
    restyled_.clear();
    layout::ElementRefAdapter::clearCache();
    perf_.styleMs += std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - styleT0).count();
}

// A <style> inserted at runtime (createElement("style") + head.appendChild, the
// CSS-in-JS pattern) never went through parse(), so its rules aren't in the
// cascade. Scan the connected tree for any <style> not yet added and add it.
// Incremental (no cascade clear) to preserve UA / linked / shadow-scoped sheets
// and @keyframes/@font-face. A <style> whose text is still empty is left for a
// later pass (its content may be assigned after insertion).
bool Document::reconcileStyleElements() {
    styleElsDirty_ = false;
    if (!documentElement_) return false;
    std::vector<Element*> all;
    collectElements(root_, all);
    bool added = false;
    for (auto* e : all) {
        if (e->tagName() != "STYLE" || e->styleSheetAdded()) continue;
        std::string css = e->textContent();
        if (css.empty()) continue;
        addSheetToCascade(htmlayout::css::parse(css));
        e->setStyleSheetAdded(true);
        added = true;
    }
    return added;
}

namespace {

// Per-property flags the restyle diff needs. Both questions the diff asks are
// answered from one lookup instead of two separate hash-set probes per property:
//
//   PAINT_ONLY — value affects only how a box is painted, never its size or that
//     of anything around it. A restyle (e.g. a :hover) touching ONLY these can
//     re-record without re-running layout. Anything NOT flagged is treated as
//     layout-affecting — the conservative default, so a new or unrecognised
//     property forces a layout rather than risking a stale one.
//   INHERITED — the property inherits. This is the only channel through which a
//     change to one element's style can reach a descendant that did not itself
//     change: if none of these moved, no descendant's style can differ.
//
// The two source lists are kept verbatim below (bro's own policy — deliberately
// NOT htmlayout's classification, which differs on custom properties and on the
// paint-only/layout-read distinction) and unioned once into a single flag map.
enum PropFlags : uint8_t { PROP_PAINT_ONLY = 1, PROP_INHERITED = 2 };

uint8_t propFlags(const std::string& p) {
    // Custom properties (--*) inherit; they are never paint-only.
    if (p.size() >= 2 && p[0] == '-' && p[1] == '-') return PROP_INHERITED;
    static const std::unordered_map<std::string, uint8_t> kFlags = [] {
        static const char* kPaintOnly[] = {
            "color", "background", "background-color", "background-image",
            "background-position", "background-position-x", "background-position-y",
            "background-size", "background-repeat", "background-origin",
            "background-clip", "background-attachment", "background-blend-mode",
            "mix-blend-mode", "isolation",
            "border-color", "border-top-color", "border-right-color",
            "border-bottom-color", "border-left-color",
            "outline", "outline-color", "outline-style", "outline-width",
            "outline-offset",
            "box-shadow", "text-shadow",
            "border-radius", "border-top-left-radius", "border-top-right-radius",
            "border-bottom-left-radius", "border-bottom-right-radius",
            "opacity", "transform", "transform-origin", "transform-style",
            "translate", "rotate", "scale",
            "perspective", "perspective-origin", "backface-visibility",
            "transition", "transition-property", "transition-duration",
            "transition-timing-function", "transition-delay",
            "animation", "animation-name", "animation-duration",
            "animation-timing-function", "animation-delay",
            "animation-iteration-count", "animation-direction",
            "animation-fill-mode", "animation-play-state",
            "cursor", "text-decoration", "text-decoration-color",
            "text-decoration-line", "text-decoration-style",
            "text-decoration-thickness", "text-underline-offset",
            "filter", "backdrop-filter", "visibility", "pointer-events",
            "z-index", "user-select", "-webkit-user-select",
            "caret-color", "accent-color", "fill", "stroke", "stroke-width",
            "clip-path", "mask", "will-change", "-webkit-text-fill-color",
            "text-emphasis-color", "scrollbar-color", "color-scheme",
            "object-position", "image-rendering",
        };
        static const char* kInherited[] = {
            "color", "cursor", "direction", "visibility", "pointer-events",
            "font", "font-family", "font-size", "font-style", "font-variant",
            "font-weight", "font-stretch", "font-feature-settings",
            "font-variant-numeric", "-webkit-font-smoothing",
            "letter-spacing", "line-height", "word-spacing", "text-align",
            "text-align-last", "text-indent", "text-justify", "text-shadow",
            "text-transform", "text-rendering", "-webkit-text-fill-color",
            "text-emphasis-color", "text-orientation", "writing-mode",
            "white-space", "word-break", "word-wrap", "overflow-wrap", "hyphens",
            "tab-size", "quotes", "orphans", "widows",
            "list-style", "list-style-image", "list-style-position",
            "list-style-type",
            "border-collapse", "border-spacing", "empty-cells", "caption-side",
            "caret-color", "accent-color", "color-scheme", "scrollbar-color",
            "user-select", "-webkit-user-select", "image-rendering",
            "fill", "stroke", "stroke-width", "text-anchor", "paint-order",
        };
        std::unordered_map<std::string, uint8_t> m;
        for (const char* k : kPaintOnly) m[k] |= PROP_PAINT_ONLY;
        for (const char* k : kInherited) m[k] |= PROP_INHERITED;
        return m;
    }();
    auto it = kFlags.find(p);
    return it == kFlags.end() ? 0 : it->second;
}

// Two questions the caller asks of a re-resolve, answered in one walk:
//   layoutAffecting — did any layout-affecting property differ? (paint-only
//     props ignored, so a :hover that only recolours skips layout.)
//   inherited       — did any inherited property differ? i.e. can this
//     re-resolve have changed a descendant's style.
struct StyleChange {
    bool layoutAffecting = false;
    bool inherited = false;
};

// Combined single-pass diff replacing the old separate layoutAffectingChanged /
// inheritedChanged, each of which walked both maps in full (four traversals) and
// paid the property classification and the cross-map find() twice per key. Here
// each key is visited once, classified once for each question, and the expensive
// find() is skipped entirely when neither question cares about the key.
//
// `wantLayout` lets the caller drop the layout-affecting question when the whole
// tree is already queued for relayout (fullLayout_) — the diff would tell it
// nothing. When false, `.layoutAffecting` stays false and its classification is
// never run, and the walk short-circuits as soon as the inherited answer is in.
StyleChange classifyStyleChange(const htmlayout::css::ComputedStyle& a,
                                const htmlayout::css::ComputedStyle& b,
                                bool wantLayout) {
    StyleChange c;
    bool layoutDone = !wantLayout;   // "already answered" ⇒ skip the layout half

    // New or changed: every key in b, compared against a.
    for (const auto& [k, v] : b) {
        const uint8_t f = propFlags(k);                      // one lookup, both flags
        const bool inhRelevant = !c.inherited && (f & PROP_INHERITED);
        const bool layRelevant = !layoutDone && !(f & PROP_PAINT_ONLY);
        if (!inhRelevant && !layRelevant) continue;          // neither cares — no find
        auto it = a.find(k);
        if (it != a.end() && it->second == v) continue;      // unchanged
        if (inhRelevant) c.inherited = true;                 // added or changed
        if (layRelevant) { c.layoutAffecting = true; layoutDone = true; }
        if (c.inherited && layoutDone) return c;             // both answered
    }
    // Removed: keys in a that no longer appear in b.
    for (const auto& [k, v] : a) {
        const uint8_t f = propFlags(k);
        const bool inhRelevant = !c.inherited && (f & PROP_INHERITED);
        const bool layRelevant = !layoutDone && !(f & PROP_PAINT_ONLY);
        if (!inhRelevant && !layRelevant) continue;
        if (b.find(k) != b.end()) continue;                  // still present
        if (inhRelevant) c.inherited = true;
        if (layRelevant) { c.layoutAffecting = true; layoutDone = true; }
        if (c.inherited && layoutDone) return c;
    }
    return c;
}

} // namespace

void Document::resolveStylesRecursive(Element* elem,
                                       const htmlayout::css::ComputedStyle* parentStyle,
                                       bool force,
                                       bool selectorForce,
                                       bool hoverForce) {
    // Did a selector input change on this element (class/id/attribute/:hover),
    // or on an ancestor? Either way every rule in this subtree may now match
    // differently, so the subtree has to re-resolve and `selDirty` carries that
    // all the way down — `.dark .btn` can match a grandchild.
    //
    // An inline-style write sets neither: it cannot change what matches, only
    // what this element hands down. Then the inherited diff below decides, and a
    // paint-only write like `container.style.opacity = x` re-resolves exactly one
    // element instead of its entire subtree.
    const bool selDirty = selectorForce | elem->takeSelectorDirty();

    // A :hover flipped at or under this element's scope (set on the hovered
    // chains' common ancestor, so siblings are covered too). Unlike selDirty
    // this does NOT re-resolve the subtree: the only selector input that moved
    // is :hover, so an element can only re-match if some rule names :hover
    // outside its subject compound AND names this element as that subject
    // (`.row:hover .label` — hoverCanAffect). Everything else in the subtree
    // keeps the style it has, which is what keeps a mouse move off the bill of
    // whatever container it happens to be over.
    const bool hoverDirty = hoverForce | elem->takeHoverScopeDirty();
    const bool hoverResolve =
        hoverDirty && !selDirty &&
        cascade_.hoverCanAffect(elem->tagName(), elem->getAttribute("id"),
                                elem->getAttribute("class"));

    // An element with an active CSS animation or transition must re-resolve
    // its style every frame so applyOverrides() below re-runs and advances the
    // interpolated value — even when nothing marked it dirty. This is
    // deliberately decoupled from markDirty(): a compositor-promoted animation
    // (transform/opacity only) advances its style here without dirtying the
    // document, so the cached base is never re-recorded on its account. Without
    // this, applyOverrides only ran on the frame the animation was registered
    // and every animation froze on its first applied value.
    bool animatingSelf =
        (animationManager_ && animationManager_->hasActive(elem)) ||
        (transitionManager_ && transitionManager_->hasActive(elem)) ||
        (webAnimationManager_ && webAnimationManager_->hasActive(elem));

    bool needsResolve = force || selDirty || hoverResolve || elem->isDirty() ||
                        elem->computedStyle().empty() || animatingSelf;

    // Set below from the style diff: can this element's re-resolve have changed
    // anything a descendant sees? Only through an inherited value.
    bool passedDownChanged = false;

    if (needsResolve) {
        perf_.elementsStyled++;
        auto* adapter = layout::ElementRefAdapter::getOrCreate(elem);

        // Inline style: StyleProxy is the sole source (Element::setAttribute
        // routes "style" attribute writes into it too, see element.cpp), so
        // there's only one declaration block to resolve, not two to merge.
        auto computed = cascade_.resolve(*adapter, elem->style().cssText(), parentStyle);

        // <svg> width/height attributes are presentational hints: they map to
        // the CSS width/height properties below author-stylesheet priority
        // (SVG 2). When the cascade produced no value, the attribute applies
        // directly — this is what makes `svg { width: 100% }` plus
        // height="180" lay out 180px tall in browsers, rather than deriving
        // the height from an intrinsic aspect ratio.
        {
            const std::string& tag = elem->tagName();
            if (tag == "svg" || tag == "SVG") {
                for (const char* prop : {"width", "height"}) {
                    if (computed.find(prop) != computed.end()) continue;
                    const std::string& v = elem->getAttribute(prop);
                    if (v.empty() || v == "auto") continue;
                    char* end = nullptr;
                    float num = std::strtof(v.c_str(), &end);
                    if (end == v.c_str() || num < 0) continue;
                    std::string rest(end);
                    if (rest.empty())
                        computed[prop] = v + "px";
                    else if (rest == "px" || rest == "%")
                        computed[prop] = v;
                }
            }
        }

        // Resolve font-size to absolute px so all consumers get a usable value.
        // em/% are relative to the parent's (already-resolved) font-size.
        auto fsIt = computed.find("font-size");
        if (fsIt != computed.end() && !fsIt->second.empty()) {
            const auto& val = fsIt->second;
            char* end = nullptr;
            float num = std::strtof(val.c_str(), &end);
            if (end != val.c_str() && num > 0) {
                std::string unit(end);
                float resolved = num; // default: px or unitless
                if (unit == "em") {
                    float parentFs = 16.0f;
                    if (parentStyle) {
                        auto pit = parentStyle->find("font-size");
                        if (pit != parentStyle->end()) {
                            char* pe = nullptr;
                            float pv = std::strtof(pit->second.c_str(), &pe);
                            if (pe != pit->second.c_str() && pv > 0) parentFs = pv;
                        }
                    }
                    resolved = num * parentFs;
                } else if (unit == "%") {
                    float parentFs = 16.0f;
                    if (parentStyle) {
                        auto pit = parentStyle->find("font-size");
                        if (pit != parentStyle->end()) {
                            char* pe = nullptr;
                            float pv = std::strtof(pit->second.c_str(), &pe);
                            if (pe != pit->second.c_str() && pv > 0) parentFs = pv;
                        }
                    }
                    resolved = num * parentFs / 100.0f;
                } else if (unit == "rem") {
                    resolved = num * rootFontSize_;
                } else if (unit == "pt") {
                    resolved = num * 96.0f / 72.0f;
                }
                fsIt->second = std::to_string(resolved);
                // The document element (<html>) defines the rem reference for
                // every descendant. It is resolved first (parentStyle==nullptr),
                // so capture its px font-size before children consume rem.
                if (!parentStyle) rootFontSize_ = resolved;
                // Clean up trailing zeros for readability (e.g. "32.000000" -> "32")
                auto& s = fsIt->second;
                if (s.find('.') != std::string::npos) {
                    s.erase(s.find_last_not_of('0') + 1, std::string::npos);
                    if (s.back() == '.') s.pop_back();
                }
            }
        }

        // CSS transitions: detect property changes and start transitions
        if (transitionManager_ && !elem->computedStyle().empty()) {
            transitionManager_->onStyleChange(elem, elem->computedStyle(), computed, transitionTime_);
        }

        // CSS animations: detect animation-name and start animations
        if (animationManager_) {
            animationManager_->onStyleChange(elem, computed, transitionTime_);
        }

        // Did this re-resolve move any geometry? A hover that only repainted a
        // background didn't, and skips layout entirely; a change to width or
        // font-size did, and dirties this element's layout node (and, through
        // it, the ancestors that have to reflow around it) so the incremental
        // pass recomputes that chain and reuses every other subtree. Skipped
        // when the whole tree is already queued for relayout — the diff would
        // tell us nothing and every element would pay for it.
        // Both questions in one walk. When fullLayout_ is set the whole tree is
        // already relaying out, so the layout-affecting half is skipped (and can
        // never come back true).
        StyleChange change = classifyStyleChange(elem->computedStyle(), computed,
                                                 /*wantLayout=*/!fullLayout_);
        if (change.layoutAffecting) {
            layoutDirty_ = true;
            elem->markLayoutDirty();
        }

        // Nothing inherited moved ⇒ no descendant's computed style can differ,
        // so the recursion below stops here unless a descendant is dirty on its
        // own account.
        passedDownChanged = change.inherited;

        elem->setComputedStyle(std::move(computed));

        // CSS transitions: apply interpolated overrides after setting style
        if (transitionManager_) {
            transitionManager_->applyOverrides(elem, elem->computedStyleMut(), transitionTime_);
        }

        // CSS animations: apply keyframe overrides
        if (animationManager_) {
            animationManager_->applyOverrides(elem, elem->computedStyleMut(), transitionTime_);
        }

        // Web Animations (element.animate): script animations sit above both
        // CSS transitions and CSS animations in composite order.
        if (webAnimationManager_) {
            webAnimationManager_->applyOverrides(elem, elem->computedStyleMut(), transitionTime_);
        }

        // ::before / ::after generated content is resolved in a separate pass
        // (resolveGeneratedContent) once all styles are known — counter() and
        // the quote keywords depend on scopes and nesting that only make sense
        // in tree order. Note this element for that pass: its pseudo-elements
        // are the only ones that can have changed, since a pseudo's rules are
        // matched against its originating element and inherit from its style.
        restyled_.push_back(elem);

        elem->clearDirty();
    }

    // Recurse into children. They must re-resolve when a selector input changed
    // at or above this element (their rule set may differ) or when an inherited
    // value this element hands down actually changed. Otherwise they keep the
    // style they have — a child that is dirty on its own account still resolves,
    // the recursion always walks the tree.
    //
    // Unless the page forces `inherit` on a property that does not normally
    // inherit (`border: inherit`): that ties a descendant's value to a parent
    // property the inherited-value diff above never looks at, so give up the
    // scoping and re-resolve the subtree the way we always did.
    const bool childForce =
        needsResolve && (selDirty || passedDownChanged || forcedInherit_);
    // The hover scope carries all the way down: `.row:hover .cell .label` names
    // a subject several levels below the element whose :hover flipped, and the
    // levels in between re-match nothing themselves.
    const bool childHoverForce = hoverDirty;

    for (auto* child : elem->childNodes()) {
        if (child->nodeType() == NodeType::Element) {
            resolveStylesRecursive(static_cast<Element*>(child), &elem->computedStyle(),
                                   childForce, selDirty, childHoverForce);
        }
    }

    // Recurse into shadow DOM children
    if (elem->hasShadow()) {
        auto* sr = elem->shadowRoot();
        for (auto* child : sr->childNodes()) {
            if (child->nodeType() == NodeType::Element) {
                resolveStylesRecursive(static_cast<Element*>(child), &elem->computedStyle(),
                                       childForce, selDirty, childHoverForce);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Generated content (::before / ::after): counters, counters(), attr(), quotes
// ---------------------------------------------------------------------------

namespace {

// A live CSS counter instance. `depth` is the tree depth of the element whose
// counter-reset created it; scope closes when traversal returns to a shallower
// or equal depth on a following element (see resolveGeneratedContentRecursive).
struct CounterEntry { long value = 0; int depth = 0; };

// Split a `counter-reset` / `counter-increment` value into (name, number)
// pairs. Numbers are optional (default `dflt`): "item" -> {item, dflt};
// "a 3 b" -> {a,3},{b,dflt-for-b? no: b gets dflt}. A number binds to the
// preceding name.
std::vector<std::pair<std::string,long>> parseCounterOps(const std::string& v, long dflt) {
    std::vector<std::pair<std::string,long>> out;
    std::istringstream ss(v);
    std::string tok;
    while (ss >> tok) {
        // Is tok a (possibly signed) integer?
        char* end = nullptr;
        long n = std::strtol(tok.c_str(), &end, 10);
        if (end != tok.c_str() && *end == '\0') {
            if (!out.empty()) out.back().second = n;   // number for preceding name
        } else if (tok != "none") {
            out.push_back({tok, dflt});
        }
    }
    return out;
}

// Parse the `quotes` property into open/close pairs. Empty or auto/none ->
// the English default (curly double then single). Value form:
//   "open1" "close1" "open2" "close2" ...
std::vector<std::pair<std::string,std::string>> parseQuotes(const std::string& v) {
    std::vector<std::string> strs;
    size_t i = 0;
    while (i < v.size()) {
        char c = v[i];
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            std::string s;
            while (j < v.size() && v[j] != c) { s += v[j]; ++j; }
            strs.push_back(s);
            i = (j < v.size()) ? j + 1 : j;
        } else {
            ++i;
        }
    }
    std::vector<std::pair<std::string,std::string>> pairs;
    for (size_t k = 0; k + 1 < strs.size(); k += 2) pairs.push_back({strs[k], strs[k+1]});
    if (pairs.empty()) {
        // English default: U+201C/U+201D, then U+2018/U+2019 (UTF-8 bytes).
        pairs.push_back({"\xE2\x80\x9C", "\xE2\x80\x9D"});
        pairs.push_back({"\xE2\x80\x98", "\xE2\x80\x99"});
    }
    return pairs;
}

} // namespace

struct Document::GenContentState {
    std::unordered_map<std::string, std::vector<CounterEntry>> counters;
    int quoteDepth = 0;

    // Pop every counter instance created deeper than `depth` — those scopes
    // closed once traversal returned to this level.
    void popDeeperThan(int depth) {
        for (auto& [name, stack] : counters) {
            while (!stack.empty() && stack.back().depth > depth) stack.pop_back();
        }
    }
    long counterValue(const std::string& name) const {
        auto it = counters.find(name);
        if (it == counters.end() || it->second.empty()) return 0;
        return it->second.back().value;
    }
    std::string countersValue(const std::string& name, const std::string& sep) const {
        auto it = counters.find(name);
        if (it == counters.end() || it->second.empty()) return "";
        std::string out;
        for (size_t i = 0; i < it->second.size(); ++i) {
            if (i) out += sep;
            out += std::to_string(it->second[i].value);
        }
        return out;
    }
    void reset(const std::string& name, long value, int depth) {
        auto& stack = counters[name];
        while (!stack.empty() && stack.back().depth >= depth) stack.pop_back();
        stack.push_back({value, depth});
    }
    void increment(const std::string& name, long value, int) {
        auto& stack = counters[name];
        // Incrementing a counter that no counter-reset created acts as though it
        // had been reset to 0 on the ROOT element (CSS 2.1 §12.4.3) — so the
        // implicit instance belongs to the root scope, at depth 0. Creating it at
        // the incrementing element's depth instead would scope it to that
        // element: `li::before { counter-increment: item }` increments at the
        // pseudo's depth (one deeper than the <li>), and popDeeperThan would drop
        // it the moment traversal reached the next <li> — restarting every marker
        // at 1.
        if (stack.empty()) stack.push_back({0, 0});
        stack.back().value += value;
    }
};

void Document::resolveGeneratedContent() {
    if (!documentElement_) return;

    // No ::before/::after rule anywhere ⇒ no element can carry generated
    // content, and none ever did, so there is nothing to resolve or clear.
    if (!cascade_.hasPseudoElementRules("before") &&
        !cascade_.hasPseudoElementRules("after")) {
        return;
    }

    // counter()/counters()/quotes read state that accumulates across the whole
    // document in tree order, so they leave us no choice but to walk all of it.
    if (statefulGenContent_) {
        GenContentState st;
        resolveGeneratedContentRecursive(documentElement_, 0, st);
        return;
    }

    // Otherwise a pseudo-element is a pure function of its originating element:
    // its rules are matched against that element, and it inherits from that
    // element's computed style. So only an element whose style was re-resolved
    // this pass can have different generated content — every other element
    // keeps the pseudo it already has.
    //
    // This is the same assumption resolveStylesRecursive already makes for real
    // elements (it re-resolves only dirty elements and their forced subtrees),
    // so the two passes now agree on what "could have changed" means. Walking
    // the whole document here instead cost ~3.5 ms per hover on a 391-row list:
    // an O(document) price paid on every pointer move, for generated content
    // that most pages don't have at all.
    //
    // The very first pass re-resolves every element, so a document that does
    // use counters or quotes is guaranteed to trip the flag below on its way
    // through — and then re-runs in document order, which is what makes this
    // safe to decide from the elements we happen to be visiting.
    GenContentState st;   // counter state goes unread on this path
    for (auto* elem : restyled_) {
        const auto& style = elem->computedStyle();
        auto dispIt = style.find("display");
        if (dispIt != style.end() && dispIt->second == "none") {
            elem->clearPseudos();
            continue;
        }
        applyPseudo(elem, "before", 0, st);
        applyPseudo(elem, "after", 0, st);
    }

    // A stateful value turned up while we were resolving locally, so the values
    // just written may have the wrong counter/quote state. Redo the pass the
    // slow, correct way — and, the flag being sticky, every pass after it.
    if (statefulGenContent_) {
        GenContentState full;
        resolveGeneratedContentRecursive(documentElement_, 0, full);
    }
}

// Apply an element's counter-reset then counter-increment declarations at the
// given depth. Shared by real elements and pseudo-elements.
void Document::applyCounterOps(Element* elem, const htmlayout::css::ComputedStyle& style,
                               int depth, Document::GenContentState& st) {
    auto rit = style.find("counter-reset");
    if (rit != style.end() && rit->second != "none" && !rit->second.empty()) {
        for (auto& [name, val] : parseCounterOps(rit->second, 0))
            st.reset(name, val, depth);
    }
    auto iit = style.find("counter-increment");
    if (iit != style.end() && iit->second != "none" && !iit->second.empty()) {
        for (auto& [name, val] : parseCounterOps(iit->second, 1))
            st.increment(name, val, depth);
    }
    (void)elem;
}

void Document::resolveGeneratedContentRecursive(Element* elem, int depth, GenContentState& st) {
    // Close counter scopes from any preceding cousin subtree deeper than us.
    st.popDeeperThan(depth);

    const auto& style = elem->computedStyle();
    auto dispIt = style.find("display");
    bool isNone = (dispIt != style.end() && dispIt->second == "none");

    // The element's own counter operations (reset before increment).
    if (!isNone) applyCounterOps(elem, style, depth, st);

    // display:none paints nothing, pseudo-elements included. Otherwise leave the
    // existing pseudo state alone — applyPseudo diffs against it to decide
    // whether the box moved, so clearing it up front would make every pseudo
    // look brand new and promote a layout on every restyle.
    if (isNone) elem->clearPseudos();

    // ::before is the element's first child — resolve it (and its counter ops)
    // after the element's own increment so counter() sees the post-increment
    // value, matching the "first child" model.
    if (!isNone) applyPseudo(elem, "before", depth + 1, st);

    // Recurse into real children in document order.
    if (!isNone) {
        for (auto* child : elem->childNodes()) {
            if (child->nodeType() == NodeType::Element)
                resolveGeneratedContentRecursive(static_cast<Element*>(child), depth + 1, st);
        }
        if (elem->hasShadow()) {
            auto* sr = elem->shadowRoot();
            for (auto* child : sr->childNodes()) {
                if (child->nodeType() == NodeType::Element)
                    resolveGeneratedContentRecursive(static_cast<Element*>(child), depth + 1, st);
            }
        }
    }

    // ::after is the last child — resolve after children so it sees their
    // counter state (and the correct close-quote nesting depth).
    if (!isNone) applyPseudo(elem, "after", depth + 1, st);
}

void Document::applyPseudo(Element* elem, const char* which, int depth, GenContentState& st) {
    // A pseudo-element that stops matching has to be torn back down, so this
    // owns both directions. The layout tree syncs its synthetic pseudo box in
    // ensurePseudo() during layout, which means an appearing or disappearing
    // pseudo is a GEOMETRY change: without the promotions below, a
    // `:hover::before { content: "..." }` would restyle paint-only, skip layout
    // entirely, and never actually show up (or, once shown, never go away).
    auto dropPseudo = [&] {
        if (!elem->hasPseudo(which)) return;
        elem->clearPseudo(which);
        layoutDirty_ = true;
        elem->markLayoutDirty();
    };

    // Cheapest possible miss: a sheet with no ::after rule shouldn't cost an
    // adapter allocation per element just to be told nothing matched.
    if (!cascade_.hasPseudoElementRules(which)) { dropPseudo(); return; }
    auto* adapter = layout::ElementRefAdapter::getOrCreate(elem);
    auto pseudoStyle = cascade_.resolvePseudo(*adapter, which, elem->computedStyle());
    auto cIt = pseudoStyle.find("content");
    if (cIt == pseudoStyle.end()) { dropPseudo(); return; }
    const std::string& raw = cIt->second;
    if (raw.empty() || raw == "normal" || raw == "none") { dropPseudo(); return; }

    // This element really does generate counter/quote content, so the document
    // needs the stateful document-order pass from here on (see
    // resolveGeneratedContent). Latched on the RESOLVED value, not on the
    // stylesheet: the UA sheet's `q::before { content: open-quote }` means every
    // document has such a rule, but only one with an actual <q> in it pays.
    if (!statefulGenContent_ && htmlayout::css::Cascade::contentIsStateful(raw))
        statefulGenContent_ = true;

    // A pseudo-element can itself carry counter-reset / counter-increment; it
    // acts as a child of its originating element, so apply at depth.
    applyCounterOps(elem, pseudoStyle, depth, st);

    // Quotes come from the pseudo's (inherited) `quotes` property.
    auto qIt = pseudoStyle.find("quotes");
    std::string quotesVal = (qIt != pseudoStyle.end()) ? qIt->second : std::string();
    if (quotesVal == "auto" || quotesVal == "none") quotesVal.clear();
    auto quotePairs = parseQuotes(quotesVal);

    // Tokenize the content value into: quoted strings, counter()/counters(),
    // attr(), and the quote keywords. Whitespace between components is a token
    // separator and contributes no text (only string literals do).
    std::string out;
    size_t i = 0;
    const size_t n = raw.size();
    auto quoteIndex = [&](int d) -> size_t {
        if (d < 0) d = 0;
        return std::min<size_t>(static_cast<size_t>(d), quotePairs.size() - 1);
    };
    while (i < n) {
        char c = raw[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') { ++i; continue; }
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < n && raw[j] != c) {
                if (raw[j] == '\\' && j + 1 < n) { out += raw[j+1]; j += 2; }
                else { out += raw[j]; ++j; }
            }
            i = (j < n) ? j + 1 : j;
            continue;
        }
        // Read an identifier / function token up to a delimiter.
        size_t j = i;
        while (j < n && raw[j] != ' ' && raw[j] != '\t' && raw[j] != '\n' &&
               raw[j] != '\r' && raw[j] != '\f' && raw[j] != '(') ++j;
        std::string tok = raw.substr(i, j - i);
        if (j < n && raw[j] == '(') {
            // Function: capture the parenthesized argument text.
            size_t k = j + 1;
            int depthP = 1;
            std::string args;
            while (k < n && depthP > 0) {
                if (raw[k] == '(') { ++depthP; args += raw[k]; }
                else if (raw[k] == ')') { --depthP; if (depthP > 0) args += raw[k]; }
                else args += raw[k];
                ++k;
            }
            i = k;
            if (tok == "attr") {
                std::string an = args;
                // trim
                size_t a = an.find_first_not_of(" \t");
                size_t b = an.find_last_not_of(" \t");
                if (a != std::string::npos) an = an.substr(a, b - a + 1);
                out += elem->getAttribute(an);
            } else if (tok == "counter") {
                // counter( name [, style] ) — style ignored (decimal).
                std::string name = args;
                auto comma = name.find(',');
                if (comma != std::string::npos) name = name.substr(0, comma);
                size_t a = name.find_first_not_of(" \t");
                size_t b = name.find_last_not_of(" \t");
                if (a != std::string::npos) name = name.substr(a, b - a + 1);
                out += std::to_string(st.counterValue(name));
            } else if (tok == "counters") {
                // counters( name, sep [, style] )
                std::string name, sep;
                auto comma = args.find(',');
                if (comma != std::string::npos) {
                    name = args.substr(0, comma);
                    std::string rest = args.substr(comma + 1);
                    // sep is the first quoted string in rest.
                    size_t q = rest.find_first_of("\"'");
                    if (q != std::string::npos) {
                        char qc = rest[q];
                        size_t e = rest.find(qc, q + 1);
                        if (e != std::string::npos) sep = rest.substr(q + 1, e - q - 1);
                    }
                } else {
                    name = args;
                }
                size_t a = name.find_first_not_of(" \t");
                size_t b = name.find_last_not_of(" \t");
                if (a != std::string::npos) name = name.substr(a, b - a + 1);
                out += st.countersValue(name, sep);
            }
            // url(...) and other functions contribute no text.
            continue;
        }
        i = j;
        if (tok == "open-quote") {
            out += quotePairs[quoteIndex(st.quoteDepth)].first;
            ++st.quoteDepth;
        } else if (tok == "close-quote") {
            if (st.quoteDepth > 0) --st.quoteDepth;
            out += quotePairs[quoteIndex(st.quoteDepth)].second;
        } else if (tok == "no-open-quote") {
            ++st.quoteDepth;
        } else if (tok == "no-close-quote") {
            if (st.quoteDepth > 0) --st.quoteDepth;
        }
        // Bare idents (e.g. `normal`) contribute nothing.
    }

    // Same paint-only test the real elements get: a pseudo whose text changed —
    // or appeared — has moved geometry and needs a layout; one that only changed
    // colour has not, and stays on the cheap paint-only path.
    if (!elem->hasPseudo(which) || elem->pseudoContent(which) != out ||
        classifyStyleChange(elem->pseudoStyle(which), pseudoStyle,
                            /*wantLayout=*/true).layoutAffecting) {
        layoutDirty_ = true;
        elem->markLayoutDirty();
    }

    elem->setPseudo(which, std::move(out), std::move(pseudoStyle));
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
// Node creation
// ---------------------------------------------------------------------------

Element* Document::createElement(const std::string& tag) {
    auto* elem = allocateNode<Element>(tag);
    elem->setDocument(this);
    return elem;
}

TextNode* Document::createTextNode(const std::string& text) {
    return allocateNode<TextNode>(text);
}

CommentNode* Document::createComment(const std::string& data) {
    return allocateNode<CommentNode>(data);
}

DocumentFragment* Document::createDocumentFragment() {
    return allocateNode<DocumentFragment>();
}

Node* Document::cloneNode(Node* src, bool deep, bool preserveId) {
    if (!src) return nullptr;

    switch (src->nodeType()) {
        case NodeType::Text:
            return createTextNode(static_cast<TextNode*>(src)->data());
        case NodeType::Comment:
            return createComment(static_cast<CommentNode*>(src)->data());
        case NodeType::Element:
            break;
        default:
            return nullptr;
    }

    auto* srcEl = static_cast<Element*>(src);
    Element* clone = createElement(srcEl->tagName());
    if (!clone) return nullptr;

    for (const auto& [name, val] : srcEl->attributes()) {
        if (!preserveId && name == "id") continue;
        clone->setAttribute(name, val);
    }
    // "style" never lives in attributes_ (see Element::setAttribute) — copy the
    // declaration block from StyleProxy, and only when the attribute is really
    // present, so an element with no style attribute doesn't grow one.
    if (srcEl->hasAttribute("style"))
        clone->setAttribute("style", srcEl->style().cssText());

    if (deep) {
        for (auto* child : srcEl->childNodes()) {
            if (Node* childClone = cloneNode(child, true, preserveId))
                clone->appendChild(childClone);
        }
    }

    // Children first: the hook stamps <select> state onto cloned <option>s.
    if (elementClonedCb_) elementClonedCb_(this, srcEl, clone);

    return clone;
}

ShadowRoot* Document::allocateShadowRoot(Element* host, ShadowRoot::Mode mode) {
    return allocateNode<ShadowRoot>(host, mode);
}

// Move one node's ownership record from `src` into this document. The subtree
// walk is the caller's job (adoptNode) so the whole tree moves before any
// callback observes a half-adopted state.
void Document::adoptOne(Node* node, Document* src) {
    if (src && src != this) {
        // Element ids are per-document: drop the source's registration before
        // the node stops belonging to it, add ours after.
        if (node->nodeType() == NodeType::Element) {
            auto* elem = static_cast<Element*>(node);
            if (!elem->id().empty()) src->unregisterElementId(elem->id());
            if (elem == src->focusedElement_) src->focusedElement_ = nullptr;
        }
        // Live ranges in the source document can't span into another document.
        for (auto* r : src->liveRanges_) r->onNodeDestroyed(node);

        auto it = src->ownedNodes_.find(node);
        if (it != src->ownedNodes_.end()) {
            ownedNodes_[node] = std::move(it->second);
            src->ownedNodes_.erase(it);
        }
    }
    node->setDocument(this);
    if (node->nodeType() == NodeType::Element) {
        auto* elem = static_cast<Element*>(node);
        if (!elem->id().empty()) registerElementId(elem->id(), elem);
        // Styles were resolved against the source document's cascade.
        elem->markDirty();
        elem->markStructureDirty();
    }
    if (nodeAdoptedCb_) nodeAdoptedCb_(this, src, node);
}

Node* Document::adoptNode(Node* node) {
    if (!node) return nullptr;
    Document* src = node->document();
    if (src == this) {
        // Same document: spec still removes the node from its parent.
        if (auto* p = node->parentNode()) p->removeChild(node);
        return node;
    }
    if (auto* p = node->parentNode()) p->removeChild(node);

    // Deepest-last walk: every node in the subtree changes owner.
    std::vector<Node*> stack{node};
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        for (auto* child : n->childNodes()) stack.push_back(child);
        adoptOne(n, src);
    }
    markDirty();
    return node;
}

void Document::freeNode(Node* node) {
    if (!node) return;
    auto kids = node->childNodes();
    for (auto* child : kids) {
        freeNode(child);
    }
    // Clear any live Range endpoints that still reference this node. Paths
    // like setTextContent and innerHTML-replacement detach children without
    // firing notifyNodeRemoved, so endpoints can outlive the node they
    // point at — this is the last chance to break that reference before
    // the memory is (eventually) freed from pendingFrees_.
    for (auto* r : liveRanges_) {
        r->onNodeDestroyed(node);
    }
    // Unregister element id from the lookup map
    if (node->nodeType() == NodeType::Element) {
        auto* elem = static_cast<Element*>(node);
        std::string id = elem->id();
        if (!id.empty())
            unregisterElementId(id);
        // The focused element is about to be deallocated — drop the document's
        // reference so activeElement() can never hand out freed memory. (The
        // engine polls activeElement() on every mouse event: a button that
        // removes itself from its own click handler — focused by the very
        // mousedown that triggered it — would otherwise dangle here and crash
        // the next mousemove. Engine::reapDeadInputPointers scrubs the
        // engine's own cached pointers but not this document-owned one.)
        if (elem == focusedElement_) focusedElement_ = nullptr;
    }
    // Let the JS layer drop this node's wrapper (raw Element* + elem-map entry)
    // while the memory is still valid. Children were handled by the recursion
    // above. Without this, wrappers created for nodes freed via paths that
    // don't call invalidateWrapper (innerHTML/textContent replacement, range
    // extraction, the orphan-fragment sweep) outlive drainPendingFrees() and
    // dangle — a later property access or sweepOrphanedWrappers() then reads
    // the freed Element and faults.
    if (nodeFreedCb_) nodeFreedCb_(this, node);

    // Move the owning unique_ptr into pendingFrees_ rather than destroying
    // it now. The raster thread may still hold a raw pointer from an
    // in-flight traversal; delaying destruction until both threads are
    // idle keeps those pointers valid for their brief lifetime.
    auto it = ownedNodes_.find(node);
    if (it != ownedNodes_.end()) {
        pendingFrees_.push_back(std::move(it->second));
        pendingSet_.insert(node);
        ownedNodes_.erase(it);
    }
}

void Document::drainPendingFrees() {
    // Last chance to sever anything still pointing into this storage, while it
    // is unambiguously valid. freeNode() already fired nodeFreedCb_ for every
    // one of these nodes, so this normally finds nothing left to do; it catches
    // references taken during the window BETWEEN freeNode() and here — notably
    // a JS wrapper handed out for an already-doomed node by an event dispatch
    // still unwinding its propagation path.
    if (nodeDestroyingCb_) {
        std::function<void(Node*)> walk = [&](Node* n) {
            if (!n) return;
            for (Node* child : n->childNodes()) walk(child);
            nodeDestroyingCb_(this, n);
        };
        for (auto& root : pendingFrees_) walk(root.get());
    }
    pendingFrees_.clear();
    pendingSet_.clear();
}

void Document::forEachLiveElement(const std::function<void(Element*)>& fn) {
    if (!fn) return;

    for (auto& [n, _] : ownedNodes_) {
        if (n && n->nodeType() == NodeType::Element)
            fn(static_cast<Element*>(n));
    }

    // pendingFrees_ holds detached subtree roots whose memory is still alive.
    // Their descendants are owned by the root (not necessarily still listed in
    // ownedNodes_), so walk each subtree explicitly.
    std::function<void(Node*)> walk = [&](Node* n) {
        if (!n) return;
        if (n->nodeType() == NodeType::Element)
            fn(static_cast<Element*>(n));
        for (Node* child : n->childNodes()) walk(child);
    };
    for (auto& root : pendingFrees_) walk(root.get());
}

bool Document::ownsNode(const Node* n) const {
    if (!n) return false;
    return ownedNodes_.find(const_cast<Node*>(n)) != ownedNodes_.end();
}

bool Document::isNodeLive(const Node* n) const {
    if (!n) return false;
    Node* key = const_cast<Node*>(n);
    return ownedNodes_.find(key) != ownedNodes_.end() ||
           pendingSet_.find(key) != pendingSet_.end();
}

Node* Document::resolveNode(const Node* ptr, uint32_t id) const {
    if (!ptr) return nullptr;
    Node* key = const_cast<Node*>(ptr);
    // Pointer-value lookup first — never dereference `ptr` until it is proven
    // to be a node this document keeps alive. The id check then closes the
    // ABA hole isNodeLive alone has: if the allocator reused the address for
    // a *new* node, its nodeId (globally monotonic, never recycled) differs
    // and the stale handle resolves to null instead of the impostor.
    if (ownedNodes_.find(key) == ownedNodes_.end() &&
        pendingSet_.find(key) == pendingSet_.end())
        return nullptr;
    return key->nodeId() == id ? key : nullptr;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

Element* Document::getElementById(const std::string& id) {
    auto it = idMap_.find(id);
    return (it != idMap_.end()) ? it->second : nullptr;
}

Element* Document::querySelector(const std::string& selector) {
    if (root_ && root_->nodeType() == NodeType::Element) {
        return static_cast<Element*>(root_)->querySelector(selector);
    }
    return nullptr;
}

std::vector<Element*> Document::querySelectorAll(const std::string& selector) {
    if (root_ && root_->nodeType() == NodeType::Element) {
        return static_cast<Element*>(root_)->querySelectorAll(selector);
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
    if (!id.empty() && elem) {
        idMap_[id] = elem;
    }
}

void Document::unregisterElementId(const std::string& id) {
    idMap_.erase(id);
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
// Template extraction — pre-process HTML before gumbo parsing
// ---------------------------------------------------------------------------

std::string Document::extractTemplates(const std::string& html,
                                       std::vector<TemplateBlock>& out)
{
    std::string result;
    result.reserve(html.size());
    size_t pos = 0;
    int genId = 0;

    while (pos < html.size()) {
        // Skip HTML comments that might contain "<template" as text
        size_t commentStart = html.find("<!--", pos);
        size_t start = html.find("<template", pos);
        if (start == std::string::npos) {
            result.append(html, pos, html.size() - pos);
            break;
        }
        // If a comment starts before this match, skip past it first
        while (commentStart != std::string::npos && commentStart < start) {
            size_t commentEnd = html.find("-->", commentStart + 4);
            if (commentEnd == std::string::npos) break;
            commentEnd += 3; // past "-->"
            if (start < commentEnd) {
                // The "<template" was inside a comment — skip and re-search
                result.append(html, pos, commentEnd - pos);
                pos = commentEnd;
                start = html.find("<template", pos);
                if (start == std::string::npos) break;
                commentStart = html.find("<!--", pos);
                continue;
            }
            break;
        }
        if (start == std::string::npos) {
            result.append(html, pos, html.size() - pos);
            break;
        }
        result.append(html, pos, start - pos);

        size_t tagEnd = html.find('>', start);
        if (tagEnd == std::string::npos) {
            result.append(html, start, html.size() - start);
            break;
        }

        std::string openTag = html.substr(start, tagEnd - start + 1);
        std::string id;
        size_t idPos = openTag.find("id=\"");
        if (idPos == std::string::npos) idPos = openTag.find("id='");
        if (idPos != std::string::npos) {
            char quote = openTag[idPos + 3];
            size_t idStart = idPos + 4;
            size_t idEnd = openTag.find(quote, idStart);
            if (idEnd != std::string::npos)
                id = openTag.substr(idStart, idEnd - idStart);
        }
        if (id.empty()) {
            id = "__bro_tmpl_" + std::to_string(genId++);
        }

        size_t contentStart = tagEnd + 1;
        size_t closeTag = html.find("</template>", contentStart);
        if (closeTag == std::string::npos) {
            result.append(html, start, html.size() - start);
            break;
        }

        std::string innerHTML = html.substr(contentStart, closeTag - contentStart);

        TemplateBlock block;
        block.id = id;
        block.innerHTML = innerHTML;
        out.push_back(std::move(block));

        result += "<div data-bro-template=\"" + id + "\" id=\"" + id + "\" style=\"display:none\"></div>";
        pos = closeTag + 11;
    }

    return result;
}

void Document::injectTemplates(const std::vector<TemplateBlock>& templates) {
    for (auto& tmpl : templates) {
        Element* placeholder = getElementById(tmpl.id);
        if (!placeholder) continue;

        auto* tmplElem = createElement("TEMPLATE");
        tmplElem->setAttribute("id", tmpl.id);
        tmplElem->setAttribute("data-bro-template-html", tmpl.innerHTML);

        auto* parent = placeholder->parentElement();
        if (parent) {
            parent->insertBefore(tmplElem, placeholder);
            parent->removeChild(placeholder);
            registerElementId(tmpl.id, tmplElem);
        }
    }
}

// ---------------------------------------------------------------------------
// innerHTML parsing with gumbo
// ---------------------------------------------------------------------------

void Document::parseInnerHTML(Element* parent, const std::string& html) {
    if (!parent) return;

    // Clear existing children
    auto oldKids = parent->childNodes();
    for (auto* child : oldKids) {
        child->setParent(nullptr);
    }
    parent->childNodes().clear();
    for (auto* child : oldKids) {
        freeNode(child);
    }

    if (html.empty()) {
        parent->markStructureDirty();
        return;
    }

    // Parse the fragment in the context of the element receiving it. This is
    // what the HTML fragment parsing algorithm requires, and skipping it is not
    // cosmetic: without a context the tree builder sits in "in body" mode,
    // where <tr>, <td>, <tbody>, <thead>, <tfoot> and <caption> start tags are
    // parse errors and get DROPPED, keeping only their text. So
    //
    //     tbody.innerHTML = '<tr><td>a</td></tr>'
    //
    // used to yield a row-less table of height 0, and setting a <tr>'s
    // innerHTML collapsed its cells into one bare text node. gumbo takes the
    // context as a tag enum; anything it doesn't know (custom elements) falls
    // back to the historical <div> wrapper.
    std::string lowerTag = parent->tagName();
    for (auto& c : lowerTag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    GumboTag ctxTag = gumbo_tag_enum(lowerTag.c_str());

    GumboOptions opts = kGumboDefaultOptions;
    std::string wrapper;
    GumboOutput* output = nullptr;

    if (ctxTag != GUMBO_TAG_UNKNOWN && ctxTag != GUMBO_TAG_LAST) {
        opts.fragment_context = ctxTag;
        opts.fragment_namespace = GUMBO_NAMESPACE_HTML;
        output = gumbo_parse_with_options(&opts, html.c_str(), html.length());
    } else {
        wrapper = "<html><body><div>" + html + "</div></body></html>";
        output = gumbo_parse_with_options(&opts, wrapper.c_str(), wrapper.length());
    }
    if (!output) {
        parent->markStructureDirty();
        return;
    }

    // Navigate to our wrapper div: html > body > div
    std::function<GumboNode*(GumboNode*)> findWrapper = [&](GumboNode* node) -> GumboNode* {
        if (!node || node->type != GUMBO_NODE_ELEMENT) return nullptr;
        GumboVector* children = &node->v.element.children;
        for (unsigned int i = 0; i < children->length; ++i) {
            auto* child = static_cast<GumboNode*>(children->data[i]);
            if (child->type == GUMBO_NODE_ELEMENT) {
                GumboTag tag = child->v.element.tag;
                if (tag == GUMBO_TAG_BODY) {
                    // Look for div inside body
                    GumboVector* bodyChildren = &child->v.element.children;
                    for (unsigned int j = 0; j < bodyChildren->length; ++j) {
                        auto* bodyChild = static_cast<GumboNode*>(bodyChildren->data[j]);
                        if (bodyChild->type == GUMBO_NODE_ELEMENT &&
                            bodyChild->v.element.tag == GUMBO_TAG_DIV) {
                            return bodyChild;
                        }
                    }
                }
                auto* found = findWrapper(child);
                if (found) return found;
            }
        }
        return nullptr;
    };

    // In fragment mode gumbo synthesizes an <html> root and inserts the parsed
    // children straight into it, so that root IS the source node. The wrapper
    // path still has to dig out its <div>.
    GumboNode* source = (opts.fragment_context != GUMBO_TAG_LAST)
                      ? output->root
                      : findWrapper(output->root);
    if (source) {
        buildTreeFromGumbo(source, parent);
    }

    gumbo_destroy_output(&opts, output);

    // Extract <style> elements from the fragment and add CSS to the cascade
    std::vector<Element*> newElems;
    for (auto* child : parent->childNodes()) {
        if (child->nodeType() == NodeType::Element)
            collectElements(child, newElems);
    }
    for (auto* elem : newElems) {
        if (elem->tagName() == "STYLE") {
            std::string css = elem->textContent();
            if (!css.empty()) {
                addSheetToCascade(htmlayout::css::parse(css));
                // Mark added so the post-mutation reconcile (armed by the
                // markStructureDirty below) doesn't add these rules a second time.
                elem->setStyleSheetAdded(true);
            }
        }
        std::string elemId = elem->id();
        if (!elemId.empty()) idMap_[elemId] = elem;
    }

    // Only this element's children moved, so only its layout children have to be
    // rebuilt. The document-wide mark would throw away the cached geometry of
    // every subtree in the document — and `host.innerHTML = ...` on one small
    // container is the single most common DOM update an app makes.
    parent->markStructureDirty();
}

// ---------------------------------------------------------------------------
// Shadow DOM CSS
// ---------------------------------------------------------------------------

void Document::addShadowStylesheet(ShadowRoot* sr, const std::string& css) {
    if (!sr || css.empty()) return;
    addSheetToCascade(htmlayout::css::parse(css), static_cast<void*>(sr));
}

} // namespace bro::dom
