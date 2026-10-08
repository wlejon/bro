// Shell-drawn window frames (window_frames.h): finding the frame elements,
// keeping each on its window, and the client-window runs paint passes record.
#include "engine/window_frames.h"

#include "dom/document.h"
#include "dom/element.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>

namespace bro::engine {

namespace {

constexpr const char* kFrameAttr = "data-window-frame";
constexpr size_t kKeptLists = 8;

std::string px(int v) { return std::to_string(v) + "px"; }

uint64_t parseId(const std::string& s) {
    uint64_t id = 0;
    const char* b = s.data();
    while (b < s.data() + s.size() && (*b == ' ' || *b == '\t')) ++b;
    std::from_chars(b, s.data() + s.size(), id);
    return id;
}

}  // namespace

void WindowFrames::rescan(dom::Document* doc) {
    std::vector<Entry> next;
    if (doc && doc->documentElement()) {
        std::function<void(dom::Element*)> walk = [&](dom::Element* el) {
            if (el->hasAttribute(kFrameAttr)) {
                Entry e;
                e.element = dom::ElementHandle(doc, el);
                e.window = parseId(el->getAttribute(kFrameAttr));
                // Keep what was written on an element seen before.
                for (auto& old : entries_)
                    if (old.element.get() == el) {
                        e.written = old.written;
                        break;
                    }
                next.push_back(std::move(e));
            }
            for (dom::Node* child : el->childNodes())
                if (child->nodeType() == dom::NodeType::Element) walk(static_cast<dom::Element*>(child));
        };
        walk(doc->documentElement());
    }
    entries_ = std::move(next);
}

WindowFrames::Box WindowFrames::outerOf(const FrameWindow& w) {
    return Box{static_cast<float>(w.x - w.insetLeft), static_cast<float>(w.y - w.insetTop),
               static_cast<float>(w.width + w.insetLeft + w.insetRight),
               static_cast<float>(w.height + w.insetTop + w.insetBottom)};
}

void WindowFrames::write(Entry& e, dom::Element* el, const FrameWindow* w, const Box& box, int z) {
    Written& was = e.written;
    auto& style = el->style();
    if (!w) {
        if (!was.hidden) {
            style.setProperty("display", "none");
            was.hidden = true;
        }
        return;
    }
    if (was.hidden) {
        style.removeProperty("display");
        was.hidden = false;
    }
    if (!was.placed) {
        style.setProperty("position", "fixed");
        style.setProperty("box-sizing", "border-box");
        was.placed = true;
    }
    auto set = [&](const char* prop, std::string& last, std::string value) {
        if (last == value) return;
        style.setProperty(prop, value);
        last = std::move(value);
    };
    set("left", was.left, px(static_cast<int>(std::lround(box.x))));
    set("top", was.top, px(static_cast<int>(std::lround(box.y))));
    set("width", was.width, px(static_cast<int>(std::lround(box.w))));
    set("height", was.height, px(static_cast<int>(std::lround(box.h))));
    set("z-index", was.z, std::to_string(z));

    const std::string state = w->maximized ? "maximized" : "normal";
    if (was.state != state) {
        el->setAttribute("data-window-state", state);
        was.state = state;
    }
    const std::string snap = w->snap == "none" || w->snap == "maximize" ? "" : w->snap;
    if (was.snap != snap) {
        if (snap.empty()) el->removeAttribute("data-window-snap");
        else el->setAttribute("data-window-snap", snap);
        was.snap = snap;
    }
    if (was.focused != w->focused) {
        if (w->focused) el->setAttribute("data-window-focused", "");
        else el->removeAttribute("data-window-focused");
        was.focused = w->focused;
    }
    const bool borderless = w->borderless();
    if (was.borderless != borderless) {
        if (borderless) el->setAttribute("data-window-borderless", "");
        else el->removeAttribute("data-window-borderless");
        was.borderless = borderless;
    }
}

void WindowFrames::sync(dom::Document* doc, std::vector<FrameWindow> stack, double nowMs) {
    // A framed window whose state changed glides from where it was shown. A
    // state change lands in steps (the band changes when it is asked for;
    // the client acks the state, then draws the new size): each step within
    // the settle time re-aims the glide from where it has got to.
    std::unordered_map<uint64_t, Box> shownNow;
    for (const auto& w : stack) {
        if (!w.framed) continue;
        const Box to = outerOf(w);
        auto shown = shownBox_.find(w.id);
        auto prev = std::find_if(stack_.begin(), stack_.end(), [&](const FrameWindow& p) { return p.id == w.id; });
        const bool stateChanged =
            prev != stack_.end() && prev->framed &&
            (prev->maximized != w.maximized || prev->snap != w.snap || prev->borderless() != w.borderless());
        auto m = motions_.find(w.id);
        const bool settling = m != motions_.end() && nowMs <= m->second.settleUntil;
        if ((stateChanged || (settling && !(m->second.to == to))) && shown != shownBox_.end()) {
            const double settleUntil = stateChanged ? nowMs + 2 * kMotionMs : m->second.settleUntil;
            motions_[w.id] = Motion{shown->second, to, nowMs, settleUntil};
            m = motions_.find(w.id);
        }
        Box box = to;
        if (m != motions_.end()) {
            m->second.to = to;
            const double t = (nowMs - m->second.start) / kMotionMs;
            if ((t >= 1.0 && nowMs > m->second.settleUntil) || t < 0.0) {
                motions_.erase(m);
            } else if (t < 1.0) {
                // Ease out (cubic): quick to leave, settling into place.
                const float k = static_cast<float>(1.0 - std::pow(1.0 - t, 3.0));
                const Box& f = m->second.from;
                box = Box{f.x + (to.x - f.x) * k, f.y + (to.y - f.y) * k, f.w + (to.w - f.w) * k,
                          f.h + (to.h - f.h) * k};
            }
        }
        shownNow[w.id] = box;
    }
    for (auto it = motions_.begin(); it != motions_.end();)
        it = shownNow.count(it->first) ? std::next(it) : motions_.erase(it);
    shownBox_ = std::move(shownNow);

    stack_ = std::move(stack);
    shown_.clear();
    if (!doc) {
        entries_.clear();
        doc_ = nullptr;
        motions_.clear();
        return;
    }
    if (doc != doc_ || doc->mutationEpoch() != scannedEpoch_) {
        if (doc != doc_) entries_.clear();
        doc_ = doc;
        rescan(doc);
    }
    for (auto& e : entries_) {
        dom::Element* el = e.element.get();
        if (!el) continue;
        const FrameWindow* w = nullptr;
        int z = 0;
        // One frame per window: the first element naming it.
        if (e.window != 0 && !shown_.count(e.window)) {
            for (size_t i = 0; i < stack_.size(); ++i) {
                if (stack_[i].id != e.window) continue;
                if (stack_[i].framed) {
                    w = &stack_[i];
                    z = static_cast<int>(i) + 1;
                }
                break;
            }
        }
        write(e, el, w, w ? shownBox_[w->id] : Box{}, z);
        if (w) shown_[e.window] = e.element;
    }
    // What was just written is not a reason to look again.
    scannedEpoch_ = doc->mutationEpoch();
}

dom::Element* WindowFrames::frameOf(uint64_t windowId) const {
    auto it = shown_.find(windowId);
    return it == shown_.end() ? nullptr : it->second.get();
}

bool WindowFrames::overlayHit(uint64_t windowId, const dom::Element* hit) const {
    const dom::Element* frame = frameOf(windowId);
    if (!frame || !hit) return false;
    bool inOverlay = false;
    for (const dom::Element* cur = hit; cur; cur = cur->parentElement()) {
        if (cur == frame) return inOverlay;
        if (cur->hasAttribute("data-window-overlay")) inOverlay = true;
    }
    return false;
}

std::vector<layout::DrawTraversal::ClientWindowSlot> WindowFrames::slots() const {
    std::vector<layout::DrawTraversal::ClientWindowSlot> out;
    out.reserve(stack_.size());
    for (const auto& w : stack_) {
        layout::DrawTraversal::ClientWindowSlot s;
        s.windowId = w.id;
        s.frame = frameOf(w.id);
        s.insetLeft = static_cast<float>(w.insetLeft);
        s.insetTop = static_cast<float>(w.insetTop);
        s.insetRight = static_cast<float>(w.insetRight);
        s.insetBottom = static_cast<float>(w.insetBottom);
        out.push_back(s);
    }
    return out;
}

std::vector<render::ClientWindowRef>& WindowFrames::beginList(uint32_t& id) {
    id = nextList_++;
    if (nextList_ == 0) nextList_ = 1;
    while (lists_.size() >= kKeptLists) lists_.pop_front();
    lists_.emplace_back(id, std::vector<render::ClientWindowRef>{});
    return lists_.back().second;
}

const std::vector<render::ClientWindowRef>* WindowFrames::list(uint32_t id) const {
    for (const auto& [lid, refs] : lists_)
        if (lid == id) return &refs;
    return nullptr;
}

}  // namespace bro::engine
