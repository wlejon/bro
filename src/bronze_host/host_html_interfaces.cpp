#include "bronze_host/host_html_interfaces.h"
#include "bronze_host/host_shadow_dom.h"
#include "bronze_host/host_template.h"
#include "bronze_host/host_iframe.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_globals_internal.h"

#include <cctype>
#include <string>

namespace bro::bronze_host {

void decorateElementProto(ObjectBuilder& b);
void installNodeTree(ObjectBuilder& b);
Value constructCustomElementBase();

namespace {

HostClass g_nodeClass;
HostClass g_documentClass;
HostClass g_elementClass;
HostClass g_htmlElementClass;

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
HostClass g_htmlHtmlElementClass;
HostClass g_htmlBodyElementClass;

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

void installHtmlInterfaces() {
    static bool s_installed = false;
    if (s_installed) return;
    s_installed = true;

    // 1. Node
    g_nodeClass.install("Node", 0, illegalConstructor, decorateNodeProto);
    for (const auto& c : kNodeConstants) {
        g_nodeClass.setStatic(c.name, ev::fromDouble(c.val));
    }

    // Document
    g_documentClass.install("Document", 0, illegalConstructor, nullptr);
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
    g_elementClass.install("Element", 0, [](Value, std::span<const Value>) -> Value {
        return constructCustomElementBase();
    }, decorateElementProto);
    g_elementClass.inherit(g_nodeClass);

    // 3. HTMLElement
    g_htmlElementClass.install("HTMLElement", 0, [](Value, std::span<const Value>) -> Value {
        return constructCustomElementBase();
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
        {g_htmlOptionElementClass, "HTMLOptionElement"},
        {g_htmlTemplateElementClass, "HTMLTemplateElement", decorateTemplateProto},
        {g_htmlHtmlElementClass, "HTMLHtmlElement"},
        {g_htmlBodyElementClass, "HTMLBodyElement"},
    };

    for (auto& item : htmlTagClasses) {
        item.cls.install(item.name, 0, illegalConstructor, item.decorator);
        item.cls.inherit(g_htmlElementClass);
    }
}

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
    if (tag == "template") return g_htmlTemplateElementClass.prototype();
    if (tag == "html") return g_htmlHtmlElementClass.prototype();
    if (tag == "body") return g_htmlBodyElementClass.prototype();

    return g_htmlElementClass.prototype();
}

}  // namespace bro::bronze_host
