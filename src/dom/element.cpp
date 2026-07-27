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

void Node::appendChild(Node* child) {
    if (!child) return;
    if (child->parent_) {
        child->parent_->removeChild(child);
    }
    child->parent_ = this;
    children_.push_back(child);
}

// Walk up from any node to its owning Document (via the nearest Element).
static Document* findDocument(Node* node) {
    for (Node* n = node; n; n = n->parentNode()) {
        if (n->nodeType() == NodeType::Element)
            return static_cast<Element*>(n)->document();
    }
    return nullptr;
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
        (*it)->parent_ = nullptr;
        children_.erase(it);
    }
}

void Node::insertBefore(Node* newChild, Node* refChild) {
    if (!newChild) return;
    if (!refChild) {
        appendChild(newChild);
        return;
    }
    if (newChild->parent_) {
        newChild->parent_->removeChild(newChild);
    }
    auto it = std::find(children_.begin(), children_.end(), refChild);
    if (it != children_.end()) {
        newChild->parent_ = this;
        children_.insert(it, newChild);
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
    attributes_.erase(name);
    if (name == "class" || name == "id") markPaintDirty();  // see setAttribute
    else markDirty();
}

std::string Element::textContent() const {
    std::string result;
    for (const auto& child : children_) {
        if (child->nodeType() == NodeType::Text) {
            auto* text = static_cast<const TextNode*>(child);
            result += text->data();
        } else if (child->nodeType() == NodeType::Element) {
            auto* elem = static_cast<const Element*>(child);
            result += elem->textContent();
        }
    }
    return result;
}

void Element::setTextContent(const std::string& text) {
    if (children_.size() == 1 && children_[0]->nodeType() == NodeType::Text) {
        auto* existing = static_cast<TextNode*>(children_[0]);
        if (existing->data() == text) return;
        // Rewriting the lone text child in place leaves the tree shape alone, so
        // the layout tree stays valid (its adapter reads the TextNode live) and
        // this is a plain layout invalidation rather than a structural rebuild —
        // the difference between relaying out one element and relaying out the
        // document. Emptying the element still takes the slow path: layout drops
        // empty text nodes, so the tree really does change shape.
        if (!text.empty()) {
            existing->setData(text);
            markDirty();
            return;
        }
    }

    // Free old children
    auto oldKids = children_;
    for (auto& child : oldKids) {
        child->setParent(nullptr);
    }
    children_.clear();
    if (document_) {
        for (auto* child : oldKids) {
            document_->freeNode(child);
        }
    }

    // Add text node
    if (!text.empty() && document_) {
        auto* textNode = document_->createTextNode(text);
        appendChild(textNode);
    }

    markDirty();
    markStructureDirty();
}

std::string Element::innerHTML() const {
    std::ostringstream oss;
    for (const auto& child : children_) {
        if (child->nodeType() == NodeType::Text) {
            auto* text = static_cast<const TextNode*>(child);
            oss << text->data();
        } else if (child->nodeType() == NodeType::Comment) {
            auto* comment = static_cast<const CommentNode*>(child);
            oss << "<!--" << comment->data() << "-->";
        } else if (child->nodeType() == NodeType::Element) {
            auto* elem = static_cast<const Element*>(child);
            oss << elem->outerHTML();
        }
    }
    return oss.str();
}

static std::string htmlEscapeAttr(const std::string& val) {
    std::string result;
    result.reserve(val.size());
    for (char c : val) {
        switch (c) {
            case '"':  result += "&quot;"; break;
            case '&':  result += "&amp;"; break;
            case '<':  result += "&lt;"; break;
            case '>':  result += "&gt;"; break;
            default:   result += c; break;
        }
    }
    return result;
}

// HTML5 spec: SVG elements that require mixed-case tag names.
// Maps UPPERCASED tag → correct SVG casing.
static const std::unordered_map<std::string, std::string> kSvgTagCaseMap = {
    {"CLIPPATH", "clipPath"},
    {"LINEARGRADIENT", "linearGradient"},
    {"RADIALGRADIENT", "radialGradient"},
    {"TEXTPATH", "textPath"},
    {"FEBLEND", "feBlend"},
    {"FECOLORMATRIX", "feColorMatrix"},
    {"FECOMPONENTTRANSFER", "feComponentTransfer"},
    {"FECOMPOSITE", "feComposite"},
    {"FEDIFFUSELIGHTING", "feDiffuseLighting"},
    {"FEDISPLACEMENTMAP", "feDisplacementMap"},
    {"FEDISTANTLIGHT", "feDistantLight"},
    {"FEDROPSHADOW", "feDropShadow"},
    {"FEFLOOD", "feFlood"},
    {"FEFUNCA", "feFuncA"},
    {"FEFUNCB", "feFuncB"},
    {"FEFUNCG", "feFuncG"},
    {"FEFUNCR", "feFuncR"},
    {"FEGAUSSIANBLUR", "feGaussianBlur"},
    {"FEIMAGE", "feImage"},
    {"FEMERGE", "feMerge"},
    {"FEMERGENODE", "feMergeNode"},
    {"FEMORPHOLOGY", "feMorphology"},
    {"FEOFFSET", "feOffset"},
    {"FEPOINTLIGHT", "fePointLight"},
    {"FESPECULARLIGHTING", "feSpecularLighting"},
    {"FESPOTLIGHT", "feSpotLight"},
    {"FETILE", "feTile"},
    {"FETURBULENCE", "feTurbulence"},
    {"FOREIGNOBJECT", "foreignObject"},
    {"GLYPHREF", "glyphRef"},
    {"ALTGLYPH", "altGlyph"},
    {"ALTGLYPHDEF", "altGlyphDef"},
    {"ALTGLYPHITEM", "altGlyphItem"},
    {"ANIMATECOLOR", "animateColor"},
    {"ANIMATEMOTION", "animateMotion"},
    {"ANIMATETRANSFORM", "animateTransform"},
};

static std::string svgCorrectTagName(const std::string& upperTag) {
    auto it = kSvgTagCaseMap.find(upperTag);
    if (it != kSvgTagCaseMap.end()) return it->second;
    // Default: lowercase (works for svg, rect, circle, path, g, defs, etc.)
    std::string lower = upperTag;
    for (auto& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower;
}

// SVG attributes that need mixed-case (gumbo lowercases all attributes).
static const std::unordered_map<std::string, std::string> kSvgAttrCaseMap = {
    // Core SVG attributes
    {"viewbox", "viewBox"},
    {"preserveaspectratio", "preserveAspectRatio"},
    // Gradient attributes
    {"gradientunits", "gradientUnits"},
    {"gradienttransform", "gradientTransform"},
    {"spreadmethod", "spreadMethod"},
    // Pattern attributes
    {"patternunits", "patternUnits"},
    {"patterntransform", "patternTransform"},
    {"patterncontentunits", "patternContentUnits"},
    // Filter attributes
    {"filterunits", "filterUnits"},
    {"stddeviation", "stdDeviation"},
    {"basefrequency", "baseFrequency"},
    {"numoctaves", "numOctaves"},
    {"kernelunitlength", "kernelUnitLength"},
    {"surfacescale", "surfaceScale"},
    {"diffuseconstant", "diffuseConstant"},
    {"specularconstant", "specularConstant"},
    {"specularexponent", "specularExponent"},
    {"limitingconeangle", "limitingConeAngle"},
    {"pointsatx", "pointsAtX"},
    {"pointsaty", "pointsAtY"},
    {"pointsatz", "pointsAtZ"},
    {"xchannelselector", "xChannelSelector"},
    {"ychannelselector", "yChannelSelector"},
    {"tablevalues", "tableValues"},
    // Clip / Mask attributes
    {"clippathunits", "clipPathUnits"},
    {"maskunits", "maskUnits"},
    {"maskcontentunits", "maskContentUnits"},
    // Marker attributes
    {"markerunits", "markerUnits"},
    {"markerwidth", "markerWidth"},
    {"markerheight", "markerHeight"},
    {"refx", "refX"},
    {"refy", "refY"},
    // Text attributes
    {"startoffset", "startOffset"},
    {"textlength", "textLength"},
    {"lengthadjust", "lengthAdjust"},
    // Namespace prefixed
    {"xlink:href", "xlink:href"},
};

std::string Element::outerHTML() const {
    std::ostringstream oss;
    std::string serialized_tag = svgCorrectTagName(tag_);
    oss << "<" << serialized_tag;
    for (const auto& [key, val] : attributes_) {
        auto attrIt = kSvgAttrCaseMap.find(key);
        const std::string& attrName = (attrIt != kSvgAttrCaseMap.end()) ? attrIt->second : key;
        oss << " " << attrName << "=\"" << htmlEscapeAttr(val) << "\"";
    }
    // "style" is never stored in attributes_ (see setAttribute) — StyleProxy
    // is the sole source, so always serialize from it directly.
    {
        const std::string& css = style_.cssText();
        if (!css.empty()) {
            oss << " style=\"" << css << "\"";
        }
    }
    oss << ">";
    oss << innerHTML();
    oss << "</" << serialized_tag << ">";
    return oss.str();
}

// ---------------------------------------------------------------------------
// SVG serialization for the SkSVGDOM fallback renderer.
//
// Two Skia-specific transforms on top of plain outerHTML:
//  - Skia's SVG module only parses `xlink:href` (SkSVGUse/SkSVGGradient etc.),
//    so SVG2-style plain `href` attributes are renamed on the way out.
//  - Skia has no <symbol> node. A <use> whose target is a <symbol> is
//    expanded inline into the <svg> viewport the SVG spec defines for that
//    instantiation (x/y/width/height from the use, viewBox/preserveAspectRatio
//    from the symbol, children cloned). Bare <symbol> elements serialize to
//    nothing — they are invisible unless instantiated.
// ---------------------------------------------------------------------------

namespace {

bool svgTagReferencesHref(const std::string& lowerTag) {
    return lowerTag == "use" || lowerTag == "lineargradient" ||
           lowerTag == "radialgradient" || lowerTag == "pattern" ||
           lowerTag == "image" || lowerTag == "textpath" ||
           lowerTag == "mpath" || lowerTag == "feimage" || lowerTag == "filter";
}

std::string svgLowerTag(const Element* el) {
    std::string t = el->tagName();
    for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t;
}

const Element* findSvgElementById(const Element* root, const std::string& id) {
    if (root->getAttribute("id") == id) return root;
    for (const auto* child : root->children()) {
        if (const Element* found = findSvgElementById(child, id)) return found;
    }
    return nullptr;
}

const Element* resolveSvgHrefTarget(const Element* el, const Element* svgRoot) {
    std::string href = el->getAttribute("href");
    if (href.empty()) href = el->getAttribute("xlink:href");
    if (href.size() < 2 || href[0] != '#') return nullptr;
    return findSvgElementById(svgRoot, href.substr(1));
}

void serializeSvgAttrs(std::ostringstream& oss, const Element* el, bool renameHref) {
    for (const auto& [key, val] : el->attributes()) {
        auto attrIt = kSvgAttrCaseMap.find(key);
        std::string attrName = (attrIt != kSvgAttrCaseMap.end()) ? attrIt->second : key;
        if (renameHref && attrName == "href") attrName = "xlink:href";
        oss << " " << attrName << "=\"" << htmlEscapeAttr(val) << "\"";
    }
    const std::string& css = el->style().cssText();
    if (!css.empty()) oss << " style=\"" << css << "\"";
}

void serializeSvgNode(std::ostringstream& oss, const Element* el,
                      const Element* svgRoot, int depth) {
    if (depth > 16) return; // use/symbol reference cycle guard
    std::string lowerTag = svgLowerTag(el);

    // Invisible unless instantiated via <use>; Skia would drop it anyway.
    if (lowerTag == "symbol") return;

    if (lowerTag == "use") {
        const Element* target = resolveSvgHrefTarget(el, svgRoot);
        if (target && target != el && svgLowerTag(target) == "symbol") {
            // Instantiate the symbol as the <svg> viewport the spec defines.
            oss << "<svg";
            for (const char* a : {"x", "y", "width", "height", "transform"}) {
                const std::string& v = el->getAttribute(a);
                if (!v.empty()) oss << " " << a << "=\"" << htmlEscapeAttr(v) << "\"";
            }
            for (const char* a : {"viewBox", "preserveAspectRatio"}) {
                // gumbo case-adjusts these per the HTML5 SVG attribute table,
                // so camelCase is the stored spelling; try lowercase too for
                // attributes set through other paths.
                std::string v = target->getAttribute(a);
                if (v.empty()) {
                    std::string lower = a;
                    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    v = target->getAttribute(lower);
                }
                if (!v.empty()) oss << " " << a << "=\"" << htmlEscapeAttr(v) << "\"";
            }
            oss << ">";
            for (const auto* child : target->children()) {
                serializeSvgNode(oss, child, svgRoot, depth + 1);
            }
            oss << "</svg>";
            return;
        }
        // Plain <use>: serialize with the href spelling Skia understands.
    }

    std::string serializedTag = svgCorrectTagName(el->tagName());
    oss << "<" << serializedTag;
    serializeSvgAttrs(oss, el, svgTagReferencesHref(lowerTag));
    oss << ">";
    for (const auto& child : el->childNodes()) {
        if (child->nodeType() == NodeType::Text) {
            oss << static_cast<const TextNode*>(child)->data();
        } else if (child->nodeType() == NodeType::Element) {
            serializeSvgNode(oss, static_cast<const Element*>(child), svgRoot, depth + 1);
        }
    }
    oss << "</" << serializedTag << ">";
}

} // namespace

std::string serializeSvgForRenderer(const Element* svgRoot) {
    if (!svgRoot) return {};
    std::ostringstream oss;
    serializeSvgNode(oss, svgRoot, svgRoot, 0);
    return oss.str();
}

void Element::setInnerHTML(const std::string& html) {
    if (document_) {
        document_->parseInnerHTML(this, html);
        return;
    }
    auto oldKids = children_;
    for (auto& child : oldKids) {
        child->setParent(nullptr);
    }
    children_.clear();
    markDirty();
}

void Element::setOuterHTML(const std::string& html) {
    if (!parent_ || !document_) return;

    // Parse the new HTML into a temporary container
    auto* tempContainer = document_->createElement("DIV");
    document_->parseInnerHTML(tempContainer, html);

    // Insert all parsed children before this element in the parent
    auto newChildren = tempContainer->childNodes();
    for (auto* child : newChildren) {
        child->setParent(nullptr);
    }
    tempContainer->childNodes().clear();

    for (auto* child : newChildren) {
        parent_->insertBefore(child, this);
        if (child->nodeType() == NodeType::Element) {
            auto* childElem = static_cast<Element*>(child);
            childElem->setDocument(document_);
        }
    }

    // Unregister this element's ID before removal
    if (!id().empty()) {
        document_->unregisterElementId(id(), this);
    }

    // Remove this element from parent. Mark the parent, not this element: its
    // child list is the one that changed, and this element is about to leave the
    // tree — a mark left on it would never be read.
    Element* parentEl = parentElement();
    parent_->removeChild(this);
    if (parentEl) parentEl->markStructureDirty();
    else document_->markStructureDirty();

    // Free the temporary container
    document_->freeNode(tempContainer);
}

void Element::addEventListener(const std::string& type, uint64_t listenerId) {
    listeners_[type].push_back(listenerId);
}

void Element::removeEventListener(const std::string& type, uint64_t listenerId) {
    auto it = listeners_.find(type);
    if (it != listeners_.end()) {
        auto& vec = it->second;
        vec.erase(std::remove(vec.begin(), vec.end(), listenerId), vec.end());
        if (vec.empty()) {
            listeners_.erase(it);
        }
    }
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

Element* Element::parentElement() const {
    if (parent_ && parent_->nodeType() == NodeType::Element) {
        return static_cast<Element*>(parent_);
    }
    return nullptr;
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
// subtree is rendered by SkSVGDOM — none of them have layout nodes, so a
// structural mark left on one would never be consumed and the change would
// silently never reach the screen. The element layout stops at does have a node,
// and re-laying it out is what re-measures the control, so redirect there.
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

void Element::setInputControl(std::unique_ptr<layout::ElInput> ctrl) {
    inputControl_ = std::move(ctrl);
}

void Element::setTextareaControl(std::unique_ptr<layout::ElTextarea> ctrl) {
    textareaControl_ = std::move(ctrl);
}

void Element::setSelectControl(std::unique_ptr<layout::ElSelect> ctrl) {
    selectControl_ = std::move(ctrl);
}

void Element::setSvgControl(std::unique_ptr<layout::ElSvg> ctrl) {
    svgControl_ = std::move(ctrl);
}

void Element::setVideoControl(std::unique_ptr<layout::ElVideo> ctrl) {
    videoControl_ = std::move(ctrl);
}

} // namespace bro::dom
