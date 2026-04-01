#include "js/dom_bindings_internal.h"
#include "util/log.h"

namespace bro::js {

// ===========================================================================
// NodeList wrapper – wraps a vector<Element*>
// ===========================================================================

struct NodeListData {
    std::vector<bro::dom::Element*> elements;
};

static void js_nodelist_finalizer(JSRuntime* /*rt*/, JSValue val)
{
    auto* data = static_cast<NodeListData*>(
        JS_GetOpaque(val, js_nodelist_class_id));
    delete data;
}

static JSClassDef js_nodelist_class = {
    "NodeList",
    js_nodelist_finalizer,
    nullptr, nullptr, nullptr
};

static JSValue js_nodelist_length(JSContext* ctx, JSValueConst this_val)
{
    auto* data = static_cast<NodeListData*>(
        JS_GetOpaque(this_val, js_nodelist_class_id));
    if (!data) return JS_UNDEFINED;
    return JS_NewInt32(ctx, static_cast<int32_t>(data->elements.size()));
}

static JSValue js_nodelist_item(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv)
{
    auto* data = static_cast<NodeListData*>(
        JS_GetOpaque(this_val, js_nodelist_class_id));
    if (!data || argc < 1) return JS_UNDEFINED;

    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    if (idx < 0 || static_cast<size_t>(idx) >= data->elements.size())
        return JS_NULL;

    return DomBindings::wrapElement(ctx, data->elements[static_cast<size_t>(idx)]);
}

static const JSCFunctionListEntry js_nodelist_proto_funcs[] = {
    JS_CGETSET_DEF("length", js_nodelist_length, nullptr),
    JS_CFUNC_DEF("item", 1, js_nodelist_item),
};

JSValue wrapNodeList(JSContext* ctx,
                     const std::vector<bro::dom::Element*>& elems)
{
    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_nodelist_class_id));
    if (JS_IsException(obj)) return obj;

    auto* data = new NodeListData{elems};
    JS_SetOpaque(obj, data);

    // Also set indexed properties so nodeList[0] works.
    for (size_t i = 0; i < elems.size(); ++i) {
        JS_SetPropertyUint32(ctx, obj, static_cast<uint32_t>(i),
                             DomBindings::wrapElement(ctx, elems[i]));
    }

    return obj;
}

// ===========================================================================
// Generic Node wrapper (comment nodes, text nodes in tree ops)
// ===========================================================================

static JSClassDef js_node_class = {
    "Node",
    nullptr,  // finalizer — Node lifetime managed by Document
    nullptr, nullptr, nullptr
};

bro::dom::Node* unwrapNode(JSContext* ctx, JSValueConst val)
{
    void* ptr = JS_GetOpaque(val, js_element_class_id);
    if (ptr) return static_cast<bro::dom::Node*>(static_cast<bro::dom::Element*>(ptr));
    ptr = JS_GetOpaque(val, js_node_class_id);
    if (ptr) return static_cast<bro::dom::Node*>(ptr);
    return nullptr;
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
    JS_SetOpaque(obj, node);

    // Set basic DOM properties
    JS_SetPropertyStr(ctx, obj, "nodeType",
        JS_NewInt32(ctx, static_cast<int32_t>(node->nodeType())));
    JS_SetPropertyStr(ctx, obj, "nodeName",
        JS_NewString(ctx, node->nodeName().c_str()));

    if (node->nodeType() == bro::dom::NodeType::Comment) {
        auto* comment = static_cast<bro::dom::CommentNode*>(node);
        JS_SetPropertyStr(ctx, obj, "nodeValue",
            JS_NewString(ctx, comment->data().c_str()));
        JS_SetPropertyStr(ctx, obj, "textContent",
            JS_NewString(ctx, comment->data().c_str()));
    } else if (node->nodeType() == bro::dom::NodeType::Text) {
        // Define nodeValue and textContent as live getter/setter pairs
        // so that Vue's `textNode.nodeValue = "..."` actually updates
        // the C++ TextNode and triggers re-layout.
        JSValue getNodeValue = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* n = static_cast<bro::dom::Node*>(JS_GetOpaque(this_val, js_node_class_id));
            if (!n || n->nodeType() != bro::dom::NodeType::Text) return JS_NULL;
            auto* tn = static_cast<bro::dom::TextNode*>(n);
            return JS_NewString(cx, tn->data().c_str());
        }, "get nodeValue", 0, JS_CFUNC_generic, 0);

        JSValue setNodeValue = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
            if (argc < 1) return JS_UNDEFINED;
            auto* n = static_cast<bro::dom::Node*>(JS_GetOpaque(this_val, js_node_class_id));
            if (!n || n->nodeType() != bro::dom::NodeType::Text) return JS_UNDEFINED;
            auto* tn = static_cast<bro::dom::TextNode*>(n);
            const char* str = JS_ToCString(cx, argv[0]);
            if (!str) return JS_UNDEFINED;
            std::string newText(str);
            JS_FreeCString(cx, str);

            if (tn->data() == newText) return JS_UNDEFINED; // no change

            tn->setData(newText);

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
            auto* n = static_cast<bro::dom::Node*>(JS_GetOpaque(this_val, js_node_class_id));
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
    }

    // Define parentNode/nextSibling/previousSibling as live getters
    {
        JSValue getParent = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* n = static_cast<bro::dom::Node*>(JS_GetOpaque(this_val, js_node_class_id));
            if (!n || !n->parentNode()) return JS_NULL;
            if (n->parentNode()->nodeType() == bro::dom::NodeType::Element)
                return DomBindings::wrapElement(cx, static_cast<bro::dom::Element*>(n->parentNode()));
            return wrapAnyNode(cx, n->parentNode());
        }, "get parentNode", 0, JS_CFUNC_generic, 0);

        JSValue getNextSibling = JS_NewCFunction2(ctx, [](JSContext* cx,
            JSValueConst this_val, int, JSValueConst*) -> JSValue {
            auto* n = static_cast<bro::dom::Node*>(JS_GetOpaque(this_val, js_node_class_id));
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
            auto* n = static_cast<bro::dom::Node*>(JS_GetOpaque(this_val, js_node_class_id));
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
        JSAtom nextAtom = JS_NewAtom(ctx, "nextSibling");
        JSAtom prevAtom = JS_NewAtom(ctx, "previousSibling");
        JS_DefinePropertyGetSet(ctx, obj, parentAtom, getParent, JS_UNDEFINED, 0);
        JS_DefinePropertyGetSet(ctx, obj, nextAtom, getNextSibling, JS_UNDEFINED, 0);
        JS_DefinePropertyGetSet(ctx, obj, prevAtom, getPrevSibling, JS_UNDEFINED, 0);
        JS_FreeAtom(ctx, parentAtom);
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

void registerNodeClasses(JSRuntime* rt) {
    JS_NewClass(rt, js_nodelist_class_id, &js_nodelist_class);
    JS_NewClass(rt, js_node_class_id, &js_node_class);
}

void installNodePrototypes(JSContext* ctx) {
    JSValue nl_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, nl_proto, js_nodelist_proto_funcs,
                               sizeof(js_nodelist_proto_funcs) / sizeof(js_nodelist_proto_funcs[0]));
    JS_SetClassProto(ctx, js_nodelist_class_id, nl_proto);
    // Node class has no prototype functions — properties are set per-instance
}

} // namespace bro::js
