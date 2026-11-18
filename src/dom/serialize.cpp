// Markup out of a tree (serialize.h): the HTML fragment serialization
// algorithm (HTML §13.3) behind innerHTML / outerHTML, and the XML
// serialization algorithm (DOM Parsing §3.2) behind XMLSerializer, written to
// give what Chromium gives: text escaped outside the raw-text elements, void
// elements without end tags, `&nbsp;` in HTML, namespace declarations and
// self-closing empty elements in XML.

#include "dom/serialize.h"
#include "dom/comment_node.h"
#include "dom/document.h"
#include "dom/document_type.h"
#include "dom/element.h"
#include "dom/text_node.h"

#include <cctype>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::dom {

namespace {

// HTML5 spec: SVG elements that require mixed-case tag names.
// Maps UPPERCASED tag → correct SVG casing.
const std::unordered_map<std::string, std::string> kSvgTagCaseMap = {
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

std::string svgCorrectTagName(const std::string& upperTag) {
    auto it = kSvgTagCaseMap.find(upperTag);
    if (it != kSvgTagCaseMap.end()) return it->second;
    // Default: lowercase (works for svg, rect, circle, path, g, defs, etc.)
    std::string lower = upperTag;
    for (auto& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower;
}

// SVG attributes that need mixed-case (gumbo lowercases all attributes).
const std::unordered_map<std::string, std::string> kSvgAttrCaseMap = {
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


std::string lowerAscii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const std::string& attrSpelling(const std::string& key) {
    auto it = kSvgAttrCaseMap.find(key);
    return it != kSvgAttrCaseMap.end() ? it->second : key;
}

// HTML: & and U+00A0 in text and attributes, < > in both (Chromium escapes
// them in attribute values too), " in attributes.
void escapeHtml(std::string& out, const std::string& s, bool attr) {
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\xC2' && i + 1 < s.size() && s[i + 1] == '\xA0') {
            out += "&nbsp;";
            ++i;
            continue;
        }
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"':
                if (attr) out += "&quot;";
                else out += c;
                break;
            default: out += c; break;
        }
    }
}

// XML: & < > in text; & " < > and the whitespace controls in attributes.
void escapeXml(std::string& out, const std::string& s, bool attr) {
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"':
                if (attr) out += "&quot;";
                else out += c;
                break;
            case '\t':
                if (attr) out += "&#9;";
                else out += c;
                break;
            case '\n':
                if (attr) out += "&#10;";
                else out += c;
                break;
            case '\r':
                if (attr) out += "&#13;";
                else out += c;
                break;
            default: out += c; break;
        }
    }
}

bool isVoidElement(const std::string& lowerTag) {
    static const char* kVoid[] = {"area", "base", "basefont", "bgsound", "br", "col", "embed", "frame", "hr",
                                  "img", "input", "keygen", "link", "meta", "param", "source", "track", "wbr"};
    for (const char* v : kVoid)
        if (lowerTag == v) return true;
    return false;
}

bool isRawTextParent(const Node* parent) {
    if (!parent || parent->nodeType() != NodeType::Element) return false;
    const auto* el = static_cast<const Element*>(parent);
    if (el->ns() != Element::Namespace::HTML) return false;
    const std::string t = lowerAscii(el->tagName());
    return t == "style" || t == "script" || t == "xmp" || t == "iframe" || t == "noembed" || t == "noframes" ||
           t == "plaintext" || t == "noscript";
}

// The element's name as HTML serialization writes it.
std::string htmlName(const Element* el) {
    if (el->ns() == Element::Namespace::HTML) return lowerAscii(el->localName());
    const std::string q = el->qualifiedName();
    return q.empty() ? svgCorrectTagName(el->tagName()) : q;
}

// The children a serializer walks: a <template>'s content's.
const std::vector<Node*>& serializedChildren(const Node* node) {
    if (node->nodeType() == NodeType::Element) {
        const auto* el = static_cast<const Element*>(node);
        if (el->templateContent() && node->childNodes().empty()) return el->templateContent()->childNodes();
    }
    return node->childNodes();
}

void doctypeMarkup(std::string& out, const DocumentType* dt) {
    out += "<!DOCTYPE " + dt->name();
    if (!dt->publicId().empty()) out += " PUBLIC \"" + dt->publicId() + "\"";
    if (!dt->systemId().empty()) out += (dt->publicId().empty() ? " SYSTEM \"" : " \"") + dt->systemId() + "\"";
    out += ">";
}

void htmlNode(std::string& out, const Node* node);

void htmlChildren(std::string& out, const Node* node) {
    const bool raw = isRawTextParent(node);
    for (const Node* child : serializedChildren(node)) {
        if (child->nodeType() == NodeType::Text) {
            const auto& data = static_cast<const TextNode*>(child)->data();
            if (raw) out += data;
            else escapeHtml(out, data, false);
        } else {
            htmlNode(out, child);
        }
    }
}

void htmlNode(std::string& out, const Node* node) {
    switch (node->nodeType()) {
        case NodeType::Text:
            if (isRawTextParent(node->parentNode())) out += static_cast<const TextNode*>(node)->data();
            else escapeHtml(out, static_cast<const TextNode*>(node)->data(), false);
            return;
        case NodeType::Comment:
            out += "<!--" + static_cast<const CommentNode*>(node)->data() + "-->";
            return;
        case NodeType::DocumentType:
            doctypeMarkup(out, static_cast<const DocumentType*>(node));
            return;
        case NodeType::Element:
            break;
        default:
            htmlChildren(out, node);
            return;
    }
    const auto* el = static_cast<const Element*>(node);
    if (el->tagName() == "#DOCUMENT-FRAGMENT") {
        htmlChildren(out, node);
        return;
    }
    const std::string name = htmlName(el);
    out += "<" + name;
    for (const auto& [key, val] : el->attributes()) {
        out += " " + attrSpelling(key) + "=\"";
        escapeHtml(out, val, true);
        out += "\"";
    }
    // "style" never lives in attributes_ (Element::setAttribute routes it into
    // the StyleProxy); it is written from there.
    if (el->hasAttribute("style")) {
        out += " style=\"";
        escapeHtml(out, el->style().cssText(), true);
        out += "\"";
    }
    out += ">";
    if (el->ns() == Element::Namespace::HTML && isVoidElement(name)) return;
    htmlChildren(out, node);
    out += "</" + name + ">";
}

constexpr const char* kXhtmlNs = "http://www.w3.org/1999/xhtml";

// Prefix -> namespace declarations in scope.
using PrefixMap = std::vector<std::pair<std::string, std::string>>;

bool prefixBound(const PrefixMap& map, const std::string& prefix, const std::string& ns) {
    for (auto it = map.rbegin(); it != map.rend(); ++it)
        if (it->first == prefix) return it->second == ns;
    return false;
}

void xmlNode(std::string& out, const Node* node, const std::string& defaultNs, PrefixMap& prefixes);

void xmlElement(std::string& out, const Element* el, const std::string& defaultNs, PrefixMap& prefixes) {
    const Document* doc = el->document();
    std::string ns = el->namespaceURI();
    // An HTML element of an HTML document is in the XHTML namespace.
    if (el->ns() == Element::Namespace::HTML && !(doc && doc->isXml() && ns.empty())) ns = kXhtmlNs;
    const std::string prefix = el->prefix();
    const std::string local = el->ns() == Element::Namespace::HTML ? lowerAscii(el->localName()) : el->localName();
    const std::string name = prefix.empty() ? local : prefix + ":" + local;
    out += "<" + name;
    std::string childDefault = defaultNs;
    const size_t mark = prefixes.size();
    bool hasXmlnsAttr = false;
    for (const auto& [key, val] : el->attributes()) {
        if (key == "xmlns") hasXmlnsAttr = true;
        if (key.rfind("xmlns:", 0) == 0) prefixes.emplace_back(key.substr(6), val);
    }
    if (prefix.empty()) {
        if (ns != defaultNs && !hasXmlnsAttr) {
            out += " xmlns=\"";
            escapeXml(out, ns, true);
            out += "\"";
        }
        childDefault = ns;
    } else if (!prefixBound(prefixes, prefix, ns)) {
        out += " xmlns:" + prefix + "=\"";
        escapeXml(out, ns, true);
        out += "\"";
        prefixes.emplace_back(prefix, ns);
    }
    for (const auto& [key, val] : el->attributes()) {
        out += " " + attrSpelling(key) + "=\"";
        escapeXml(out, val, true);
        out += "\"";
    }
    if (el->hasAttribute("style")) {
        out += " style=\"";
        escapeXml(out, el->style().cssText(), true);
        out += "\"";
    }
    const auto& kids = serializedChildren(el);
    if (kids.empty()) {
        if (ns == kXhtmlNs) {
            if (isVoidElement(local)) out += " />";
            else out += "></" + name + ">";
        } else {
            out += "/>";
        }
    } else {
        out += ">";
        for (const Node* child : kids) xmlNode(out, child, childDefault, prefixes);
        out += "</" + name + ">";
    }
    prefixes.resize(mark);
}

void xmlNode(std::string& out, const Node* node, const std::string& defaultNs, PrefixMap& prefixes) {
    switch (node->nodeType()) {
        case NodeType::Text:
            escapeXml(out, static_cast<const TextNode*>(node)->data(), false);
            return;
        case NodeType::Comment:
            out += "<!--" + static_cast<const CommentNode*>(node)->data() + "-->";
            return;
        case NodeType::DocumentType:
            doctypeMarkup(out, static_cast<const DocumentType*>(node));
            return;
        case NodeType::Element: {
            const auto* el = static_cast<const Element*>(node);
            if (el->tagName() != "#DOCUMENT-FRAGMENT") {
                xmlElement(out, el, defaultNs, prefixes);
                return;
            }
            break;
        }
        default:
            break;
    }
    for (const Node* child : node->childNodes()) xmlNode(out, child, defaultNs, prefixes);
}

}  // namespace

std::string serializeHtmlChildren(const Node* node) {
    std::string out;
    if (node) htmlChildren(out, node);
    return out;
}

std::string serializeHtmlNode(const Node* node) {
    std::string out;
    if (node) htmlNode(out, node);
    return out;
}

std::string serializeXml(const Node* node, const Document* doc) {
    std::string out;
    PrefixMap prefixes;
    if (doc) {
        if (doc->doctype()) doctypeMarkup(out, doc->doctype());
        if (doc->documentElement()) xmlNode(out, doc->documentElement(), "", prefixes);
        return out;
    }
    if (node) xmlNode(out, node, "", prefixes);
    return out;
}

} // namespace bro::dom
