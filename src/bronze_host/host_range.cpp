// DOM Range implementation for bronze_host.

#include "bronze_host/host_range.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "dom/range.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/text_offsets.h"
#include "dom/text_node.h"
#include "layout/selection_geometry.h"
#include "engine/engine.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

HostClass g_rangeClass;

namespace {

inline constexpr uint32_t kHostRangeTag = 0x524E4745u;  // 'RNGE'

struct HostRangeCell {
    uint32_t tag = kHostRangeTag;
    bro::dom::Range* range = nullptr;
    bool owned = true;
};

void hostRangeDtor(void* p) {
    auto* cell = static_cast<HostRangeCell*>(p);
    if (cell) {
        if (cell->range && cell->owned) {
            cell->range->setDocument(nullptr);
            delete cell->range;
        }
        delete cell;
    }
}

HostRangeCell* hostRangeCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* cell = static_cast<HostRangeCell*>(ev::handleData(v));
    if (!cell || cell->tag != kHostRangeTag) return nullptr;
    return cell;
}

Value makeDomRectValue(double x, double y, double w, double h) {
    ObjectBuilder r;
    r.set("x", ev::fromDouble(x));
    r.set("y", ev::fromDouble(y));
    r.set("left", ev::fromDouble(x));
    r.set("top", ev::fromDouble(y));
    r.set("right", ev::fromDouble(x + w));
    r.set("bottom", ev::fromDouble(y + h));
    r.set("width", ev::fromDouble(w));
    r.set("height", ev::fromDouble(h));
    return r.get();
}

bro::dom::Element* nearestElementAncestor(dom::Node* node) {
    for (dom::Node* n = node; n; n = n->parentNode()) {
        if (n->nodeType() == bro::dom::NodeType::Element)
            return static_cast<bro::dom::Element*>(n);
    }
    return nullptr;
}

bool fetchGeometryDeps(bro::dom::Document*& outDoc,
                       htmlayout::layout::TextMetrics*& outMetrics,
                       float& outOffsetY) {
    auto* engine = hostEngine();
    if (!engine) return false;
    outDoc = engine->document();
    if (!outDoc) return false;
    outMetrics = engine->textMetrics();
    if (!outMetrics) return false;
    outOffsetY = engine->docContentOffsetY();
    return true;
}

Value js_range_ctor(Value, std::span<const Value>) {
    auto* r = new bro::dom::Range();
    if (auto* engine = hostEngine()) {
        if (auto* doc = engine->document()) {
            r->setDocument(doc);
        }
    }
    return wrapOwnedRange(r);
}

void decorateRangeProto(ObjectBuilder& b) {
    // Spec constants on prototype
    b.set("START_TO_START", ev::fromDouble(0));
    b.set("START_TO_END",   ev::fromDouble(1));
    b.set("END_TO_END",     ev::fromDouble(2));
    b.set("END_TO_START",   ev::fromDouble(3));

    b.accessor("startContainer", [](Value self_, std::span<const Value>) {
        auto* r = hostRangeOf(self_);
        if (!r || !r->startContainer()) return ev::null();
        return hostNodeValue(r->startContainer());
    }, nullptr);

    b.accessor("endContainer", [](Value self_, std::span<const Value>) {
        auto* r = hostRangeOf(self_);
        if (!r || !r->endContainer()) return ev::null();
        return hostNodeValue(r->endContainer());
    }, nullptr);

    b.accessor("startOffset", [](Value self_, std::span<const Value>) {
        auto* r = hostRangeOf(self_);
        if (!r) return ev::fromDouble(0.0);
        return ev::fromDouble(bro::dom::nodeOffsetToUtf16(r->startContainer(), r->startOffset()));
    }, nullptr);

    b.accessor("endOffset", [](Value self_, std::span<const Value>) {
        auto* r = hostRangeOf(self_);
        if (!r) return ev::fromDouble(0.0);
        return ev::fromDouble(bro::dom::nodeOffsetToUtf16(r->endContainer(), r->endOffset()));
    }, nullptr);

    b.accessor("collapsed", [](Value self_, std::span<const Value>) {
        auto* r = hostRangeOf(self_);
        return ev::fromBool(r ? r->collapsed() : true);
    }, nullptr);

    b.accessor("commonAncestorContainer", [](Value self_, std::span<const Value>) {
        auto* r = hostRangeOf(self_);
        if (!r || !r->commonAncestorContainer()) return ev::null();
        return hostNodeValue(r->commonAncestorContainer());
    }, nullptr);

    b.def("setStart", 2, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        int off = a.size() > 1 ? static_cast<int>(ev::toDouble(a[1])) : 0;
        r->setStart(n, bro::dom::nodeOffsetToBytes(n, off));
        return ev::undefined();
    });

    b.def("setEnd", 2, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        int off = a.size() > 1 ? static_cast<int>(ev::toDouble(a[1])) : 0;
        r->setEnd(n, bro::dom::nodeOffsetToBytes(n, off));
        return ev::undefined();
    });

    b.def("setStartBefore", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        if (n) r->setStartBefore(n);
        return ev::undefined();
    });

    b.def("setStartAfter", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        if (n) r->setStartAfter(n);
        return ev::undefined();
    });

    b.def("setEndBefore", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        if (n) r->setEndBefore(n);
        return ev::undefined();
    });

    b.def("setEndAfter", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        if (n) r->setEndAfter(n);
        return ev::undefined();
    });

    b.def("collapse", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r) return ev::undefined();
        bool toStart = a.empty() || ev::toBool(a[0]);
        r->collapse(toStart);
        return ev::undefined();
    });

    b.def("selectNode", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        if (n) r->selectNode(n);
        return ev::undefined();
    });

    b.def("selectNodeContents", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        if (n) r->selectNodeContents(n);
        return ev::undefined();
    });

    b.def("comparePoint", 2, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::fromDouble(0.0);
        auto* n = hostNodeOf(a[0]);
        int off = a.size() > 1 ? static_cast<int>(ev::toDouble(a[1])) : 0;
        return ev::fromDouble(r->comparePoint(n, bro::dom::nodeOffsetToBytes(n, off)));
    });

    b.def("isPointInRange", 2, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::fromBool(false);
        auto* n = hostNodeOf(a[0]);
        int off = a.size() > 1 ? static_cast<int>(ev::toDouble(a[1])) : 0;
        return ev::fromBool(r->isPointInRange(n, bro::dom::nodeOffsetToBytes(n, off)));
    });

    b.def("intersectsNode", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::fromBool(false);
        auto* n = hostNodeOf(a[0]);
        return ev::fromBool(n ? r->intersectsNode(n) : false);
    });

    b.def("deleteContents", 0, [](Value self_, std::span<const Value>) {
        if (auto* r = hostRangeOf(self_)) r->deleteContents();
        return ev::undefined();
    });

    b.def("cloneContents", 0, [](Value self_, std::span<const Value>) {
        auto* r = hostRangeOf(self_);
        if (!r) return ev::null();
        dom::Node* frag = r->cloneContents();
        return hostNodeValue(frag);
    });

    b.def("extractContents", 0, [](Value self_, std::span<const Value>) {
        auto* r = hostRangeOf(self_);
        if (!r) return ev::null();
        dom::Node* frag = r->extractContents();
        return hostNodeValue(frag);
    });

    b.def("insertNode", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        if (n) r->insertNode(n);
        return ev::undefined();
    });

    b.def("surroundContents", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* el = hostElementOf(a[0]);
        if (el) r->surroundContents(el);
        return ev::undefined();
    });

    b.def("createContextualFragment", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r) return ev::null();
        std::string html = (!a.empty() && !ev::isObject(a[0]) && !ev::isUndefined(a[0]))
                               ? ev::toUtf8(a[0]) : "";
        dom::Node* frag = r->createContextualFragment(html);
        return hostNodeValue(frag);
    });

    b.def("cloneRange", 0, [](Value self_, std::span<const Value>) {
        auto* r = hostRangeOf(self_);
        if (!r) return ev::null();
        return wrapOwnedRange(r->cloneRange());
    });

    b.def("toString", 0, [](Value self_, std::span<const Value>) {
        auto* r = hostRangeOf(self_);
        return ev::fromUtf8(r ? r->toString() : "");
    });

    b.def("detach", 0, [](Value, std::span<const Value>) {
        // Spec: no-op
        return ev::undefined();
    });

    b.def("getClientRects", 0, [](Value self_, std::span<const Value>) {
        auto* r = hostRangeOf(self_);
        if (!r) return hostArrayOf(0, [](size_t) { return ev::undefined(); });

        bro::dom::Document* doc = nullptr;
        htmlayout::layout::TextMetrics* metrics = nullptr;
        float offY = 0.0f;
        if (!fetchGeometryDeps(doc, metrics, offY)) {
            return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        }

        auto rects = bro::layout::getSelectionRects(
            doc, r->startContainer(), r->startOffset(),
            r->endContainer(), r->endOffset(), *metrics);

        auto* ctxEl = nearestElementAncestor(r->startContainer());
        return hostArrayOf(rects.size(), [&](size_t i) {
            const auto& rect = rects[i];
            auto pr = ctxEl
                ? bro::dom::projectRectThroughAncestors(ctxEl, rect.x, rect.y, rect.width, rect.height)
                : bro::dom::AbsoluteRect{rect.x, rect.y, rect.width, rect.height};
            return makeDomRectValue(pr.x, pr.y + offY, pr.width, pr.height);
        });
    });

    b.def("getBoundingClientRect", 0, [](Value self_, std::span<const Value>) {
        auto* r = hostRangeOf(self_);
        if (!r) return makeDomRectValue(0, 0, 0, 0);

        bro::dom::Document* doc = nullptr;
        htmlayout::layout::TextMetrics* metrics = nullptr;
        float offY = 0.0f;
        if (!fetchGeometryDeps(doc, metrics, offY)) {
            return makeDomRectValue(0, 0, 0, 0);
        }

        auto rects = bro::layout::getSelectionRects(
            doc, r->startContainer(), r->startOffset(),
            r->endContainer(), r->endOffset(), *metrics);

        if (rects.empty() && r->collapsed()) {
            if (auto* tn = dynamic_cast<bro::dom::TextNode*>(r->startContainer())) {
                float cx = 0.0f, cy = 0.0f, ch = 0.0f;
                if (bro::layout::getCaretRect(doc, tn, r->startOffset(), *metrics, cx, cy, ch)) {
                    auto* el = nearestElementAncestor(r->startContainer());
                    auto pr = el ? bro::dom::projectRectThroughAncestors(el, cx, cy, 0.0f, ch)
                                 : bro::dom::AbsoluteRect{cx, cy, 0.0f, ch};
                    return makeDomRectValue(pr.x, pr.y + offY, 0.0f, pr.height);
                }
            }
        }
        if (rects.empty()) return makeDomRectValue(0, 0, 0, 0);

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
        return makeDomRectValue(left, top + offY, right - left, bottom - top);
    });
}

} // namespace

bro::dom::Range* hostRangeOf(Value v) {
    auto* cell = hostRangeCellOf(v);
    return cell ? cell->range : nullptr;
}

Value wrapOwnedRange(bro::dom::Range* r) {
    if (!r) return ev::null();
    auto* cell = new HostRangeCell{kHostRangeTag, r, true};
    return g_rangeClass.make(cell, hostRangeDtor);
}

void installRangeGlobals() {
    g_rangeClass.install("Range", 0, js_range_ctor, decorateRangeProto);
    g_rangeClass.setStatic("START_TO_START", ev::fromDouble(0));
    g_rangeClass.setStatic("START_TO_END",   ev::fromDouble(1));
    g_rangeClass.setStatic("END_TO_END",     ev::fromDouble(2));
    g_rangeClass.setStatic("END_TO_START",   ev::fromDouble(3));
}

} // namespace bro::bronze_host
