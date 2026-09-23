// DOM Selection implementation for bronze_host.

#include "bronze_host/host_selection.h"
#include "bronze_host/host_range.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "dom/selection.h"
#include "dom/range.h"
#include "dom/document.h"
#include "dom/node.h"
#include "dom/text_offsets.h"

#include <string>
#include <unordered_map>

namespace bro::bronze_host {

HostClass g_selectionClass;

namespace {

inline constexpr uint32_t kHostSelectionTag = 0x53454C43u;  // 'SELC'

struct HostSelectionCell {
    uint32_t tag = kHostSelectionTag;
    bro::dom::Selection* selection = nullptr;
};

void hostSelectionDtor(void* p) {
    delete static_cast<HostSelectionCell*>(p);
}

HostSelectionCell* hostSelectionCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* cell = static_cast<HostSelectionCell*>(ev::handleData(v));
    if (!cell || cell->tag != kHostSelectionTag) return nullptr;
    return cell;
}

std::unordered_map<bro::dom::Selection*, ev::Persistent> s_selectionWrappers;

void decorateSelectionProto(ObjectBuilder& b) {
    b.accessor("anchorNode", [](Value self_, std::span<const Value>) {
        auto* s = hostSelectionOf(self_);
        if (!s || !s->anchorNode()) return ev::null();
        return hostNodeValue(s->anchorNode());
    }, nullptr);

    b.accessor("anchorOffset", [](Value self_, std::span<const Value>) {
        auto* s = hostSelectionOf(self_);
        if (!s) return ev::fromDouble(0.0);
        return ev::fromDouble(bro::dom::nodeOffsetToUtf16(s->anchorNode(), s->anchorOffset()));
    }, nullptr);

    b.accessor("focusNode", [](Value self_, std::span<const Value>) {
        auto* s = hostSelectionOf(self_);
        if (!s || !s->focusNode()) return ev::null();
        return hostNodeValue(s->focusNode());
    }, nullptr);

    b.accessor("focusOffset", [](Value self_, std::span<const Value>) {
        auto* s = hostSelectionOf(self_);
        if (!s) return ev::fromDouble(0.0);
        return ev::fromDouble(bro::dom::nodeOffsetToUtf16(s->focusNode(), s->focusOffset()));
    }, nullptr);

    b.accessor("isCollapsed", [](Value self_, std::span<const Value>) {
        auto* s = hostSelectionOf(self_);
        return ev::fromBool(s ? s->isCollapsed() : true);
    }, nullptr);

    b.accessor("rangeCount", [](Value self_, std::span<const Value>) {
        auto* s = hostSelectionOf(self_);
        return ev::fromDouble(s ? s->rangeCount() : 0.0);
    }, nullptr);

    b.accessor("type", [](Value self_, std::span<const Value>) {
        auto* s = hostSelectionOf(self_);
        return ev::fromUtf8(s ? s->type() : "None");
    }, nullptr);

    b.def("getRangeAt", 1, [](Value self_, std::span<const Value> a) {
        auto* s = hostSelectionOf(self_);
        if (!s) return ev::null();
        int idx = a.empty() ? 0 : satCast<int>(ev::toDouble(a[0]));
        auto* src = s->getRangeAt(idx);
        if (!src) return ev::null();
        return wrapOwnedRange(src->cloneRange());
    });

    b.def("addRange", 1, [](Value self_, std::span<const Value> a) {
        auto* s = hostSelectionOf(self_);
        if (!s || a.empty()) return ev::undefined();
        auto* r = hostRangeOf(a[0]);
        if (r) s->addRange(*r);
        return ev::undefined();
    });

    b.def("removeRange", 1, [](Value self_, std::span<const Value> a) {
        auto* s = hostSelectionOf(self_);
        if (!s || a.empty()) return ev::undefined();
        auto* r = hostRangeOf(a[0]);
        if (r) s->removeRange(*r);
        return ev::undefined();
    });

    b.def("removeAllRanges", 0, [](Value self_, std::span<const Value>) {
        if (auto* s = hostSelectionOf(self_)) s->removeAllRanges();
        return ev::undefined();
    });

    b.def("empty", 0, [](Value self_, std::span<const Value>) {
        if (auto* s = hostSelectionOf(self_)) s->empty();
        return ev::undefined();
    });

    b.def("collapse", 2, [](Value self_, std::span<const Value> a) {
        auto* s = hostSelectionOf(self_);
        if (!s || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        int off = a.size() > 1 ? satCast<int>(ev::toDouble(a[1])) : 0;
        s->collapse(n, bro::dom::nodeOffsetToBytes(n, off));
        return ev::undefined();
    });

    b.def("setPosition", 2, [](Value self_, std::span<const Value> a) {
        auto* s = hostSelectionOf(self_);
        if (!s || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        int off = a.size() > 1 ? satCast<int>(ev::toDouble(a[1])) : 0;
        s->collapse(n, bro::dom::nodeOffsetToBytes(n, off));
        return ev::undefined();
    });

    b.def("collapseToStart", 0, [](Value self_, std::span<const Value>) {
        if (auto* s = hostSelectionOf(self_)) s->collapseToStart();
        return ev::undefined();
    });

    b.def("collapseToEnd", 0, [](Value self_, std::span<const Value>) {
        if (auto* s = hostSelectionOf(self_)) s->collapseToEnd();
        return ev::undefined();
    });

    b.def("extend", 2, [](Value self_, std::span<const Value> a) {
        auto* s = hostSelectionOf(self_);
        if (!s || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        int off = a.size() > 1 ? satCast<int>(ev::toDouble(a[1])) : 0;
        s->extend(n, bro::dom::nodeOffsetToBytes(n, off));
        return ev::undefined();
    });

    b.def("selectAllChildren", 1, [](Value self_, std::span<const Value> a) {
        auto* s = hostSelectionOf(self_);
        if (!s || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        if (n) s->selectAllChildren(n);
        return ev::undefined();
    });

    b.def("setBaseAndExtent", 4, [](Value self_, std::span<const Value> a) {
        auto* s = hostSelectionOf(self_);
        if (!s || a.size() < 4) return ev::undefined();
        auto* anchor = hostNodeOf(a[0]);
        int anchorOff = satCast<int>(ev::toDouble(a[1]));
        auto* focus = hostNodeOf(a[2]);
        int focusOff = satCast<int>(ev::toDouble(a[3]));
        if (!anchor || !focus) return ev::undefined();

        anchorOff = bro::dom::nodeOffsetToBytes(anchor, anchorOff);
        focusOff  = bro::dom::nodeOffsetToBytes(focus,  focusOff);

        bro::dom::Range probe;
        probe.setStart(anchor, anchorOff);
        probe.setEnd(anchor, anchorOff);
        const bool backward = probe.comparePoint(focus, focusOff) < 0;
        if (backward) {
            s->setRange(focus, focusOff, anchor, anchorOff, bro::dom::Selection::Backward);
        } else {
            s->setRange(anchor, anchorOff, focus, focusOff, bro::dom::Selection::Forward);
        }
        return ev::undefined();
    });

    b.def("containsNode", 2, [](Value self_, std::span<const Value> a) {
        auto* s = hostSelectionOf(self_);
        if (!s || a.empty()) return ev::fromBool(false);
        auto* n = hostNodeOf(a[0]);
        bool partial = a.size() > 1 && ev::toBool(a[1]);
        return ev::fromBool(n ? s->containsNode(n, partial) : false);
    });

    b.def("toString", 0, [](Value self_, std::span<const Value>) {
        auto* s = hostSelectionOf(self_);
        return ev::fromUtf8(s ? s->toString() : "");
    });
}

} // namespace

bro::dom::Selection* hostSelectionOf(Value v) {
    auto* cell = hostSelectionCellOf(v);
    return cell ? cell->selection : nullptr;
}

Value wrapSelection(bro::dom::Selection* s) {
    if (!s) return ev::null();
    auto it = s_selectionWrappers.find(s);
    if (it != s_selectionWrappers.end() && !it->second.get().isUndefined()) {
        return it->second.get();
    }
    auto* cell = new HostSelectionCell{kHostSelectionTag, s};
    Value v = g_selectionClass.make(cell, hostSelectionDtor);
    s_selectionWrappers[s].set(v);
    return v;
}

void installSelectionGlobals() {
    g_selectionClass.install(
        "Selection", 0,
        [](Value, std::span<const Value>) {
            return ev::throwTypeError("Illegal constructor");
        },
        decorateSelectionProto);
}

} // namespace bro::bronze_host
