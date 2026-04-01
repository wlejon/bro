#include "js/dom_bindings_internal.h"
#include "js/custom_elements.h"
#include "js/image_bindings.h"

namespace bro::js {

// ===========================================================================
// Document wrapper
// ===========================================================================

static JSClassDef js_document_class = {
    "Document",
    nullptr, nullptr, nullptr, nullptr
};

bro::dom::Document* getDocument(JSValueConst val)
{
    return static_cast<bro::dom::Document*>(
        JS_GetOpaque(val, js_document_class_id));
}

static JSValue js_document_get_title(JSContext* ctx, JSValueConst this_val)
{
    auto* doc = getDocument(this_val);
    if (!doc) return JS_UNDEFINED;
    return JS_NewString(ctx, doc->title().c_str());
}

static JSValue js_document_set_title(JSContext* ctx, JSValueConst this_val,
                                     JSValueConst val)
{
    auto* doc = getDocument(this_val);
    if (!doc) return JS_UNDEFINED;
    doc->setTitle(jsToStdString(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_document_get_body(JSContext* ctx, JSValueConst this_val)
{
    auto* doc = getDocument(this_val);
    if (!doc) return JS_NULL;
    auto* body = doc->body();
    if (!body) return JS_NULL;
    return DomBindings::wrapElement(ctx, body);
}

static JSValue js_document_get_documentElement(JSContext* ctx,
                                               JSValueConst this_val)
{
    auto* doc = getDocument(this_val);
    if (!doc) return JS_NULL;
    auto* root = doc->documentElement();
    if (!root) return JS_NULL;
    return DomBindings::wrapElement(ctx, root);
}

static JSValue js_document_getElementById(JSContext* ctx,
                                          JSValueConst this_val,
                                          int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NULL;
    std::string id = jsToStdString(ctx, argv[0]);
    auto* el = doc->getElementById(id);
    if (!el) return JS_NULL;
    return DomBindings::wrapElement(ctx, el);
}

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

static JSValue js_document_createTextNode(JSContext* ctx,
                                          JSValueConst this_val,
                                          int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NULL;
    std::string text = jsToStdString(ctx, argv[0]);
    auto* textNode = doc->createTextNode(text);
    if (!textNode) return JS_NULL;
    return wrapAnyNode(ctx, textNode);
}

static JSValue js_document_createDocumentFragment(JSContext* ctx,
                                                  JSValueConst this_val,
                                                  int /*argc*/, JSValueConst* /*argv*/)
{
    auto* doc = getDocument(this_val);
    if (!doc) return JS_NULL;
    auto* el = doc->createElement("#DOCUMENT-FRAGMENT");
    if (!el) return JS_NULL;
    return DomBindings::wrapElement(ctx, el);
}

static JSValue js_document_querySelector(JSContext* ctx,
                                         JSValueConst this_val,
                                         int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NULL;
    std::string sel = jsToStdString(ctx, argv[0]);
    auto results = doc->querySelectorAll(sel);
    if (results.empty()) return JS_NULL;
    return DomBindings::wrapElement(ctx, results[0]);
}

static JSValue js_document_querySelectorAll(JSContext* ctx,
                                            JSValueConst this_val,
                                            int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NewArray(ctx);
    std::string sel = jsToStdString(ctx, argv[0]);
    auto results = doc->querySelectorAll(sel);
    return wrapNodeList(ctx, results);
}

static JSValue js_document_get_nodeType(JSContext* ctx, JSValueConst /*this_val*/)
{
    return JS_NewInt32(ctx, 9);
}

static JSValue js_document_get_nodeName(JSContext* ctx, JSValueConst /*this_val*/)
{
    return JS_NewString(ctx, "#document");
}

static JSValue js_document_createComment(JSContext* ctx,
                                         JSValueConst this_val,
                                         int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc) return JS_NULL;
    std::string text = (argc >= 1) ? jsToStdString(ctx, argv[0]) : "";
    auto* comment = doc->createComment(text);
    if (!comment) return JS_NULL;
    return wrapAnyNode(ctx, comment);
}

static JSValue js_document_getElementsByTagName(JSContext* ctx,
                                                JSValueConst this_val,
                                                int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NewArray(ctx);
    std::string tag = jsToStdString(ctx, argv[0]);
    auto results = doc->querySelectorAll(tag);
    return wrapNodeList(ctx, results);
}

static JSValue js_document_getElementsByClassName(JSContext* ctx,
                                                  JSValueConst this_val,
                                                  int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NewArray(ctx);
    std::string cls = jsToStdString(ctx, argv[0]);
    auto results = doc->querySelectorAll("." + cls);
    return wrapNodeList(ctx, results);
}

static JSValue js_document_getElementsByName(JSContext* ctx,
                                             JSValueConst this_val,
                                             int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return wrapNodeList(ctx, {});
    std::string name = jsToStdString(ctx, argv[0]);
    auto results = doc->querySelectorAll("[name=\"" + name + "\"]");
    return wrapNodeList(ctx, results);
}

static JSValue js_document_get_defaultView(JSContext* ctx, JSValueConst /*this_val*/)
{
    return JS_GetGlobalObject(ctx);
}

static JSValue js_document_get_activeElement(JSContext* ctx, JSValueConst this_val)
{
    auto* doc = getDocument(this_val);
    if (!doc) return JS_NULL;
    auto* el = doc->activeElement();
    if (!el) return JS_NULL;
    return DomBindings::wrapElement(ctx, el);
}

static const JSCFunctionListEntry js_document_proto_funcs[] = {
    // Properties
    JS_CGETSET_DEF("title",           js_document_get_title,           js_document_set_title),
    JS_CGETSET_DEF("body",            js_document_get_body,            nullptr),
    JS_CGETSET_DEF("documentElement", js_document_get_documentElement, nullptr),
    JS_CGETSET_DEF("nodeType",        js_document_get_nodeType,        nullptr),
    JS_CGETSET_DEF("nodeName",        js_document_get_nodeName,        nullptr),
    JS_CGETSET_DEF("defaultView",     js_document_get_defaultView,     nullptr),
    JS_CGETSET_DEF("activeElement",  js_document_get_activeElement,   nullptr),
    // Methods
    JS_CFUNC_DEF("getElementById",          1, js_document_getElementById),
    JS_CFUNC_DEF("createElement",           1, js_document_createElement),
    JS_CFUNC_DEF("createElementNS",         2, js_document_createElementNS),
    JS_CFUNC_DEF("createTextNode",          1, js_document_createTextNode),
    JS_CFUNC_DEF("createComment",           1, js_document_createComment),
    JS_CFUNC_DEF("createDocumentFragment",  0, js_document_createDocumentFragment),
    JS_CFUNC_DEF("querySelector",           1, js_document_querySelector),
    JS_CFUNC_DEF("querySelectorAll",        1, js_document_querySelectorAll),
    JS_CFUNC_DEF("getElementsByTagName",    1, js_document_getElementsByTagName),
    JS_CFUNC_DEF("getElementsByClassName",  1, js_document_getElementsByClassName),
    JS_CFUNC_DEF("getElementsByName",       1, js_document_getElementsByName),
};

// ===========================================================================
// Registration
// ===========================================================================

void registerDocumentClasses(JSRuntime* rt) {
    JS_NewClass(rt, js_document_class_id, &js_document_class);
}

void installDocumentPrototypes(JSContext* ctx) {
    JSValue doc_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, doc_proto, js_document_proto_funcs,
                               sizeof(js_document_proto_funcs) / sizeof(js_document_proto_funcs[0]));
    JS_SetClassProto(ctx, js_document_class_id, doc_proto);
}

} // namespace bro::js
