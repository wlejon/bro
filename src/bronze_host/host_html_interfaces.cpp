#include "bronze_host/host_html_interfaces.h"
#include "bronze_host/host_dom_lists.h"
#include "bronze_host/host_compartment.h"
#include "bronze_host/host_canvas_path2d.h"
#include "bronze_host/host_shadow_dom.h"
#include "bronze_host/host_template.h"
#include "bronze_host/host_iframe.h"
#include "bronze_host/host_appview.h"
#include "bronze_host/host_element_video.h"
#include "bronze_host/host_element_terminal.h"
#if BRO_WITH_REMOTE
#include "bronze_host/host_remote_view.h"
#endif
#include "bronze_host/host_builder.h"
#include "bronze_host/host_events.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_svg_interfaces.h"
#include "bronze_host/host_url_reflect.h"
#include "bronze_host/host_values.h"
#include "engine/engine.h"
#include "dom/document.h"
#include "dom/text_node.h"
#include "util/remote_asset.h"

#include <cctype>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

namespace bro::bronze_host {

void decorateElementProto(ObjectBuilder& b);
void installNodeTree(ObjectBuilder& b);
Value constructCustomElementBase(Value newObject);

namespace {

HostClass g_nodeClass;
HostClass g_documentClass;
HostClass g_htmlDocumentClass;
HostClass g_xmlDocumentClass;
HostClass g_documentTypeClass;
HostClass g_domImplementationClass;
HostClass g_elementClass;
HostClass g_htmlElementClass;
HostClass g_characterDataClass;
HostClass g_textClass;
HostClass g_commentClass;
HostClass g_documentFragmentClass;

HostClass g_htmlCanvasElementClass;
HostClass g_htmlDivElementClass;
HostClass g_htmlInputElementClass;
HostClass g_htmlSelectElementClass;
HostClass g_htmlTextAreaElementClass;
HostClass g_htmlButtonElementClass;
HostClass g_htmlScriptElementClass;
HostClass g_htmlStyleElementClass;
HostClass g_htmlAnchorElementClass;
HostClass g_htmlAreaElementClass;
HostClass g_htmlSpanElementClass;
HostClass g_htmlParagraphElementClass;
HostClass g_htmlTableElementClass;
HostClass g_htmlTableRowElementClass;
HostClass g_htmlTableCellElementClass;
HostClass g_htmlUListElementClass;
HostClass g_htmlLIElementClass;
HostClass g_htmlFormElementClass;
HostClass g_htmlIFrameElementClass;
HostClass g_htmlAppViewElementClass;
HostClass g_htmlHeadingElementClass;
HostClass g_htmlOptionElementClass;
HostClass g_htmlTemplateElementClass;
HostClass g_htmlDialogElementClass;
HostClass g_htmlHtmlElementClass;
HostClass g_htmlBodyElementClass;
HostClass g_htmlMediaElementClass;
HostClass g_htmlVideoElementClass;
HostClass g_htmlAudioElementClass;
HostClass g_audioClass;
HostClass g_htmlDetailsElementClass;
HostClass g_htmlTerminalElementClass;
HostClass g_htmlRemoteViewElementClass;

// `new Option(text, value, defaultSelected, selected)`, the legacy factory:
// an <option> holding `text`, with `value` when one is given. Selectedness
// is the `selected` attribute here, so either flag sets it.
Value optionConstructor(Value, std::span<const Value> a) {
    // The calling compartment's document, never another's.
    dom::Document* doc = currentHostDocument();
    if (!doc) return ev::throwError("new Option(): the engine has no document");
    dom::Element* el = doc->createElement("option");
    if (!el) return ev::throwError("new Option(): the document refused an <option>");
    if (!a.empty() && !a[0].isUndefined()) {
        std::string text = ev::toUtf8(a[0]);
        if (!text.empty()) el->appendChild(doc->createTextNode(text));
    }
    if (a.size() > 1 && !a[1].isUndefined()) el->setAttribute("value", ev::toUtf8(a[1]));
    const bool defaultSelected = a.size() > 2 && ev::toBool(a[2]);
    const bool selected = a.size() > 3 && ev::toBool(a[3]);
    if (defaultSelected || selected) el->setAttribute("selected", "");
    return hostElementValue(el);
}

// HTMLDetailsElement.open reflects the `open` attribute; the UA sheet shows
// the details' content only while it is present.
void decorateDetailsProto(ObjectBuilder& b) {
    b.accessor(
        "open",
        [](Value self, std::span<const Value>) -> Value {
            HostNodeState* st = hostNodeStateOfValue(self);
            return ev::fromBool(st && st->el && st->el->hasAttribute("open"));
        },
        [](Value self, std::span<const Value> a) -> Value {
            HostNodeState* st = hostNodeStateOfValue(self);
            if (!st || !st->el) return ev::undefined();
            if (ev::toBool(argAt(a, 0))) st->el->setAttribute("open", "");
            else st->el->removeAttribute("open");
            return ev::undefined();
        });
}

// HTMLScriptElement.src: the src attribute as an absolute URL, resolved
// against the document's base URL ("" without the attribute). Potree finds
// its own folder from document.currentScript.src.
void decorateScriptProto(ObjectBuilder& b) {
    b.accessor(
        "src",
        [](Value self, std::span<const Value>) -> Value {
            HostNodeState* st = hostNodeStateOfValue(self);
            if (!st || !st->el || !st->el->hasAttribute("src")) return ev::fromUtf8("");
            return ev::fromUtf8(util::resolveUrl(documentBaseUrl(st->el->document()), st->el->getAttribute("src")));
        },
        [](Value self, std::span<const Value> a) -> Value {
            ev::Persistent receiver(self);
            const std::string v = ev::toUtf8(argAt(a, 0));
            HostNodeState* st = hostNodeStateOfValue(receiver.get());
            if (st && st->el) st->el->setAttribute("src", v);
            return ev::undefined();
        });
    // text: the script's child text (jQuery's globalEval sets it); type: the
    // reflected attribute.
    b.accessor(
        "text",
        [](Value self, std::span<const Value>) -> Value {
            HostNodeState* st = hostNodeStateOfValue(self);
            if (!st || !st->el) return ev::fromUtf8("");
            std::string out;
            for (dom::Node* n : st->el->childNodes())
                if (n->nodeType() == dom::NodeType::Text) out += static_cast<dom::TextNode*>(n)->data();
            return ev::fromUtf8(out);
        },
        [](Value self, std::span<const Value> a) -> Value {
            ev::Persistent receiver(self);
            const std::string v = hostNullableString(argAt(a, 0));
            HostNodeState* st = hostNodeStateOfValue(receiver.get());
            if (st && st->el) st->el->setTextContent(v);
            return ev::undefined();
        });
    b.accessor(
        "type",
        [](Value self, std::span<const Value>) -> Value {
            HostNodeState* st = hostNodeStateOfValue(self);
            return ev::fromUtf8(st && st->el ? st->el->getAttribute("type") : std::string());
        },
        [](Value self, std::span<const Value> a) -> Value {
            ev::Persistent receiver(self);
            const std::string v = hostDomString(argAt(a, 0));
            HostNodeState* st = hostNodeStateOfValue(receiver.get());
            if (st && st->el) st->el->setAttribute("type", v);
            return ev::undefined();
        });
}

Value audioConstructor(Value, std::span<const Value> a) {
    dom::Document* doc = currentHostDocument();
    if (!doc) return ev::throwError("new Audio(): the engine has no document");
    dom::Element* el = doc->createElement("audio");
    if (!el) return ev::throwError("new Audio(): the document refused an <audio>");
    if (!a.empty() && !a[0].isUndefined()) {
        std::string src = ev::toUtf8(a[0]);
        el->setAttribute("src", src);
    }
    return hostElementValue(el);
}

// The rest of the per-tag HTML*Element interfaces: undecorated brands one
// level under HTMLElement, so `el instanceof HTMLLabelElement` and
// `el.constructor.name` answer what a library sniffing the element kind
// expects. Each names the tags it wraps, comma-separated.
struct ExtraTagDef {
    const char* name;
    const char* tags;
    void (*decorator)(ObjectBuilder&) = nullptr;
};
constexpr ExtraTagDef kExtraTags[] = {
    {"HTMLBRElement", "br"},
    {"HTMLBaseElement", "base"},
    {"HTMLDListElement", "dl"},
    {"HTMLDataElement", "data"},
    {"HTMLDataListElement", "datalist"},
    {"HTMLEmbedElement", "embed"},
    {"HTMLFieldSetElement", "fieldset"},
    {"HTMLHRElement", "hr"},
    {"HTMLHeadElement", "head"},
    {"HTMLLabelElement", "label"},
    {"HTMLLegendElement", "legend"},
    {"HTMLLinkElement", "link", decorateRelList},
    {"HTMLMapElement", "map"},
    {"HTMLMenuElement", "menu"},
    {"HTMLMetaElement", "meta"},
    {"HTMLMeterElement", "meter"},
    {"HTMLModElement", "del,ins"},
    {"HTMLOListElement", "ol"},
    {"HTMLObjectElement", "object"},
    {"HTMLOptGroupElement", "optgroup"},
    {"HTMLOutputElement", "output"},
    {"HTMLPictureElement", "picture"},
    {"HTMLPreElement", "pre"},
    {"HTMLProgressElement", "progress"},
    {"HTMLQuoteElement", "blockquote,q"},
    {"HTMLSlotElement", "slot"},
    {"HTMLSourceElement", "source"},
    {"HTMLTableCaptionElement", "caption"},
    {"HTMLTableColElement", "col,colgroup"},
    {"HTMLTableSectionElement", "thead,tbody,tfoot"},
    {"HTMLTimeElement", "time"},
    {"HTMLTitleElement", "title"},
    {"HTMLTrackElement", "track"},
    {"HTMLUnknownElement", ""},
};
HostClass g_extraTagClasses[sizeof(kExtraTags) / sizeof(kExtraTags[0])];
std::unordered_map<std::string, const HostClass*>* g_extraTagByName = nullptr;

// MathML elements are Elements branded by their namespace; the SVG
// interfaces are host_svg_interfaces.cpp.
HostClass g_mathMLElementClass;
// Platform brands for objects the host builds as plain objects — the 2D
// context, drag / clipboard dataTransfer, AudioDestinationNode — so
// `ctx instanceof CanvasRenderingContext2D` holds and the names exist.
HostClass g_canvas2DContextClass;
HostClass g_dataTransferClass;
HostClass g_audioDestinationNodeClass;

Value illegalConstructor(Value, std::span<const Value>) {
    return ev::throwTypeError("Illegal constructor");
}

struct ConstDef { const char* name; double val; };
static const ConstDef kNodeConstants[] = {
    {"ELEMENT_NODE", 1},
    {"ATTRIBUTE_NODE", 2},
    {"TEXT_NODE", 3},
    {"CDATA_SECTION_NODE", 4},
    {"ENTITY_REFERENCE_NODE", 5},
    {"ENTITY_NODE", 6},
    {"PROCESSING_INSTRUCTION_NODE", 7},
    {"COMMENT_NODE", 8},
    {"DOCUMENT_NODE", 9},
    {"DOCUMENT_TYPE_NODE", 10},
    {"DOCUMENT_FRAGMENT_NODE", 11},
    {"NOTATION_NODE", 12},
};

void decorateNodeProto(ObjectBuilder& b) {
    for (const auto& c : kNodeConstants) {
        b.set(c.name, ev::fromDouble(c.val));
    }
    installNodeTree(b);
}

}  // namespace

const HostClass& nodeHostClass() { return g_nodeClass; }
const HostClass& documentHostClass() { return g_documentClass; }
const HostClass& elementHostClass() { return g_elementClass; }
const HostClass& htmlElementHostClass() { return g_htmlElementClass; }
const HostClass& characterDataHostClass() { return g_characterDataClass; }
const HostClass& textHostClass() { return g_textClass; }
const HostClass& commentHostClass() { return g_commentClass; }
const HostClass& documentFragmentHostClass() { return g_documentFragmentClass; }
const HostClass& documentTypeHostClass() { return g_documentTypeClass; }
const HostClass& domImplementationHostClass() { return g_domImplementationClass; }

Value documentInterfaceProto(const dom::Document* doc) {
    return (doc && doc->isXml() ? g_xmlDocumentClass : g_htmlDocumentClass).prototype();
}

// to[name] = the own property descriptor of from[name], when it has one.
static void copyOwnProperty(Value fromIn, Value toIn, const char* name) {
    ev::Persistent from(fromIn), to(toIn);
    ev::GlobalValue objectG = ev::globalValue("Object");
    if (!objectG.found || !ev::isObject(objectG.value)) return;
    ev::Persistent object(objectG.value);
    ev::Persistent getDesc(ev::getProperty(object.get(), "getOwnPropertyDescriptor"));
    ev::Persistent define(ev::getProperty(object.get(), "defineProperty"));
    ev::Persistent key(ev::fromUtf8(name));
    if (!ev::isFunction(getDesc.get()) || !ev::isFunction(define.get())) return;
    const Value getArgs[2] = {from.get(), key.get()};
    ev::CallResult desc = ev::call(getDesc.get(), object.get(), std::span<const Value>(getArgs, 2));
    if (desc.thrown || !ev::isObject(desc.value)) return;
    ev::Persistent descP(desc.value);
    const Value defArgs[3] = {to.get(), key.get(), descP.get()};
    ev::call(define.get(), object.get(), std::span<const Value>(defArgs, 3));
}

void installHtmlInterfaces() {
    // Once per realm: each compartment has its own classes (HostClass keeps
    // one constructor and prototype per realm).
    if (!firstInRealm(&g_nodeClass)) return;

    // 1. Node
    g_nodeClass.install("Node", 0, illegalConstructor, decorateNodeProto);
    for (const auto& c : kNodeConstants) {
        g_nodeClass.setStatic(c.name, ev::fromDouble(c.val));
    }

    // CharacterData
    g_characterDataClass.install("CharacterData", 0, illegalConstructor, nullptr);
    g_characterDataClass.inherit(g_nodeClass);

    // Text
    g_textClass.install("Text", 1, [](Value, std::span<const Value> a) -> Value {
        std::string data = a.empty() ? "" : ev::toUtf8(a[0]);
        dom::Document* doc = currentHostDocument();
        if (!doc) return ev::throwTypeError("No active document");
        auto* t = doc->createTextNode(data);
        return hostNodeValue(t);
    }, nullptr);
    g_textClass.inherit(g_characterDataClass);

    // Comment
    g_commentClass.install("Comment", 1, [](Value, std::span<const Value> a) -> Value {
        std::string data = a.empty() ? "" : ev::toUtf8(a[0]);
        dom::Document* doc = currentHostDocument();
        if (!doc) return ev::throwTypeError("No active document");
        auto* c = doc->createComment(data);
        return hostNodeValue(c);
    }, nullptr);
    g_commentClass.inherit(g_characterDataClass);

    // DocumentFragment
    g_documentFragmentClass.install("DocumentFragment", 0, [](Value, std::span<const Value>) -> Value {
        dom::Document* doc = currentHostDocument();
        if (!doc) return ev::throwTypeError("No active document");
        auto* f = doc->createDocumentFragment();
        return hostNodeValue(f);
    }, nullptr);
    g_documentFragmentClass.inherit(g_nodeClass);

    // Document
    g_documentClass.install("Document", 0, documentConstructor, decorateDocumentProto);
    g_documentClass.inherit(g_nodeClass);
    g_htmlDocumentClass.install("HTMLDocument", 0, illegalConstructor, nullptr);
    g_htmlDocumentClass.inherit(g_documentClass);
    g_xmlDocumentClass.install("XMLDocument", 0, illegalConstructor, nullptr);
    g_xmlDocumentClass.inherit(g_documentClass);
    g_documentTypeClass.install("DocumentType", 0, illegalConstructor, decorateDocumentTypeProto);
    g_documentTypeClass.inherit(g_nodeClass);
    g_domImplementationClass.install("DOMImplementation", 0, illegalConstructor, nullptr);

    // ShadowRoot
    installShadowRootClass();

    // 2. Element
    g_elementClass.install("Element", 0, [](Value self_, std::span<const Value>) -> Value {
        return constructCustomElementBase(self_);
    }, decorateElementProto);
    g_elementClass.inherit(g_nodeClass);

    // 3. HTMLElement
    g_htmlElementClass.install("HTMLElement", 0, [](Value self_, std::span<const Value>) -> Value {
        return constructCustomElementBase(self_);
    }, nullptr);
    g_htmlElementClass.inherit(g_elementClass);

    // 4. Per-tag interfaces extending HTMLElement
    struct TagClassInit {
        HostClass& cls;
        const char* name;
        void (*decorator)(ObjectBuilder&) = nullptr;
    };
    TagClassInit htmlTagClasses[] = {
        {g_htmlCanvasElementClass, "HTMLCanvasElement", decorateCanvasProto},
        {g_htmlDivElementClass, "HTMLDivElement"},
        {g_htmlInputElementClass, "HTMLInputElement", decorateInputProto},
        {g_htmlSelectElementClass, "HTMLSelectElement"},
        {g_htmlTextAreaElementClass, "HTMLTextAreaElement"},
        {g_htmlButtonElementClass, "HTMLButtonElement"},
        {g_htmlScriptElementClass, "HTMLScriptElement", decorateScriptProto},
        {g_htmlStyleElementClass, "HTMLStyleElement"},
        {g_htmlAnchorElementClass, "HTMLAnchorElement", decorateHyperlinkProto},
        {g_htmlAreaElementClass, "HTMLAreaElement", decorateHyperlinkProto},
        {g_htmlSpanElementClass, "HTMLSpanElement"},
        {g_htmlParagraphElementClass, "HTMLParagraphElement"},
        {g_htmlTableElementClass, "HTMLTableElement"},
        {g_htmlTableRowElementClass, "HTMLTableRowElement"},
        {g_htmlTableCellElementClass, "HTMLTableCellElement"},
        {g_htmlUListElementClass, "HTMLUListElement"},
        {g_htmlLIElementClass, "HTMLLIElement"},
        {g_htmlFormElementClass, "HTMLFormElement"},
        {g_htmlIFrameElementClass, "HTMLIFrameElement", decorateIFrameProto},
        {g_htmlAppViewElementClass, "HTMLAppViewElement", decorateAppViewProto},
        {g_htmlHeadingElementClass, "HTMLHeadingElement"},
        {g_htmlDetailsElementClass, "HTMLDetailsElement", decorateDetailsProto},
        {g_htmlTemplateElementClass, "HTMLTemplateElement", decorateTemplateProto},
        {g_htmlDialogElementClass, "HTMLDialogElement", decorateDialogProto},
        {g_htmlTerminalElementClass, "HTMLTerminalElement", decorateTerminalProto},
#if BRO_WITH_REMOTE
        {g_htmlRemoteViewElementClass, "HTMLRemoteViewElement", decorateRemoteViewProto},
#else
        {g_htmlRemoteViewElementClass, "HTMLRemoteViewElement"},
#endif
        {g_htmlHtmlElementClass, "HTMLHtmlElement"},
        {g_htmlBodyElementClass, "HTMLBodyElement"},
    };

    for (auto& item : htmlTagClasses) {
        item.cls.install(item.name, 0, illegalConstructor, item.decorator);
        item.cls.inherit(g_htmlElementClass);
    }
    // `Option` is HTMLOptionElement's legacy factory, the way `Image` is
    // HTMLImageElement's: one constructor under both names, so
    // `new Option() instanceof HTMLOptionElement` and `Option.prototype`
    // is the interface prototype.
    g_htmlOptionElementClass.install("HTMLOptionElement", 0, optionConstructor, nullptr);
    g_htmlOptionElementClass.alias("Option");
    g_htmlOptionElementClass.inherit(g_htmlElementClass);

    // 5. The media family is one level deeper: <video> and <audio> are
    // HTMLMediaElements, which is the interface a player library tests for
    // (`el instanceof HTMLMediaElement`) before it reads currentTime.
    // The playback surface (src, currentTime, play, readyState, ...) is
    // host_element_video.cpp; the video-only half (videoWidth, stepFrame,
    // frameRate) sits on HTMLVideoElement.
    g_htmlMediaElementClass.install("HTMLMediaElement", 0, illegalConstructor, decorateMediaProto);
    g_htmlMediaElementClass.inherit(g_htmlElementClass);
    g_htmlVideoElementClass.install("HTMLVideoElement", 0, illegalConstructor, decorateVideoProto);
    g_htmlVideoElementClass.inherit(g_htmlMediaElementClass);
    g_htmlAudioElementClass.install("HTMLAudioElement", 0, illegalConstructor, nullptr);
    g_htmlAudioElementClass.inherit(g_htmlMediaElementClass);
    g_audioClass.install("Audio", 1, audioConstructor, nullptr);
    g_audioClass.inherit(g_htmlAudioElementClass);

    // 6. The remaining per-tag brands, and the tag -> class map
    // elementInterfaceProto reads.
    g_extraTagByName = new std::unordered_map<std::string, const HostClass*>();
    for (size_t i = 0; i < sizeof(kExtraTags) / sizeof(kExtraTags[0]); ++i) {
        HostClass& cls = g_extraTagClasses[i];
        cls.install(kExtraTags[i].name, 0, illegalConstructor, kExtraTags[i].decorator);
        cls.inherit(g_htmlElementClass);
        std::string tags = kExtraTags[i].tags;
        size_t start = 0;
        while (start < tags.size()) {
            size_t comma = tags.find(',', start);
            if (comma == std::string::npos) comma = tags.size();
            std::string tag = tags.substr(start, comma - start);
            if (!tag.empty()) (*g_extraTagByName)[tag] = &cls;
            start = comma + 1;
        }
    }

    // 7. Non-HTML namespaces
    installSvgInterfaces();
    g_mathMLElementClass.install("MathMLElement", 0, illegalConstructor, nullptr);
    g_mathMLElementClass.inherit(g_elementClass);

    // 8. Platform brands
    g_canvas2DContextClass.install("CanvasRenderingContext2D", 0, illegalConstructor, nullptr);
    g_dataTransferClass.install("DataTransfer", 0, illegalConstructor, nullptr);
    g_audioDestinationNodeClass.install("AudioDestinationNode", 0, illegalConstructor, nullptr);
    installPath2DClass();

    // 9. The list interfaces (NodeList, HTMLCollection and its HTML kinds).
    installDomListInterfaces();

    // 10. The form controls' own members, where a page looks for them. They
    // live on Element.prototype (a <div>'s `value` is an expando there), and
    // the controls' prototypes carry the same accessors as own properties, as
    // in a browser: React reads Object.getOwnPropertyDescriptor(
    // HTMLInputElement.prototype, 'value') to track an input's value and
    // calls its setter.
    const std::pair<const HostClass*, std::vector<const char*>> controls[] = {
        {&g_htmlInputElementClass, {"value", "checked", "selectionStart", "selectionEnd"}},
        {&g_htmlTextAreaElementClass, {"value", "selectionStart", "selectionEnd"}},
        {&g_htmlSelectElementClass, {"value", "selectedIndex"}},
        {&g_htmlButtonElementClass, {"value"}},
        {&g_htmlOptionElementClass, {"value"}},
    };
    for (const auto& [cls, names] : controls)
        for (const char* name : names) copyOwnProperty(g_elementClass.prototype(), cls->prototype(), name);
}

const HostClass& canvasRenderingContext2DHostClass() { return g_canvas2DContextClass; }
const HostClass& dataTransferHostClass() { return g_dataTransferClass; }

Value elementInterfaceProto(const dom::Element* el) {
    switch (el->ns()) {
        case dom::Element::Namespace::SVG: return svgInterfaceProto(el->tagName());
        case dom::Element::Namespace::MathML: return g_mathMLElementClass.prototype();
        case dom::Element::Namespace::HTML: break;
        default: return g_elementClass.prototype();
    }
    std::string tag = el->tagName();
    for (char& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (tag == "canvas") return g_htmlCanvasElementClass.prototype();
    if (tag == "img") return htmlImageElementClass().prototype();
    if (tag == "div") return g_htmlDivElementClass.prototype();
    if (tag == "input") return g_htmlInputElementClass.prototype();
    if (tag == "select") return g_htmlSelectElementClass.prototype();
    if (tag == "textarea") return g_htmlTextAreaElementClass.prototype();
    if (tag == "button") return g_htmlButtonElementClass.prototype();
    if (tag == "script") return g_htmlScriptElementClass.prototype();
    if (tag == "style") return g_htmlStyleElementClass.prototype();
    if (tag == "a") return g_htmlAnchorElementClass.prototype();
    if (tag == "area") return g_htmlAreaElementClass.prototype();
    if (tag == "span") return g_htmlSpanElementClass.prototype();
    if (tag == "p") return g_htmlParagraphElementClass.prototype();
    if (tag == "table") return g_htmlTableElementClass.prototype();
    if (tag == "tr") return g_htmlTableRowElementClass.prototype();
    if (tag == "td" || tag == "th") return g_htmlTableCellElementClass.prototype();
    if (tag == "ul") return g_htmlUListElementClass.prototype();
    if (tag == "li") return g_htmlLIElementClass.prototype();
    if (tag == "form") return g_htmlFormElementClass.prototype();
    if (tag == "iframe") return g_htmlIFrameElementClass.prototype();
    if (tag == "appview") return g_htmlAppViewElementClass.prototype();
    if (tag == "h1" || tag == "h2" || tag == "h3" ||
        tag == "h4" || tag == "h5" || tag == "h6") {
        return g_htmlHeadingElementClass.prototype();
    }
    if (tag == "option") return g_htmlOptionElementClass.prototype();
    if (tag == "details") return g_htmlDetailsElementClass.prototype();
    if (tag == "template") return g_htmlTemplateElementClass.prototype();
    if (tag == "dialog") return g_htmlDialogElementClass.prototype();
    if (tag == "terminal") return g_htmlTerminalElementClass.prototype();
    if (tag == "remoteview") return g_htmlRemoteViewElementClass.prototype();
    if (tag == "html") return g_htmlHtmlElementClass.prototype();
    if (tag == "body") return g_htmlBodyElementClass.prototype();
    if (tag == "video") return g_htmlVideoElementClass.prototype();
    if (tag == "audio") return g_audioClass.prototype();
    if (g_extraTagByName) {
        auto it = g_extraTagByName->find(tag);
        if (it != g_extraTagByName->end()) return it->second->prototype();
    }

    return g_htmlElementClass.prototype();
}

}  // namespace bro::bronze_host
