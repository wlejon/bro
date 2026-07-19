#include "js/dom_bindings_internal.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/text_offsets.h"
#include "layout/selection_geometry.h"
#include "layout/skia_text_metrics.h"
#include "engine/engine.h"

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

// ---- Boundary-point offset domain ----------------------------------------
// A Range stores its endpoints the way the whole engine does: for Text/Comment
// containers, a UTF-8 BYTE offset into the character data — which is what
// layout, caret placement and the contenteditable/IME paths read straight off
// the C++ Range. The DOM spec's `startOffset`/`setStart` speak UTF-16 code
// units instead, so those convert here and only here (converting deeper would
// double-convert against the internal readers). Offsets on Element containers
// are child indices and pass through untouched — see dom/text_offsets.h.
static int offsetToJs(Node* container, int byteOffset) {
    return bro::dom::nodeOffsetToUtf16(container, byteOffset);
}
static int offsetFromJs(Node* container, int utf16Offset) {
    return bro::dom::nodeOffsetToBytes(container, utf16Offset);
}

// The nearest Element ancestor (or the node itself if already an Element) —
// used to look up the CSS transform chain applying to a Range endpoint,
// since getSelectionRects()'s geometry (below) is transform-unaware.
static bro::dom::Element* nearestElementAncestor(Node* node) {
    for (Node* n = node; n; n = n->parentNode()) {
        if (n->nodeType() == bro::dom::NodeType::Element)
            return static_cast<bro::dom::Element*>(n);
    }
    return nullptr;
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
    r->setStart(n, offsetFromJs(n, off));
    return JS_UNDEFINED;
}

static JSValue js_range_setEnd(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r || argc < 2) return JS_UNDEFINED;
    auto* n = unwrapNode(ctx, argv[0]);
    int32_t off = 0; JS_ToInt32(ctx, &off, argv[1]);
    r->setEnd(n, offsetFromJs(n, off));
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
    auto* n = unwrapNode(ctx, argv[0]);
    return JS_NewInt32(ctx, r->comparePoint(n, offsetFromJs(n, off)));
}

static JSValue js_range_isPointInRange(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* r = getRange(this_val);
    if (!r || argc < 2) return JS_FALSE;
    int32_t off = 0; JS_ToInt32(ctx, &off, argv[1]);
    auto* n = unwrapNode(ctx, argv[0]);
    return JS_NewBool(ctx, r->isPointInRange(n, offsetFromJs(n, off)));
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

// Build a plain JS object shaped like DOMRect: {x,y,width,height,top,right,
// bottom,left}. Callers treat these as POJOs — we don't need a class since
// DOMRect is a value type with only accessors.
static JSValue makeDomRect(JSContext* ctx, float x, float y, float w, float h) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, o, "y", JS_NewFloat64(ctx, y));
    JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, w));
    JS_SetPropertyStr(ctx, o, "height", JS_NewFloat64(ctx, h));
    JS_SetPropertyStr(ctx, o, "top", JS_NewFloat64(ctx, y));
    JS_SetPropertyStr(ctx, o, "left", JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, o, "right", JS_NewFloat64(ctx, x + w));
    JS_SetPropertyStr(ctx, o, "bottom", JS_NewFloat64(ctx, y + h));
    return o;
}

// Fetch (document, textMetrics, docOffsetY) by looking up the engine bound
// to this JSContext. Returns false when not available (e.g. no engine or
// layout hasn't run yet).
static bool fetchGeometryDeps(JSContext* ctx,
                              bro::dom::Document*& outDoc,
                              htmlayout::layout::TextMetrics*& outMetrics,
                              float& outOffsetY) {
    outDoc = bro::js::getDocumentForCtx(ctx);
    if (!outDoc) return false;
    auto it = bro::js::s_ctx_engines.find(ctx);
    if (it == bro::js::s_ctx_engines.end() || !it->second) return false;
    auto* engine = static_cast<bro::engine::Engine*>(it->second);
    outMetrics = engine->textMetrics();
    if (!outMetrics) return false;
    outOffsetY = engine->docContentOffsetY();
    return true;
}

static JSValue js_range_getClientRects(JSContext* ctx, JSValueConst this_val,
                                       int /*argc*/, JSValueConst* /*argv*/) {
    auto* r = getRange(this_val);
    if (!r) return JS_NewArray(ctx);
    bro::dom::Document* doc;
    htmlayout::layout::TextMetrics* metrics;
    float offY;
    if (!fetchGeometryDeps(ctx, doc, metrics, offY)) return JS_NewArray(ctx);

    auto rects = bro::layout::getSelectionRects(
        doc, r->startContainer(), r->startOffset(),
        r->endContainer(), r->endOffset(), *metrics);

    // getSelectionRects() is transform-unaware — project through the
    // ancestor chain of the range's start (the common-case approximation:
    // a range normally stays within one transformed subtree).
    auto* ctxEl = nearestElementAncestor(r->startContainer());

    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const auto& rect : rects) {
        auto pr = ctxEl
            ? bro::dom::projectRectThroughAncestors(ctxEl, rect.x, rect.y, rect.width, rect.height)
            : bro::dom::AbsoluteRect{rect.x, rect.y, rect.width, rect.height};
        JS_SetPropertyUint32(ctx, arr, i++,
            makeDomRect(ctx, pr.x, pr.y + offY, pr.width, pr.height));
    }
    JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, static_cast<int>(i)));
    return arr;
}

static JSValue js_range_getBoundingClientRect(JSContext* ctx, JSValueConst this_val,
                                              int /*argc*/, JSValueConst* /*argv*/) {
    auto* r = getRange(this_val);
    if (!r) return makeDomRect(ctx, 0, 0, 0, 0);
    bro::dom::Document* doc;
    htmlayout::layout::TextMetrics* metrics;
    float offY;
    if (!fetchGeometryDeps(ctx, doc, metrics, offY))
        return makeDomRect(ctx, 0, 0, 0, 0);

    auto rects = bro::layout::getSelectionRects(
        doc, r->startContainer(), r->startOffset(),
        r->endContainer(), r->endOffset(), *metrics);

    // A collapsed range selects no text, so there are no highlight rects — but
    // it still has a position, and per spec its rect is an empty one AT the
    // caret rather than a degenerate one at the document origin. Returning
    // {0,0,0,0} here made a collapsed range indistinguishable from a failed
    // query, and put every "where is the caret" answer in the top-left corner.
    if (rects.empty() && r->collapsed()) {
        if (auto* tn = dynamic_cast<bro::dom::TextNode*>(r->startContainer())) {
            float cx = 0.0f, cy = 0.0f, ch = 0.0f;
            if (bro::layout::getCaretRect(doc, tn, r->startOffset(), *metrics,
                                          cx, cy, ch)) {
                auto* el = nearestElementAncestor(r->startContainer());
                auto pr = el ? bro::dom::projectRectThroughAncestors(el, cx, cy, 0.0f, ch)
                             : bro::dom::AbsoluteRect{cx, cy, 0.0f, ch};
                return makeDomRect(ctx, pr.x, pr.y + offY, 0.0f, pr.height);
            }
        }
    }
    if (rects.empty()) return makeDomRect(ctx, 0, 0, 0, 0);

    // See getClientRects() above — project each rect before taking the AABB.
    auto* ctxEl = nearestElementAncestor(r->startContainer());
    auto proj = [&](const htmlayout::layout::Rect& rect) {
        return ctxEl
            ? bro::dom::projectRectThroughAncestors(ctxEl, rect.x, rect.y, rect.width, rect.height)
            : bro::dom::AbsoluteRect{rect.x, rect.y, rect.width, rect.height};
    };

    auto first = proj(rects.front());
    float left = first.x, top = first.y;
    float right = first.x + first.width, bottom = first.y + first.height;
    for (const auto& rect : rects) {
        auto pr = proj(rect);
        left   = std::min(left,   pr.x);
        top    = std::min(top,    pr.y);
        right  = std::max(right,  pr.x + pr.width);
        bottom = std::max(bottom, pr.y + pr.height);
    }
    return makeDomRect(ctx, left, top + offY, right - left, bottom - top);
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
        .get("startOffset", [](Range* r) -> int {
            return offsetToJs(r->startContainer(), r->startOffset());
        })
        .get("endOffset",   [](Range* r) -> int {
            return offsetToJs(r->endContainer(), r->endOffset());
        })
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
        .method_raw("detach",                  js_range_detach, 0)
        .method_raw("getClientRects",          js_range_getClientRects, 0)
        .method_raw("getBoundingClientRect",   js_range_getBoundingClientRect, 0);

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
