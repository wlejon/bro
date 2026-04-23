#include "dom/element.h"
#include "dom/document.h"
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
#include <sstream>
#include <unordered_map>

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

void Node::removeChild(Node* child) {
    if (!child) return;
    auto it = std::find(children_.begin(), children_.end(), child);
    if (it != children_.end()) {
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

Element::~Element() { magic_ = 0xDEAD; }

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
    for (auto& c : tag_) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
}

std::string Element::id() const {
    return getAttribute("id");
}

void Element::setId(const std::string& val) {
    setAttribute("id", val);
}

std::string Element::className() const {
    return getAttribute("class");
}

void Element::setClassName(const std::string& val) {
    setAttribute("class", val);
}

std::string Element::getAttribute(const std::string& name) const {
    auto it = attributes_.find(name);
    if (it != attributes_.end()) {
        return it->second;
    }
    return {};
}

bool Element::hasAttribute(const std::string& name) const {
    return attributes_.find(name) != attributes_.end();
}

void Element::setAttribute(const std::string& name, const std::string& val) {
    auto existing = attributes_.find(name);
    if (existing != attributes_.end() && existing->second == val) return;

    if (name == "id" && document_) {
        std::string oldId = getAttribute("id");
        if (!oldId.empty()) document_->unregisterElementId(oldId);
        if (!val.empty()) document_->registerElementId(val, this);
    }
    attributes_[name] = val;
    markDirty();
}

void Element::removeAttribute(const std::string& name) {
    if (name == "id" && document_) {
        std::string oldId = getAttribute("id");
        if (!oldId.empty()) document_->unregisterElementId(oldId);
    }
    attributes_.erase(name);
    markDirty();
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
    // Skip if text unchanged
    if (children_.size() == 1 && children_[0]->nodeType() == NodeType::Text) {
        auto* existing = static_cast<TextNode*>(children_[0]);
        if (existing->data() == text) return;
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
    if (attributes_.find("style") == attributes_.end()) {
        std::string css = style_.cssText();
        if (!css.empty()) {
            oss << " style=\"" << css << "\"";
        }
    }
    oss << ">";
    oss << innerHTML();
    oss << "</" << serialized_tag << ">";
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
        document_->unregisterElementId(id());
    }

    // Remove this element from parent
    parent_->removeChild(this);
    markStructureDirty();

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

std::vector<Element*> Element::querySelectorAll(const std::string& selector) {
    std::vector<Element*> result;
    auto selectors = htmlayout::css::parseSelectorList(selector);

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
    auto selectors = htmlayout::css::parseSelectorList(selector);

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
    auto selectors = htmlayout::css::parseSelectorList(selector);
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
        std::string cls = getAttribute("class");
        for (auto& rc : reqClasses) {
            bool found = false;
            std::istringstream iss(cls);
            std::string tok;
            while (iss >> tok) {
                if (tok == rc) { found = true; break; }
            }
            if (!found) return false;
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
    if (document_) {
        document_->markDirty();
    }
}

void Element::markStructureDirty() {
    dirty_ = true;
    if (document_) {
        document_->markStructureDirty();
    }
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
