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
    return wrapLiveHTMLCollection(ctx, nullptr, doc, tag);
}

static JSValue js_document_getElementsByClassName(JSContext* ctx,
                                                  JSValueConst this_val,
                                                  int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NewArray(ctx);
    std::string cls = jsToStdString(ctx, argv[0]);
    return wrapLiveHTMLCollection(ctx, nullptr, doc, "." + cls);
}

static JSValue js_document_getElementsByName(JSContext* ctx,
                                             JSValueConst this_val,
                                             int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return wrapNodeList(ctx, {});
    std::string name = jsToStdString(ctx, argv[0]);
    return wrapLiveHTMLCollection(ctx, nullptr, doc, "[name=\"" + name + "\"]");
}

static JSValue js_document_importNode(JSContext* ctx,
                                      JSValueConst this_val,
                                      int argc, JSValueConst* argv)
{
    auto* doc = getDocument(this_val);
    if (!doc || argc < 1) return JS_NULL;

    // importNode clones the node into this document
    // We delegate to cloneNode since we have a single document model
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
            // Recursively clone children via JS cloneNode
            JSValue srcWrapper = DomBindings::wrapElement(ctx, srcEl);
            JSValue trueVal = JS_TRUE;
            // We need a deep clone - build it manually
            for (auto* child : srcEl->childNodes()) {
                if (child->nodeType() == bro::dom::NodeType::Element) {
                    // Recursive import
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
    // adoptNode removes the node from its parent and returns it
    // In our single-document model, this is equivalent to removing from parent
    if (argc < 1) return JS_NULL;
    auto* node = unwrapNode(ctx, argv[0]);
    if (!node) return JS_NULL;
    auto* parent = node->parentNode();
    if (parent) parent->removeChild(node);
    return JS_DupValue(ctx, argv[0]);
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

// ---------------------------------------------------------------------------
// Event listener delegation — forward to documentElement
// ---------------------------------------------------------------------------

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
    JS_CFUNC_DEF("importNode",              2, js_document_importNode),
    JS_CFUNC_DEF("adoptNode",               1, js_document_adoptNode),
    JS_CFUNC_DEF("addEventListener",         2, js_document_addEventListener),
    JS_CFUNC_DEF("removeEventListener",      2, js_document_removeEventListener),
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
