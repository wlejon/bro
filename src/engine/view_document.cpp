#include "engine/view_document.h"

#include "dom/accessibility.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/text_node.h"
#include "layout/el_select.h"
#include "layout/el_terminal.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <unordered_set>
#include <vector>

namespace bro::engine {

namespace {

// A run of more than this many same-role controls under one element (a
// sidebar of 24 blocks) is shown like a list: its first kRunShow lines, the
// count on a "group" line, paged with `view N.M --from=K`.
constexpr int kRunMax = 20;
constexpr int kRunShow = 12;
// A terminal shows at most its last this many non-blank rows.
constexpr int kTerminalRows = 60;

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
    // A compositor window's frame (engine/window_frames.h): its title bar
    // and buttons are the window's line, and `focus wN` / `close wN`.
    if (el->hasAttribute("data-window-frame")) return true;
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
// Live regions: their text is their name, and their controls (a toast's
// Dismiss) lines under them.
bool isLiveRole(const std::string& r) {
    return r == "status" || r == "alert" || r == "log" || r == "marquee" || r == "timer";
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

// The text of a subtree outside its controls and hidden parts (a toast's
// message, not its Dismiss button's label).
void appendTextOutsideControls(const dom::Element* el, std::string& out) {
    for (const dom::Node* c : el->childNodes()) {
        if (out.size() > 512) return;
        if (c->nodeType() == dom::NodeType::Text) {
            out += static_cast<const dom::TextNode*>(c)->data();
            out += ' ';
        } else if (c->nodeType() == dom::NodeType::Element) {
            const auto* e = static_cast<const dom::Element*>(c);
            const std::string tag = dom::lowerTagName(e);
            if (hiddenElement(e, tag) || dom::isInteractiveRole(dom::accessibleRole(e)) || clickableHint(e)) continue;
            appendTextOutsideControls(e, out);
        }
    }
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

// Whether more than kRunMax of an element's children are controls of one
// role (a long sidebar of buttons): such a run is paged like a list.
bool longRun(const dom::Element* el) {
    size_t kids = 0;
    for (const dom::Node* c : el->childNodes())
        if (c->nodeType() == dom::NodeType::Element) ++kids;
    if (kids <= static_cast<size_t>(kRunMax)) return false;
    std::unordered_map<std::string, int> counts;
    bool run = false;
    el->forEachComposedChild([&](dom::Element* c) {
        if (run || hiddenElement(c, dom::lowerTagName(c))) return;
        std::string role = dom::accessibleRole(c);
        if (!dom::isInteractiveRole(role)) {
            if (!clickableHint(c)) return;
            role = "button";
        }
        if (++counts[role] > kRunMax) run = true;
    });
    return run;
}

}  // namespace

struct ViewWalk {
    DocumentView& dv;
    dom::Document* doc;
    DocumentView::Viewport vp;
    DocumentView::Options opt;
    view::Snapshot& out;
    std::vector<uint32_t>& nodes;  // the node of each line emitted
    const dom::Element* focus = nullptr;
    bool unclipped = false;
    // Inside a label that names its control, or a live region whose text is
    // its name: text gets no line of its own there, only controls do.
    bool muteText = false;
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
        const uint32_t n = dv.handleFor(el);
        dv.lastSeen_[n] = dv.builds_;
        it.handle = std::to_string(opt.window) + "." + std::to_string(n);
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
        nodes.push_back(el->nodeId());
    }

    void fillState(const dom::Element* el, view::Item& it) {
        const std::string tag = dom::lowerTagName(el);
        const std::string& role = it.role;
        auto aria = [&](const char* n) -> const std::string& {
            static const std::string none;
            return el->hasAttribute(n) ? el->getAttribute(n) : none;
        };
        bool on = false;
        if (tag == "input" && (role == "checkbox" || role == "radio" || role == "switch"))
            on = el->hasAttribute("checked");
        if (aria("aria-checked") == "true" || aria("aria-checked") == "mixed" || aria("aria-selected") == "true" ||
            aria("aria-pressed") == "true" || aria("aria-pressed") == "mixed")
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

    // A <terminal>: "terminal@ "title" [cwd]", then a "text" line per
    // non-blank row of its live screen (trailing blanks trimmed, the last
    // kTerminalRows of them), each row's handle fixed to its row index so a
    // diff is row by row.
    void emitTerminal(const dom::Element* el, int depth, const dom::AbsoluteRect& r) {
        const layout::ElTerminal* t = el->terminalControl();
        std::string name = el->hasAttribute("aria-label") || el->hasAttribute("aria-labelledby")
                               ? dom::accessibleName(el, doc, "terminal")
                               : t->title();
        emit(el, depth, "terminal", std::move(name), r);
        const size_t termLine = out.size() - 1;
        out[termLine].extra = t->cwd();
        const uint32_t termNum = view::elementOf(out[termLine].handle);
        if (muteText) return;

        std::vector<std::pair<int, std::string>> rows;
        const std::string screen = t->screenText();
        int row = 0;
        for (size_t i = 0; i <= screen.size(); ++row) {
            size_t j = screen.find('\n', i);
            if (j == std::string::npos) j = screen.size();
            std::string line = screen.substr(i, j - i);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\r' || line.back() == '\t'))
                line.pop_back();
            if (!line.empty()) rows.emplace_back(row, std::move(line));
            i = j + 1;
        }
        const size_t total = rows.size();
        if (total > static_cast<size_t>(kTerminalRows))
            rows.erase(rows.begin(), rows.end() - kTerminalRows);
        if (rows.size() < total) {
            std::string& ex = out[termLine].extra;
            ex += (ex.empty() ? "" : ", ") + std::string("last ") + std::to_string(rows.size()) + " of " +
                  std::to_string(total) + " rows";
        }
        const auto m = t->metrics();
        const float padX = static_cast<float>(std::atof(styleOf(el, "padding-left").c_str()));
        const float padY = static_cast<float>(std::atof(styleOf(el, "padding-top").c_str()));
        for (auto& [ri, text] : rows) {
            if (lines >= opt.maxLines) {
                cut = true;
                return;
            }
            ++lines;
            view::Item it;
            const uint32_t n = dv.terminalRowHandle(termNum, ri);
            it.handle = std::to_string(opt.window) + "." + std::to_string(n);
            it.depth = depth + 1;
            it.role = "text";
            it.name = std::move(text);
            it.hasRect = true;
            it.x = r.x + padX;
            it.y = r.y + padY + static_cast<float>(ri) * m.cellHeight;
            it.w = r.width - 2 * padX;
            it.h = m.cellHeight;
            out.push_back(std::move(it));
            nodes.push_back(el->nodeId());
        }
    }

    // Children walked with text muted (inside a label, a live region).
    template <typename Fn>
    void muted(Fn&& fn) {
        const bool was = muteText;
        muteText = true;
        fn();
        muteText = was;
    }

    void walk(dom::Element* el, int depth, Clip clip, bool parentPointer) {
        if (cut) return;
        const std::string tag = dom::lowerTagName(el);
        if (hiddenElement(el, tag)) return;
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
        // The element as a list: its line, then a page of its items.
        auto asList = [&](const std::string& shownRole, std::string name, int defaultCount) {
            emit(el, depth, shownRole, std::move(name), r);
            ListCtx l{depth};
            l.lineIndex = out.size() - 1;
            const bool paged = opt.root && dv.byNode_.count(el->nodeId()) && dv.byNode_[el->nodeId()] == opt.root;
            if (paged) {
                l.from = opt.from;
                l.count = opt.count;
            } else {
                l.count = std::min(opt.count, defaultCount);
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
            if (isListRole(role)) return asList(role, dom::accessibleName(el, doc, role), 20);
            emit(el, depth, role, dom::accessibleName(el, doc, role), r);
            return children(depth + 1);
        }

        // A terminal: its line (focus, title, cwd), then its screen's rows.
        if (tag == "terminal" && el->terminalControl()) {
            if (shown && admit(depth)) emitTerminal(el, depth, r);
            return;
        }

        // Text with a line of its own: a heading, an image, a status.
        if (isLeafTextRole(role)) {
            const Below b = below(el);
            if (!b.significant) {
                if (!shown || muteText) return;
                std::string name = dom::accessibleName(el, doc, role);
                if (name.empty()) name = dom::renderedText(el);
                if ((name.empty() && role != "img" && role != "progressbar" && role != "meter") || !admit(depth))
                    return;
                emit(el, depth, role, std::move(name), r);
                return;
            }
            // A live region holding controls (a toast and its Dismiss): its
            // text is its name, its controls lines under it.
            if (isLiveRole(role)) {
                if (!shown) return muted([&] { children(depth); });
                if (!admit(depth)) return;
                std::string name;
                if (el->hasAttribute("aria-label") || el->hasAttribute("aria-labelledby"))
                    name = dom::accessibleName(el, doc, role);
                if (name.empty()) {
                    appendTextOutsideControls(el, name);
                    name = dom::collapseWhitespace(name);
                }
                emit(el, depth, role, std::move(name), r);
                return muted([&] { children(depth + 1); });
            }
            // A list item holding controls: its own text, then them.
            if (shown && admit(depth)) {
                emit(el, depth, role, muteText ? std::string() : ownText(el), r);
                return children(depth + 1);
            }
            if (shown) return;
            return children(depth);
        }

        // A label names its control; neither it nor its text gets a line.
        if (tag == "label" && (el->hasAttribute("for") || below(el).interactive))
            return muted([&] { children(depth); });

        // A wrapper: its text, if it has any, then what is inside it.
        const std::string own = muteText ? std::string() : ownText(el);
        if (!own.empty() && shown) {
            const Below b = below(el);
            if (!b.significant) {
                if (admit(depth)) emit(el, depth, "text", dom::renderedText(el), r);
                return;
            }
            if (admit(depth)) emit(el, depth, "text", own, r);
        }
        // A long run of same-role controls: paged like a list.
        if (shown && longRun(el)) {
            if (!admit(depth)) return;
            return asList("group", dom::accessibleName(el, doc, "group"), kRunShow);
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

uint32_t DocumentView::terminalRowHandle(uint32_t terminal, int row) {
    const uint64_t key = (static_cast<uint64_t>(terminal) << 32) | static_cast<uint32_t>(row);
    auto it = termRows_.find(key);
    if (it != termRows_.end()) return it->second;
    const uint32_t n = next_++;
    termRows_.emplace(key, n);
    return n;
}

uint64_t DocumentView::terminalsSignature() {
    // FNV-1a over every terminal's screen, title and cwd.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const std::string& s) {
        for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
        h = (h ^ 0xff) * 1099511628211ull;
    };
    layout::ElTerminal::forEach([&](layout::ElTerminal& t) {
        mix(t.screenText());
        mix(t.title());
        mix(t.cwd());
    });
    return h;
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

void collectNodes(const dom::Element* el, std::unordered_set<uint32_t>& out) {
    out.insert(el->nodeId());
    el->forEachComposedChild([&](dom::Element* c) { collectNodes(c, out); });
}
}  // namespace

dom::Element* DocumentView::resolve(dom::Document* doc, uint32_t n) const {
    auto it = byHandle_.find(n);
    if (it == byHandle_.end() || !doc || !doc->documentElement()) return nullptr;
    return findNode(doc->documentElement(), it->second);
}

void DocumentView::reconcile(dom::Document* doc, view::Snapshot& out, size_t start,
                             const std::vector<uint32_t>& nodes, int window) {
    std::vector<Line> cur;
    cur.reserve(out.size() - start);
    for (size_t i = start; i < out.size(); ++i)
        cur.push_back(Line{view::elementOf(out[i].handle), nodes[i - start], out[i].depth, out[i].role, out[i].name});
    std::vector<Line>& prev = prev_;
    std::unordered_set<uint32_t> prevNums, curNums;
    for (const Line& l : prev) prevNums.insert(l.num);
    for (const Line& l : cur) curNums.insert(l.num);

    // New lines (a number the last view did not show), and lines gone from
    // it whose element has left the document (replaced, not just hidden):
    // a new line that stands where an equivalent gone one stood takes its
    // number, so a re-render that changes nothing visible changes nothing.
    std::vector<char> isNew(cur.size()), gone(prev.size());
    bool anyNew = false, anyGone = false;
    // (Only element lines: a terminal's rows keep their row numbers.)
    for (size_t j = 0; j < cur.size(); ++j)
        anyNew |= (isNew[j] = !prevNums.count(cur[j].num) && byHandle_.count(cur[j].num)) != 0;
    for (size_t i = 0; i < prev.size(); ++i)
        anyGone |= (gone[i] = !curNums.count(prev[i].num) && byHandle_.count(prev[i].num)) != 0;
    if (anyNew && anyGone && doc->documentElement()) {
        std::unordered_set<uint32_t> live;
        collectNodes(doc->documentElement(), live);
        anyGone = false;
        for (size_t i = 0; i < prev.size(); ++i) {
            if (gone[i] && live.count(prev[i].node)) gone[i] = 0;
            anyGone |= gone[i] != 0;
        }
    }
    if (!anyNew || !anyGone) {
        prev_ = std::move(cur);
        return;
    }

    // Align the two views: lines with the same number are anchors; a gone
    // line and a new one match when role, name and depth agree. The longest
    // such alignment (anchors weigh most) decides which pairs are kept.
    const size_t m = prev.size(), n = cur.size();
    auto weight = [&](size_t i, size_t j) -> int {
        if (prev[i].num == cur[j].num) return 1000;
        if (gone[i] && isNew[j] && prev[i].depth == cur[j].depth && prev[i].role == cur[j].role &&
            prev[i].name == cur[j].name)
            return 1;
        return 0;
    };
    std::vector<int> dp((m + 1) * (n + 1), 0);
    auto at = [&](size_t i, size_t j) -> int& { return dp[i * (n + 1) + j]; };
    for (size_t i = m; i-- > 0;)
        for (size_t j = n; j-- > 0;) {
            int best = std::max(at(i + 1, j), at(i, j + 1));
            if (const int w = weight(i, j)) best = std::max(best, w + at(i + 1, j + 1));
            at(i, j) = best;
        }
    std::vector<std::pair<size_t, size_t>> pairs;
    for (size_t i = 0, j = 0; i < m && j < n;) {
        const int w = weight(i, j);
        if (w && at(i, j) == w + at(i + 1, j + 1)) {
            pairs.emplace_back(i, j);
            ++i;
            ++j;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            ++i;
        } else {
            ++j;
        }
    }
    // Between two aligned pairs, gone and new lines of the same role and
    // depth pair up in order: a row whose text changed as it was rebuilt
    // ("Alarms 2" to "Alarms 1") is one changed line, not one out and one in.
    std::vector<std::pair<size_t, size_t>> renumber;
    size_t pi = 0, pj = 0;
    auto fillGap = [&](size_t endI, size_t endJ) {
        size_t k = pi;
        for (size_t j = pj; j < endJ; ++j) {
            if (!isNew[j]) continue;
            for (size_t i = k; i < endI; ++i)
                if (gone[i] && prev[i].role == cur[j].role && prev[i].depth == cur[j].depth) {
                    renumber.emplace_back(i, j);
                    k = i + 1;
                    break;
                }
        }
    };
    for (const auto& [i, j] : pairs) {
        fillGap(i, j);
        if (prev[i].num != cur[j].num) renumber.emplace_back(i, j);
        pi = i + 1;
        pj = j + 1;
    }
    fillGap(m, n);

    for (const auto& [i, j] : renumber) {
        const uint32_t oldNum = prev[i].num, newNum = cur[j].num;
        auto bn = byNode_.find(prev[i].node);
        if (bn != byNode_.end() && bn->second == oldNum) byNode_.erase(bn);
        byHandle_.erase(newNum);
        lastSeen_.erase(newNum);
        byNode_[cur[j].node] = oldNum;
        byHandle_[oldNum] = cur[j].node;
        lastSeen_[oldNum] = builds_;
        cur[j].num = oldNum;
        out[start + j].handle = std::to_string(window) + "." + std::to_string(oldNum);
    }
    prev_ = std::move(cur);
}

bool DocumentView::build(dom::Document* doc, const Viewport& vp, const Options& opt, view::Snapshot& out) {
    if (!doc || !doc->documentElement()) return true;
    ++builds_;
    std::vector<uint32_t> nodes;
    const size_t start = out.size();
    ViewWalk w{*this, doc, vp, opt, out, nodes};
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
        reconcile(doc, out, start, nodes, opt.window);
    }
    prune();
    return !w.cut;
}

}  // namespace bro::engine
