#include "js/dom_bindings_internal.h"
#include "js/custom_elements.h"

#include <qjsbind/qjsbind.h>

namespace bro::js {

// ===========================================================================
// ShadowRoot wrapper
// ===========================================================================

using SR = bro::dom::ShadowRoot;

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

    JSValue obj = qjsbind::wrap_unowned<SR>(ctx, sr);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, elemMap);
        JS_FreeValue(ctx, global);
        return obj;
    }
    JS_SetPropertyStr(ctx, elemMap, key.c_str(), JS_DupValue(ctx, obj));
    JS_FreeValue(ctx, elemMap);
    JS_FreeValue(ctx, global);
    return obj;
}

// ---------------------------------------------------------------------------
// Complex methods requiring raw signatures
// ---------------------------------------------------------------------------


static JSValue js_shadowroot_set_innerHTML(JSContext* ctx, JSValueConst this_val,
                                           int /*argc*/, JSValueConst* argv) {
    auto* sr = qjsbind::unwrap<SR>(ctx, this_val);
    if (!sr) return JS_UNDEFINED;

    // Capture old children for MutationObserver
    JSValue removedArr = JS_NewArray(ctx);
    uint32_t rmIdx = 0;
    for (auto* child : sr->childNodes()) {
        JS_SetPropertyUint32(ctx, removedArr, rmIdx++, wrapAnyNode(ctx, child));
    }

    std::string html = jsToStdString(ctx, argv[0]);
    auto* doc = getDocumentForCtx(ctx);
    sr->setInnerHTML(html, doc);

    upgradeCustomElementsInSubtree(ctx, sr);

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

static JSValue js_shadowroot_appendChild(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    auto* sr = qjsbind::unwrap<SR>(ctx, this_val);
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
    auto* sr = qjsbind::unwrap<SR>(ctx, this_val);
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
    auto* sr = qjsbind::unwrap<SR>(ctx, this_val);
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
// Registration
// ===========================================================================

void installShadowRootBindings(JSContext* ctx) {
    // innerHTML needs a raw setter — register as get() then override below
    qjsbind::Class<SR>(ctx, "ShadowRoot", qjsbind::NoGlobal | qjsbind::NoDestructor)
        // Properties
        .get("host", [](SR* sr, JSContext* cx) -> JSValue {
            if (!sr->host()) return JS_NULL;
            return DomBindings::wrapElement(cx, sr->host());
        })
        .get("mode", [](SR* sr) -> std::string { return sr->modeString(); })
        .get("innerHTML", [](SR* sr) -> std::string { return sr->innerHTML(); })
        .get("nodeType", [](SR*) -> int { return 11; })
        .get("nodeName", [](SR*) -> std::string { return "#document-fragment"; })
        .get("childNodes", [](SR* sr, JSContext* cx) -> JSValue {
            JSValue arr = JS_NewArray(cx);
            uint32_t idx = 0;
            for (auto* child : sr->childNodes()) {
                JSValue w;
                if (child->nodeType() == bro::dom::NodeType::Element)
                    w = DomBindings::wrapElement(cx, child);
                else
                    w = wrapAnyNode(cx, child);
                JS_SetPropertyUint32(cx, arr, idx++, w);
            }
            JS_SetPropertyStr(cx, arr, "length", JS_NewInt32(cx, static_cast<int32_t>(idx)));
            return arr;
        })
        .get("children", [](SR* sr, JSContext* cx) -> JSValue {
            std::vector<bro::dom::Element*> elems;
            for (auto* child : sr->childNodes()) {
                if (child->nodeType() == bro::dom::NodeType::Element)
                    elems.push_back(static_cast<bro::dom::Element*>(child));
            }
            return wrapNodeList(cx, elems);
        })
        .get("firstChild", [](SR* sr, JSContext* cx) -> JSValue {
            if (sr->childNodes().empty()) return JS_NULL;
            auto* child = sr->childNodes().front();
            if (child->nodeType() == bro::dom::NodeType::Element)
                return DomBindings::wrapElement(cx, child);
            return wrapAnyNode(cx, child);
        })
        .get("lastChild", [](SR* sr, JSContext* cx) -> JSValue {
            if (sr->childNodes().empty()) return JS_NULL;
            auto* child = sr->childNodes().back();
            if (child->nodeType() == bro::dom::NodeType::Element)
                return DomBindings::wrapElement(cx, child);
            return wrapAnyNode(cx, child);
        })
        // Query methods
        .method("getElementById", [](SR* sr, JSContext* cx, std::string id) -> JSValue {
            auto* el = sr->getElementById(id);
            return el ? DomBindings::wrapElement(cx, el) : JS_NULL;
        })
        .method("querySelector", [](SR* sr, JSContext* cx, std::string sel) -> JSValue {
            auto* el = sr->querySelector(sel);
            return el ? DomBindings::wrapElement(cx, el) : JS_NULL;
        })
        .method("querySelectorAll", [](SR* sr, JSContext* cx, std::string sel) -> JSValue {
            return wrapNodeList(cx, sr->querySelectorAll(sel));
        })
        // DOM manipulation — raw signatures
        .method_raw("appendChild", js_shadowroot_appendChild, 1)
        .method_raw("removeChild", js_shadowroot_removeChild, 1)
        .method_raw("insertBefore", js_shadowroot_insertBefore, 2);

    // Override innerHTML with getter+setter (raw setter for MutationObserver support)
    JSValue proto = JS_GetClassProto(ctx, qjsbind::class_id<SR>());
    {
        JSAtom atom = JS_NewAtom(ctx, "innerHTML");
        JS_DefinePropertyGetSet(ctx, proto, atom,
            JS_NewCFunction(ctx, [](JSContext* cx, JSValueConst this_val, int, JSValueConst*) -> JSValue {
                auto* sr = qjsbind::unwrap<SR>(cx, this_val);
                if (!sr) return JS_UNDEFINED;
                return JS_NewString(cx, sr->innerHTML().c_str());
            }, "innerHTML", 0),
            JS_NewCFunction(ctx, js_shadowroot_set_innerHTML, "innerHTML", 1),
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
    }
    JS_FreeValue(ctx, proto);

    js_shadowroot_class_id = qjsbind::class_id<SR>();
}

} // namespace bro::js
