#include "js/dom_bindings_internal.h"
#include "js/custom_elements.h"

namespace bro::js {

// ===========================================================================
// ShadowRoot wrapper
// ===========================================================================

static JSClassDef js_shadowroot_class = {
    "ShadowRoot",
    nullptr,
    nullptr, nullptr, nullptr
};

static bro::dom::ShadowRoot* getShadowRoot(JSValueConst val) {
    return static_cast<bro::dom::ShadowRoot*>(
        JS_GetOpaque(val, js_shadowroot_class_id));
}

JSValue wrapShadowRoot(JSContext* ctx, bro::dom::ShadowRoot* sr) {
    if (!sr) return JS_NULL;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (JS_IsUndefined(elemMap)) {
        elemMap = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__bro_elem_map", JS_DupValue(ctx, elemMap));
    }
    std::string key = "sr_" + std::to_string(sr->nodeId());
    JSValue existing = JS_GetPropertyStr(ctx, elemMap, key.c_str());
    if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) {
        JS_FreeValue(ctx, elemMap);
        JS_FreeValue(ctx, global);
        return existing;
    }
    JS_FreeValue(ctx, existing);

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_shadowroot_class_id));
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, elemMap);
        JS_FreeValue(ctx, global);
        return obj;
    }
    JS_SetOpaque(obj, sr);
    JS_SetPropertyStr(ctx, elemMap, key.c_str(), JS_DupValue(ctx, obj));
    JS_FreeValue(ctx, elemMap);
    JS_FreeValue(ctx, global);
    return obj;
}

// ---- Properties -----------------------------------------------------------

static JSValue js_shadowroot_get_host(JSContext* ctx, JSValueConst this_val) {
    auto* sr = getShadowRoot(this_val);
    if (!sr || !sr->host()) return JS_NULL;
    return DomBindings::wrapElement(ctx, sr->host());
}

static JSValue js_shadowroot_get_mode(JSContext* ctx, JSValueConst this_val) {
    auto* sr = getShadowRoot(this_val);
    if (!sr) return JS_UNDEFINED;
    return JS_NewString(ctx, sr->modeString().c_str());
}

static JSValue js_shadowroot_get_innerHTML(JSContext* ctx, JSValueConst this_val) {
    auto* sr = getShadowRoot(this_val);
    if (!sr) return JS_UNDEFINED;
    return JS_NewString(ctx, sr->innerHTML().c_str());
}

static void upgradeShadowChildren(JSContext* ctx, bro::dom::Node* node) {
    if (!node) return;
    for (auto* child : node->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element) {
            auto* elem = static_cast<bro::dom::Element*>(child);
            std::string tag = elem->tagName();
            std::string lower = tag;
            for (auto& c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower.find('-') != std::string::npos) {
                JSValue upgraded = createCustomElement(ctx, elem, lower);
                if (!JS_IsException(upgraded) && !JS_IsUndefined(upgraded)) {
                    JS_FreeValue(ctx, upgraded);
                }
            }
            if (!elem->hasShadow()) {
                upgradeShadowChildren(ctx, child);
            }
        }
    }
}

static JSValue js_shadowroot_set_innerHTML(JSContext* ctx, JSValueConst this_val,
                                           JSValueConst val) {
    auto* sr = getShadowRoot(this_val);
    if (!sr) return JS_UNDEFINED;

    // Capture old children for MutationObserver
    JSValue removedArr = JS_NewArray(ctx);
    uint32_t rmIdx = 0;
    for (auto* child : sr->childNodes()) {
        JS_SetPropertyUint32(ctx, removedArr, rmIdx++, wrapAnyNode(ctx, child));
    }

    std::string html = jsToStdString(ctx, val);
    auto* doc = getDocumentForCtx(ctx);
    sr->setInnerHTML(html, doc);

    upgradeShadowChildren(ctx, sr);

    // Capture new children
    JSValue addedArr = JS_NewArray(ctx);
    uint32_t addIdx = 0;
    for (auto* child : sr->childNodes()) {
        JS_SetPropertyUint32(ctx, addedArr, addIdx++, wrapAnyNode(ctx, child));
    }
    notifyMutationObservers(ctx, this_val, "childList",
        nullptr, nullptr, addedArr, removedArr);
    JS_FreeValue(ctx, addedArr);
    JS_FreeValue(ctx, removedArr);

    if (doc && sr->host()) {
        for (auto& css : sr->styleSheets()) {
            doc->addShadowStylesheet(sr, css);
        }
        doc->markStructureDirty();
    }
    return JS_UNDEFINED;
}

static JSValue js_shadowroot_get_nodeType(JSContext* ctx, JSValueConst /*this_val*/) {
    return JS_NewInt32(ctx, 11);
}

static JSValue js_shadowroot_get_nodeName(JSContext* ctx, JSValueConst /*this_val*/) {
    return JS_NewString(ctx, "#document-fragment");
}

// ---- Tree navigation ------------------------------------------------------

static JSValue js_shadowroot_get_childNodes(JSContext* ctx, JSValueConst this_val) {
    auto* sr = getShadowRoot(this_val);
    if (!sr) return JS_NewArray(ctx);
    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    for (auto* child : sr->childNodes()) {
        JSValue w;
        if (child->nodeType() == bro::dom::NodeType::Element)
            w = DomBindings::wrapElement(ctx, child);
        else
            w = wrapAnyNode(ctx, child);
        JS_SetPropertyUint32(ctx, arr, idx++, w);
    }
    JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, static_cast<int32_t>(idx)));
    return arr;
}

static JSValue js_shadowroot_get_children(JSContext* ctx, JSValueConst this_val) {
    auto* sr = getShadowRoot(this_val);
    if (!sr) return JS_NewArray(ctx);
    std::vector<bro::dom::Element*> elems;
    for (auto* child : sr->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element)
            elems.push_back(static_cast<bro::dom::Element*>(child));
    }
    return wrapNodeList(ctx, elems);
}

static JSValue js_shadowroot_get_firstChild(JSContext* ctx, JSValueConst this_val) {
    auto* sr = getShadowRoot(this_val);
    if (!sr || sr->childNodes().empty()) return JS_NULL;
    auto* child = sr->childNodes().front();
    if (child->nodeType() == bro::dom::NodeType::Element)
        return DomBindings::wrapElement(ctx, child);
    return wrapAnyNode(ctx, child);
}

static JSValue js_shadowroot_get_lastChild(JSContext* ctx, JSValueConst this_val) {
    auto* sr = getShadowRoot(this_val);
    if (!sr || sr->childNodes().empty()) return JS_NULL;
    auto* child = sr->childNodes().back();
    if (child->nodeType() == bro::dom::NodeType::Element)
        return DomBindings::wrapElement(ctx, child);
    return wrapAnyNode(ctx, child);
}

// ---- Query methods --------------------------------------------------------

static JSValue js_shadowroot_getElementById(JSContext* ctx, JSValueConst this_val,
                                             int argc, JSValueConst* argv) {
    auto* sr = getShadowRoot(this_val);
    if (!sr || argc < 1) return JS_NULL;
    std::string id = jsToStdString(ctx, argv[0]);
    auto* el = sr->getElementById(id);
    if (!el) return JS_NULL;
    return DomBindings::wrapElement(ctx, el);
}

static JSValue js_shadowroot_querySelector(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    auto* sr = getShadowRoot(this_val);
    if (!sr || argc < 1) return JS_NULL;
    std::string sel = jsToStdString(ctx, argv[0]);
    auto* el = sr->querySelector(sel);
    if (!el) return JS_NULL;
    return DomBindings::wrapElement(ctx, el);
}

static JSValue js_shadowroot_querySelectorAll(JSContext* ctx, JSValueConst this_val,
                                               int argc, JSValueConst* argv) {
    auto* sr = getShadowRoot(this_val);
    if (!sr || argc < 1) return wrapNodeList(ctx, {});
    std::string sel = jsToStdString(ctx, argv[0]);
    auto results = sr->querySelectorAll(sel);
    return wrapNodeList(ctx, results);
}

// ---- DOM manipulation -----------------------------------------------------

static JSValue js_shadowroot_appendChild(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    auto* sr = getShadowRoot(this_val);
    if (!sr || argc < 1) return JS_UNDEFINED;
    auto* child = unwrapNode(ctx, argv[0]);
    if (!child) return JS_UNDEFINED;

    JSValue addedArr = JS_NewArray(ctx);
    uint32_t addedIdx = 0;
    if (child->nodeName() == "#DOCUMENT-FRAGMENT" ||
        child->nodeType() == bro::dom::NodeType::DocumentFragment) {
        auto kids = child->childNodes();
        for (auto* kid : kids) {
            sr->appendChild(kid);
            if (kid->nodeType() == bro::dom::NodeType::Element) {
                auto* elem = static_cast<bro::dom::Element*>(kid);
                if (sr->host() && sr->host()->document())
                    elem->setDocument(sr->host()->document());
            }
            JS_SetPropertyUint32(ctx, addedArr, addedIdx++, wrapAnyNode(ctx, kid));
        }
    } else {
        sr->appendChild(child);
        if (child->nodeType() == bro::dom::NodeType::Element) {
            auto* elem = static_cast<bro::dom::Element*>(child);
            if (sr->host() && sr->host()->document())
                elem->setDocument(sr->host()->document());
        }
        JS_SetPropertyUint32(ctx, addedArr, addedIdx++, wrapAnyNode(ctx, child));
    }
    notifyMutationObservers(ctx, this_val, "childList",
        nullptr, nullptr, addedArr, JS_NULL);
    JS_FreeValue(ctx, addedArr);

    sr->invalidateSlots();
    if (sr->host()) {
        sr->host()->markDirty();
        sr->host()->markStructureDirty();
    }

    auto checkStyles = [&](bro::dom::Node* node) {
        if (node->nodeType() == bro::dom::NodeType::Element) {
            auto* elem = static_cast<bro::dom::Element*>(node);
            if (elem->tagName() == "STYLE") {
                sr->addStyleSheet(elem->textContent());
            }
        }
    };
    if (child->nodeName() == "#DOCUMENT-FRAGMENT" ||
        child->nodeType() == bro::dom::NodeType::DocumentFragment) {
        for (auto* c : sr->childNodes()) checkStyles(c);
    } else {
        checkStyles(child);
    }

    auto* doc = getDocumentForCtx(ctx);
    if (doc && sr->host()) {
        for (auto& css : sr->styleSheets()) {
            doc->addShadowStylesheet(sr, css);
        }
        doc->markStructureDirty();
    }

    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_shadowroot_removeChild(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    auto* sr = getShadowRoot(this_val);
    if (!sr || argc < 1) return JS_UNDEFINED;
    auto* child = unwrapNode(ctx, argv[0]);
    if (child) {
        JSValue removedArr = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, removedArr, 0, wrapAnyNode(ctx, child));
        sr->removeChild(child);
        notifyMutationObservers(ctx, this_val, "childList",
            nullptr, nullptr, JS_NULL, removedArr);
        JS_FreeValue(ctx, removedArr);
        sr->invalidateSlots();
        auto* doc = getDocumentForCtx(ctx);
        if (doc && sr->host()) {
            for (auto& css : sr->styleSheets()) {
                doc->addShadowStylesheet(sr, css);
            }
            doc->markStructureDirty();
        }
    }
    return argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
}

static JSValue js_shadowroot_insertBefore(JSContext* ctx, JSValueConst this_val,
                                           int argc, JSValueConst* argv) {
    auto* sr = getShadowRoot(this_val);
    if (!sr || argc < 2) return JS_UNDEFINED;
    auto* newChild = unwrapNode(ctx, argv[0]);
    bro::dom::Node* refChild = nullptr;
    if (!JS_IsNull(argv[1])) {
        refChild = unwrapNode(ctx, argv[1]);
    }
    if (newChild) {
        sr->insertBefore(newChild, refChild);
        if (newChild->nodeType() == bro::dom::NodeType::Element) {
            auto* elem = static_cast<bro::dom::Element*>(newChild);
            if (sr->host() && sr->host()->document())
                elem->setDocument(sr->host()->document());
        }
        JSValue addedArr = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, addedArr, 0, wrapAnyNode(ctx, newChild));
        notifyMutationObservers(ctx, this_val, "childList",
            nullptr, nullptr, addedArr, JS_NULL);
        JS_FreeValue(ctx, addedArr);
        sr->invalidateSlots();
        auto* doc = getDocumentForCtx(ctx);
        if (doc && sr->host()) {
            for (auto& css : sr->styleSheets()) {
                doc->addShadowStylesheet(sr, css);
            }
            doc->markStructureDirty();
        }
    }
    return argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
}

// ===========================================================================
// ShadowRoot prototype function list
// ===========================================================================

static const JSCFunctionListEntry js_shadowroot_proto_funcs[] = {
    JS_CGETSET_DEF("host",       js_shadowroot_get_host,      nullptr),
    JS_CGETSET_DEF("mode",       js_shadowroot_get_mode,      nullptr),
    JS_CGETSET_DEF("innerHTML",  js_shadowroot_get_innerHTML, js_shadowroot_set_innerHTML),
    JS_CGETSET_DEF("nodeType",   js_shadowroot_get_nodeType,  nullptr),
    JS_CGETSET_DEF("nodeName",   js_shadowroot_get_nodeName,  nullptr),
    JS_CGETSET_DEF("childNodes", js_shadowroot_get_childNodes, nullptr),
    JS_CGETSET_DEF("children",   js_shadowroot_get_children,  nullptr),
    JS_CGETSET_DEF("firstChild", js_shadowroot_get_firstChild, nullptr),
    JS_CGETSET_DEF("lastChild",  js_shadowroot_get_lastChild, nullptr),
    JS_CFUNC_DEF("getElementById",  1, js_shadowroot_getElementById),
    JS_CFUNC_DEF("querySelector",   1, js_shadowroot_querySelector),
    JS_CFUNC_DEF("querySelectorAll",1, js_shadowroot_querySelectorAll),
    JS_CFUNC_DEF("appendChild",     1, js_shadowroot_appendChild),
    JS_CFUNC_DEF("removeChild",     1, js_shadowroot_removeChild),
    JS_CFUNC_DEF("insertBefore",    2, js_shadowroot_insertBefore),
};

// ===========================================================================
// Registration
// ===========================================================================

void registerShadowRootClasses(JSRuntime* rt) {
    JS_NewClass(rt, js_shadowroot_class_id, &js_shadowroot_class);
}

void installShadowRootPrototypes(JSContext* ctx) {
    JSValue sr_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, sr_proto, js_shadowroot_proto_funcs,
                               sizeof(js_shadowroot_proto_funcs) / sizeof(js_shadowroot_proto_funcs[0]));
    JS_SetClassProto(ctx, js_shadowroot_class_id, sr_proto);
}

} // namespace bro::js
