#include "js/dom_bindings_internal.h"
#include "dom/node_handle.h"
#include "dom/text_offsets.h"
#include "util/log.h"

#include <qjsbind/qjsbind.h>

namespace bro::js {

// ===========================================================================
// NodeList wrapper – wraps a vector<Element*>
// ===========================================================================

struct NodeListData {
    std::vector<bro::dom::Element*> elements;
};

static JSValue js_nodelist_item(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv)
{
    auto* data = qjsbind::unwrap<NodeListData>(ctx, this_val);
    if (!data || argc < 1) return JS_UNDEFINED;

    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    if (idx < 0 || static_cast<size_t>(idx) >= data->elements.size())
        return JS_NULL;

    return DomBindings::wrapElement(ctx, data->elements[static_cast<size_t>(idx)]);
}

JSValue wrapNodeList(JSContext* ctx,
                     const std::vector<bro::dom::Element*>& elems)
{
    auto* data = new NodeListData{elems};
    JSValue obj = qjsbind::wrap<NodeListData>(ctx, data);
    if (JS_IsException(obj)) return obj;

    // Also set indexed properties so nodeList[0] works.
    for (size_t i = 0; i < elems.size(); ++i) {
        JS_SetPropertyUint32(ctx, obj, static_cast<uint32_t>(i),
                             DomBindings::wrapElement(ctx, elems[i]));
    }

    return obj;
}

// ===========================================================================
// Live HTMLCollection — re-queries DOM on every access
// ===========================================================================

struct HTMLCollectionData {
    bro::dom::Element* root;     // element to search from (or nullptr for document)
    bro::dom::Document* doc;     // document to search from (when root is nullptr)
    std::string selector;
};

static std::vector<bro::dom::Element*> htmlcollection_query(HTMLCollectionData* data)
{
    if (!data) return {};
    if (data->root)
        return data->root->querySelectorAll(data->selector);
    // The collection holds a raw Document*; a document-scoped collection can
    // outlive its document (detached DOMParser docs, closed panels) — go
    // empty rather than dangle.
    if (data->doc && bro::dom::Document::isLiveDocument(data->doc))
        return data->doc->querySelectorAll(data->selector);
    return {};
}

static JSValue js_htmlcollection_length(JSContext* ctx, JSValueConst this_val)
{
    auto* data = qjsbind::unwrap<HTMLCollectionData>(ctx, this_val);
    if (!data) return JS_NewInt32(ctx, 0);
    auto results = htmlcollection_query(data);
    return JS_NewInt32(ctx, static_cast<int32_t>(results.size()));
}

static JSValue js_htmlcollection_item(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv)
{
    auto* data = qjsbind::unwrap<HTMLCollectionData>(ctx, this_val);
    if (!data || argc < 1) return JS_NULL;
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    auto results = htmlcollection_query(data);
    if (idx < 0 || static_cast<size_t>(idx) >= results.size())
        return JS_NULL;
    return DomBindings::wrapElement(ctx, results[static_cast<size_t>(idx)]);
}

// namedItem(name) — search by id or name attribute
static JSValue js_htmlcollection_namedItem(JSContext* ctx, JSValueConst this_val,
                                           int argc, JSValueConst* argv)
{
    auto* data = qjsbind::unwrap<HTMLCollectionData>(ctx, this_val);
    if (!data || argc < 1) return JS_NULL;
    std::string name = jsToStdString(ctx, argv[0]);
    auto results = htmlcollection_query(data);
    for (auto* el : results) {
        if (el->id() == name || el->getAttribute("name") == name)
            return DomBindings::wrapElement(ctx, el);
    }
    return JS_NULL;
}

JSValue wrapLiveHTMLCollection(JSContext* ctx, bro::dom::Element* root,
                               bro::dom::Document* doc,
                               const std::string& selector)
{
    auto* data = new HTMLCollectionData{root, doc, selector};
    JSValue obj = qjsbind::wrap<HTMLCollectionData>(ctx, data);
    if (JS_IsException(obj)) return obj;

    // Override length as a live getter
    JSValue getLen = JS_NewCFunction2(ctx, [](JSContext* cx,
        JSValueConst this_val, int, JSValueConst*) -> JSValue {
        return js_htmlcollection_length(cx, this_val);
    }, "get length", 0, JS_CFUNC_generic, 0);
    JSAtom lenAtom = JS_NewAtom(ctx, "length");
    JS_DefinePropertyGetSet(ctx, obj, lenAtom, getLen, JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, lenAtom);

    JS_SetPropertyStr(ctx, obj, "item",
        JS_NewCFunction(ctx, js_htmlcollection_item, "item", 1));
    JS_SetPropertyStr(ctx, obj, "namedItem",
        JS_NewCFunction(ctx, js_htmlcollection_namedItem, "namedItem", 1));

    // Set current indexed properties (will become stale, but length getter is live)
    auto results = htmlcollection_query(data);
    for (size_t i = 0; i < results.size(); ++i) {
        JS_SetPropertyUint32(ctx, obj, static_cast<uint32_t>(i),
                             DomBindings::wrapElement(ctx, results[i]));
    }

    return obj;
}

// ===========================================================================
// Generic Node wrapper (comment nodes, text nodes in tree ops)
// ===========================================================================

// A Text/Comment wrapper's opaque. Holding the raw Node* here is what used to
// dangle: the wrapper is cached strongly in __bro_node_map (never collected),
// while Document::freeNode + drainPendingFrees destroy the node underneath it —
// fireNodeFreed only ever invalidated *element* wrappers. Any later `.data`,
// `.length` or `.parentNode` then read a destroyed TextNode. So the opaque is
// now a generation-checked handle: it resolves to null once the node (or its
// whole document) is gone, and every accessor already takes a safe default on
// null, giving the same inert behaviour an invalidated element wrapper has.
//
// `unowned` covers the one path that allocates a node outside any document
// (splitText with no document in scope). Such nodes are never freed, so the
// raw pointer stays valid for the life of the runtime.
struct NodeRef {
    bro::dom::NodeHandle<bro::dom::Node> handle;
    bro::dom::Node* unowned = nullptr;

    bro::dom::Node* get() const { return unowned ? unowned : handle.get(); }
};

// ---- CharacterData offset domain -----------------------------------------
// TextNode/CommentNode store data as UTF-8 and index it by BYTE; the DOM spec
// indexes CharacterData by UTF-16 code unit (`node.substringData(i, n)` must
// equal `node.data.substr(i, n)` in the JS string domain). Every offset and
// count crossing this binding converts — see dom/text_offsets.h.

// The (offset, count) pair of substringData/deleteData/replaceData, converted
// to a byte [start, start+len) span. Widened to int64 first so a caller's
// `count = 2^31-1` (idiomatic "to the end") can't overflow the addition.
static void cdSpanToBytes(const std::string& data, int32_t off, int32_t cnt,
                          int& outStart, int& outLen) {
    const int64_t end64 = static_cast<int64_t>(off) + static_cast<int64_t>(cnt);
    const int u16Len = bro::dom::utf16Length(data);
    const int endU16 = static_cast<int>(std::clamp<int64_t>(end64, 0, u16Len));
    outStart = bro::dom::utf16ToUtf8Byte(data, off);
    const int byteEnd = bro::dom::utf16ToUtf8Byte(data, endU16);
    outLen = byteEnd > outStart ? byteEnd - outStart : 0;
}

static void js_node_finalizer(JSRuntime* /*rt*/, JSValue val)
{
    delete static_cast<NodeRef*>(JS_GetOpaque(val, js_node_class_id));
}

static JSClassDef js_node_class = {
    "Node",
    js_node_finalizer,  // frees the NodeRef; the Node itself is Document-owned
    nullptr, nullptr, nullptr
};

// Resolve the Node behind a Text/Comment wrapper, or null if it has been freed.
// Every binding below funnels through this — never JS_GetOpaque directly.
static bro::dom::Node* nodeSelf(JSValueConst val)
{
    auto* ref = static_cast<NodeRef*>(JS_GetOpaque(val, js_node_class_id));
    return ref ? ref->get() : nullptr;
}

bro::dom::Node* unwrapNode(JSContext* ctx, JSValueConst val)
{
    (void)ctx;
    void* ptr = JS_GetOpaque(val, js_element_class_id);
    if (ptr) return static_cast<bro::dom::Node*>(static_cast<bro::dom::Element*>(ptr));
    return nodeSelf(val);
}

// Re-point a Text/Comment wrapper's handle after the node was adopted into
// another document. The handle names the document it was created against, so
// without this an adopted (very much alive) node's wrapper would resolve to
// null the moment ownership moved.
void repointNodeWrapper(JSContext* ctx, bro::dom::Node* node,
                        bro::dom::Document* newDoc)
{
    if (!node) return;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue nodeMap = JS_GetPropertyStr(ctx, global, "__bro_node_map");
    if (!JS_IsUndefined(nodeMap) && !JS_IsNull(nodeMap)) {
        std::string key = std::to_string(node->nodeId());
        JSValue wrapper = JS_GetPropertyStr(ctx, nodeMap, key.c_str());
        if (!JS_IsUndefined(wrapper) && !JS_IsNull(wrapper)) {
            if (auto* ref = static_cast<NodeRef*>(
                    JS_GetOpaque(wrapper, js_node_class_id))) {
                ref->unowned = nullptr;
                ref->handle.assign(newDoc, node);
            }
            JS_FreeValue(ctx, wrapper);
        }
    }
    JS_FreeValue(ctx, nodeMap);
    JS_FreeValue(ctx, global);
}

// Drop a freed Text/Comment node's cached wrapper: make it inert now (the
// handle would resolve to null anyway, but the entry must also leave the map or
// __bro_node_map grows without bound — every text node ever wrapped and
// replaced used to stay in it forever). Called from fireNodeFreed.
void invalidateNodeWrapper(JSContext* ctx, bro::dom::Node* node)
{
    if (!node) return;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue nodeMap = JS_GetPropertyStr(ctx, global, "__bro_node_map");
    if (!JS_IsUndefined(nodeMap) && !JS_IsNull(nodeMap)) {
        std::string key = std::to_string(node->nodeId());
        JSValue wrapper = JS_GetPropertyStr(ctx, nodeMap, key.c_str());
        if (!JS_IsUndefined(wrapper) && !JS_IsNull(wrapper)) {
            if (auto* ref = static_cast<NodeRef*>(
                    JS_GetOpaque(wrapper, js_node_class_id))) {
                ref->handle.reset();
                ref->unowned = nullptr;
            }
            JS_FreeValue(ctx, wrapper);
        }
        JSAtom atom = JS_NewAtom(ctx, key.c_str());
        JS_DeleteProperty(ctx, nodeMap, atom, 0);
        JS_FreeAtom(ctx, atom);
    }
    JS_FreeValue(ctx, nodeMap);
    JS_FreeValue(ctx, global);
}

JSValue wrapAnyNode(JSContext* ctx, bro::dom::Node* node)
{
    if (!node) return JS_NULL;
    if (node->nodeType() == bro::dom::NodeType::Element)
        return DomBindings::wrapElement(ctx, static_cast<bro::dom::Element*>(node));

    // Check existing wrapper in __bro_node_map
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue nodeMap = JS_GetPropertyStr(ctx, global, "__bro_node_map");
    if (JS_IsUndefined(nodeMap)) {
        nodeMap = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__bro_node_map", JS_DupValue(ctx, nodeMap));
    }
    std::string key = std::to_string(node->nodeId());
    JSValue existing = JS_GetPropertyStr(ctx, nodeMap, key.c_str());
    if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) {
        JS_FreeValue(ctx, nodeMap);
        JS_FreeValue(ctx, global);
        return existing;
    }
    JS_FreeValue(ctx, existing);

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_node_class_id));
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, nodeMap);
        JS_FreeValue(ctx, global);
        return obj;
    }
    {
        auto* ref = new NodeRef();
        if (auto* owner = node->document())
            ref->handle.assign(owner, node);
        else
            ref->unowned = node;
        JS_SetOpaque(obj, ref);
    }

    // Set basic DOM properties
    JS_SetPropertyStr(ctx, obj, "nodeType",
        JS_NewInt32(ctx, static_cast<int32_t>(node->nodeType())));
    JS_SetPropertyStr(ctx, obj, "nodeName",
        JS_NewString(ctx, node->nodeName().c_str()));

    // ownerDocument. This is a Node property, but only the Element wrapper had
    // it — Text/Comment/DocumentFragment wrappers answered `undefined`, which
    // made a correctly adopted text node look like adoption had not run. It
    // must be a live getter rather than a value set at wrap time: adoption
    // re-points the wrapper's handle at a different document, and a snapshot
    // taken here would keep naming the old one.
    {
        JSValue getOwnerDoc = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* nd = nodeSelf(this_val);
            if (!nd || !nd->document()) return JS_NULL;
            // Detached (DOMParser) nodes answer with THEIR Document wrapper,
            // not the realm's global document — same rule as Element.
            JSValue detached = detachedDocumentWrapper(cx, nd->document());
            if (!JS_IsNull(detached)) return detached;
            JSValue g = JS_GetGlobalObject(cx);
            JSValue d = JS_GetPropertyStr(cx, g, "document");
            JS_FreeValue(cx, g);
            return d;
        }, "get ownerDocument", 0, JS_CFUNC_generic, 0);
        JSAtom odAtom = JS_NewAtom(ctx, "ownerDocument");
        JS_DefinePropertyGetSet(ctx, obj, odAtom, getOwnerDoc, JS_UNDEFINED, 0);
        JS_FreeAtom(ctx, odAtom);
    }

    // -- CharacterData methods shared by Text and Comment nodes --
    auto installCharacterDataMethods = [&](JSValue obj, bro::dom::Node* n) {
        // length getter
        JSValue getLen = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* nd = nodeSelf(this_val);
            if (!nd) return JS_NewInt32(cx, 0);
            // UTF-16 code units, not the UTF-8 byte size of the storage.
            if (const std::string* d = bro::dom::characterDataOf(nd))
                return JS_NewInt32(cx, bro::dom::utf16Length(*d));
            return JS_NewInt32(cx, 0);
        }, "get length", 0, JS_CFUNC_generic, 0);
        JSAtom lenAtom = JS_NewAtom(ctx, "length");
        JS_DefinePropertyGetSet(ctx, obj, lenAtom, getLen, JS_UNDEFINED, 0);
        JS_FreeAtom(ctx, lenAtom);

        // data getter/setter
        JSValue getData = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* nd = nodeSelf(this_val);
            if (!nd) return JS_NULL;
            if (nd->nodeType() == bro::dom::NodeType::Text)
                return JS_NewString(cx, static_cast<bro::dom::TextNode*>(nd)->data().c_str());
            if (nd->nodeType() == bro::dom::NodeType::Comment)
                return JS_NewString(cx, static_cast<bro::dom::CommentNode*>(nd)->data().c_str());
            return JS_NULL;
        }, "get data", 0, JS_CFUNC_generic, 0);
        JSValue setData = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
            if (argc < 1) return JS_UNDEFINED;
            auto* nd = nodeSelf(this_val);
            if (!nd) return JS_UNDEFINED;
            const char* s = JS_ToCString(cx, argv[0]);
            std::string v(s ? s : "");
            if (s) JS_FreeCString(cx, s);
            std::string oldData;
            if (nd->nodeType() == bro::dom::NodeType::Text) {
                oldData = static_cast<bro::dom::TextNode*>(nd)->data();
                static_cast<bro::dom::TextNode*>(nd)->setData(v);
            } else if (nd->nodeType() == bro::dom::NodeType::Comment) {
                oldData = static_cast<bro::dom::CommentNode*>(nd)->data();
                static_cast<bro::dom::CommentNode*>(nd)->setData(v);
            }
            notifyMutationObservers(cx, this_val, "characterData",
                nullptr, oldData.c_str(), JS_NULL, JS_NULL);
            auto* parent = nd->parentNode();
            if (parent && parent->nodeType() == bro::dom::NodeType::Element) {
                auto* parentEl = static_cast<bro::dom::Element*>(parent);
                parentEl->markDirty();
                parentEl->markStructureDirty();
            }
            return JS_UNDEFINED;
        }, "set data", 1, JS_CFUNC_generic, 0);
        JSAtom dataAtom = JS_NewAtom(ctx, "data");
        JS_DefinePropertyGetSet(ctx, obj, dataAtom, getData, setData, 0);
        JS_FreeAtom(ctx, dataAtom);

        // substringData(offset, count)
        JS_SetPropertyStr(ctx, obj, "substringData",
            JS_NewCFunction(ctx, [](JSContext* cx, JSValueConst this_val,
                int argc, JSValueConst* argv) -> JSValue {
                auto* nd = nodeSelf(this_val);
                if (!nd || argc < 2) return JS_NewString(cx, "");
                int32_t off = 0, cnt = 0;
                JS_ToInt32(cx, &off, argv[0]);
                JS_ToInt32(cx, &cnt, argv[1]);
                const std::string* d = bro::dom::characterDataOf(nd);
                if (!d) return JS_NewString(cx, "");
                int bOff = 0, bLen = 0;
                cdSpanToBytes(*d, off, cnt, bOff, bLen);
                std::string result;
                if (nd->nodeType() == bro::dom::NodeType::Text)
                    result = static_cast<bro::dom::TextNode*>(nd)->substringData(bOff, bLen);
                else
                    result = static_cast<bro::dom::CommentNode*>(nd)->substringData(bOff, bLen);
                return JS_NewString(cx, result.c_str());
            }, "substringData", 2));

        // appendData(data)
        JS_SetPropertyStr(ctx, obj, "appendData",
            JS_NewCFunction(ctx, [](JSContext* cx, JSValueConst this_val,
                int argc, JSValueConst* argv) -> JSValue {
                auto* nd = nodeSelf(this_val);
                if (!nd || argc < 1) return JS_UNDEFINED;
                const char* s = JS_ToCString(cx, argv[0]);
                std::string v(s ? s : "");
                if (s) JS_FreeCString(cx, s);
                std::string oldData;
                if (nd->nodeType() == bro::dom::NodeType::Text) {
                    oldData = static_cast<bro::dom::TextNode*>(nd)->data();
                    static_cast<bro::dom::TextNode*>(nd)->appendData(v);
                } else if (nd->nodeType() == bro::dom::NodeType::Comment) {
                    oldData = static_cast<bro::dom::CommentNode*>(nd)->data();
                    static_cast<bro::dom::CommentNode*>(nd)->appendData(v);
                }
                notifyMutationObservers(cx, this_val, "characterData",
                    nullptr, oldData.c_str(), JS_NULL, JS_NULL);
                auto* parent = nd->parentNode();
                if (parent && parent->nodeType() == bro::dom::NodeType::Element) {
                    static_cast<bro::dom::Element*>(parent)->markDirty();
                    static_cast<bro::dom::Element*>(parent)->markStructureDirty();
                }
                return JS_UNDEFINED;
            }, "appendData", 1));

        // insertData(offset, data)
        JS_SetPropertyStr(ctx, obj, "insertData",
            JS_NewCFunction(ctx, [](JSContext* cx, JSValueConst this_val,
                int argc, JSValueConst* argv) -> JSValue {
                auto* nd = nodeSelf(this_val);
                if (!nd || argc < 2) return JS_UNDEFINED;
                int32_t off = 0;
                JS_ToInt32(cx, &off, argv[0]);
                const char* s = JS_ToCString(cx, argv[1]);
                std::string v(s ? s : "");
                if (s) JS_FreeCString(cx, s);
                const std::string* d = bro::dom::characterDataOf(nd);
                if (!d) return JS_UNDEFINED;
                const int bOff = bro::dom::utf16ToUtf8Byte(*d, off);
                std::string oldData;
                if (nd->nodeType() == bro::dom::NodeType::Text) {
                    oldData = static_cast<bro::dom::TextNode*>(nd)->data();
                    static_cast<bro::dom::TextNode*>(nd)->insertData(bOff, v);
                } else {
                    oldData = static_cast<bro::dom::CommentNode*>(nd)->data();
                    static_cast<bro::dom::CommentNode*>(nd)->insertData(bOff, v);
                }
                notifyMutationObservers(cx, this_val, "characterData",
                    nullptr, oldData.c_str(), JS_NULL, JS_NULL);
                auto* parent = nd->parentNode();
                if (parent && parent->nodeType() == bro::dom::NodeType::Element) {
                    static_cast<bro::dom::Element*>(parent)->markDirty();
                    static_cast<bro::dom::Element*>(parent)->markStructureDirty();
                }
                return JS_UNDEFINED;
            }, "insertData", 2));

        // deleteData(offset, count)
        JS_SetPropertyStr(ctx, obj, "deleteData",
            JS_NewCFunction(ctx, [](JSContext* cx, JSValueConst this_val,
                int argc, JSValueConst* argv) -> JSValue {
                auto* nd = nodeSelf(this_val);
                if (!nd || argc < 2) return JS_UNDEFINED;
                int32_t off = 0, cnt = 0;
                JS_ToInt32(cx, &off, argv[0]);
                JS_ToInt32(cx, &cnt, argv[1]);
                const std::string* d = bro::dom::characterDataOf(nd);
                if (!d) return JS_UNDEFINED;
                int bOff = 0, bLen = 0;
                cdSpanToBytes(*d, off, cnt, bOff, bLen);
                std::string oldData;
                if (nd->nodeType() == bro::dom::NodeType::Text) {
                    oldData = static_cast<bro::dom::TextNode*>(nd)->data();
                    static_cast<bro::dom::TextNode*>(nd)->deleteData(bOff, bLen);
                } else {
                    oldData = static_cast<bro::dom::CommentNode*>(nd)->data();
                    static_cast<bro::dom::CommentNode*>(nd)->deleteData(bOff, bLen);
                }
                notifyMutationObservers(cx, this_val, "characterData",
                    nullptr, oldData.c_str(), JS_NULL, JS_NULL);
                auto* parent = nd->parentNode();
                if (parent && parent->nodeType() == bro::dom::NodeType::Element) {
                    static_cast<bro::dom::Element*>(parent)->markDirty();
                    static_cast<bro::dom::Element*>(parent)->markStructureDirty();
                }
                return JS_UNDEFINED;
            }, "deleteData", 2));

        // replaceData(offset, count, data)
        JS_SetPropertyStr(ctx, obj, "replaceData",
            JS_NewCFunction(ctx, [](JSContext* cx, JSValueConst this_val,
                int argc, JSValueConst* argv) -> JSValue {
                auto* nd = nodeSelf(this_val);
                if (!nd || argc < 3) return JS_UNDEFINED;
                int32_t off = 0, cnt = 0;
                JS_ToInt32(cx, &off, argv[0]);
                JS_ToInt32(cx, &cnt, argv[1]);
                const char* s = JS_ToCString(cx, argv[2]);
                std::string v(s ? s : "");
                if (s) JS_FreeCString(cx, s);
                const std::string* d = bro::dom::characterDataOf(nd);
                if (!d) return JS_UNDEFINED;
                int bOff = 0, bLen = 0;
                cdSpanToBytes(*d, off, cnt, bOff, bLen);
                std::string oldData;
                if (nd->nodeType() == bro::dom::NodeType::Text) {
                    oldData = static_cast<bro::dom::TextNode*>(nd)->data();
                    static_cast<bro::dom::TextNode*>(nd)->replaceData(bOff, bLen, v);
                } else {
                    oldData = static_cast<bro::dom::CommentNode*>(nd)->data();
                    static_cast<bro::dom::CommentNode*>(nd)->replaceData(bOff, bLen, v);
                }
                notifyMutationObservers(cx, this_val, "characterData",
                    nullptr, oldData.c_str(), JS_NULL, JS_NULL);
                auto* parent = nd->parentNode();
                if (parent && parent->nodeType() == bro::dom::NodeType::Element) {
                    static_cast<bro::dom::Element*>(parent)->markDirty();
                    static_cast<bro::dom::Element*>(parent)->markStructureDirty();
                }
                return JS_UNDEFINED;
            }, "replaceData", 3));
    };

    if (node->nodeType() == bro::dom::NodeType::Comment) {
        auto* comment = static_cast<bro::dom::CommentNode*>(node);
        JS_SetPropertyStr(ctx, obj, "nodeValue",
            JS_NewString(ctx, comment->data().c_str()));
        JS_SetPropertyStr(ctx, obj, "textContent",
            JS_NewString(ctx, comment->data().c_str()));
        installCharacterDataMethods(obj, node);
    } else if (node->nodeType() == bro::dom::NodeType::Text) {
        // Define nodeValue and textContent as live getter/setter pairs
        // so that Vue's `textNode.nodeValue = "..."` actually updates
        // the C++ TextNode and triggers re-layout.
        JSValue getNodeValue = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* n = nodeSelf(this_val);
            if (!n || n->nodeType() != bro::dom::NodeType::Text) return JS_NULL;
            auto* tn = static_cast<bro::dom::TextNode*>(n);
            return JS_NewString(cx, tn->data().c_str());
        }, "get nodeValue", 0, JS_CFUNC_generic, 0);

        JSValue setNodeValue = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
            if (argc < 1) return JS_UNDEFINED;
            auto* n = nodeSelf(this_val);
            if (!n || n->nodeType() != bro::dom::NodeType::Text) return JS_UNDEFINED;
            auto* tn = static_cast<bro::dom::TextNode*>(n);
            const char* str = JS_ToCString(cx, argv[0]);
            if (!str) return JS_UNDEFINED;
            std::string newText(str);
            JS_FreeCString(cx, str);

            if (tn->data() == newText) return JS_UNDEFINED; // no change

            std::string oldData = tn->data();
            tn->setData(newText);

            // Notify MutationObservers of characterData change
            notifyMutationObservers(cx, this_val, "characterData",
                nullptr, oldData.c_str(), JS_NULL, JS_NULL);

            // Mark parent dirty for re-layout
            auto* parent = n->parentNode();
            if (parent && parent->nodeType() == bro::dom::NodeType::Element) {
                auto* parentEl = static_cast<bro::dom::Element*>(parent);
                parentEl->markDirty();
                parentEl->markStructureDirty();
            }
            return JS_UNDEFINED;
        }, "set nodeValue", 1, JS_CFUNC_generic, 0);

        JSAtom nvAtom = JS_NewAtom(ctx, "nodeValue");
        JS_DefinePropertyGetSet(ctx, obj, nvAtom, getNodeValue, setNodeValue, 0);
        JS_FreeAtom(ctx, nvAtom);

        // textContent aliases nodeValue for text nodes — needs its own
        // getter/setter functions (DefinePropertyGetSet takes ownership)
        JSValue getTC = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* n = nodeSelf(this_val);
            if (!n || n->nodeType() != bro::dom::NodeType::Text) return JS_NULL;
            auto* tn = static_cast<bro::dom::TextNode*>(n);
            return JS_NewString(cx, tn->data().c_str());
        }, "get textContent", 0, JS_CFUNC_generic, 0);
        JSValue setTC = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
            JSAtom nvAtom2 = JS_NewAtom(cx, "nodeValue");
            JS_SetProperty(cx, this_val, nvAtom2, JS_DupValue(cx, argv[0]));
            JS_FreeAtom(cx, nvAtom2);
            return JS_UNDEFINED;
        }, "set textContent", 1, JS_CFUNC_generic, 0);
        JSAtom tcAtom = JS_NewAtom(ctx, "textContent");
        JS_DefinePropertyGetSet(ctx, obj, tcAtom, getTC, setTC, 0);
        JS_FreeAtom(ctx, tcAtom);

        // CharacterData methods for text nodes
        installCharacterDataMethods(obj, node);

        // splitText(offset) — Text-specific
        JS_SetPropertyStr(ctx, obj, "splitText",
            JS_NewCFunction(ctx, [](JSContext* cx, JSValueConst this_val,
                int argc, JSValueConst* argv) -> JSValue {
                auto* nd = nodeSelf(this_val);
                if (!nd || nd->nodeType() != bro::dom::NodeType::Text || argc < 1)
                    return JS_NULL;
                auto* tn = static_cast<bro::dom::TextNode*>(nd);
                int32_t off = 0;
                JS_ToInt32(cx, &off, argv[0]);
                // `off` is a UTF-16 code-unit index; the storage is byte-indexed.
                // An index inside a surrogate pair resolves to the preceding
                // code-point boundary rather than cutting a code point in half.
                const size_t splitOff = static_cast<size_t>(
                    bro::dom::utf16ToUtf8Byte(tn->data(), off));
                std::string tail = tn->data().substr(splitOff);
                tn->setData(tn->data().substr(0, splitOff));

                // Create new text node via document (for proper ownership)
                auto* doc = getDocumentForCtx(cx);
                bro::dom::TextNode* newNode = nullptr;
                if (doc) {
                    newNode = doc->createTextNode(tail);
                } else {
                    // Fallback: allocate without document (will leak)
                    newNode = new bro::dom::TextNode(tail);
                }

                // Insert new node after this one in the parent
                auto* parent = nd->parentNode();
                if (parent) {
                    auto& kids = parent->childNodes();
                    for (size_t i = 0; i < kids.size(); ++i) {
                        if (kids[i] == nd) {
                            newNode->setParent(parent);
                            kids.insert(kids.begin() + static_cast<ptrdiff_t>(i) + 1, newNode);
                            break;
                        }
                    }
                    if (parent->nodeType() == bro::dom::NodeType::Element) {
                        static_cast<bro::dom::Element*>(parent)->markDirty();
                        static_cast<bro::dom::Element*>(parent)->markStructureDirty();
                    }
                }
                return wrapAnyNode(cx, newNode);
            }, "splitText", 1));

        // wholeText getter
        JSValue getWholeText = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* nd = nodeSelf(this_val);
            if (!nd || nd->nodeType() != bro::dom::NodeType::Text) return JS_NewString(cx, "");
            std::string result;
            // Collect contiguous text nodes
            auto* parent = nd->parentNode();
            if (!parent) return JS_NewString(cx, static_cast<bro::dom::TextNode*>(nd)->data().c_str());
            auto& kids = parent->childNodes();
            // Find start of contiguous text run
            size_t myIdx = 0;
            for (size_t i = 0; i < kids.size(); ++i) {
                if (kids[i] == nd) { myIdx = i; break; }
            }
            size_t start = myIdx;
            while (start > 0 && kids[start - 1]->nodeType() == bro::dom::NodeType::Text) --start;
            for (size_t i = start; i < kids.size(); ++i) {
                if (kids[i]->nodeType() != bro::dom::NodeType::Text) break;
                result += static_cast<bro::dom::TextNode*>(kids[i])->data();
            }
            return JS_NewString(cx, result.c_str());
        }, "get wholeText", 0, JS_CFUNC_generic, 0);
        JSAtom wtAtom = JS_NewAtom(ctx, "wholeText");
        JS_DefinePropertyGetSet(ctx, obj, wtAtom, getWholeText, JS_UNDEFINED, 0);
        JS_FreeAtom(ctx, wtAtom);
    }

    // Define parentNode/nextSibling/previousSibling as live getters
    {
        JSValue getParent = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* n = nodeSelf(this_val);
            if (!n || !n->parentNode()) return JS_NULL;
            if (n->parentNode()->nodeType() == bro::dom::NodeType::Element)
                return DomBindings::wrapElement(cx, static_cast<bro::dom::Element*>(n->parentNode()));
            return wrapAnyNode(cx, n->parentNode());
        }, "get parentNode", 0, JS_CFUNC_generic, 0);

        // parentElement is parentNode narrowed to elements — null when the
        // parent is a Document or a DocumentFragment, the element otherwise.
        // Element has it; without it here a Text node reported null for a
        // parent it plainly has, so `node.parentElement.tagName` threw on
        // every text node in the document.
        JSValue getParentElement = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* n = nodeSelf(this_val);
            if (!n || !n->parentNode()) return JS_NULL;
            auto* p = n->parentNode();
            if (p->nodeType() != bro::dom::NodeType::Element) return JS_NULL;
            // createDocumentFragment() models a fragment as an Element with a
            // reserved tag (see document_bindings.cpp), so the nodeType check
            // above waves it through. A fragment is not an element parent.
            if (static_cast<bro::dom::Element*>(p)->tagName() == "#DOCUMENT-FRAGMENT")
                return JS_NULL;
            return DomBindings::wrapElement(cx, static_cast<bro::dom::Element*>(p));
        }, "get parentElement", 0, JS_CFUNC_generic, 0);

        JSValue getNextSibling = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* n = nodeSelf(this_val);
            if (!n || !n->parentNode()) return JS_NULL;
            auto& kids = n->parentNode()->childNodes();
            for (size_t i = 0; i < kids.size(); ++i) {
                if (kids[i] == n && i + 1 < kids.size())
                    return wrapAnyNode(cx, kids[i + 1]);
            }
            return JS_NULL;
        }, "get nextSibling", 0, JS_CFUNC_generic, 0);

        JSValue getPrevSibling = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* n = nodeSelf(this_val);
            if (!n || !n->parentNode()) return JS_NULL;
            auto& kids = n->parentNode()->childNodes();
            for (size_t i = 0; i < kids.size(); ++i) {
                if (kids[i] == n) {
                    if (i > 0) return wrapAnyNode(cx, kids[i - 1]);
                    return JS_NULL;
                }
            }
            return JS_NULL;
        }, "get previousSibling", 0, JS_CFUNC_generic, 0);

        JSAtom parentAtom = JS_NewAtom(ctx, "parentNode");
        JSAtom parentElAtom = JS_NewAtom(ctx, "parentElement");
        JSAtom nextAtom = JS_NewAtom(ctx, "nextSibling");
        JSAtom prevAtom = JS_NewAtom(ctx, "previousSibling");
        JS_DefinePropertyGetSet(ctx, obj, parentAtom, getParent, JS_UNDEFINED, 0);
        JS_DefinePropertyGetSet(ctx, obj, parentElAtom, getParentElement, JS_UNDEFINED, 0);
        JS_DefinePropertyGetSet(ctx, obj, nextAtom, getNextSibling, JS_UNDEFINED, 0);
        JS_DefinePropertyGetSet(ctx, obj, prevAtom, getPrevSibling, JS_UNDEFINED, 0);
        JS_FreeAtom(ctx, parentAtom);
        JS_FreeAtom(ctx, parentElAtom);
        JS_FreeAtom(ctx, nextAtom);
        JS_FreeAtom(ctx, prevAtom);
    }

    // Cache the wrapper
    JS_SetPropertyStr(ctx, nodeMap, key.c_str(), JS_DupValue(ctx, obj));

    JS_FreeValue(ctx, nodeMap);
    JS_FreeValue(ctx, global);
    return obj;
}

// ===========================================================================
// Registration
// ===========================================================================

// `new Node()` is not allowed by the spec, and a constructor that silently
// produced a broken object would be worse than one that says so.
static JSValue js_node_illegal_ctor(JSContext* ctx, JSValueConst /*new_target*/,
                                    int /*argc*/, JSValueConst* /*argv*/)
{
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

void installNodeBindings(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);

    // NodeList via qjsbind
    qjsbind::Class<NodeListData>(ctx, "NodeList", qjsbind::NoGlobal)
        .get("length", [](NodeListData* d) -> int {
            return static_cast<int>(d->elements.size());
        })
        .method_raw("item", js_nodelist_item, 1);
    js_nodelist_class_id = qjsbind::class_id<NodeListData>();

    // HTMLCollection via qjsbind (methods set per-instance by wrapLiveHTMLCollection)
    qjsbind::Class<HTMLCollectionData>(ctx, "HTMLCollection", qjsbind::NoGlobal);
    js_htmlcollection_class_id = qjsbind::class_id<HTMLCollectionData>();

    // Node class — manual (per-instance pattern doesn't fit qjsbind)
    JS_NewClassID(rt, &js_node_class_id);
    JS_NewClass(rt, js_node_class_id, &js_node_class);
    // Node class has no prototype functions — properties are set per-instance

    // globalThis.Node, with the nodeType constants.
    //
    // Without this, `x instanceof Node` — which is how ordinary DOM code asks
    // "is this a thing I can append?" — throws a ReferenceError rather than
    // returning false, and there is no correct way to write the test. Node is
    // also where the nodeType numbers are defined; code that reads
    // `n.nodeType === Node.TEXT_NODE` is not being clever, it is being
    // readable, and the alternative is a bare 3.
    //
    // linkNodePrototype() below then puts Element and Document underneath this
    // prototype, so the instanceof holds for every node kind rather than only
    // for text and comment nodes.
    {
        JSValue global = JS_GetGlobalObject(ctx);

        // The class was registered on the RUNTIME but never given a prototype
        // in this CONTEXT — the wrappers set every property per instance, so
        // nothing needed one. `JS_GetClassProto` therefore returned null, which
        // makes `x instanceof Node` fail with "operand 'prototype' property is
        // not an object" rather than returning false. Create the prototype and
        // register it, which also gives every existing text and comment node
        // wrapper somewhere to inherit the nodeType constants from.
        JSValue proto = JS_NewObject(ctx);
        JS_SetClassProto(ctx, js_node_class_id, JS_DupValue(ctx, proto));

        JSValue ctor = JS_NewCFunction2(ctx, js_node_illegal_ctor, "Node", 0,
                                        JS_CFUNC_constructor, 0);
        JS_SetConstructor(ctx, ctor, proto);

        static const struct { const char* name; int32_t value; } kNodeTypes[] = {
            { "ELEMENT_NODE", 1 },
            { "ATTRIBUTE_NODE", 2 },
            { "TEXT_NODE", 3 },
            { "CDATA_SECTION_NODE", 4 },
            { "ENTITY_REFERENCE_NODE", 5 },
            { "ENTITY_NODE", 6 },
            { "PROCESSING_INSTRUCTION_NODE", 7 },
            { "COMMENT_NODE", 8 },
            { "DOCUMENT_NODE", 9 },
            { "DOCUMENT_TYPE_NODE", 10 },
            { "DOCUMENT_FRAGMENT_NODE", 11 },
            { "NOTATION_NODE", 12 },
        };
        // The spec puts these on both the constructor and the prototype, and
        // real code uses both spellings.
        for (const auto& t : kNodeTypes) {
            JS_SetPropertyStr(ctx, ctor, t.name, JS_NewInt32(ctx, t.value));
            JS_SetPropertyStr(ctx, proto, t.name, JS_NewInt32(ctx, t.value));
        }
        JS_FreeValue(ctx, proto);
        JS_SetPropertyStr(ctx, global, "Node", ctor);
        JS_FreeValue(ctx, global);
    }
}

void linkNodePrototype(JSContext* ctx, JSClassID childClassId)
{
    // Splice Node.prototype in underneath a class prototype that qjsbind
    // created with Object.prototype as its parent. Called after every DOM class
    // is registered; doing it here rather than at each registration site keeps
    // the ordering requirement — Node first, then everything else — in one
    // place where it can be seen.
    JSValue nodeProto = JS_GetClassProto(ctx, js_node_class_id);
    JSValue childProto = JS_GetClassProto(ctx, childClassId);
    if (JS_IsObject(nodeProto) && JS_IsObject(childProto)) {
        JS_SetPrototype(ctx, childProto, nodeProto);
    }
    JS_FreeValue(ctx, childProto);
    JS_FreeValue(ctx, nodeProto);
}

} // namespace bro::js
