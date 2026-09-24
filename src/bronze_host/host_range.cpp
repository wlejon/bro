// DOM Range implementation for bronze_host.

#include "bronze_host/host_range.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "dom/range.h"
#include "dom/selection.h"
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

// Shared, not owned outright: the object a script holds may also be the
// document Selection's live range (getRangeAt / addRange share it), and
// either side can outlive the other.
struct HostRangeCell {
    uint32_t tag = kHostRangeTag;
    std::shared_ptr<bro::dom::Range> range;
};

void hostRangeDtor(void* p) {
    delete static_cast<HostRangeCell*>(p);
}

// A Range method moved this range's boundaries. If it is the selection's live
// range, the selection moved: report it (and repaint).
void noteRangeMutated(bro::dom::Range* r) {
    if (!r || !r->document()) return;
    auto* doc = r->document();
    if (auto* sel = doc->selection(); sel && sel->getRangeAt(0) == r)
        doc->fireSelectionChange();
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
    // A geometry read lays the document out first, as an element's does.
    engine->flushLayoutForRead(outDoc);
    // Viewport space: the document scroll comes off, the menu bar's inset
    // does NOT go on. Client coordinates start below the menu bar — that is
    // where element rects and mouse clientY already start — so adding
    // contentTop() put every range rect one menu bar too low.
    outOffsetY = -engine->viewportScrollY();
    return true;
}

// The DOMException a Range mutation reports, or undefined when it succeeded.
Value throwRangeError(bro::dom::Range::Error e, const char* who) {
    using E = bro::dom::Range::Error;
    switch (e) {
    case E::None: return ev::undefined();
    case E::InvalidState:
        return ev::throwValue(hostMakeDomError("InvalidStateError",
            std::string(who) + ": the range partially selects a non-Text node"));
    case E::HierarchyRequest:
        return ev::throwValue(hostMakeDomError("HierarchyRequestError",
            std::string(who) + ": the node cannot be inserted at the range's start"));
    case E::InvalidNodeType:
        return ev::throwValue(hostMakeDomError("InvalidNodeTypeError",
            std::string(who) + ": the new parent cannot be a document fragment"));
    }
    return ev::undefined();
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

bool computeCollapsedCaret(bro::dom::Document* doc,
                           bro::dom::Range* r,
                           htmlayout::layout::TextMetrics* metrics,
                           float offY,
                           double& outX, double& outY, double& outW, double& outH) {
    if (!r || !doc) return false;

    // 1. Try text boundary resolution via toTextBoundary / getCaretRect
    dom::TextNode* tn = nullptr;
    int tOff = 0;
    bro::layout::toTextBoundary(r->startContainer(), r->startOffset(), /*preferLeading=*/true, tn, tOff);
    if (!tn) {
        bro::layout::toTextBoundary(r->startContainer(), r->startOffset(), /*preferLeading=*/false, tn, tOff);
    }
    if (tn && metrics) {
        float cx = 0.0f, cy = 0.0f, ch = 0.0f;
        if (bro::layout::getCaretRect(doc, tn, tOff, *metrics, cx, cy, ch)) {
            auto* el = nearestElementAncestor(tn);
            auto pr = el ? bro::dom::projectRectThroughAncestors(el, cx, cy, 0.0f, ch)
                         : bro::dom::AbsoluteRect{cx, cy, 0.0f, ch};
            outX = pr.x;
            outY = pr.y + offY;
            outW = 0.0;
            outH = pr.height;
            return true;
        }
    }

    // 2. Fall back to nearest element layout box for non-text / empty element containers
    auto* el = nearestElementAncestor(r->startContainer());
    if (el) {
        if (auto* eng = hostEngine()) {
            eng->flushLayoutForRead(doc);
        }
        const auto& box = el->layoutBox();
        auto bbox = bro::dom::absoluteBorderBox(el);
        float ch = box.contentRect.height > 0.0f ? box.contentRect.height
                 : (bbox.height > 0.0f ? bbox.height : 16.0f);
        float cx = bbox.x + box.border.left + box.padding.left;
        float cy = bbox.y + box.border.top + box.padding.top;
        outX = cx;
        outY = cy + offY;
        outW = 0.0;
        outH = ch;
        return true;
    }

    return false;
}

// The topmost elements the range selects whole, in tree order — the ones
// whose own boxes CSSOM puts in the range's client rects.
void collectContainedElements(bro::dom::Range* r, dom::Node* n,
                              std::vector<dom::Element*>& out) {
    const auto& kids = n->childNodes();
    for (size_t i = 0; i < kids.size(); ++i) {
        dom::Node* c = kids[i];
        const int idx = static_cast<int>(i);
        if (r->comparePoint(n, idx) == 0 && r->comparePoint(n, idx + 1) == 0) {
            if (c->nodeType() == dom::NodeType::Element)
                out.push_back(static_cast<dom::Element*>(c));
            continue;
        }
        if (r->intersectsNode(c)) collectContainedElements(r, c, out);
    }
}

// Range.getClientRects() in viewport space: the border boxes of the elements
// the range selects whole (per line fragment for an inline), then the
// selected text, band per line. Not clipped to scrollers — CSSOM reports text
// scrolled out of view where it is. A range selecting no box answers its
// caret, so a collapsed or empty range still has a position.
std::vector<bro::dom::AbsoluteRect> rangeClientRects(bro::dom::Range* r) {
    std::vector<bro::dom::AbsoluteRect> out;
    bro::dom::Document* doc = nullptr;
    htmlayout::layout::TextMetrics* metrics = nullptr;
    float offY = 0.0f;
    if (!r || !r->startContainer() || !fetchGeometryDeps(doc, metrics, offY)) return out;

    if (!r->collapsed()) {
        std::vector<dom::Element*> whole;
        if (dom::Node* common = r->commonAncestorContainer())
            collectContainedElements(r, common, whole);
        for (dom::Element* el : whole) {
            for (auto pr : clientRectsOf(el)) {
                pr.y += offY;
                out.push_back(pr);
            }
        }
        auto rects = bro::layout::getSelectionRects(
            doc, r->startContainer(), r->startOffset(),
            r->endContainer(), r->endOffset(), *metrics, /*clipToOverflow=*/false);
        auto* ctxEl = nearestElementAncestor(r->startContainer());
        for (const auto& rect : rects) {
            auto pr = ctxEl
                ? bro::dom::projectRectThroughAncestors(ctxEl, rect.x, rect.y, rect.width, rect.height)
                : bro::dom::AbsoluteRect{rect.x, rect.y, rect.width, rect.height};
            pr.y += offY;
            out.push_back(pr);
        }
        if (!out.empty()) return out;
    }

    double cx = 0, cy = 0, cw = 0, ch = 0;
    if (computeCollapsedCaret(doc, r, metrics, offY, cx, cy, cw, ch))
        out.push_back({static_cast<float>(cx), static_cast<float>(cy),
                       static_cast<float>(cw), static_cast<float>(ch)});
    return out;
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
        int off = a.size() > 1 ? satCast<int>(ev::toDouble(a[1])) : 0;
        r->setStart(n, bro::dom::nodeOffsetToBytes(n, off));
        noteRangeMutated(r);
        return ev::undefined();
    });

    b.def("setEnd", 2, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        int off = a.size() > 1 ? satCast<int>(ev::toDouble(a[1])) : 0;
        r->setEnd(n, bro::dom::nodeOffsetToBytes(n, off));
        noteRangeMutated(r);
        return ev::undefined();
    });

    b.def("setStartBefore", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        if (n) r->setStartBefore(n);
        noteRangeMutated(r);
        return ev::undefined();
    });

    b.def("setStartAfter", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        if (n) r->setStartAfter(n);
        noteRangeMutated(r);
        return ev::undefined();
    });

    b.def("setEndBefore", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        if (n) r->setEndBefore(n);
        noteRangeMutated(r);
        return ev::undefined();
    });

    b.def("setEndAfter", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        if (n) r->setEndAfter(n);
        noteRangeMutated(r);
        return ev::undefined();
    });

    b.def("collapse", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r) return ev::undefined();
        bool toStart = a.empty() || ev::toBool(a[0]);
        r->collapse(toStart);
        noteRangeMutated(r);
        return ev::undefined();
    });

    b.def("selectNode", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        if (n) r->selectNode(n);
        noteRangeMutated(r);
        return ev::undefined();
    });

    b.def("selectNodeContents", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        if (n) r->selectNodeContents(n);
        noteRangeMutated(r);
        return ev::undefined();
    });

    b.def("comparePoint", 2, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::fromDouble(0.0);
        auto* n = hostNodeOf(a[0]);
        int off = a.size() > 1 ? satCast<int>(ev::toDouble(a[1])) : 0;
        return ev::fromDouble(r->comparePoint(n, bro::dom::nodeOffsetToBytes(n, off)));
    });

    b.def("isPointInRange", 2, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::fromBool(false);
        auto* n = hostNodeOf(a[0]);
        int off = a.size() > 1 ? satCast<int>(ev::toDouble(a[1])) : 0;
        return ev::fromBool(r->isPointInRange(n, bro::dom::nodeOffsetToBytes(n, off)));
    });

    b.def("intersectsNode", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::fromBool(false);
        auto* n = hostNodeOf(a[0]);
        return ev::fromBool(n ? r->intersectsNode(n) : false);
    });

    b.def("deleteContents", 0, [](Value self_, std::span<const Value>) {
        if (auto* r = hostRangeOf(self_)) {
            r->deleteContents();
            noteRangeMutated(r);
        }
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
        noteRangeMutated(r);
        return hostNodeValue(frag);
    });

    b.def("insertNode", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* n = hostNodeOf(a[0]);
        if (!n) return ev::throwTypeError("insertNode: argument is not a node");
        auto err = r->insertNode(n);
        noteRangeMutated(r);
        return throwRangeError(err, "insertNode");
    });

    b.def("surroundContents", 1, [](Value self_, std::span<const Value> a) {
        auto* r = hostRangeOf(self_);
        if (!r || a.empty()) return ev::undefined();
        auto* el = hostElementOf(a[0]);
        if (!el) return ev::throwTypeError("surroundContents: argument is not an element");
        auto err = r->surroundContents(el);
        noteRangeMutated(r);
        return throwRangeError(err, "surroundContents");
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

        std::vector<bro::dom::AbsoluteRect> rects = rangeClientRects(r);
        return hostArrayOf(rects.size(), [&](size_t i) {
            const auto& pr = rects[i];
            return makeDomRectValue(pr.x, pr.y, pr.width, pr.height);
        });
    });

    b.def("getBoundingClientRect", 0, [](Value self_, std::span<const Value>) {
        auto* r = hostRangeOf(self_);
        if (!r) return makeDomRectValue(0, 0, 0, 0);

        std::vector<bro::dom::AbsoluteRect> rects = rangeClientRects(r);
        if (rects.empty()) return makeDomRectValue(0, 0, 0, 0);
        // The union of the client rects (CSSOM), skipping empty ones unless
        // every one is empty — a collapsed caret is all-empty and still has a
        // position.
        bool any = false;
        float left = 0, top = 0, right = 0, bottom = 0;
        for (const auto& pr : rects) {
            if (any && (pr.width <= 0 && pr.height <= 0)) continue;
            if (!any) {
                left = pr.x; top = pr.y; right = pr.x + pr.width; bottom = pr.y + pr.height;
                any = true;
                continue;
            }
            left   = std::min(left,   pr.x);
            top    = std::min(top,    pr.y);
            right  = std::max(right,  pr.x + pr.width);
            bottom = std::max(bottom, pr.y + pr.height);
        }
        return makeDomRectValue(left, top, right - left, bottom - top);
    });
}

} // namespace

bro::dom::Range* hostRangeOf(Value v) {
    auto* cell = hostRangeCellOf(v);
    return cell ? cell->range.get() : nullptr;
}

std::shared_ptr<bro::dom::Range> hostSharedRangeOf(Value v) {
    auto* cell = hostRangeCellOf(v);
    return cell ? cell->range : nullptr;
}

Value wrapOwnedRange(bro::dom::Range* r) {
    if (!r) return ev::null();
    // The deleter unregisters from the document first (~Range would too; this
    // keeps the order explicit when the document is already gone).
    std::shared_ptr<bro::dom::Range> sp(r, [](bro::dom::Range* p) {
        p->setDocument(nullptr);
        delete p;
    });
    return wrapSharedRange(std::move(sp));
}

Value wrapSharedRange(std::shared_ptr<bro::dom::Range> r) {
    if (!r) return ev::null();
    auto* cell = new HostRangeCell{kHostRangeTag, std::move(r)};
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
