#pragma once

// A document's part of the agent view (docs/agent-view.md): its visible
// interactive elements and meaningful text, as view::Items with handles that
// stay the same for as long as their element lives.
//
// Roles and names are the accessibility tree's (dom/accessibility.h).
// Wrappers collapse away: an element gets a line when it is something to
// operate (a button, a field, an option), a structure worth naming (a
// dialog, a list, a named group), or text. Anything without a box, hidden,
// transparent, or clipped out of sight is left out; a long list shows its
// first items and how many there are, and paging through one (`root` and
// `from`) lists its items whether scrolled into view or not.

#include "engine/agent_view.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::dom {
class Document;
class Element;
}

namespace bro::engine {

class DocumentView {
public:
    struct Options {
        int window = 0;       // the handle prefix: <window>.<n>
        uint32_t root = 0;    // an element handle: only its subtree, unclipped (paging)
        int from = 0;         // items of a list to skip
        int count = 20;       // items of a list to show
        int maxLines = 400;   // the whole view
    };
    /// The client area: size, and the page's scroll (element boxes are in
    /// document space).
    struct Viewport {
        float width = 0, height = 0, scrollX = 0, scrollY = 0;
    };

    /// Appends the elements of `doc` (laid out) to `out`. False when the
    /// view was cut at maxLines (or `root` names no live element).
    bool build(dom::Document* doc, const Viewport& vp, const Options& opt, view::Snapshot& out);

    /// The live element behind element number `n`, else null.
    dom::Element* resolve(dom::Document* doc, uint32_t n) const;

    /// The number an element has (given one on first sight).
    uint32_t handleFor(const dom::Element* el);

    /// The number of a terminal's screen row (row lines have no element).
    uint32_t terminalRowHandle(uint32_t terminal, int row);

    /// A hash of every terminal's screen, title and cwd: output that changes
    /// no DOM still changes the view (its cache key, an action's settle).
    static uint64_t terminalsSignature();

    /// How many elements have numbers (tests).
    size_t known() const { return byNode_.size(); }

private:
    friend struct ViewWalk;
    // A line of the last whole-document view, for reconcile().
    struct Line {
        uint32_t num = 0, node = 0;
        int depth = 0;
        std::string role, name;
    };
    std::unordered_map<uint32_t, uint32_t> byNode_;    // node id -> number
    std::unordered_map<uint32_t, uint32_t> byHandle_;  // number -> node id
    std::unordered_map<uint32_t, uint64_t> lastSeen_;  // number -> build it was last seen in
    std::vector<Line> prev_;
    std::unordered_map<uint64_t, uint32_t> termRows_;  // terminal number << 32 | row -> number
    uint32_t next_ = 1;
    uint64_t builds_ = 0;
    void prune();
    // Handles survive an element replaced by an equivalent one (a list
    // rebuilt on every change): a new line standing where a line of the last
    // view stood, whose element has left the document, with the same role,
    // name and depth (or the same role and depth between lines that kept
    // their numbers), takes that line's number.
    void reconcile(dom::Document* doc, view::Snapshot& out, size_t start, const std::vector<uint32_t>& nodes,
                   int window);
};

}  // namespace bro::engine
