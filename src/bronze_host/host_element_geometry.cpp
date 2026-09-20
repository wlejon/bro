// The measurable element: box metrics, scrolling, and the two serializations
// that read the rendered tree rather than the source one (innerText,
// outerHTML).
//
// Every number here is a CSSOM number, and CSSOM numbers are not the layout
// box's fields read back out. `clientWidth` is content PLUS padding and zero
// for a non-replaced inline; `scrollHeight` is the unclamped content height
// plus padding, never less than clientHeight; `scrollTop` clamps, defers, and
// fires an event. The bronze port answered `contentRect.width` and friends for
// all of them — close enough to look right on a plain <div> and wrong for
// anything with padding, which is most of a real UI.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_globals_internal.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/element_scroll.h"
#include "dom/shadow_root.h"
#include "dom/text_node.h"
#include "engine/engine.h"

#include <algorithm>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

const htmlayout::layout::LayoutBox& laidOutBox(dom::Element* el) {
    hostEngine()->flushLayoutForRead(el->document());
    return el->layoutBox();
}

// A non-replaced inline box has no client/scroll geometry at all — CSSOM says
// those properties are 0 for it. Reading the content rect instead reports the
// inline's text extent, so `span.clientWidth` came back non-zero and any code
// using it to decide "is this a box I can size?" got the wrong answer.
bool isInlineDisplay(dom::Element* el) {
    if (!el) return false;
    hostEngine()->flushLayoutForRead(el->document());
    const auto& style = el->computedStyle();
    auto it = style.find("display");
    return it != style.end() && it->second == "inline";
}

// The `width`/`height` CONTENT attributes, which <canvas> and <img> carry and
// which the old binding fell back to when layout had produced no box yet —
// a canvas measured before its first frame still has to report its size.
double attrDimension(dom::Element* el, const char* name) {
    if (!el || !el->hasAttribute(name)) return 0.0;
    try { return std::stod(el->getAttribute(name)); } catch (...) { return 0.0; }
}

// CSSOM's client*/offset*/scroll* properties are `long`, so the value a page
// reads back is truncated, not rounded. A layout that lands on 33.8 answers 33
// in every browser, and code that compares two of them (`a.clientWidth ===
// b.clientWidth`) depends on it.
Value fromCssPixels(double v) {
    return ev::fromDouble(static_cast<double>(static_cast<int>(v)));
}

// ---------------------------------------------------------------------------
// innerText — the RENDERED text, which is the point of it
// ---------------------------------------------------------------------------
void collectInnerText(const dom::Element* el, std::string& out) {
    for (const dom::Node* child : el->childNodes()) {
        if (child->nodeType() == dom::NodeType::Text) {
            out += static_cast<const dom::TextNode*>(child)->data();
            continue;
        }
        if (child->nodeType() != dom::NodeType::Element) continue;
        const auto* childEl = static_cast<const dom::Element*>(child);
        const std::string& tag = childEl->tagName();
        if (tag == "SCRIPT" || tag == "STYLE" || tag == "script" || tag == "style")
            continue;
        const auto& style = childEl->computedStyle();
        auto dIt = style.find("display");
        std::string display = (dIt != style.end()) ? dIt->second : "inline";
        if (display == "none") continue;
        // Flow-collapsed content (a closed <details>'s body) is not rendered,
        // so it does not contribute — matching Chromium.
        auto fcIt = style.find("-x-flow-collapse");
        if (fcIt != style.end() && fcIt->second == "collapse") continue;
        const bool isBlock =
            (display == "block" || display == "list-item" || display == "table");
        if (isBlock && !out.empty() && out.back() != '\n') out += '\n';
        collectInnerText(childEl, out);
        if (isBlock && !out.empty() && out.back() != '\n') out += '\n';
    }
}

std::string outerHtmlOf(dom::Element* el) {
    std::string tag = el->tagName();
    for (char& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string result = "<" + tag;
    for (const auto& [name, val] : el->attributes()) {
        result += " " + name + "=\"" + val + "\"";
    }
    // "style" never lives in attributes_ (Element::setAttribute routes it into
    // the StyleProxy), so it is appended on its own or it vanishes from the
    // serialization.
    if (el->hasAttribute("style")) {
        result += " style=\"" + el->style().cssText() + "\"";
    }
    result += ">";
    result += el->innerHTML();
    result += "</" + tag + ">";
    return result;
}

// scrollTo / scrollBy take either (x, y) or a {top, left} options object.
bool readScrollTopArg(std::span<const Value> a, double& top) {
    if (!a.empty() && ev::isObject(a[0])) {
        Value t = ev::getProperty(a[0], "top");
        if (ev::isUndefined(t)) return false;
        top = ev::toDouble(t);
        return true;
    }
    if (a.size() > 1 && !ev::isUndefined(a[1])) {
        top = ev::toDouble(a[1]);
        return true;
    }
    return false;
}

bool readScrollLeftArg(std::span<const Value> a, double& left) {
    if (!a.empty() && ev::isObject(a[0])) {
        Value l = ev::getProperty(a[0], "left");
        if (ev::isUndefined(l)) return false;
        left = ev::toDouble(l);
        return true;
    }
    if (!a.empty() && !ev::isObject(a[0]) && !ev::isUndefined(a[0])) {
        left = ev::toDouble(a[0]);
        return true;
    }
    return false;
}

bool isNodeConnected(dom::Node* node) {
    if (!node) return false;
    dom::Document* doc = node->document();
    if (!doc) return false;
    dom::Node* docEl = doc->documentElement();
    if (!docEl) return false;
    for (dom::Node* n = node; n; ) {
        if (n == docEl) return true;
        dom::Node* parent = n->parentNode();
        if (!parent) {
            if (auto* sr = dynamic_cast<dom::ShadowRoot*>(n)) {
                n = sr->host();
                continue;
            }
            return false;
        }
        n = parent;
    }
    return false;
}

dom::Element* computeOffsetParent(dom::Element* el) {
    if (!el || !isNodeConnected(el)) return nullptr;
    const std::string& tag = el->tagName();
    if (tag == "BODY" || tag == "HTML" || tag == "body" || tag == "html") return nullptr;
    if (isInlineDisplay(el)) return nullptr;

    const auto& style = el->computedStyle();
    auto posIt = style.find("position");
    if (posIt != style.end() && posIt->second == "fixed") return nullptr;

    dom::Document* doc = el->document();
    if (!doc) return nullptr;
    dom::Element* body = doc->body();

    for (dom::Element* p = el->parentElement(); p; p = p->parentElement()) {
        if (p == body) return p;
        const std::string& pTag = p->tagName();
        if (pTag == "BODY" || pTag == "body") return p;
        if (pTag == "HTML" || pTag == "html") return body ? body : nullptr;

        const auto& pStyle = p->computedStyle();
        auto dIt = pStyle.find("display");
        if (dIt != pStyle.end() && dIt->second == "none") return nullptr;

        auto pPosIt = pStyle.find("position");
        if (pPosIt != pStyle.end() && pPosIt->second != "static" && !pPosIt->second.empty()) {
            return p;
        }
        if (pTag == "TABLE" || pTag == "TH" || pTag == "TD" ||
            pTag == "table" || pTag == "th" || pTag == "td") {
            return p;
        }
    }
    return body ? body : nullptr;
}

}  // namespace

void decorateElementGeometry(ObjectBuilder& b) {
    b.def("getBoundingClientRect", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
        if (!st->el) return makeHostRectValue(0, 0, 0, 0);
        dom::AbsoluteRect r = borderBoxOf(st->el);
        return makeHostRectValue(r.x, r.y, r.width, r.height);
    });
    // One rect, because this engine has no fragmented boxes: an inline that
    // wraps is still one layout box here. A page that iterates getClientRects()
    // gets the same geometry getBoundingClientRect() reports rather than
    // nothing at all, which is what `undefined` gave it.
    b.def("getClientRects", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        dom::AbsoluteRect r = borderBoxOf(st->el);
        return hostArrayOf(1, [&r](size_t) {
            return makeHostRectValue(r.x, r.y, r.width, r.height);
        });
    });

    b.accessor("clientWidth",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (!st->el) return ev::fromDouble(0.0);
                   if (isInlineDisplay(st->el)) return ev::fromDouble(0.0);
                   const auto& box = laidOutBox(st->el);
                   double cw = box.contentRect.width + box.padding.left + box.padding.right;
                   if (cw > 0) return fromCssPixels(cw);
                   return fromCssPixels(attrDimension(st->el, "width"));
               },
               nullptr);
    b.accessor("clientHeight",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (!st->el) return ev::fromDouble(0.0);
                   if (isInlineDisplay(st->el)) return ev::fromDouble(0.0);
                   const auto& box = laidOutBox(st->el);
                   double ch = box.contentRect.height + box.padding.top + box.padding.bottom;
                   if (ch > 0) return fromCssPixels(ch);
                   return fromCssPixels(attrDimension(st->el, "height"));
               },
               nullptr);
    // The border widths, which is all clientLeft/clientTop are — and 0 on an
    // inline, like everything else in this family.
    b.accessor("clientLeft",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el || isInlineDisplay(st->el)) return ev::fromDouble(0.0);
                   return fromCssPixels(laidOutBox(st->el).border.left);
               },
               nullptr);
    b.accessor("clientTop",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el || isInlineDisplay(st->el)) return ev::fromDouble(0.0);
                   return fromCssPixels(laidOutBox(st->el).border.top);
               },
               nullptr);

    b.accessor("offsetWidth",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   return ev::fromDouble(st->el ? borderBoxOf(st->el).width : 0.0);
               },
               nullptr);
    b.accessor("offsetHeight",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   return ev::fromDouble(st->el ? borderBoxOf(st->el).height : 0.0);
               },
               nullptr);
    b.accessor("offsetParent",
               [](Value self_, std::span<const Value>) -> Value {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el) return ev::null();
                   dom::Element* op = computeOffsetParent(st->el);
                   return op ? hostElementValue(op) : ev::null();
               },
               nullptr);
    b.accessor("offsetLeft",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (!st->el) return ev::fromDouble(0.0);
                   dom::AbsoluteRect r = borderBoxOf(st->el);
                   dom::Element* op = computeOffsetParent(st->el);
                   if (!op) return ev::fromDouble(r.x);
                   dom::AbsoluteRect opRect = borderBoxOf(op);
                   const auto& opBox = laidOutBox(op);
                   return ev::fromDouble(r.x - opRect.x - opBox.border.left);
               },
               nullptr);
    b.accessor("offsetTop",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (!st->el) return ev::fromDouble(0.0);
                   dom::AbsoluteRect r = borderBoxOf(st->el);
                   dom::Element* op = computeOffsetParent(st->el);
                   if (!op) return ev::fromDouble(r.y);
                   dom::AbsoluteRect opRect = borderBoxOf(op);
                   const auto& opBox = laidOutBox(op);
                   return ev::fromDouble(r.y - opRect.y - opBox.border.top);
               },
               nullptr);

    b.accessor("scrollWidth",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (!st->el || isInlineDisplay(st->el)) return ev::fromDouble(0.0);
                   const auto& box = laidOutBox(st->el);
                   double clientW = box.contentRect.width + box.padding.left + box.padding.right;
                   if (clientW > 0) return fromCssPixels(clientW);
                   return fromCssPixels(borderBoxOf(st->el).width);
               },
               nullptr);
    b.accessor("scrollHeight",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (!st->el || isInlineDisplay(st->el)) return ev::fromDouble(0.0);
                   const auto& box = laidOutBox(st->el);
                   double clientH = box.contentRect.height + box.padding.top + box.padding.bottom;
                   // naturalHeight is the content height BEFORE min/max-height
                   // clamping — the number a scroll container has to know to
                   // scroll to the end of its content.
                   double scrollH = box.naturalHeight + box.padding.top + box.padding.bottom;
                   double h = std::max(scrollH, clientH);
                   if (h > 0) return fromCssPixels(h);
                   return fromCssPixels(borderBoxOf(st->el).height);
               },
               nullptr);

    b.accessor("scrollTop",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   return ev::fromDouble(st->el ? st->el->scrollTopValue() : 0.0);
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el) return ev::undefined();
                   dom::setElementScrollTop(st->el, ev::toDouble(argAt(a, 0)));
                   return ev::undefined();
               });
    b.accessor("scrollLeft",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   return ev::fromDouble(st->el ? st->el->scrollLeftValue() : 0.0);
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el) return ev::undefined();
                   dom::setElementScrollLeft(st->el, ev::toDouble(argAt(a, 0)));
                   return ev::undefined();
               });

    b.def("scrollTo", 2, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        double top = 0;
        if (readScrollTopArg(a, top)) dom::setElementScrollTop(st->el, top);
        double left = 0;
        if (readScrollLeftArg(a, left)) dom::setElementScrollLeft(st->el, left);
        return ev::undefined();
    });
    b.def("scrollBy", 2, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        double top = 0;
        if (readScrollTopArg(a, top)) dom::scrollElementBy(st->el, top);
        double left = 0;
        if (readScrollLeftArg(a, left)) dom::scrollElementLeftBy(st->el, left);
        return ev::undefined();
    });

    // ---- serializations ---------------------------------------------------
    b.accessor("innerText",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (!st->el) return ev::fromUtf8("");
                   std::string out;
                   collectInnerText(st->el, out);
                   while (!out.empty() && out.back() == '\n') out.pop_back();
                   return ev::fromUtf8(out);
               },
               // Writing innerText is textContent with newlines kept, which is
               // what this DOM's text nodes already do.
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (!ev::isObject(v)) st->el->setTextContent(hostNullableString(v));
                   return ev::undefined();
               });

    b.accessor("outerHTML",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st) return ev::undefined();
                   if (!st->el) return ev::fromUtf8("");
                   return ev::fromUtf8(outerHtmlOf(st->el));
               },
               [](Value self_, std::span<const Value> a) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->el) return ev::undefined();
                   Value v = argAt(a, 0);
                   if (ev::isObject(v)) return ev::undefined();
                   dom::Element* parent = st->el->parentElement();
                   st->el->setOuterHTML(hostNullableString(v));
                   if (parent) upgradeCustomElementsInSubtree(parent);
                   return ev::undefined();
               });

    // ---- isConnected ------------------------------------------------------
    // bro's Document is not a Node, so "in the document" means "the walk up
    // reaches documentElement" — crossing a shadow boundary to the host on the
    // way, because a node inside an attached shadow tree IS connected.
    b.accessor("isConnected",
               [](Value self_, std::span<const Value>) {
                   HostNodeState* st = hostNodeStateOfValue(self_);
                   if (!st || !st->node) return ev::fromBool(false);
                   return ev::fromBool(isNodeConnected(st->node));
               },
               nullptr);
}

}  // namespace bro::bronze_host
