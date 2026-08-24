#include "js/html_interfaces.h"
#include "util/string_utils.h"
#include <unordered_map>
#include <vector>

namespace bro::js {


namespace {

// One interface object per entry: the JS-visible name, and the tags it covers.
// A tag with no entry gets HTMLElement, which is what the HTML spec says for
// unknown elements too. `<audio>` and `<video>` chain through HTMLMediaElement
// rather than straight to HTMLElement, because that is where the playback
// members live in every feature test that goes looking for them.
struct InterfaceDef {
    const char* name;
    const char* parent;              // nullptr => HTMLElement
    std::vector<const char*> tags;   // lowercase; may be empty for a pure base
};

const std::vector<InterfaceDef>& interfaceDefs() {
    static const std::vector<InterfaceDef> defs = {
        // Pure bases first — a child names its parent, so the parent must exist.
        { "HTMLMediaElement",         nullptr, {} },

        { "HTMLAnchorElement",        nullptr, { "a" } },
        { "HTMLAreaElement",          nullptr, { "area" } },
        { "HTMLAudioElement",         "HTMLMediaElement", { "audio" } },
        { "HTMLBRElement",            nullptr, { "br" } },
        { "HTMLBaseElement",          nullptr, { "base" } },
        { "HTMLBodyElement",          nullptr, { "body" } },
        { "HTMLButtonElement",        nullptr, { "button" } },
        { "HTMLCanvasElement",        nullptr, { "canvas" } },
        { "HTMLDListElement",         nullptr, { "dl" } },
        { "HTMLDataElement",          nullptr, { "data" } },
        { "HTMLDataListElement",      nullptr, { "datalist" } },
        { "HTMLDetailsElement",       nullptr, { "details" } },
        { "HTMLDialogElement",        nullptr, { "dialog" } },
        { "HTMLDivElement",           nullptr, { "div" } },
        { "HTMLEmbedElement",         nullptr, { "embed" } },
        { "HTMLFieldSetElement",      nullptr, { "fieldset" } },
        { "HTMLFormElement",          nullptr, { "form" } },
        { "HTMLHRElement",            nullptr, { "hr" } },
        { "HTMLHeadElement",          nullptr, { "head" } },
        { "HTMLHeadingElement",       nullptr, { "h1", "h2", "h3", "h4", "h5", "h6" } },
        { "HTMLHtmlElement",          nullptr, { "html" } },
        { "HTMLIFrameElement",        nullptr, { "iframe" } },
        { "HTMLImageElement",         nullptr, { "img" } },
        { "HTMLInputElement",         nullptr, { "input" } },
        { "HTMLLIElement",            nullptr, { "li" } },
        { "HTMLLabelElement",         nullptr, { "label" } },
        { "HTMLLegendElement",        nullptr, { "legend" } },
        { "HTMLLinkElement",          nullptr, { "link" } },
        { "HTMLMapElement",           nullptr, { "map" } },
        { "HTMLMenuElement",          nullptr, { "menu" } },
        { "HTMLMetaElement",          nullptr, { "meta" } },
        { "HTMLMeterElement",         nullptr, { "meter" } },
        { "HTMLModElement",           nullptr, { "del", "ins" } },
        { "HTMLOListElement",         nullptr, { "ol" } },
        { "HTMLObjectElement",        nullptr, { "object" } },
        { "HTMLOptGroupElement",      nullptr, { "optgroup" } },
        { "HTMLOptionElement",        nullptr, { "option" } },
        { "HTMLOutputElement",        nullptr, { "output" } },
        { "HTMLParagraphElement",     nullptr, { "p" } },
        { "HTMLPictureElement",       nullptr, { "picture" } },
        { "HTMLPreElement",           nullptr, { "pre" } },
        { "HTMLProgressElement",      nullptr, { "progress" } },
        { "HTMLQuoteElement",         nullptr, { "blockquote", "q" } },
        { "HTMLScriptElement",        nullptr, { "script" } },
        { "HTMLSelectElement",        nullptr, { "select" } },
        { "HTMLSlotElement",          nullptr, { "slot" } },
        { "HTMLSourceElement",        nullptr, { "source" } },
        { "HTMLSpanElement",          nullptr, { "span" } },
        { "HTMLStyleElement",         nullptr, { "style" } },
        { "HTMLTableCaptionElement",  nullptr, { "caption" } },
        { "HTMLTableCellElement",     nullptr, { "td", "th" } },
        { "HTMLTableColElement",      nullptr, { "col", "colgroup" } },
        { "HTMLTableElement",         nullptr, { "table" } },
        { "HTMLTableRowElement",      nullptr, { "tr" } },
        { "HTMLTableSectionElement",  nullptr, { "thead", "tbody", "tfoot" } },
        { "HTMLTemplateElement",      nullptr, { "template" } },
        { "HTMLTextAreaElement",      nullptr, { "textarea" } },
        { "HTMLTimeElement",          nullptr, { "time" } },
        { "HTMLTitleElement",         nullptr, { "title" } },
        { "HTMLTrackElement",         nullptr, { "track" } },
        { "HTMLUListElement",         nullptr, { "ul" } },
        { "HTMLVideoElement",         "HTMLMediaElement", { "video" } },
    };
    return defs;
}

struct Interfaces {
    // Prototype per lowercase tag. Strong refs, freed in cleanup.
    std::unordered_map<std::string, JSValue> byTag;
    // What a tag with no dedicated interface gets: HTMLElement.prototype. The
    // HTML spec gives every element in an HTML document an HTMLElement (an
    // unrecognised tag is HTMLUnknownElement, still an HTMLElement), and bro
    // parses HTML documents. SVG content is not carved out because bro has no
    // real SVGElement to carve it out to — dom_polyfills.js defines an empty
    // stub class that nothing has ever inherited from.
    JSValue defaultProto = JS_UNDEFINED;
    std::vector<JSValue> protos;    // everything to free
};

std::unordered_map<JSContext*, Interfaces*> s_interfaces;

/// An interface object is a constructor that cannot be called: `new
/// HTMLCanvasElement()` is a TypeError on the web, and the object exists to
/// carry a prototype and a name.
JSValue js_illegal_constructor(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

} // namespace


void installHtmlInterfaces(JSContext* ctx, JSClassID elementClassId)
{
    if (s_interfaces.count(ctx)) return;

    JSValue global = JS_GetGlobalObject(ctx);

    // HTMLElement is installed by custom_elements.cpp (user classes extend it
    // via super()), and its prototype already sits on Element.prototype. Reuse
    // it so there is one hierarchy rather than two that disagree.
    JSValue htmlProto = JS_UNDEFINED;
    JSValue htmlCtor = JS_GetPropertyStr(ctx, global, "HTMLElement");
    if (JS_IsFunction(ctx, htmlCtor)) {
        htmlProto = JS_GetPropertyStr(ctx, htmlCtor, "prototype");
    }
    JS_FreeValue(ctx, htmlCtor);
    if (!JS_IsObject(htmlProto)) {
        // No HTMLElement yet — sit directly on Element.prototype rather than
        // leaving the interfaces rootless.
        JS_FreeValue(ctx, htmlProto);
        htmlProto = JS_GetClassProto(ctx, elementClassId);
    }

    auto* ifs = new Interfaces();
    ifs->defaultProto = JS_DupValue(ctx, htmlProto);
    std::unordered_map<std::string, JSValue> byName;   // borrowed

    for (const auto& def : interfaceDefs()) {
        JSValue parentProto = htmlProto;
        if (def.parent) {
            auto it = byName.find(def.parent);
            if (it != byName.end()) parentProto = it->second;
        }

        JSValue proto = JS_NewObjectProto(ctx, parentProto);
        JSValue ctor = JS_NewCFunction2(ctx, js_illegal_constructor, def.name, 0,
                                        JS_CFUNC_constructor, 0);
        JS_SetPropertyStr(ctx, ctor, "prototype", JS_DupValue(ctx, proto));
        JS_SetPropertyStr(ctx, proto, "constructor", JS_DupValue(ctx, ctor));
        JS_SetPropertyStr(ctx, global, def.name, ctor);

        byName[def.name] = proto;
        ifs->protos.push_back(proto);            // owns this ref
        for (const char* tag : def.tags) {
            ifs->byTag[tag] = JS_DupValue(ctx, proto);
        }
    }

    JS_FreeValue(ctx, htmlProto);
    s_interfaces[ctx] = ifs;

    // Wrappers minted before this point still carry the bare Element prototype.
    // The DOM polyfills run during DomBindings::install — two steps before
    // interfaces can exist, since they need the HTMLElement that custom
    // elements create — and touching document.body there caches its wrapper
    // for good. Re-prototype whatever is already in the cache so the handful of
    // elements that predate the interfaces are not permanently second-class.
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (JS_IsObject(elemMap)) {
        JSPropertyEnum* props = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &count, elemMap,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < count; ++i) {
                JSValue wrapper = JS_GetProperty(ctx, elemMap, props[i].atom);
                if (JS_IsObject(wrapper)) {
                    // Read the tag through JS rather than unwrapping the opaque:
                    // the accessor already normalises, and this file stays clear
                    // of dom_bindings internals.
                    JSValue tagVal = JS_GetPropertyStr(ctx, wrapper, "tagName");
                    if (const char* tag = JS_ToCString(ctx, tagVal)) {
                        JSValue proto = htmlInterfaceProto(ctx, tag);
                        if (JS_IsObject(proto)) JS_SetPrototype(ctx, wrapper, proto);
                        JS_FreeCString(ctx, tag);
                    }
                    JS_FreeValue(ctx, tagVal);
                }
                JS_FreeValue(ctx, wrapper);
                JS_FreeAtom(ctx, props[i].atom);
            }
            js_free(ctx, props);
        }
    }
    JS_FreeValue(ctx, elemMap);

    JS_FreeValue(ctx, global);
}

JSValue htmlInterfaceProto(JSContext* ctx, const std::string& tagName) {
    auto it = s_interfaces.find(ctx);
    if (it == s_interfaces.end()) return JS_UNDEFINED;
    // Tags arrive from the parser uppercased and from createElement as given,
    // so normalise rather than storing both spellings.
    auto proto = it->second->byTag.find(bro::util::toLower(tagName));
    if (proto == it->second->byTag.end()) return it->second->defaultProto;
    return proto->second;
}

void cleanupHtmlInterfaces(JSContext* ctx) {
    auto it = s_interfaces.find(ctx);
    if (it == s_interfaces.end()) return;
    for (auto& [tag, val] : it->second->byTag) JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, it->second->defaultProto);
    for (auto& val : it->second->protos) JS_FreeValue(ctx, val);
    delete it->second;
    s_interfaces.erase(it);
}



} // namespace bro::js
