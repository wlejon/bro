#include "js/dom_bindings_internal.h"
#include "js/custom_elements.h"
#include "js/image_bindings.h"
#include "engine/engine.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "util/string_utils.h"

#include <qjsbind/qjsbind.h>

namespace bro::js {

// ===========================================================================
// Document wrapper
// ===========================================================================

using Doc = bro::dom::Document;

bro::dom::Document* getDocument(JSValueConst val)
{
    return static_cast<bro::dom::Document*>(
        JS_GetOpaque(val, js_document_class_id));
}

// ---------------------------------------------------------------------------
// Complex methods requiring raw signatures
// ---------------------------------------------------------------------------

static JSValue js_document_createElement(JSContext* ctx,
                                         JSValueConst this_val,
                                         int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NULL;
    std::string tag = jsToStdString(ctx, argv[0]);
    // `<img>` is a real element here, not the standalone decode helper `new
    // Image()` hands back. The helper is not a Node: it has no tagName, no
    // nodeType, and appendChild silently drops it — so the ordinary way a page
    // builds an icon,
    //     const i = document.createElement('img'); i.src = …; btn.appendChild(i);
    // produced an image that never reached the document and never appeared,
    // with no error to say why. Returning an element makes `.src` reflect to
    // the attribute (js_element_video_set_src), which is what layout probes for
    // the intrinsic size and what the paint path loads. `new Image()` keeps its
    // own object, and getImagePixels now accepts both, so an <img> built this
    // way is still a valid drawImage / texImage2D source.
    auto* el = doc->createElement(tag);
    if (!el) return JS_NULL;
    JSValue ce = createCustomElement(ctx, el, tag);
    if (!JS_IsUndefined(ce)) return ce;
    return DomBindings::wrapElement(ctx, el);
}

static JSValue js_document_createElementNS(JSContext* ctx,
                                           JSValueConst this_val,
                                           int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 2) return JS_NULL;
    std::string tag = jsToStdString(ctx, argv[1]);
    // `<img>` goes through the ordinary element path, same as createElement —
    // the two name the same tag and must not hand back two different kinds of
    // object. three.js's own ImageLoader builds its images through *this* one.
    auto* el = doc->createElement(tag);
    if (!el) return JS_NULL;
    JSValue ce = createCustomElement(ctx, el, tag);
    if (!JS_IsUndefined(ce)) return ce;
    return DomBindings::wrapElement(ctx, el);
}

static JSValue js_document_importNode(JSContext* ctx,
                                      JSValueConst this_val,
                                      int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NULL;

    // importNode clones the node into this document
    auto* node = unwrapNode(ctx, argv[0]);
    if (!node) return JS_NULL;

    bool deep = (argc >= 2) ? JS_ToBool(ctx, argv[1]) : false;

    if (node->nodeType() == bro::dom::NodeType::Element) {
        auto* srcEl = static_cast<bro::dom::Element*>(node);
        auto* clone = doc->createElement(srcEl->tagName());
        if (!clone) return JS_NULL;

        for (auto& [name, val] : srcEl->attributes()) {
            clone->setAttribute(name, val);
        }
        // "style" lives in StyleProxy, not attributes_ (see Element::setAttribute).
        if (srcEl->hasAttribute("style")) {
            clone->setAttribute("style", srcEl->style().cssText());
        }

        if (deep) {
            JSValue srcWrapper = DomBindings::wrapElement(ctx, srcEl);
            for (auto* child : srcEl->childNodes()) {
                if (child->nodeType() == bro::dom::NodeType::Element) {
                    JSValue childWrapper = DomBindings::wrapElement(ctx, child);
                    JSValue deepArgs[2] = { childWrapper, JS_TRUE };
                    JSValue imported = js_document_importNode(ctx, this_val, 2, deepArgs);
                    auto* importedNode = unwrapNode(ctx, imported);
                    if (importedNode) clone->appendChild(importedNode);
                    JS_FreeValue(ctx, imported);
                    JS_FreeValue(ctx, childWrapper);
                } else if (child->nodeType() == bro::dom::NodeType::Text) {
                    auto* textNode = static_cast<bro::dom::TextNode*>(child);
                    auto* clonedText = doc->createTextNode(textNode->data());
                    if (clonedText) clone->appendChild(clonedText);
                } else if (child->nodeType() == bro::dom::NodeType::Comment) {
                    auto* commentNode = static_cast<bro::dom::CommentNode*>(child);
                    auto* clonedComment = doc->createComment(commentNode->data());
                    if (clonedComment) clone->appendChild(clonedComment);
                }
            }
            JS_FreeValue(ctx, srcWrapper);
        }

        return DomBindings::wrapElement(ctx, clone);
    } else if (node->nodeType() == bro::dom::NodeType::Text) {
        auto* textNode = static_cast<bro::dom::TextNode*>(node);
        auto* clone = doc->createTextNode(textNode->data());
        if (!clone) return JS_NULL;
        return wrapAnyNode(ctx, clone);
    } else if (node->nodeType() == bro::dom::NodeType::Comment) {
        auto* commentNode = static_cast<bro::dom::CommentNode*>(node);
        auto* clone = doc->createComment(commentNode->data());
        if (!clone) return JS_NULL;
        return wrapAnyNode(ctx, clone);
    }

    return JS_NULL;
}

static JSValue js_document_adoptNode(JSContext* ctx,
                                     JSValueConst this_val,
                                     int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_NULL;
    auto* node = unwrapNode(ctx, argv[0]);
    if (!node) return JS_NULL;
    // Real adoption: transfer ownership of the whole subtree into this
    // document. This used to only detach the node from its parent, leaving it
    // owned (and eventually destroyed) by a document it no longer belonged to.
    auto* doc = getDocument(this_val);
    if (!doc) return JS_NULL;
    doc->adoptNode(node);
    return JS_DupValue(ctx, argv[0]);
}

// Event listener delegation — forward to documentElement
static JSValue js_document_addEventListener(JSContext* ctx,
                                            JSValueConst this_val,
                                            int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 2) return JS_UNDEFINED;
    auto* root = doc->documentElement();
    if (!root) return JS_UNDEFINED;
    JSValue rootVal = DomBindings::wrapElement(ctx, root);
    JSAtom fn = JS_NewAtom(ctx, "addEventListener");
    JSValue result = JS_Invoke(ctx, rootVal, fn, argc, argv);
    JS_FreeAtom(ctx, fn);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, rootVal);
    return JS_UNDEFINED;
}

static JSValue js_document_removeEventListener(JSContext* ctx,
                                               JSValueConst this_val,
                                               int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 2) return JS_UNDEFINED;
    auto* root = doc->documentElement();
    if (!root) return JS_UNDEFINED;
    JSValue rootVal = DomBindings::wrapElement(ctx, root);
    JSAtom fn = JS_NewAtom(ctx, "removeEventListener");
    JSValue result = JS_Invoke(ctx, rootVal, fn, argc, argv);
    JS_FreeAtom(ctx, fn);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, rootVal);
    return JS_UNDEFINED;
}

// dispatchEvent delegates the same way, and it has to: addEventListener above
// stores the document's listeners on documentElement, so an event aimed at the
// document has to be dispatched there or none of them would run. Delegating
// also means the document gets the propagation order a bubbled event already
// takes — window's capture pass, the document's own listeners at target, then
// the bubble back out to window — rather than a second, nearly-identical order
// written out here that could drift from it.
//
// Consequence worth knowing: currentTarget inside the handler is the <html>
// element, not the document. That is already true of every event that reaches
// a document listener by bubbling, for the same reason.
static JSValue js_document_dispatchEvent(JSContext* ctx,
                                         JSValueConst this_val,
                                         int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_FALSE;
    auto* root = doc->documentElement();
    if (!root) return JS_FALSE;
    JSValue rootVal = DomBindings::wrapElement(ctx, root);
    JSAtom fn = JS_NewAtom(ctx, "dispatchEvent");
    // Element.dispatchEvent already answers !defaultPrevented, which is what
    // this returns too — so hand its result straight back rather than deciding
    // the same thing twice.
    JSValue result = JS_Invoke(ctx, rootVal, fn, argc, argv);
    JS_FreeAtom(ctx, fn);
    JS_FreeValue(ctx, rootVal);
    return result;
}

// document.elementFromPoint(x, y) / elementsFromPoint(x, y) — CSSOM View.
//
// The same hit test the engine runs for a real click, reached from JS. Layout
// libraries use it to measure things they cannot ask for directly: CodeMirror
// probes the element at the far edge of its scroller to work out the native
// scrollbar width, and a `document.elementFromPoint` that is simply undefined
// takes the whole editor down with a TypeError.
//
// Arguments are client coordinates (the space clientX/clientY and
// getBoundingClientRect speak); the engine hit-tests in document space, which
// is that plus the viewport scroll. A point outside the viewport returns null
// per spec, rather than falling back to the document element.
static bro::engine::Engine* engineForPointQuery(JSContext* ctx, JSValueConst this_val,
                                                float& docX, float& docY,
                                                double x, double y) {
    auto it = s_ctx_engines.find(ctx);
    if (it == s_ctx_engines.end() || !it->second) return nullptr;
    auto* engine = static_cast<bro::engine::Engine*>(it->second);
    // Only the engine's own document has boxes on screen to hit; a detached
    // DOMParser document is laid out nowhere.
    if (getDocument(this_val) != engine->document()) return nullptr;
    if (x < 0 || y < 0 ||
        x >= engine->contentWidth() || y >= engine->contentHeight()) return nullptr;
    engine->flushLayoutForRead(engine->document());
    docX = static_cast<float>(x);
    docY = static_cast<float>(y) + engine->viewportScrollY();
    return engine;
}

static JSValue js_document_elementFromPoint(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    double x = 0, y = 0;
    if (argc < 2 || JS_ToFloat64(ctx, &x, argv[0]) || JS_ToFloat64(ctx, &y, argv[1]))
        return JS_NULL;
    float docX = 0, docY = 0;
    auto* engine = engineForPointQuery(ctx, this_val, docX, docY, x, y);
    if (!engine) return JS_NULL;
    auto* hit = engine->hitTest(docX, docY);
    return hit ? DomBindings::wrapElement(ctx, hit) : JS_NULL;
}

// The spec's list is "every element the point lands in, topmost first". bro's
// hit test names the topmost one; the rest of the list is its ancestor chain,
// which is what callers walk it for. Elements merely *overlapped* by the hit —
// a lower sibling under a covering box — are not reported.
static JSValue js_document_elementsFromPoint(JSContext* ctx, JSValueConst this_val,
                                             int argc, JSValueConst* argv) {
    JSValue arr = JS_NewArray(ctx);
    double x = 0, y = 0;
    if (argc < 2 || JS_ToFloat64(ctx, &x, argv[0]) || JS_ToFloat64(ctx, &y, argv[1]))
        return arr;
    float docX = 0, docY = 0;
    auto* engine = engineForPointQuery(ctx, this_val, docX, docY, x, y);
    if (!engine) return arr;
    uint32_t i = 0;
    for (auto* el = engine->hitTest(docX, docY); el; el = el->parentElement()) {
        JS_SetPropertyUint32(ctx, arr, i++, DomBindings::wrapElement(ctx, el));
    }
    return arr;
}

static JSValue js_document_exitPointerLock(JSContext* ctx, JSValueConst /*this_val*/,
                                            int /*argc*/, JSValueConst* /*argv*/) {
    auto it = s_ctx_engines.find(ctx);
    if (it == s_ctx_engines.end() || !it->second) return JS_UNDEFINED;
    auto* engine = static_cast<bro::engine::Engine*>(it->second);
    engine->exitPointerLock();
    return JS_UNDEFINED;
}

// ===========================================================================
// DOMParser — parseFromString returns a REAL detached Document
// ===========================================================================
//
// The string is parsed by the same gumbo path as the app document into a
// fresh bro::dom::Document owned by JS (see wrapDetachedDocument for the
// lifetime contract). No layout, rendering, or engine attachment — it is a
// plain tree with the full Document/Element API surface. All mime types go
// through the HTML parser: bro has no XML parser, matching the previous
// polyfill's best-effort behavior for 'text/xml' / 'image/svg+xml'.
static JSValue js_domparser_parseFromString(JSContext* ctx, JSValueConst /*this_val*/,
                                            int argc, JSValueConst* argv)
{
    std::string html = argc >= 1 ? jsToStdString(ctx, argv[0]) : std::string();
    auto* doc = new bro::dom::Document();
    doc->parse(html);
    return wrapDetachedDocument(ctx, doc);
}

static JSValue js_domparser_ctor(JSContext* ctx, JSValueConst new_target,
                                 int /*argc*/, JSValueConst* /*argv*/)
{
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue obj = JS_IsObject(proto) ? JS_NewObjectProto(ctx, proto)
                                     : JS_NewObject(ctx);
    JS_FreeValue(ctx, proto);
    return obj;
}

// globalThis.Document exists for instanceof checks (both the realm document
// and DOMParser results are instances). Constructing one directly is not
// supported — browsers allow `new Document()`, but a bro Document is engine-
// or DOMParser-owned; throw the DOM's "Illegal constructor" instead.
static JSValue js_document_illegal_ctor(JSContext* ctx, JSValueConst /*new_target*/,
                                        int /*argc*/, JSValueConst* /*argv*/)
{
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

// ===========================================================================
// Registration
// ===========================================================================

void installDocumentBindings(JSContext* ctx) {
    qjsbind::Class<Doc>(ctx, "Document", qjsbind::NoGlobal | qjsbind::NoDestructor)
        // Properties
        .prop("title",
            [](Doc* d) -> std::string { return d->title(); },
            [](Doc* d, std::string val) { d->setTitle(val); })
        .get("body", [](Doc* d, JSContext* cx) -> JSValue {
            auto* body = d->body();
            return body ? DomBindings::wrapElement(cx, body) : JS_NULL;
        })
        .get("head", [](Doc* d, JSContext* cx) -> JSValue {
            auto* root = d->documentElement();
            if (!root) return JS_NULL;
            for (auto* child : root->childNodes()) {
                if (child->nodeType() != bro::dom::NodeType::Element) continue;
                auto* el = static_cast<bro::dom::Element*>(child);
                const auto& tag = el->tagName();
                if (tag == "HEAD" || tag == "head")
                    return DomBindings::wrapElement(cx, el);
            }
            return JS_NULL;
        })
        .get("URL", [](Doc*, JSContext* cx) -> JSValue {
            JSValue g = JS_GetGlobalObject(cx);
            JSValue loc = JS_GetPropertyStr(cx, g, "location");
            JS_FreeValue(cx, g);
            if (JS_IsUndefined(loc) || JS_IsNull(loc)) {
                JS_FreeValue(cx, loc);
                return JS_NewString(cx, "");
            }
            JSValue href = JS_GetPropertyStr(cx, loc, "href");
            JS_FreeValue(cx, loc);
            if (JS_IsString(href)) return href;
            JS_FreeValue(cx, href);
            return JS_NewString(cx, "");
        })
        .get("documentURI", [](Doc*, JSContext* cx) -> JSValue {
            JSValue g = JS_GetGlobalObject(cx);
            JSValue loc = JS_GetPropertyStr(cx, g, "location");
            JS_FreeValue(cx, g);
            if (JS_IsUndefined(loc) || JS_IsNull(loc)) {
                JS_FreeValue(cx, loc);
                return JS_NewString(cx, "");
            }
            JSValue href = JS_GetPropertyStr(cx, loc, "href");
            JS_FreeValue(cx, loc);
            if (JS_IsString(href)) return href;
            JS_FreeValue(cx, href);
            return JS_NewString(cx, "");
        })
        .get("documentElement", [](Doc* d, JSContext* cx) -> JSValue {
            auto* root = d->documentElement();
            return root ? DomBindings::wrapElement(cx, root) : JS_NULL;
        })
        .get("nodeType", [](Doc*) -> int { return 9; })
        .get("nodeName", [](Doc*) -> std::string { return "#document"; })
        .get("readyState", [](Doc*, JSContext* cx) -> std::string {
            // Real lifecycle value: "loading" while user scripts run (no
            // layout yet), then "interactive"/"complete". Apps gate DOM
            // measurement on this, so it must not claim "complete" early.
            auto it = s_ctx_engines.find(cx);
            if (it == s_ctx_engines.end() || !it->second) return "complete";
            auto* engine = static_cast<bro::engine::Engine*>(it->second);
            return engine->documentReadyState();
        })
        .get("defaultView", [](Doc*, JSContext* cx) -> JSValue {
            return JS_GetGlobalObject(cx);
        })
        .get("activeElement", [](Doc* d, JSContext* cx) -> JSValue {
            auto* el = d->activeElement();
            return el ? DomBindings::wrapElement(cx, el) : JS_NULL;
        })
        .get("pointerLockElement", [](Doc*, JSContext* cx) -> JSValue {
            auto it = s_ctx_engines.find(cx);
            if (it == s_ctx_engines.end() || !it->second) return JS_NULL;
            auto* engine = static_cast<bro::engine::Engine*>(it->second);
            auto* el = engine->pointerLockElement();
            return el ? DomBindings::wrapElement(cx, el) : JS_NULL;
        })
        // Editing commands. All three resolve the engine the same way the
        // other engine-backed members here do, and report the browser's
        // "unsupported / nothing to do" answer (false) when there is no
        // engine at all — a document parsed with no engine behind it can't
        // edit anything.
        .method("execCommand", [](Doc*, JSContext* cx, std::string name,
                                  JSValue showUI, JSValue value) -> bool {
            auto it = s_ctx_engines.find(cx);
            if (it == s_ctx_engines.end() || !it->second) return false;
            auto* engine = static_cast<bro::engine::Engine*>(it->second);
            // Both trailing arguments are optional in every browser, and
            // callers routinely pass only the name.
            const bool ui = JS_ToBool(cx, showUI) > 0;
            std::string arg;
            if (!JS_IsUndefined(value) && !JS_IsNull(value)) {
                if (const char* s = JS_ToCString(cx, value)) {
                    arg = s;
                    JS_FreeCString(cx, s);
                }
            }
            return engine->execCommand(name, ui, arg);
        })
        .method("queryCommandSupported", [](Doc*, JSContext* cx,
                                            std::string name) -> bool {
            auto it = s_ctx_engines.find(cx);
            if (it == s_ctx_engines.end() || !it->second) return false;
            return static_cast<bro::engine::Engine*>(it->second)
                ->queryCommandSupported(name);
        })
        .method("queryCommandEnabled", [](Doc*, JSContext* cx,
                                          std::string name) -> bool {
            auto it = s_ctx_engines.find(cx);
            if (it == s_ctx_engines.end() || !it->second) return false;
            return static_cast<bro::engine::Engine*>(it->second)
                ->queryCommandEnabled(name);
        })
        // Simple methods with auto conversion
        .method("getElementById", [](Doc* d, JSContext* cx, std::string id) -> JSValue {
            auto* el = d->getElementById(id);
            return el ? DomBindings::wrapElement(cx, el) : JS_NULL;
        })
        .method("createTextNode", [](Doc* d, JSContext* cx, std::string text) -> JSValue {
            auto* tn = d->createTextNode(text);
            return tn ? wrapAnyNode(cx, tn) : JS_NULL;
        })
        .method("createComment", [](Doc* d, JSContext* cx, std::string text) -> JSValue {
            auto* c = d->createComment(text);
            return c ? wrapAnyNode(cx, c) : JS_NULL;
        })
        // Legacy event construction: createEvent() makes an uninitialized
        // event, initEvent() names it, dispatchEvent() sends it. Superseded by
        // `new Event(...)` and deprecated for a decade, and still how a great
        // deal of shipped code builds its events — so it is the difference
        // between running such a page and throwing partway through building
        // its UI.
        //
        // Every interface name answers with the one Event type bro has. The
        // argument selected an IDL interface on the web, and the ones that
        // differ (MouseEvent's coordinates, KeyboardEvent's key) only carry
        // data a *synthetic* event has none of anyway: what the caller does
        // next is initEvent() and dispatch. An unknown name is still a
        // NotSupportedError, because a caller asking for an interface nobody
        // recognises has misspelled something.
        .method("createEvent", [](Doc*, JSContext* cx, std::string interfaceName) -> JSValue {
            std::string name = util::toLower(interfaceName);
            static const char* kKnown[] = {
                "event", "events", "htmlevents", "svgevents",
                "customevent", "uievent", "uievents",
                "mouseevent", "mouseevents", "keyboardevent", "keyevents",
                "touchevent", "focusevent", "inputevent", "wheelevent",
                "pointerevent", "dragevent", "compositionevent", "messageevent",
            };
            for (const char* k : kKnown) {
                if (name == k) return createUninitializedEvent(cx);
            }
            return JS_ThrowTypeError(
                cx, "NotSupportedError: document.createEvent('%s') — no such event "
                    "interface", interfaceName.c_str());
        })
        .method("createDocumentFragment", [](Doc* d, JSContext* cx) -> JSValue {
            // bro::dom::DocumentFragment extends Node, not Element, so the
            // generic node wrapper lacks appendChild / innerHTML / etc. that
            // callers expect. Fall back to an Element with a reserved tag
            // and patch its nodeType to 11 (DOCUMENT_FRAGMENT_NODE) in JS —
            // see element_bindings.cpp js_element_get_nodeType.
            auto* el = d->createElement("#DOCUMENT-FRAGMENT");
            return el ? DomBindings::wrapElement(cx, el) : JS_NULL;
        })
        .method("querySelector", [](Doc* d, JSContext* cx, std::string sel) -> JSValue {
            auto results = d->querySelectorAll(sel);
            return results.empty() ? JS_NULL : DomBindings::wrapElement(cx, results[0]);
        })
        .method("querySelectorAll", [](Doc* d, JSContext* cx, std::string sel) -> JSValue {
            return wrapNodeList(cx, d->querySelectorAll(sel));
        })
        .method("getElementsByTagName", [](Doc* d, JSContext* cx, std::string tag) -> JSValue {
            return wrapLiveHTMLCollection(cx, nullptr, d, tag);
        })
        .method("getElementsByClassName", [](Doc* d, JSContext* cx, std::string cls) -> JSValue {
            return wrapLiveHTMLCollection(cx, nullptr, d, "." + cls);
        })
        .method("getElementsByName", [](Doc* d, JSContext* cx, std::string name) -> JSValue {
            return wrapLiveHTMLCollection(cx, nullptr, d, "[name=\"" + name + "\"]");
        })
        // Complex methods — raw signatures
        .method_raw("createElement", js_document_createElement, 1)
        .method_raw("createElementNS", js_document_createElementNS, 2)
        .method_raw("importNode", js_document_importNode, 2)
        .method_raw("adoptNode", js_document_adoptNode, 1)
        .method_raw("addEventListener", js_document_addEventListener, 2)
        .method_raw("removeEventListener", js_document_removeEventListener, 2)
        .method_raw("dispatchEvent", js_document_dispatchEvent, 1)
        .method_raw("exitPointerLock", js_document_exitPointerLock, 0)
        .method_raw("elementFromPoint", js_document_elementFromPoint, 2)
        .method_raw("elementsFromPoint", js_document_elementsFromPoint, 2)
        .method("createRange", [](Doc* d, JSContext* cx) -> JSValue {
            auto* r = new bro::dom::Range();
            r->setDocument(d);
            return qjsbind::wrap<bro::dom::Range>(cx, r);
        })
        .method("getSelection", [](Doc* d, JSContext* cx) -> JSValue {
            return wrapSelection(cx, d->selection());
        });

    js_document_class_id = qjsbind::class_id<Doc>();

    JSValue global = JS_GetGlobalObject(ctx);

    // globalThis.DOMParser
    {
        JSValue proto = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, proto, "parseFromString",
            JS_NewCFunction(ctx, js_domparser_parseFromString, "parseFromString", 2));
        JSValue ctor = JS_NewCFunction2(ctx, js_domparser_ctor, "DOMParser", 0,
                                        JS_CFUNC_constructor, 0);
        JS_SetConstructor(ctx, ctor, proto);   // ctor.prototype / proto.constructor
        JS_FreeValue(ctx, proto);
        JS_SetPropertyStr(ctx, global, "DOMParser", ctor);
    }

    // globalThis.Document — prototype is the Document class proto, so
    // `document instanceof Document` and DOMParser results both hold.
    {
        JSValue ctor = JS_NewCFunction2(ctx, js_document_illegal_ctor, "Document", 0,
                                        JS_CFUNC_constructor, 0);
        JSValue proto = JS_GetClassProto(ctx, js_document_class_id);
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
        JS_SetPropertyStr(ctx, global, "Document", ctor);
    }

    JS_FreeValue(ctx, global);
}

} // namespace bro::js
