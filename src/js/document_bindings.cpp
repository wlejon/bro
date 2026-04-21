#include "js/dom_bindings_internal.h"
#include "js/custom_elements.h"
#include "js/image_bindings.h"
#include "engine/engine.h"

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
    if (tag == "img" || tag == "IMG")
        return ImageBindings::createImage(ctx);
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
    if (tag == "img" || tag == "IMG")
        return ImageBindings::createImage(ctx);
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
                                     JSValueConst /*this_val*/,
                                     int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_NULL;
    auto* node = unwrapNode(ctx, argv[0]);
    if (!node) return JS_NULL;
    auto* parent = node->parentNode();
    if (parent) parent->removeChild(node);
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

static JSValue js_document_exitPointerLock(JSContext* ctx, JSValueConst /*this_val*/,
                                            int /*argc*/, JSValueConst* /*argv*/) {
    auto it = s_ctx_engines.find(ctx);
    if (it == s_ctx_engines.end() || !it->second) return JS_UNDEFINED;
    auto* engine = static_cast<bro::engine::Engine*>(it->second);
    engine->exitPointerLock();
    return JS_UNDEFINED;
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
        .get("documentElement", [](Doc* d, JSContext* cx) -> JSValue {
            auto* root = d->documentElement();
            return root ? DomBindings::wrapElement(cx, root) : JS_NULL;
        })
        .get("nodeType", [](Doc*) -> int { return 9; })
        .get("nodeName", [](Doc*) -> std::string { return "#document"; })
        .get("readyState", [](Doc*) -> std::string { return "complete"; })
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
        .method_raw("exitPointerLock", js_document_exitPointerLock, 0);

    js_document_class_id = qjsbind::class_id<Doc>();
}

} // namespace bro::js
