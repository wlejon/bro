#include "engine/view_document.h"

#include "dom/accessibility.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/text_node.h"
#include "layout/el_select.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <vector>

namespace bro::engine {

namespace {

struct Clip {
    float x0, y0, x1, y1;
    bool intersects(float x, float y, float w, float h) const { return x < x1 && y < y1 && x + w > x0 && y + h > y0; }
    Clip intersect(float x, float y, float w, float h) const {
        return Clip{std::max(x0, x), std::max(y0, y), std::min(x1, x + w), std::min(y1, y + h)};
    }
};

const std::string& styleOf(const dom::Element* el, const char* prop) {
    static const std::string empty;
    const auto& s = el->computedStyle();
    auto it = s.find(prop);
    return it == s.end() ? empty : it->second;
}

bool attrIs(const dom::Element* el, const char* name, const char* value) {
    return el->hasAttribute(name) && el->getAttribute(name) == value;
}

bool skippedTag(const std::string& tag) {
    return tag == "head" || tag == "script" || tag == "style" || tag == "template" || tag == "meta" ||
           tag == "link" || tag == "noscript" || tag == "title" || tag == "slot";
}

bool hiddenElement(const dom::Element* el, const std::string& tag) {
    if (skippedTag(tag)) return true;
    if (el->hasAttribute("hidden") || el->hasAttribute("inert") || attrIs(el, "aria-hidden", "true")) return true;
    return styleOf(el, "display") == "none";
}

// Structures that get a line and indent what is inside them.
bool isContainerRole(const std::string& r) {
    return r == "dialog" || r == "alertdialog" || r == "list" || r == "listbox" || r == "menu" ||
           r == "menubar" || r == "tablist" || r == "tree" || r == "grid" || r == "table" || r == "radiogroup";
}
// Lists whose items are counted and paged.
bool isListRole(const std::string& r) {
    return r == "list" || r == "listbox" || r == "menu" || r == "menubar" || r == "tree" || r == "grid" ||
           r == "table" || r == "tablist" || r == "radiogroup";
}
// Structures that only get a line when they are named.
bool isNamedGroupRole(const std::string& r) {
    return r == "group" || r == "region" || r == "toolbar" || r == "form" || r == "navigation" ||
           r == "complementary" || r == "search" || r == "tabpanel";
}
// Lines whose text is the whole subtree's.
bool isLeafTextRole(const std::string& r) {
    return r == "heading" || r == "img" || r == "status" || r == "alert" || r == "log" || r == "tooltip" ||
           r == "listitem" || r == "cell" || r == "columnheader" || r == "rowheader" || r == "progressbar" ||
           r == "meter" || r == "terminal" || r == "note" || r == "marquee" || r == "timer";
}

bool clickableHint(const dom::Element* el) {
    if (el->hasJsListener("click")) return true;
    if (el->hasAttribute("tabindex") && std::atoi(el->getAttribute("tabindex").c_str()) >= 0) return true;
    return false;
}

std::string ownText(const dom::Element* el) {
    std::string t;
    for (const dom::Node* c : el->childNodes())
        if (c->nodeType() == dom::NodeType::Text) t += static_cast<const dom::TextNode*>(c)->data();
    return dom::collapseWhitespace(t);
}

// What lies under an element: something to operate, something with a line
// of its own. Bounded, so a huge subtree costs a fixed amount.
struct Below {
    bool interactive = false;
    bool significant = false;
};
void scanBelow(const dom::Element* el, Below& b, int& budget) {
    el->forEachComposedChild([&](dom::Element* c) {
        if (budget-- <= 0 || (b.interactive && b.significant)) return;
        const std::string tag = dom::lowerTagName(c);
        if (hiddenElement(c, tag)) return;
        const std::string role = dom::accessibleRole(c);
        if (dom::isInteractiveRole(role) || clickableHint(c)) {
            b.interactive = b.significant = true;
            return;
        }
        if (isContainerRole(role) || isLeafTextRole(role) || role == "img") b.significant = true;
        scanBelow(c, b, budget);
    });
}
Below below(const dom::Element* el) {
    Below b;
    int budget = 400;
    scanBelow(el, b, budget);
    return b;
}

}  // namespace

struct ViewWalk {
    DocumentView& dv;
    dom::Document* doc;
    DocumentView::Viewport vp;
    DocumentView::Options opt;
    view::Snapshot& out;
    const dom::Element* focus = nullptr;
    bool unclipped = false;
    int lines = 0;
    bool cut = false;

    struct ListCtx {
        int depth;
        int seen = 0, shown = 0, from = 0, count = 40;
        size_t lineIndex;  // the list's own line
        int boxes = 0;     // children with a box: the total when items are wrapped
    };
    std::vector<ListCtx> lists;

    // Whether a line at `depth` may be emitted (a list's item past the page is not).
    bool admit(int depth) {
        if (cut) return false;
        if (!lists.empty() && lists.back().depth + 1 == depth) {
            ListCtx& l = lists.back();
            ++l.seen;
            if (l.seen <= l.from || l.shown >= l.count) return false;
            ++l.shown;
        }
        if (lines >= opt.maxLines) {
            cut = true;
            return false;
        }
        ++lines;
        return true;
    }

    void emit(const dom::Element* el, int depth, std::string role, std::string name, const dom::AbsoluteRect& r) {
        view::Item it;
        it.handle = std::to_string(opt.window) + "." + std::to_string(dv.handleFor(el));
        it.depth = depth;
        it.role = std::move(role);
        it.name = std::move(name);
        it.hasRect = true;
        it.x = r.x;
        it.y = r.y;
        it.w = r.width;
        it.h = r.height;
        fillState(el, it);
        out.push_back(std::move(it));
    }

    void fillState(const dom::Element* el, view::Item& it) {
        const std::string tag = dom::lowerTagName(el);
        const std::string& role = it.role;
        auto aria = [&](const char* n) -> const std::string& {
            static const std::string none;
            return el->hasAttribute(n) ? el->getAttribute(n) : none;
        };
        bool on = false;
        if ((tag == "input") && (role == "checkbox" || role == "radio" || role == "switch"))
            on = el->hasAttribute("checked");
        if (aria("aria-checked") == "true" || aria("aria-selected") == "true" || aria("aria-pressed") == "true")
            on = true;
        const std::string& current = aria("aria-current");
        if (!current.empty() && current != "false") on = true;
        if (tag == "option" && el->hasAttribute("selected")) on = true;
        if (on) it.flags += '*';
        if ((el->hasAttribute("disabled") &&
             (tag == "button" || tag == "input" || tag == "select" || tag == "textarea" || tag == "option" ||
              tag == "fieldset")) ||
            aria("aria-disabled") == "true")
            it.flags += '!';
        if (aria("aria-expanded") == "true" || (tag == "details" && el->hasAttribute("open"))) it.flags += '^';
        if (el == focus) it.flags += '@';

        // Values.
        if (role == "textbox" || role == "searchbox" || role == "spinbutton" || role == "combobox" ||
            role == "slider") {
            std::string v;
            if (tag == "input") {
                v = el->getAttribute("value");
                if (el->getAttribute("type") == "password") v = std::string(std::min<size_t>(v.size(), 12), '*');
            } else if (tag == "textarea") {
                v = el->hasAttribute("value") ? el->getAttribute("value") : el->textContent();
            } else if (tag == "select") {
                if (auto* sel = el->selectControl()) {
                    auto opts = sel->getOptions();
                    const int i = sel->selectedIndex();
                    if (i >= 0 && i < static_cast<int>(opts.size())) v = opts[static_cast<size_t>(i)].text;
                } else {
                    for (const dom::Element* o : el->children())
                        if (o->hasAttribute("selected")) v = dom::renderedText(o);
                }
            } else if (el->hasAttribute("aria-valuetext")) {
                v = el->getAttribute("aria-valuetext");
            } else if (el->hasAttribute("aria-valuenow")) {
                v = el->getAttribute("aria-valuenow");
            } else if (role == "textbox") {
                v = dom::renderedText(el);
            }
            it.value = dom::collapseWhitespace(v);
            it.hasValue = true;
        } else if (role == "progressbar" || role == "meter") {
            std::string v = el->hasAttribute("aria-valuenow") ? el->getAttribute("aria-valuenow")
                                                              : el->getAttribute("value");
            if (!v.empty()) {
                it.value = v;
                it.hasValue = true;
            }
        }
    }

    void walk(dom::Element* el, int depth, Clip clip, bool parentPointer) {
        if (cut) return;
        const std::string tag = dom::lowerTagName(el);
        if (hiddenElement(el, tag)) return;
        dv.lastSeen_[dv.handleFor(el)] = dv.builds_;
        if (styleOf(el, "opacity") == "0") return;
        const bool selfShown = styleOf(el, "visibility") != "hidden" && styleOf(el, "visibility") != "collapse";
        dom::AbsoluteRect r = dom::absoluteBorderBox(el);
        r.x -= vp.scrollX;
        r.y -= vp.scrollY;
        const bool hasBox = r.width > 0 || r.height > 0;
        const bool inView = unclipped || clip.intersects(r.x, r.y, r.width, r.height);
        const std::string& overflow = styleOf(el, "overflow");
        const bool clips = !overflow.empty() && overflow != "visible";
        if (hasBox && clips && !inView) return;
        Clip inner = (hasBox && clips && !unclipped) ? clip.intersect(r.x, r.y, r.width, r.height) : clip;
        const bool pointer = styleOf(el, "cursor") == "pointer";
        const bool shown = selfShown && hasBox && inView && r.width >= 2 && r.height >= 2;

        std::string role = dom::accessibleRole(el);
        if (role == "document") role.clear();
        if (role.empty() && (clickableHint(el) || (pointer && !parentPointer))) role = "clickable";

        auto children = [&](int d) {
            el->forEachComposedChild([&](dom::Element* c) { walk(c, d, inner, pointer); });
        };

        // Something to operate.
        if (dom::isInteractiveRole(role) || role == "clickable") {
            const Below b = below(el);
            const std::string shownRole = role == "clickable" ? "button" : role;
            if (role == "clickable" && b.interactive) return children(depth);  // a delegating container
            if (b.interactive) {
                // An item holding controls (an option with a delete button).
                std::string name = el->hasAttribute("aria-label") ? dom::collapseWhitespace(el->getAttribute("aria-label"))
                                                                    : ownText(el);
                if (shown && admit(depth)) {
                    emit(el, depth, shownRole, std::move(name), r);
                    return children(depth + 1);
                }
                if (shown) return;  // a list's item past the page
                return children(depth);
            }
            if (shown && admit(depth)) emit(el, depth, shownRole, dom::accessibleName(el, doc, shownRole), r);
            return;
        }

        // A structure: a dialog, a list.
        if (isContainerRole(role) || (isNamedGroupRole(role) && (el->hasAttribute("aria-label") ||
                                                                   el->hasAttribute("aria-labelledby")))) {
            if (!shown) return children(depth);
            if (!admit(depth)) return;
            emit(el, depth, role, dom::accessibleName(el, doc, role), r);
            if (!isListRole(role)) return children(depth + 1);
            ListCtx l{depth};
            l.lineIndex = out.size() - 1;
            const bool paged = opt.root && dv.byNode_.count(el->nodeId()) && dv.byNode_[el->nodeId()] == opt.root;
            if (paged) {
                l.from = opt.from;
                l.count = opt.count;
            } else {
                l.count = std::min(opt.count, 40);
            }
            el->forEachComposedChild([&](dom::Element* c) {
                if (!hiddenElement(c, dom::lowerTagName(c))) ++l.boxes;
            });
            lists.push_back(l);
            children(depth + 1);
            ListCtx done = lists.back();
            lists.pop_back();
            const int total = std::max(done.seen, done.boxes);
            if (done.shown < total) {
                out[done.lineIndex].extra = done.from > 0 ? std::to_string(done.from + 1) + "-" +
                                                                std::to_string(done.from + done.shown) + " of " +
                                                                std::to_string(total)
                                                          : std::to_string(done.shown) + " of " + std::to_string(total);
            }
            return;
        }

        // Text with a line of its own: a heading, an image, a status.
        if (isLeafTextRole(role)) {
            const Below b = below(el);
            if (!b.significant) {
                if (!shown) return;
                std::string name = dom::accessibleName(el, doc, role);
                if (name.empty()) name = dom::renderedText(el);
                if ((name.empty() && role != "img" && role != "progressbar" && role != "meter") || !admit(depth))
                    return;
                emit(el, depth, role, std::move(name), r);
                return;
            }
            // A list item holding controls: its own text, then them.
            if (shown && admit(depth)) {
                emit(el, depth, role, ownText(el), r);
                return children(depth + 1);
            }
            if (shown) return;
            return children(depth);
        }

        // A label names its control; it gets no line of its own.
        if (tag == "label" && (el->hasAttribute("for") || below(el).interactive)) return children(depth);

        // A wrapper: its text, if it has any, then what is inside it.
        const std::string own = ownText(el);
        if (!own.empty() && shown) {
            const Below b = below(el);
            if (!b.significant) {
                if (admit(depth)) emit(el, depth, "text", dom::renderedText(el), r);
                return;
            }
            if (admit(depth)) emit(el, depth, "text", own, r);
        }
        children(depth);
    }
};

uint32_t DocumentView::handleFor(const dom::Element* el) {
    auto it = byNode_.find(el->nodeId());
    if (it != byNode_.end()) return it->second;
    const uint32_t n = next_++;
    byNode_.emplace(el->nodeId(), n);
    byHandle_.emplace(n, el->nodeId());
    return n;
}

void DocumentView::prune() {
    // Numbers of elements not seen for a while (gone, or long hidden) go.
    if (byNode_.size() < 4096) return;
    for (auto it = lastSeen_.begin(); it != lastSeen_.end();) {
        if (builds_ - it->second > 64) {
            auto h = byHandle_.find(it->first);
            if (h != byHandle_.end()) {
                byNode_.erase(h->second);
                byHandle_.erase(h);
            }
            it = lastSeen_.erase(it);
        } else {
            ++it;
        }
    }
}

namespace {
dom::Element* findNode(dom::Element* el, uint32_t nodeId) {
    if (el->nodeId() == nodeId) return el;
    dom::Element* hit = nullptr;
    el->forEachComposedChild([&](dom::Element* c) {
        if (!hit) hit = findNode(c, nodeId);
    });
    return hit;
}
}  // namespace

dom::Element* DocumentView::resolve(dom::Document* doc, uint32_t n) const {
    auto it = byHandle_.find(n);
    if (it == byHandle_.end() || !doc || !doc->documentElement()) return nullptr;
    return findNode(doc->documentElement(), it->second);
}

bool DocumentView::build(dom::Document* doc, const Viewport& vp, const Options& opt, view::Snapshot& out) {
    if (!doc || !doc->documentElement()) return true;
    ++builds_;
    ViewWalk w{*this, doc, vp, opt, out};
    dom::Element* active = doc->activeElement();
    if (active && active != doc->body() && active != doc->documentElement()) w.focus = active;
    const Clip all{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    if (opt.root) {
        dom::Element* root = resolve(doc, opt.root);
        if (!root) return false;
        w.unclipped = true;
        // The root's own line, then its items from `from`.
        w.walk(root, 0, all, false);
    } else {
        w.walk(doc->documentElement(), 0, Clip{0, 0, vp.width, vp.height}, false);
    }
    prune();
    return !w.cut;
}

}  // namespace bro::engine
