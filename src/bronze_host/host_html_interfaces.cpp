#include "bronze_host/host_html_interfaces.h"
#include "bronze_host/host_canvas_path2d.h"
#include "bronze_host/host_shadow_dom.h"
#include "bronze_host/host_template.h"
#include "bronze_host/host_iframe.h"
#include "bronze_host/host_element_video.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_globals_internal.h"
#include "engine/engine.h"
#include "dom/document.h"

#include <cctype>
#include <string>
#include <unordered_map>

namespace bro::bronze_host {

void decorateElementProto(ObjectBuilder& b);
void installNodeTree(ObjectBuilder& b);
Value constructCustomElementBase(Value newObject);

namespace {

HostClass g_nodeClass;
HostClass g_documentClass;
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
HostClass g_htmlSpanElementClass;
HostClass g_htmlParagraphElementClass;
HostClass g_htmlTableElementClass;
HostClass g_htmlTableRowElementClass;
HostClass g_htmlTableCellElementClass;
HostClass g_htmlUListElementClass;
HostClass g_htmlLIElementClass;
HostClass g_htmlFormElementClass;
HostClass g_htmlIFrameElementClass;
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

// `new Option(text, value, defaultSelected, selected)`, the legacy factory:
// an <option> holding `text`, with `value` when one is given. Selectedness
// is the `selected` attribute here, so either flag sets it.
Value optionConstructor(Value, std::span<const Value> a) {
    auto* eng = hostEngine();
    if (!eng || !eng->document()) return ev::throwError("new Option(): the engine has no document");
    dom::Document* doc = eng->document();
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

Value audioConstructor(Value, std::span<const Value> a) {
    auto* eng = hostEngine();
    if (!eng || !eng->document()) return ev::throwError("new Audio(): the engine has no document");
    dom::Element* el = eng->document()->createElement("audio");
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
};
constexpr ExtraTagDef kExtraTags[] = {
    {"HTMLAreaElement", "area"},
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
    {"HTMLLinkElement", "link"},
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

// Non-HTML element namespaces: an inline <svg> subtree and <math> are Elements
// (every method lives on Element's prototype) branded by their namespace.
HostClass g_svgElementClass;
HostClass g_mathMLElementClass;
constexpr const char* kSvgTags[] = {
    "svg", "g", "path", "rect", "circle", "ellipse", "line", "polyline", "polygon",
    "text", "tspan", "textpath", "defs", "use", "symbol", "lineargradient",
    "radialgradient", "stop", "clippath", "mask", "pattern", "image", "foreignobject",
    "filter", "marker", "view", "switch", "desc", "metadata", "fecolormatrix",
    "fegaussianblur", "feblend", "feoffset", "femerge", "femergenode", "feflood",
    "fecomposite", "animate", "animatetransform", "set",
};

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

void installHtmlInterfaces() {
    static bool s_installed = false;
    if (s_installed) return;
    s_installed = true;

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
        auto* eng = hostEngine();
        if (!eng || !eng->document()) return ev::throwTypeError("No active document");
        auto* t = eng->document()->createTextNode(data);
        return hostNodeValue(t);
    }, nullptr);
    g_textClass.inherit(g_characterDataClass);

    // Comment
    g_commentClass.install("Comment", 1, [](Value, std::span<const Value> a) -> Value {
        std::string data = a.empty() ? "" : ev::toUtf8(a[0]);
        auto* eng = hostEngine();
        if (!eng || !eng->document()) return ev::throwTypeError("No active document");
        auto* c = eng->document()->createComment(data);
        return hostNodeValue(c);
    }, nullptr);
    g_commentClass.inherit(g_characterDataClass);

    // DocumentFragment
    g_documentFragmentClass.install("DocumentFragment", 0, [](Value, std::span<const Value>) -> Value {
        auto* eng = hostEngine();
        if (!eng || !eng->document()) return ev::throwTypeError("No active document");
        auto* f = eng->document()->createDocumentFragment();
        return hostNodeValue(f);
    }, nullptr);
    g_documentFragmentClass.inherit(g_nodeClass);

    // Document
    g_documentClass.install("Document", 0, illegalConstructor, decorateDocumentProto);
    g_documentClass.inherit(g_nodeClass);
    ev::registerGlobal("Document", g_documentClass.constructor());
    {
        ev::GlobalValue gt = ev::globalValue("globalThis");
        if (gt.found && !gt.value.isUndefined() && ev::isObject(gt.value)) {
            ev::setProperty(gt.value, "Document", g_documentClass.constructor());
        }
    }

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
        {g_htmlCanvasElementClass, "HTMLCanvasElement"},
        {g_htmlDivElementClass, "HTMLDivElement"},
        {g_htmlInputElementClass, "HTMLInputElement"},
        {g_htmlSelectElementClass, "HTMLSelectElement"},
        {g_htmlTextAreaElementClass, "HTMLTextAreaElement"},
        {g_htmlButtonElementClass, "HTMLButtonElement"},
        {g_htmlScriptElementClass, "HTMLScriptElement"},
        {g_htmlStyleElementClass, "HTMLStyleElement"},
        {g_htmlAnchorElementClass, "HTMLAnchorElement"},
        {g_htmlSpanElementClass, "HTMLSpanElement"},
        {g_htmlParagraphElementClass, "HTMLParagraphElement"},
        {g_htmlTableElementClass, "HTMLTableElement"},
        {g_htmlTableRowElementClass, "HTMLTableRowElement"},
        {g_htmlTableCellElementClass, "HTMLTableCellElement"},
        {g_htmlUListElementClass, "HTMLUListElement"},
        {g_htmlLIElementClass, "HTMLLIElement"},
        {g_htmlFormElementClass, "HTMLFormElement"},
        {g_htmlIFrameElementClass, "HTMLIFrameElement", decorateIFrameProto},
        {g_htmlHeadingElementClass, "HTMLHeadingElement"},
        {g_htmlDetailsElementClass, "HTMLDetailsElement", decorateDetailsProto},
        {g_htmlTemplateElementClass, "HTMLTemplateElement", decorateTemplateProto},
        {g_htmlDialogElementClass, "HTMLDialogElement", decorateDialogProto},
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
    // htmlInterfaceProto reads.
    g_extraTagByName = new std::unordered_map<std::string, const HostClass*>();
    for (size_t i = 0; i < sizeof(kExtraTags) / sizeof(kExtraTags[0]); ++i) {
        HostClass& cls = g_extraTagClasses[i];
        cls.install(kExtraTags[i].name, 0, illegalConstructor, nullptr);
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
    g_svgElementClass.install("SVGElement", 0, illegalConstructor, nullptr);
    g_svgElementClass.inherit(g_elementClass);
    g_mathMLElementClass.install("MathMLElement", 0, illegalConstructor, nullptr);
    g_mathMLElementClass.inherit(g_elementClass);
    for (const char* tag : kSvgTags) (*g_extraTagByName)[tag] = &g_svgElementClass;
    (*g_extraTagByName)["math"] = &g_mathMLElementClass;

    // 8. Platform brands
    g_canvas2DContextClass.install("CanvasRenderingContext2D", 0, illegalConstructor, nullptr);
    g_dataTransferClass.install("DataTransfer", 0, illegalConstructor, nullptr);
    g_audioDestinationNodeClass.install("AudioDestinationNode", 0, illegalConstructor, nullptr);
    installPath2DClass();
}

const HostClass& canvasRenderingContext2DHostClass() { return g_canvas2DContextClass; }
const HostClass& dataTransferHostClass() { return g_dataTransferClass; }
const HostClass& svgElementHostClass() { return g_svgElementClass; }

Value htmlInterfaceProto(const std::string& tagName) {
    std::string tag = tagName;
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
    if (tag == "span") return g_htmlSpanElementClass.prototype();
    if (tag == "p") return g_htmlParagraphElementClass.prototype();
    if (tag == "table") return g_htmlTableElementClass.prototype();
    if (tag == "tr") return g_htmlTableRowElementClass.prototype();
    if (tag == "td" || tag == "th") return g_htmlTableCellElementClass.prototype();
    if (tag == "ul") return g_htmlUListElementClass.prototype();
    if (tag == "li") return g_htmlLIElementClass.prototype();
    if (tag == "form") return g_htmlFormElementClass.prototype();
    if (tag == "iframe") return g_htmlIFrameElementClass.prototype();
    if (tag == "h1" || tag == "h2" || tag == "h3" ||
        tag == "h4" || tag == "h5" || tag == "h6") {
        return g_htmlHeadingElementClass.prototype();
    }
    if (tag == "option") return g_htmlOptionElementClass.prototype();
    if (tag == "details") return g_htmlDetailsElementClass.prototype();
    if (tag == "template") return g_htmlTemplateElementClass.prototype();
    if (tag == "dialog") return g_htmlDialogElementClass.prototype();
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
