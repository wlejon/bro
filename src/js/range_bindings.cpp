#include "js/dom_bindings_internal.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "dom/document.h"

#include <qjsbind/qjsbind.h>

namespace bro::js {

// ===========================================================================
// Range wrapper — owned by JS (finalizer deletes the C++ Range).
// Selection::getRangeAt returns a non-owning wrapper instead (see below).
// ===========================================================================

using Range = bro::dom::Range;
using Node  = bro::dom::Node;
using Selection = bro::dom::Selection;

// unwrap helper — accepts either owned (qjsbind::wrap) or unowned wrappers.
static Range* getRange(JSValueConst val) {
    return static_cast<Range*>(JS_GetOpaque(val, js_range_class_id));
}

// `newRange(doc)` creates a new Range, registered with `doc` for live
// mutation tracking. Called both from JS (`new Range()` / document.createRange)
// and internally when cloning selection ranges.
JSValue wrapOwnedRange(JSContext* ctx, Range* r) {
    return qjsbind::wrap<Range>(ctx, r);
}

JSValue wrapUnownedRange(JSContext* ctx, Range* r) {
    if (!r) return JS_NULL;
    return qjsbind::wrap_unowned<Range>(ctx, r);
}

// ---------------------------------------------------------------------------
// Constructor (JS: `new Range()`). Range constructed via `new` belongs to the
// current document (first JSContext with a registered Document). If no
// Document is installed for this context, the Range still works in-memory but
// won't receive mutation updates — matches spec for detached ranges.
// ---------------------------------------------------------------------------
static JSValue js_range_ctor(JSContext* ctx, JSValueConst /*new_target*/,
                             int /*argc*/, JSValueConst* /*argv*/)
{
    auto* r = new Range();
    if (auto* doc = getDocumentForCtx(ctx)) r->setDocument(doc);
    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_range_class_id));
    if (JS_IsException(obj)) { delete r; return obj; }
    JS_SetOpaque(obj, r);
    return obj;
}

// ---------------------------------------------------------------------------
// Method helpers that need manual argv parsing
// ---------------------------------------------------------------------------

static JSValue js_range_setStart(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r || argc < 2) return JS_UNDEFINED;
    auto* n = unwrapNode(ctx, argv[0]);
    int32_t off = 0; JS_ToInt32(ctx, &off, argv[1]);
    r->setStart(n, off);
    return JS_UNDEFINED;
}

static JSValue js_range_setEnd(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r || argc < 2) return JS_UNDEFINED;
    auto* n = unwrapNode(ctx, argv[0]);
    int32_t off = 0; JS_ToInt32(ctx, &off, argv[1]);
    r->setEnd(n, off);
    return JS_UNDEFINED;
}

static JSValue js_range_setStartBefore(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r || argc < 1) return JS_UNDEFINED;
    r->setStartBefore(unwrapNode(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_range_setStartAfter(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r || argc < 1) return JS_UNDEFINED;
    r->setStartAfter(unwrapNode(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_range_setEndBefore(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r || argc < 1) return JS_UNDEFINED;
    r->setEndBefore(unwrapNode(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_range_setEndAfter(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r || argc < 1) return JS_UNDEFINED;
    r->setEndAfter(unwrapNode(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_range_collapse(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r) return JS_UNDEFINED;
    bool toStart = true;
    if (argc >= 1) toStart = JS_ToBool(ctx, argv[0]);
    r->collapse(toStart);
    return JS_UNDEFINED;
}

static JSValue js_range_selectNode(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r || argc < 1) return JS_UNDEFINED;
    r->selectNode(unwrapNode(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_range_selectNodeContents(JSContext* ctx, JSValueConst this_val,
                                           int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r || argc < 1) return JS_UNDEFINED;
    r->selectNodeContents(unwrapNode(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_range_comparePoint(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r || argc < 2) return JS_NewInt32(ctx, 0);
    int32_t off = 0; JS_ToInt32(ctx, &off, argv[1]);
    return JS_NewInt32(ctx, r->comparePoint(unwrapNode(ctx, argv[0]), off));
}

static JSValue js_range_isPointInRange(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r || argc < 2) return JS_FALSE;
    int32_t off = 0; JS_ToInt32(ctx, &off, argv[1]);
    return JS_NewBool(ctx, r->isPointInRange(unwrapNode(ctx, argv[0]), off));
}

static JSValue js_range_intersectsNode(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r || argc < 1) return JS_FALSE;
    return JS_NewBool(ctx, r->intersectsNode(unwrapNode(ctx, argv[0])));
}

static JSValue js_range_deleteContents(JSContext* ctx, JSValueConst this_val,
                                       int /*argc*/, JSValueConst* /*argv*/) {
    auto* r = getRange(this_val);
    if (!r) return JS_UNDEFINED;
    r->deleteContents();
    return JS_UNDEFINED;
}

static JSValue js_range_cloneContents(JSContext* ctx, JSValueConst this_val,
                                      int /*argc*/, JSValueConst* /*argv*/) {
    auto* r = getRange(this_val);
    if (!r) return JS_NULL;
    auto* frag = r->cloneContents();
    return wrapAnyNode(ctx, frag);
}

static JSValue js_range_extractContents(JSContext* ctx, JSValueConst this_val,
                                        int /*argc*/, JSValueConst* /*argv*/) {
    auto* r = getRange(this_val);
    if (!r) return JS_NULL;
    auto* frag = r->extractContents();
    return wrapAnyNode(ctx, frag);
}

static JSValue js_range_insertNode(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r || argc < 1) return JS_UNDEFINED;
    r->insertNode(unwrapNode(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_range_surroundContents(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r || argc < 1) return JS_UNDEFINED;
    auto* n = unwrapNode(ctx, argv[0]);
    if (n && n->nodeType() == bro::dom::NodeType::Element)
        r->surroundContents(static_cast<bro::dom::Element*>(n));
    return JS_UNDEFINED;
}

static JSValue js_range_createContextualFragment(JSContext* ctx, JSValueConst this_val,
                                                 int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r || argc < 1) return JS_NULL;
    std::string html = jsToStdString(ctx, argv[0]);
    auto* n = r->createContextualFragment(html);
    return wrapAnyNode(ctx, n);
}

static JSValue js_range_cloneRange(JSContext* ctx, JSValueConst this_val,
                                   int /*argc*/, JSValueConst* /*argv*/) {
    auto* r = getRange(this_val);
    if (!r) return JS_NULL;
    Range* clone = r->cloneRange();
    return qjsbind::wrap<Range>(ctx, clone);
}

static JSValue js_range_toString(JSContext* ctx, JSValueConst this_val,
                                 int /*argc*/, JSValueConst* /*argv*/) {
    auto* r = getRange(this_val);
    if (!r) return JS_NewString(ctx, "");
    return JS_NewString(ctx, r->toString().c_str());
}

static JSValue js_range_detach(JSContext* /*ctx*/, JSValueConst /*this_val*/,
                               int /*argc*/, JSValueConst* /*argv*/) {
    // no-op per spec
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Registration — via qjsbind::Class so the finalizer runs delete on owned
// ranges. Unowned wrappers (Selection::getRangeAt) share the same class ID but
// finalizer is a no-op for them; to keep things simple we let qjsbind manage
// the finalizer and always use ownership semantics. Selection instead exposes
// methods like collapse/extend directly rather than returning a live Range.
// Callers that want a Range from Selection use getRangeAt() which returns a
// cloned Range (owned).
// ---------------------------------------------------------------------------

void installRangeBindings(JSContext* ctx)
{
    qjsbind::Class<Range>(ctx, "Range", qjsbind::NoGlobal)
        .get("startContainer", [](Range* r, JSContext* cx) -> JSValue {
            return wrapAnyNode(cx, r->startContainer());
        })
        .get("endContainer", [](Range* r, JSContext* cx) -> JSValue {
            return wrapAnyNode(cx, r->endContainer());
        })
        .get("startOffset", [](Range* r) -> int { return r->startOffset(); })
        .get("endOffset",   [](Range* r) -> int { return r->endOffset(); })
        .get("collapsed",   [](Range* r) -> bool { return r->collapsed(); })
        .get("commonAncestorContainer", [](Range* r, JSContext* cx) -> JSValue {
            return wrapAnyNode(cx, r->commonAncestorContainer());
        })
        .method_raw("setStart",                js_range_setStart, 2)
        .method_raw("setEnd",                  js_range_setEnd, 2)
        .method_raw("setStartBefore",          js_range_setStartBefore, 1)
        .method_raw("setStartAfter",           js_range_setStartAfter, 1)
        .method_raw("setEndBefore",            js_range_setEndBefore, 1)
        .method_raw("setEndAfter",             js_range_setEndAfter, 1)
        .method_raw("collapse",                js_range_collapse, 1)
        .method_raw("selectNode",              js_range_selectNode, 1)
        .method_raw("selectNodeContents",      js_range_selectNodeContents, 1)
        .method_raw("comparePoint",            js_range_comparePoint, 2)
        .method_raw("isPointInRange",          js_range_isPointInRange, 2)
        .method_raw("intersectsNode",          js_range_intersectsNode, 1)
        .method_raw("deleteContents",          js_range_deleteContents, 0)
        .method_raw("cloneContents",           js_range_cloneContents, 0)
        .method_raw("extractContents",         js_range_extractContents, 0)
        .method_raw("insertNode",              js_range_insertNode, 1)
        .method_raw("surroundContents",        js_range_surroundContents, 1)
        .method_raw("createContextualFragment",js_range_createContextualFragment, 1)
        .method_raw("cloneRange",              js_range_cloneRange, 0)
        .method_raw("toString",                js_range_toString, 0)
        .method_raw("detach",                  js_range_detach, 0);

    js_range_class_id = qjsbind::class_id<Range>();

    // Expose `Range` as a JS constructor on the global object so
    // `new Range()` and `document.createRange() instanceof Range` work.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_NewCFunction2(ctx, js_range_ctor, "Range", 0,
                                    JS_CFUNC_constructor, 0);
    // Grab the prototype registered by qjsbind::Class so instanceof works.
    JSValue proto = JS_GetClassProto(ctx, js_range_class_id);
    if (!JS_IsUndefined(proto) && !JS_IsNull(proto)) {
        JS_SetConstructor(ctx, ctor, proto);
    }
    JS_FreeValue(ctx, proto);

    // Range boundary-comparison constants, per spec.
    JS_SetPropertyStr(ctx, ctor, "START_TO_START", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, ctor, "START_TO_END",   JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, ctor, "END_TO_END",     JS_NewInt32(ctx, 2));
    JS_SetPropertyStr(ctx, ctor, "END_TO_START",   JS_NewInt32(ctx, 3));

    JS_SetPropertyStr(ctx, global, "Range", ctor);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
