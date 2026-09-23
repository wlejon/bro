// Element text and markup: the reads (`textContent`, `innerHTML`,
// `outerHTML`), the writes that reparse or replace a subtree, and the SVG
// spelling rules the serializer has to apply on the way out.
//
// Split out of element.cpp, which had grown past the size this repo keeps its
// translation units to. Nothing here touches layout, style resolution or the
// event machinery; it is the string half of an Element and reads as one.

#include "dom/element.h"
#include "dom/comment_node.h"
#include "dom/document.h"
#include "dom/text_node.h"

#include <cctype>
#include <sstream>
#include <string>
#include <unordered_map>

namespace bro::dom {

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
            stickToBottomOnAppend(text);
            return;
        }
    }

    // Clear old children. Element children are DETACHED rather than destroyed
    // (Document::releaseChildrenPreservingElements says why); text and comment
    // nodes go.
    if (document_) {
        document_->releaseChildrenPreservingElements(this);
    } else {
        for (auto* child : children_) child->setParent(nullptr);
        children_.clear();
    }

    // Add text node
    if (!text.empty() && document_) {
        auto* textNode = document_->createTextNode(text);
        appendChild(textNode);
    }

    markDirty();
    markStructureDirty();
    stickToBottomOnAppend(text);
}

// A scroll container follows its content when text is written into it — the
// log / console / transcript pattern, which is the only reason
// `setScrollToBottom` exists. Clearing the element (textContent = "") is the
// OPPOSITE intent and must not stick, or a rebuild that empties and then
// re-appends lands pinned to the bottom of the old content.
void Element::stickToBottomOnAppend(const std::string& text) {
    if (text.empty()) return;
    auto it = computedStyle_.find("overflow-y");
    std::string ov = (it != computedStyle_.end()) ? it->second : std::string();
    if (ov.empty()) {
        auto o = computedStyle_.find("overflow");
        ov = (o != computedStyle_.end()) ? o->second : "visible";
    }
    if (ov == "visible" || ov == "initial") return;
    setScrollToBottom(true);
}

std::string Element::innerHTML() const {
    // A <template>'s markup is its content fragment's (HTML §4.12.3's
    // serialization rule), not its (always empty) child list.
    if (templateContent_ && children_.empty()) return templateContent_->innerHTML();
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
    // Setting a <template>'s innerHTML replaces its CONTENT's children: the
    // markup is parsed into the inert fragment, never into the template's own
    // child list, so it neither renders nor answers a document query.
    if (document_ && (tag_ == "TEMPLATE") && !isTemplateContent_) {
        if (!templateContent_) {
            setTemplateContent(document_->createElement("#DOCUMENT-FRAGMENT"));
        }
        document_->parseInnerHTML(templateContent_, html);
        return;
    }
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

    // Remove this element from parent. removeChild marks the PARENT — its child
    // list is the one that changed, and a mark left on this element would never
    // be read since it is leaving the tree.
    parent_->removeChild(this);

    // Free the temporary container
    document_->freeNode(tempContainer);
}

} // namespace bro::dom
