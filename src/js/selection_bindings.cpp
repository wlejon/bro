#include "js/dom_bindings_internal.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "dom/document.h"
#include "dom/text_offsets.h"

#include <qjsbind/qjsbind.h>

namespace bro::js {

using Selection = bro::dom::Selection;
using Range = bro::dom::Range;

static Selection* getSelection(JSValueConst val) {
    return static_cast<Selection*>(JS_GetOpaque(val, js_selection_class_id));
}

// Boundary-point parsing: a Node-or-null JSValue + an integer offset.
static bro::dom::Node* argNode(JSContext* ctx, JSValueConst v) {
    return unwrapNode(ctx, v);
}

// Selection endpoints are stored the engine's way — UTF-8 byte offsets into
// Text/Comment containers, child indices into Elements — because the caret,
// hit-testing and contenteditable paths read them directly. The Selection API
// speaks UTF-16 code units, so every offset converts at this boundary (and
// nowhere else). See dom/text_offsets.h.
static int selOffsetToJs(bro::dom::Node* container, int byteOffset) {
    return bro::dom::nodeOffsetToUtf16(container, byteOffset);
}
static int selOffsetFromJs(bro::dom::Node* container, int utf16Offset) {
    return bro::dom::nodeOffsetToBytes(container, utf16Offset);
}

// ---------------------------------------------------------------------------
// Methods (raw signatures — manual argv parsing for clarity)
// ---------------------------------------------------------------------------

static JSValue js_sel_getRangeAt(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* s = getSelection(this_val);
    if (!s) return JS_NULL;
    int32_t idx = 0;
    if (argc >= 1) JS_ToInt32(ctx, &idx, argv[0]);
    // Spec: getRangeAt returns the live range. Here we return a *clone* so
    // callers can mutate it independently; the Selection's own range is
    // updated separately via collapse/extend/setRange. Parity with Firefox
    // is not perfect but matches Chrome's practical behavior.
    auto* src = s->getRangeAt(idx);
    if (!src) return JS_NULL;
    Range* clone = src->cloneRange();
    return qjsbind::wrap<Range>(ctx, clone);
}

static JSValue js_sel_addRange(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    auto* s = getSelection(this_val);
    if (!s || argc < 1) return JS_UNDEFINED;
    auto* r = static_cast<Range*>(JS_GetOpaque(argv[0], js_range_class_id));
    if (r) s->addRange(*r);
    return JS_UNDEFINED;
}

static JSValue js_sel_removeRange(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    auto* s = getSelection(this_val);
    if (!s || argc < 1) return JS_UNDEFINED;
    auto* r = static_cast<Range*>(JS_GetOpaque(argv[0], js_range_class_id));
    if (r) s->removeRange(*r);
    return JS_UNDEFINED;
}

static JSValue js_sel_removeAllRanges(JSContext* /*ctx*/, JSValueConst this_val,
                                      int /*argc*/, JSValueConst* /*argv*/) {
    auto* s = getSelection(this_val);
    if (s) s->removeAllRanges();
    return JS_UNDEFINED;
}

static JSValue js_sel_empty(JSContext* /*ctx*/, JSValueConst this_val,
                            int /*argc*/, JSValueConst* /*argv*/) {
    auto* s = getSelection(this_val);
    if (s) s->empty();
    return JS_UNDEFINED;
}

static JSValue js_sel_collapse(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    auto* s = getSelection(this_val);
    if (!s || argc < 1) return JS_UNDEFINED;
    auto* n = argNode(ctx, argv[0]);
    int32_t off = 0;
    if (argc >= 2) JS_ToInt32(ctx, &off, argv[1]);
    s->collapse(n, selOffsetFromJs(n, off));
    return JS_UNDEFINED;
}

static JSValue js_sel_setPosition(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    // Alias for collapse()
    return js_sel_collapse(ctx, this_val, argc, argv);
}

static JSValue js_sel_collapseToStart(JSContext* /*ctx*/, JSValueConst this_val,
                                      int /*argc*/, JSValueConst* /*argv*/) {
    auto* s = getSelection(this_val);
    if (s) s->collapseToStart();
    return JS_UNDEFINED;
}

static JSValue js_sel_collapseToEnd(JSContext* /*ctx*/, JSValueConst this_val,
                                    int /*argc*/, JSValueConst* /*argv*/) {
    auto* s = getSelection(this_val);
    if (s) s->collapseToEnd();
    return JS_UNDEFINED;
}

static JSValue js_sel_extend(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    auto* s = getSelection(this_val);
    if (!s || argc < 1) return JS_UNDEFINED;
    auto* n = argNode(ctx, argv[0]);
    int32_t off = 0;
    if (argc >= 2) JS_ToInt32(ctx, &off, argv[1]);
    s->extend(n, selOffsetFromJs(n, off));
    return JS_UNDEFINED;
}

static JSValue js_sel_selectAllChildren(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* s = getSelection(this_val);
    if (!s || argc < 1) return JS_UNDEFINED;
    s->selectAllChildren(argNode(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_sel_setBaseAndExtent(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* s = getSelection(this_val);
    if (!s || argc < 4) return JS_UNDEFINED;
    auto* anchor = argNode(ctx, argv[0]);
    int32_t anchorOff = 0; JS_ToInt32(ctx, &anchorOff, argv[1]);
    auto* focus = argNode(ctx, argv[2]);
    int32_t focusOff = 0; JS_ToInt32(ctx, &focusOff, argv[3]);
    if (!anchor || !focus) return JS_UNDEFINED;
    // Convert once, up front: everything below (including the ordering probe)
    // works in the engine's byte domain.
    anchorOff = selOffsetFromJs(anchor, anchorOff);
    focusOff  = selOffsetFromJs(focus,  focusOff);
    // Determine direction by comparing anchor vs focus in tree order. Probe
    // from a range COLLAPSED at the anchor and ask where the focus falls:
    // setting (start=anchor, end=focus) and re-reading the start cannot work,
    // because Range::setEnd normalizes an inverted pair by dragging the END
    // back to the start — the start never moves, so a backward pair looked
    // forward and collapsed the selection onto the anchor.
    Range probe;
    probe.setStart(anchor, anchorOff);
    probe.setEnd(anchor, anchorOff);
    const bool backward = probe.comparePoint(focus, focusOff) < 0;
    if (backward) {
        s->setRange(focus, focusOff, anchor, anchorOff, Selection::Backward);
    } else {
        s->setRange(anchor, anchorOff, focus, focusOff, Selection::Forward);
    }
    return JS_UNDEFINED;
}

static JSValue js_sel_containsNode(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* s = getSelection(this_val);
    if (!s || argc < 1) return JS_FALSE;
    bool partial = false;
    if (argc >= 2) partial = JS_ToBool(ctx, argv[1]);
    return JS_NewBool(ctx, s->containsNode(argNode(ctx, argv[0]), partial));
}

static JSValue js_sel_toString(JSContext* ctx, JSValueConst this_val,
                               int /*argc*/, JSValueConst* /*argv*/) {
    auto* s = getSelection(this_val);
    if (!s) return JS_NewString(ctx, "");
    return JS_NewString(ctx, s->toString().c_str());
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void installSelectionBindings(JSContext* ctx)
{
    qjsbind::Class<Selection>(ctx, "Selection",
                              qjsbind::NoGlobal | qjsbind::NoDestructor)
        .get("anchorNode",   [](Selection* s, JSContext* cx) -> JSValue {
            return wrapAnyNode(cx, s->anchorNode());
        })
        .get("anchorOffset", [](Selection* s) -> int {
            return selOffsetToJs(s->anchorNode(), s->anchorOffset());
        })
        .get("focusNode",    [](Selection* s, JSContext* cx) -> JSValue {
            return wrapAnyNode(cx, s->focusNode());
        })
        .get("focusOffset",  [](Selection* s) -> int {
            return selOffsetToJs(s->focusNode(), s->focusOffset());
        })
        .get("isCollapsed",  [](Selection* s) -> bool { return s->isCollapsed(); })
        .get("rangeCount",   [](Selection* s) -> int { return s->rangeCount(); })
        .get("type",         [](Selection* s) -> std::string { return s->type(); })
        .method_raw("getRangeAt",        js_sel_getRangeAt, 1)
        .method_raw("addRange",          js_sel_addRange, 1)
        .method_raw("removeRange",       js_sel_removeRange, 1)
        .method_raw("removeAllRanges",   js_sel_removeAllRanges, 0)
        .method_raw("empty",             js_sel_empty, 0)
        .method_raw("collapse",          js_sel_collapse, 2)
        .method_raw("setPosition",       js_sel_setPosition, 2)
        .method_raw("collapseToStart",   js_sel_collapseToStart, 0)
        .method_raw("collapseToEnd",     js_sel_collapseToEnd, 0)
        .method_raw("extend",            js_sel_extend, 2)
        .method_raw("selectAllChildren", js_sel_selectAllChildren, 1)
        .method_raw("setBaseAndExtent",  js_sel_setBaseAndExtent, 4)
        .method_raw("containsNode",      js_sel_containsNode, 2)
        .method_raw("toString",          js_sel_toString, 0);

    js_selection_class_id = qjsbind::class_id<Selection>();
}

JSValue wrapSelection(JSContext* ctx, Selection* s) {
    if (!s) return JS_NULL;
    return qjsbind::wrap_unowned<Selection>(ctx, s);
}

} // namespace bro::js
