// Element node insertion, mutation, HTML injection, and scrolling.
//
// before, after, prepend, append, replaceWith, replaceChildren,
// insertAdjacentHTML, insertAdjacentElement, insertAdjacentText,
// and scrollIntoView.

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_element.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/element_scroll.h"
#include "dom/event.h"
#include "dom/node.h"
#include "engine/engine.h"

#include <algorithm>
#include <string>
#include <vector>

namespace bro::bronze_host {

void decorateElementMutate(ObjectBuilder& b) {
    b.def("append", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        for (const Value& v : a) {
            if (dom::Node* child = hostNodeOf(v)) {
                hostInsertNode(st->el, child, nullptr);
            } else if (!ev::isObject(v) && !ev::isUndefined(v)) {
                if (dom::Document* doc = st->el->document())
                    st->el->appendChild(doc->createTextNode(ev::toUtf8(v)));
            }
        }
        return ev::undefined();
    });

    b.def("prepend", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        dom::Node* ref = st->el->childNodes().empty() ? nullptr : st->el->childNodes().front();
        for (const Value& v : a) {
            if (dom::Node* child = hostNodeOf(v)) {
                hostInsertNode(st->el, child, ref);
            } else if (!ev::isObject(v) && !ev::isUndefined(v)) {
                if (dom::Document* doc = st->el->document()) {
                    hostInsertNode(st->el, doc->createTextNode(ev::toUtf8(v)), ref);
                }
            }
        }
        return ev::undefined();
    });

    b.def("before", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        dom::Node* parent = st->el->parentNode();
        if (!parent) return ev::undefined();
        for (const Value& v : a) {
            if (dom::Node* child = hostNodeOf(v)) {
                hostInsertNode(parent, child, st->el);
            } else if (!ev::isObject(v) && !ev::isUndefined(v)) {
                if (dom::Document* doc = st->el->document()) {
                    hostInsertNode(parent, doc->createTextNode(ev::toUtf8(v)), st->el);
                }
            }
        }
        return ev::undefined();
    });

    b.def("after", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        dom::Node* parent = st->el->parentNode();
        if (!parent) return ev::undefined();
        dom::Node* ref = nullptr;
        const auto& kids = parent->childNodes();
        for (size_t i = 0; i < kids.size(); ++i) {
            if (kids[i] == st->el && i + 1 < kids.size()) {
                ref = kids[i + 1];
                break;
            }
        }
        for (const Value& v : a) {
            if (dom::Node* child = hostNodeOf(v)) {
                hostInsertNode(parent, child, ref);
            } else if (!ev::isObject(v) && !ev::isUndefined(v)) {
                if (dom::Document* doc = st->el->document()) {
                    hostInsertNode(parent, doc->createTextNode(ev::toUtf8(v)), ref);
                }
            }
        }
        return ev::undefined();
    });

    b.def("replaceWith", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        dom::Node* parent = st->el->parentNode();
        if (!parent) return ev::undefined();
        for (const Value& v : a) {
            if (dom::Node* child = hostNodeOf(v)) {
                hostInsertNode(parent, child, st->el);
            } else if (!ev::isObject(v) && !ev::isUndefined(v)) {
                if (dom::Document* doc = st->el->document()) {
                    hostInsertNode(parent, doc->createTextNode(ev::toUtf8(v)), st->el);
                }
            }
        }
        parent->removeChild(st->el);
        return ev::undefined();
    });

    b.def("replaceChildren", 0, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        dom::Document* doc = st->el->document();
        std::vector<dom::Node*> oldKids = st->el->childNodes();
        while (!st->el->childNodes().empty()) {
            st->el->removeChild(st->el->childNodes().front());
        }
        for (const Value& v : a) {
            if (dom::Node* child = hostNodeOf(v)) {
                oldKids.erase(std::remove(oldKids.begin(), oldKids.end(), child), oldKids.end());
                hostInsertNode(st->el, child, nullptr);
            } else if (!ev::isObject(v) && !ev::isUndefined(v)) {
                if (doc)
                    st->el->appendChild(doc->createTextNode(ev::toUtf8(v)));
            }
        }
        if (doc) {
            for (auto* old : oldKids) {
                if (old->nodeType() != dom::NodeType::Element) {
                    doc->freeNode(old);
                }
            }
        }
        return ev::undefined();
    });

    b.def("insertAdjacentHTML", 2, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el || a.size() < 2) return ev::undefined();
        std::string pos = ev::toUtf8(a[0]);
        std::string html = ev::toUtf8(a[1]);
        dom::Document* doc = st->el->document();
        if (!doc) return ev::undefined();
        dom::Element* temp = doc->createElement("div");
        if (!temp) return ev::undefined();
        temp->setInnerHTML(html);
        auto kids = temp->childNodes();
        std::vector<dom::Node*> toMove(kids.begin(), kids.end());
        for (auto* k : toMove) temp->removeChild(k);
        doc->freeNode(temp);

        if (pos == "beforebegin") {
            dom::Node* parent = st->el->parentNode();
            if (parent) {
                for (auto* child : toMove) hostInsertNode(parent, child, st->el);
            }
        } else if (pos == "afterbegin") {
            dom::Node* first = st->el->childNodes().empty() ? nullptr : st->el->childNodes().front();
            for (auto* child : toMove) hostInsertNode(st->el, child, first);
        } else if (pos == "beforeend") {
            for (auto* child : toMove) hostInsertNode(st->el, child, nullptr);
        } else if (pos == "afterend") {
            dom::Node* parent = st->el->parentNode();
            if (parent) {
                dom::Node* ref = nullptr;
                const auto& sibs = parent->childNodes();
                for (size_t i = 0; i < sibs.size(); ++i) {
                    if (sibs[i] == st->el && i + 1 < sibs.size()) {
                        ref = sibs[i + 1];
                        break;
                    }
                }
                for (auto* child : toMove) hostInsertNode(parent, child, ref);
            }
        }
        return ev::undefined();
    });

    b.def("insertAdjacentElement", 2, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el || a.size() < 2) return ev::null();
        std::string pos = ev::toUtf8(a[0]);
        dom::Node* newEl = hostNodeOf(a[1]);
        if (!newEl) return ev::null();

        if (pos == "beforebegin") {
            dom::Node* parent = st->el->parentNode();
            if (parent) hostInsertNode(parent, newEl, st->el);
        } else if (pos == "afterbegin") {
            dom::Node* first = st->el->childNodes().empty() ? nullptr : st->el->childNodes().front();
            hostInsertNode(st->el, newEl, first);
        } else if (pos == "beforeend") {
            hostInsertNode(st->el, newEl, nullptr);
        } else if (pos == "afterend") {
            dom::Node* parent = st->el->parentNode();
            if (parent) {
                dom::Node* ref = nullptr;
                const auto& sibs = parent->childNodes();
                for (size_t i = 0; i < sibs.size(); ++i) {
                    if (sibs[i] == st->el && i + 1 < sibs.size()) {
                        ref = sibs[i + 1];
                        break;
                    }
                }
                hostInsertNode(parent, newEl, ref);
            }
        }
        return a[1];
    });

    b.def("insertAdjacentText", 2, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el || a.size() < 2) return ev::undefined();
        std::string pos = ev::toUtf8(a[0]);
        std::string text = ev::toUtf8(a[1]);
        dom::Document* doc = st->el->document();
        if (!doc) return ev::undefined();
        dom::TextNode* textNode = doc->createTextNode(text);
        if (!textNode) return ev::undefined();

        if (pos == "beforebegin") {
            dom::Node* parent = st->el->parentNode();
            if (parent) hostInsertNode(parent, textNode, st->el);
        } else if (pos == "afterbegin") {
            dom::Node* first = st->el->childNodes().empty() ? nullptr : st->el->childNodes().front();
            hostInsertNode(st->el, textNode, first);
        } else if (pos == "beforeend") {
            hostInsertNode(st->el, textNode, nullptr);
        } else if (pos == "afterend") {
            dom::Node* parent = st->el->parentNode();
            if (parent) {
                dom::Node* ref = nullptr;
                const auto& sibs = parent->childNodes();
                for (size_t i = 0; i < sibs.size(); ++i) {
                    if (sibs[i] == st->el && i + 1 < sibs.size()) {
                        ref = sibs[i + 1];
                        break;
                    }
                }
                hostInsertNode(parent, textNode, ref);
            }
        }
        return ev::undefined();
    });

    b.def("scrollIntoView", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        if (auto* eng = hostEngine()) {
            eng->flushLayoutForRead(st->el->document());
        }
        // 0=start, 1=center, 2=end, 3=nearest. CSSOM View: block defaults to
        // start, inline to nearest; the boolean form sets block only.
        auto parseAlign = [](const std::string& v, int fallback) {
            if (v == "start") return 0;
            if (v == "center") return 1;
            if (v == "end") return 2;
            if (v == "nearest") return 3;
            return fallback;
        };
        int align = 0;
        int alignInline = 3;
        if (!a.empty()) {
            if (ev::isBool(a[0])) {
                align = ev::toBool(a[0]) ? 0 : 2;
            } else if (ev::isObject(a[0])) {
                Value blockVal = ev::getProperty(a[0], "block");
                if (!ev::isUndefined(blockVal)) align = parseAlign(ev::toUtf8(blockVal), align);
                Value inlineVal = ev::getProperty(a[0], "inline");
                if (!ev::isUndefined(inlineVal))
                    alignInline = parseAlign(ev::toUtf8(inlineVal), alignInline);
            }
        }
        // How far the view [viewStart, +viewLen) must move to show the target
        // [start, +len) aligned per `how`.
        auto alignDelta = [](int how, float start, float len, float viewStart, float viewLen) {
            switch (how) {
                case 1: return (start + len * 0.5f) - (viewStart + viewLen * 0.5f);
                case 2: return (start + len) - (viewStart + viewLen);
                case 3:
                    if (start < viewStart) return start - viewStart;
                    if (start + len > viewStart + viewLen) return (start + len) - (viewStart + viewLen);
                    return 0.0f;
                default: return start - viewStart;
            }
        };
        dom::AbsoluteRect target = borderBoxOf(st->el);
        bool scrolled = false;
        for (auto* anc = st->el->parentElement(); anc; anc = anc->parentElement()) {
            const float maxTop = dom::elementClipsOverflow(anc) ? dom::maxScrollTopOf(anc) : 0.0f;
            const float maxLeft = dom::maxScrollLeftOf(anc);   // 0 unless it scrolls on x
            if (maxTop <= 0.0f && maxLeft <= 0.0f) continue;

            dom::AbsoluteRect view = dom::absoluteContentBox(anc);
            bool moved = false;
            if (maxTop > 0.0f) {
                const float prev = anc->scrollTopValue();
                const float next = std::clamp(
                    prev + alignDelta(align, target.y, target.height, view.y, view.height),
                    0.0f, maxTop);
                if (next != prev) { anc->setScrollTopValue(next); moved = true; }
            }
            if (maxLeft > 0.0f) {
                const float prev = anc->scrollLeftValue();
                const float next = std::clamp(
                    prev + alignDelta(alignInline, target.x, target.width, view.x, view.width),
                    0.0f, maxLeft);
                if (next != prev) { anc->setScrollLeftValue(next); moved = true; }
            }
            if (moved) {
                scrolled = true;
                dom::Event evt("scroll", false, false);
                evt.setIsTrusted(true);
                if (auto* eng = hostEngine()) eng->dispatchElementEvent(anc, evt);
            }
            target = borderBoxOf(anc);
        }
        // Last, the root scroller: the viewport, when the element is in the
        // app document and <html> does not scroll by itself (an <html> that
        // does is an ancestor, handled above).
        if (auto* eng = hostEngine();
            eng && eng->document() == st->el->document() && !rootScrollerElement()) {
            const float viewH = static_cast<float>(eng->contentHeight());
            float delta = 0.0f;
            const float top = target.y - eng->viewportScrollY();
            switch (align) {
                case 1: delta = (top + target.height * 0.5f) - viewH * 0.5f; break;
                case 2: delta = (top + target.height) - viewH; break;
                case 3:
                    if (top < 0.0f) delta = top;
                    else if (top + target.height > viewH) delta = top + target.height - viewH;
                    break;
                default: delta = top; break;
            }
            if (delta != 0.0f) eng->scrollViewportTo(eng->viewportScrollY() + delta);
        }
        if (scrolled && st->el->document()) st->el->document()->markDirty();
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
