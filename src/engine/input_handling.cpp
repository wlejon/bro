// Engine input handling methods — split from engine.cpp for readability.
// These are Engine member function implementations, not a separate class.

#include "engine/engine.h"
#include "engine/key_mapping.h"
#include "engine/overflow.h"
#include "engine/overlay.h"
#include "engine/input_common.h"
#include "engine/replaced_elements.h"
#include "engine/settings.h"

#if BRO_WITH_3D
#include "scene/scene_graph.h"
#include "scene/html_node.h"
#endif
#include "layout/layout_node_adapter.h"
#include "layout/element_ref_adapter.h"
#include "layout/box.h"

#include "platform/sdl_window.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/event_dispatch.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/event.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "dom/text_node.h"
#include "layout/control_text.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "layout/key_handle_result.h"
#include "layout/selection_geometry.h"
#include "layout/skia_text_metrics.h"
#include "util/time.h"

#include <cctype>

// ---------------------------------------------------------------------------
// Contenteditable edit helpers — shared by handleKeyDown (backspace/delete,
// cut/paste) and handleTextInput (typing). Defined here so both sites see
// the template definition.
// ---------------------------------------------------------------------------

// The nearest contenteditable host element of `node`: the closest ancestor
// (or self) with a contenteditable attribute set to anything other than
// "false". Returns nullptr when the node isn't editable — including under an
// explicit `contenteditable="false"`.
static bro::dom::Element* editableHostOf(bro::dom::Node* node) {
    for (bro::dom::Node* n = node; n; n = n->parentNode()) {
        if (n->nodeType() != bro::dom::NodeType::Element) continue;
        auto* el = static_cast<bro::dom::Element*>(n);
        if (!el->hasAttribute("contenteditable")) continue;
        return el->getAttribute("contenteditable") != "false" ? el : nullptr;
    }
    return nullptr;
}

// Return true if `node` is inside a contenteditable host (attribute set to
// anything other than "false"). Skips `contenteditable="false"`.
static bool inEditableHost(bro::dom::Node* node) {
    return editableHostOf(node) != nullptr;
}

// ---------------------------------------------------------------------------
// Cross-node deletion.
//
// Backspace at offset 0 of a text node, or Delete at its end, has no character
// left in that node to take. The character the user means lives in the
// neighbouring leaf: the tail of the text before an inline, the head of the
// text after it, or a <br> the Enter path inserted. Finding it is a walk over
// the editable host's leaves, which is all these helpers do — the key handler
// stays a decision about which direction to walk.
//
// Plaintext scope: text nodes merge and emptied inlines are dropped. Nothing
// here merges blocks or reparents content across them.
// ---------------------------------------------------------------------------

static int indexInParent(bro::dom::Node* n) {
    auto* p = n ? n->parentNode() : nullptr;
    if (!p) return -1;
    const auto& kids = p->childNodes();
    for (size_t i = 0; i < kids.size(); ++i)
        if (kids[i] == n) return static_cast<int>(i);
    return -1;
}

static bro::dom::Node* deepestLast(bro::dom::Node* n) {
    while (n && !n->childNodes().empty()) n = n->childNodes().back();
    return n;
}
static bro::dom::Node* deepestFirst(bro::dom::Node* n) {
    while (n && !n->childNodes().empty()) n = n->childNodes().front();
    return n;
}

static bool isBrElement(bro::dom::Node* n) {
    return n && n->nodeType() == bro::dom::NodeType::Element &&
           static_cast<bro::dom::Element*>(n)->tagName() == "BR";
}

// The leaf immediately before / after `n` in document order, without leaving
// `host`'s subtree. nullptr at the host's edge.
static bro::dom::Node* prevLeafWithin(bro::dom::Node* n, bro::dom::Element* host) {
    while (n && n != host) {
        const int i = indexInParent(n);
        auto* p = n->parentNode();
        if (i > 0) return deepestLast(p->childNodes()[i - 1]);
        n = p;
    }
    return nullptr;
}
static bro::dom::Node* nextLeafWithin(bro::dom::Node* n, bro::dom::Element* host) {
    while (n && n != host) {
        const int i = indexInParent(n);
        auto* p = n->parentNode();
        if (i >= 0 && i + 1 < static_cast<int>(p->childNodes().size()))
            return deepestFirst(p->childNodes()[i + 1]);
        n = p;
    }
    return nullptr;
}

// What a collapsed caret deletes: a byte range in some text node, or a whole
// node (a <br>). Both empty means there is nothing on that side to delete.
struct EditDeleteTarget {
    bro::dom::TextNode* text = nullptr;
    int start = 0;
    int end = 0;
    bro::dom::Node* remove = nullptr;
};

// Resolve the character a collapsed caret at (node, offset) deletes, walking
// `dir` (-1 backward, +1 forward) across leaf boundaries within `host`.
static EditDeleteTarget resolveDeleteTarget(bro::dom::Node* node, int offset,
                                            bro::dom::Element* host, int dir) {
    EditDeleteTarget out;
    if (!node || !host) return out;

    auto spliceAt = [&](bro::dom::TextNode* t, int off) {
        const std::string& d = t->data();
        out.text = t;
        if (dir < 0) { out.start = bro::layout::utf8Prev(d, off); out.end = off; }
        else         { out.start = off; out.end = bro::layout::utf8Next(d, off); }
    };

    bro::dom::Node* leaf = nullptr;
    if (node->nodeType() == bro::dom::NodeType::Text) {
        auto* t = static_cast<bro::dom::TextNode*>(node);
        const int len = static_cast<int>(t->length());
        offset = std::clamp(offset, 0, len);
        // The caret's own node still has a character on the requested side.
        if (dir < 0 ? offset > 0 : offset < len) { spliceAt(t, offset); return out; }
        leaf = (dir < 0) ? prevLeafWithin(t, host) : nextLeafWithin(t, host);
    } else {
        // Element position: the caret sits between children.
        const auto& kids = node->childNodes();
        offset = std::clamp(offset, 0, static_cast<int>(kids.size()));
        if (dir < 0)
            leaf = (offset > 0) ? deepestLast(kids[offset - 1])
                                : prevLeafWithin(node, host);
        else
            leaf = (offset < static_cast<int>(kids.size()))
                       ? deepestFirst(kids[offset])
                       : nextLeafWithin(node, host);
    }

    // Skip leaves that hold no character (empty text nodes, empty inlines)
    // until one does, or until the host's edge.
    while (leaf) {
        if (leaf->nodeType() == bro::dom::NodeType::Text) {
            auto* t = static_cast<bro::dom::TextNode*>(leaf);
            const int len = static_cast<int>(t->length());
            if (len > 0) { spliceAt(t, dir < 0 ? len : 0); return out; }
        } else if (isBrElement(leaf)) {
            // Enter inserts a <br>; Backspace has to be able to take it back.
            out.remove = leaf;
            return out;
        }
        leaf = (dir < 0) ? prevLeafWithin(leaf, host) : nextLeafWithin(leaf, host);
    }
    return out;
}

// Tidy up after a delete: drop an emptied inline element (and any now-empty
// ancestors below the host), then merge the text nodes the removal left
// adjacent. `caretNode`/`caretOff` are updated to follow the content.
static void pruneAndMerge(bro::dom::Element* host,
                          bro::dom::Node*& caretNode, int& caretOff) {
    if (!host || !caretNode) return;

    // An emptied text node inside a non-host inline takes the inline with it.
    if (caretNode->nodeType() == bro::dom::NodeType::Text &&
        static_cast<bro::dom::TextNode*>(caretNode)->length() == 0 &&
        caretNode->parentNode() && caretNode->parentNode() != host) {
        auto* p = caretNode->parentNode();
        p->removeChild(caretNode);
        caretNode = p;
        caretOff = static_cast<int>(p->childNodes().size());
    }
    // Then any ancestors the removal emptied, stopping below the host.
    while (caretNode != host && caretNode->childNodes().empty() &&
           caretNode->nodeType() == bro::dom::NodeType::Element &&
           caretNode->parentNode()) {
        auto* p = caretNode->parentNode();
        const int idx = indexInParent(caretNode);
        p->removeChild(caretNode);
        caretNode = p;
        caretOff = idx < 0 ? 0 : idx;
        if (p == host) break;
    }

    // Removing the inline can leave two text nodes side by side. Join them so
    // the caret sits in one node rather than at a seam that only looks like
    // one position.
    if (caretNode->nodeType() != bro::dom::NodeType::Element) return;
    auto& kids = caretNode->childNodes();
    if (caretOff <= 0 || caretOff >= static_cast<int>(kids.size())) return;
    auto* left = kids[caretOff - 1];
    auto* right = kids[caretOff];
    if (left->nodeType() != bro::dom::NodeType::Text ||
        right->nodeType() != bro::dom::NodeType::Text) return;
    auto* lt = static_cast<bro::dom::TextNode*>(left);
    auto* rt = static_cast<bro::dom::TextNode*>(right);
    const int join = static_cast<int>(lt->length());
    lt->appendData(rt->data());
    caretNode->removeChild(rt);
    caretNode = lt;
    caretOff = join;
}

// The hovered element changed from `prev` to `target`. Per CSS Selectors L4,
// :hover matches the element under the pointer AND every ancestor, so the
// pseudo flips on each element along the path from the old/new target up to
// their lowest common ancestor (the LCA and everything above it contain a
// hovered descendant both before and after, so their :hover is unchanged).
// Mark exactly those elements dirty so they re-resolve — walking each chain up
// to but excluding the LCA keeps a hover move bounded to the two changed
// subtrees instead of dirtying the whole tree. Either side may be null (first
// hover / leaving the document), in which case that chain has no shared
// ancestor and the whole path to the root legitimately changes.
//
// markStyleDirty (not markDirty): a :hover restyle is almost always paint-only
// (background/color), so it must not force the full O(N) layoutTree() pass on
// every mouse move. resolveStyles() diffs each re-resolved element and promotes
// to a real layout only if a :hover rule actually changed geometry, so a
// `:hover { padding }` still lays out correctly.
//
// And not markPaintDirty either, which would ALSO set selectorDirty_ and re-
// resolve every element under each chain element — landing the pointer on a
// container then costs a restyle of its whole subtree (a rail of 700 elements,
// every mouse move). The rules that can re-match around a hover flip are only
// those naming :hover outside their subject compound (`.row:hover .label`), so
// each flipped element instead gets a hover *scope* mark: resolveStyles walks
// its subtree and re-resolves only the elements such a rule could actually name
// (Cascade::hoverCanAffect). Everything else keeps the style it has.
//
// A rule whose :hover reaches its subject through a sibling combinator
// (`.tab:hover + .panel`) names an element OUTSIDE the flipped element's
// subtree, so a sheet that has one widens each scope to the parent. The chain
// elements' parents are chain elements themselves, so in practice that is just
// the common ancestor.
static void markHoverChainDirty(const htmlayout::css::Cascade& cascade,
                                bro::dom::Element* prev, bro::dom::Element* target) {
    const bool siblingScope = cascade.hoverAffectsSiblings();
    auto isAncestorOrSelf = [](bro::dom::Element* a, bro::dom::Element* d) {
        for (auto* e = d; e; e = e->parentElement())
            if (e == a) return true;
        return false;
    };
    // Its own :hover flipped: re-resolve it. And if some rule pairs a :hover on
    // an element like it with a subject elsewhere, open the scope that finds
    // that subject — for anything else (the container the pointer crossed, the
    // gap between two rows) the flip changes nothing but the element itself.
    auto flipped = [&](bro::dom::Element* e) {
        e->markStyleDirty();
        if (!cascade.hoverInvalidatesDescendants(e->tagName(), e->getAttribute("id"),
                                                 e->getAttribute("class")))
            return;
        e->markHoverScopeDirty();
        if (siblingScope && e->parentElement())
            e->parentElement()->markHoverScopeDirty();
    };
    bro::dom::Element* lca = nullptr;
    for (auto* e = target; e; e = e->parentElement())
        if (isAncestorOrSelf(e, prev)) { lca = e; break; }
    for (auto* e = target; e && e != lca; e = e->parentElement()) flipped(e);
    for (auto* e = prev; e && e != lca; e = e->parentElement()) flipped(e);
}

// Walk from `el` up to the root checking computed `user-select`. Returns true
// if any ancestor (or el itself) has `user-select: none`, in which case the
// engine should not initiate a text-selection drag for clicks landing on this
// element. `auto` is treated as "look further up" so a top-level
// `user-select: none` on <html> or <body> propagates without needing the
// property explicitly set on every descendant.
static bool isSelectionSuppressed(bro::dom::Element* el) {
    for (auto* cur = el; cur; cur = cur->parentElement()) {
        const auto& style = cur->computedStyle();
        auto it = style.find("user-select");
        if (it == style.end()) continue;
        const std::string& v = it->second;
        if (v == "none") return true;
        if (v == "auto") continue;
        return false;   // "text", "all", "contain" → allow selection
    }
    return false;
}

// Delete the content currently covered by the Selection's range, collapsing
// it to the start position. Returns the post-deletion caret (node, offset).
static void deleteRangeContents(bro::dom::Document* doc,
                                bro::dom::Range& r,
                                bro::dom::Node*& node, int& off) {
    // Read the caret AFTER the removal: deleteContents() collapses the range
    // to a position that survives it, whereas the pre-removal start container
    // is frequently one of the nodes it frees.
    r.deleteContents();
    node = r.startContainer();
    off = r.startOffset();
    if (doc) doc->markDirty();
}

// Dispatch beforeinput → optionally perform an edit → dispatch input.
// `runEdit` is the mutation callback; runs only when beforeinput wasn't
// default-prevented. Events fire on the nearest editable Element ancestor.
template <typename EditFn>
static void runEditableMutation(bro::dom::Document* doc,
                                bro::js::Runtime* rt,
                                bro::dom::Node* focusNode,
                                const std::string& inputType,
                                const std::string& data,
                                EditFn&& runEdit) {
    if (!doc || !focusNode) return;
    auto* host = focusNode;
    while (host && host->nodeType() != bro::dom::NodeType::Element)
        host = host->parentNode();
    auto* hostEl = host ? static_cast<bro::dom::Element*>(host) : doc->body();

    bro::dom::InputEvent beforeEvt("beforeinput", /*bubbles=*/true, /*cancelable=*/true);
    beforeEvt.setInputType(inputType);
    beforeEvt.setData(data);
    beforeEvt.setIsTrusted(true);
    if (hostEl && rt) {
        bro::js::dispatchDomEvent(rt->getContext(), hostEl, beforeEvt);
    }
    if (beforeEvt.defaultPrevented()) return;

    runEdit();

    bro::dom::InputEvent inputEvt("input", /*bubbles=*/true, /*cancelable=*/false);
    inputEvt.setInputType(inputType);
    inputEvt.setData(data);
    inputEvt.setIsTrusted(true);
    if (hostEl && rt) {
        bro::js::dispatchDomEvent(rt->getContext(), hostEl, inputEvt);
    }
    doc->markDirty();
}

// Snapshot the state one contenteditable edit needs, then hand it to the
// host's undo history. Two modes, matching DomUndoStack's two entry shapes:
//
//   - text mode (`tn` non-null): the caller knows the edit only rewrites that
//     one node's data — plain typing at a text caret, a backspace or delete
//     inside a node. Records a cheap splice that coalesces with its neighbors
//     exactly as the controls' typing/backspace/delete runs do.
//
//   - structural mode (`tn` null): the edit may reshape the tree (Enter,
//     paste, cut, typing over a selection). Records the host's serialized
//     children either side of the edit as one Discrete entry.
//
// The snapshot is taken when the scope is constructed, so callers construct
// it INSIDE the edit lambda — after a beforeinput handler has had its say,
// so script's own mutations aren't folded into the user's undo entry.
class EditUndoScope {
public:
    EditUndoScope(bro::engine::DomUndoHistories* hist, bro::dom::Document* doc,
                  bro::dom::Element* host, bro::dom::TextNode* tn,
                  bro::engine::DomUndoStack::Kind kind)
        : hist_(hist), doc_(doc), host_(host), tn_(tn), kind_(kind) {
        if (!hist_ || !host_ || !doc_) return;
        selBefore_ = bro::engine::DomUndoStack::selectionOf(doc_, host_);
        if (tn_) beforeData_ = tn_->data();
        else beforeHTML_ = host_->innerHTML();
    }

    void commit() {
        if (!hist_ || !host_ || !doc_) return;
        const auto selAfter = bro::engine::DomUndoStack::selectionOf(doc_, host_);
        const double now = bro::util::currentTimeMs();
        auto& stack = hist_->forHost(doc_, host_);
        if (tn_) {
            stack.recordTextEdit(host_, tn_, beforeData_, tn_->data(),
                                 selBefore_, selAfter, kind_, now);
        } else {
            stack.recordStructural(host_, beforeHTML_, host_->innerHTML(),
                                   selBefore_, selAfter, now);
        }
    }

private:
    bro::engine::DomUndoHistories* hist_;
    bro::dom::Document* doc_;
    bro::dom::Element* host_;
    bro::dom::TextNode* tn_;
    bro::engine::DomUndoStack::Kind kind_;
    std::string beforeData_;
    std::string beforeHTML_;
    bro::engine::DomUndoStack::Sel selBefore_;
};

// Resolve the Selection caret to a (textNode, byteOffset) insertion position,
// deleting any selected content first (typing semantics). When the caret sits
// between element children — or inside an empty element — a fresh empty text
// node is inserted at exactly that position, the same placement rule regular
// contenteditable typing uses, so plain insertion and IME composition share
// one insertion path. `created` reports whether that happened (a canceled
// composition removes the node again). Returns nullptr when there is no
// usable caret.
static bro::dom::TextNode* selectionCaretTextPosition(bro::dom::Document* doc,
                                                      int& off, bool& created) {
    off = 0;
    created = false;
    if (!doc) return nullptr;
    auto* sel = doc->selection();
    if (!sel || sel->rangeCount() == 0) return nullptr;
    auto* range = sel->getRangeAt(0);
    if (!range) return nullptr;

    bro::dom::Node* caretNode = nullptr;
    int caretOff = 0;
    if (!range->collapsed()) {
        deleteRangeContents(doc, *range, caretNode, caretOff);
    } else {
        caretNode = range->startContainer();
        caretOff = range->startOffset();
    }
    if (!caretNode) return nullptr;

    if (caretNode->nodeType() == bro::dom::NodeType::Text) {
        off = caretOff;
        return static_cast<bro::dom::TextNode*>(caretNode);
    }
    if (caretNode->nodeType() == bro::dom::NodeType::Element) {
        auto* el = static_cast<bro::dom::Element*>(caretNode);
        auto* tn = doc->createTextNode("");
        auto& kids = el->childNodes();
        if (caretOff >= static_cast<int>(kids.size())) {
            el->appendChild(tn);
        } else {
            el->insertBefore(tn, kids[caretOff]);
        }
        // (appendChild/insertBefore mark layout structure themselves now — see
        // dom/element.cpp. This used to need an explicit markStructureDirty()
        // or the new text node got no layout adapter and caret/selection
        // geometry couldn't see it.)
        created = true;
        return tn;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Whitespace rebalancing.
//
// Under `white-space: normal` a run of spaces renders as ONE space, and a
// space at the start or end of a line renders as nothing. That is correct CSS
// and completely wrong as editing behaviour: type a space at the end of a
// contenteditable and the caret does not move, type three and only one shows.
//
// Every browser solves this the same way — store U+00A0 (no-break space) in
// the positions that would otherwise collapse, so what the user typed is what
// the layout sees. The text is still N characters to the DOM and to
// Selection; only the encoding changes.
//
// The pattern, matched against Chromium exactly (a run of N spaces, `_` a
// plain space and `+` a no-break space):
//
//     "ab| "     -> ab+          a lone space at the end cannot be plain
//     "ab| c"    -> ab_c         between two words a plain space renders
//     "ab|  c"   -> ab+_c        alternate, so no two plain spaces meet
//     "ab|   c"  -> ab+_+c
//     "ab|   "   -> ab+_+
//     "ab|  "    -> ab++         alternating ends plain — forced back
//     "| ab"     -> +ab          a run at the start cannot be plain either
//
// So: alternate from the run's start beginning with no-break; force the last
// one when the run ends the text; and a lone interior space stays plain,
// which is the overwhelmingly common case and keeps the data readable.
constexpr char kNbspUtf8[] = "\xc2\xa0";

// Is the character at byte `i` a space this pass owns? Plain ASCII space or
// an existing U+00A0 — an existing one has to be re-examined, because a run
// it used to end may have grown a neighbour that changes its encoding.
static bool isRebalanceSpace(const std::string& s, size_t i, size_t& len) {
    if (s[i] == ' ') { len = 1; return true; }
    if (i + 1 < s.size() &&
        static_cast<unsigned char>(s[i]) == 0xC2 &&
        static_cast<unsigned char>(s[i + 1]) == 0xA0) { len = 2; return true; }
    return false;
}

// Re-encode every space run in `data`, moving `caret` (a byte offset) to the
// matching position in the result. Returns true if anything changed.
static bool rebalanceWhitespace(std::string& data, int& caret) {
    std::string out;
    out.reserve(data.size());
    int newCaret = caret;
    bool changed = false;

    size_t i = 0;
    while (i < data.size()) {
        size_t len = 0;
        if (!isRebalanceSpace(data, i, len)) {
            if (static_cast<int>(i) == caret) newCaret = static_cast<int>(out.size());
            out += data[i];
            ++i;
            continue;
        }
        // Collect the whole run, remembering each member's source offset so
        // the caret can be placed against the same member afterwards.
        std::vector<size_t> srcOffsets;
        size_t j = i;
        while (j < data.size()) {
            size_t l = 0;
            if (!isRebalanceSpace(data, j, l)) break;
            srcOffsets.push_back(j);
            j += l;
        }
        const size_t n = srcOffsets.size();
        const bool atStart = (i == 0);
        const bool atEnd   = (j == data.size());

        for (size_t k = 0; k < n; ++k) {
            bool nbsp = (k % 2 == 0);
            if (k + 1 == n && atEnd) nbsp = true;       // cannot end plain
            if (n == 1 && !atStart && !atEnd) nbsp = false;  // lone interior
            if (static_cast<int>(srcOffsets[k]) == caret)
                newCaret = static_cast<int>(out.size());
            if (nbsp) out += kNbspUtf8; else out += ' ';
        }
        if (static_cast<int>(j) == caret) newCaret = static_cast<int>(out.size());
        i = j;
    }
    if (static_cast<int>(data.size()) == caret) newCaret = static_cast<int>(out.size());

    changed = (out != data);
    if (changed) { data = std::move(out); caret = newCaret; }
    return changed;
}

// Rebalancing only applies where the renderer actually collapses. Under
// `pre` / `pre-wrap` / `break-spaces` a plain space already renders, and
// rewriting it to U+00A0 would change what the document means — a
// pre-formatted block is exactly where the distinction matters.
static bool collapsesWhitespace(bro::dom::Node* textNode) {
    for (bro::dom::Node* n = textNode; n; n = n->parentNode()) {
        if (n->nodeType() != bro::dom::NodeType::Element) continue;
        const auto& style = static_cast<bro::dom::Element*>(n)->computedStyle();
        auto it = style.find("white-space");
        if (it == style.end()) continue;
        return it->second == "normal" || it->second == "nowrap";
    }
    return true;   // the initial value is `normal`
}

// Re-encode the node's spaces after an edit and keep the caret on the same
// character. No-op when nothing needed changing, so the common keystroke
// costs one scan and no DOM mutation.
static void rebalanceAfterEdit(bro::dom::Document* doc,
                               bro::dom::TextNode* tn, int caret) {
    if (!tn || !collapsesWhitespace(tn)) return;
    std::string data = tn->data();
    int newCaret = caret;
    if (!rebalanceWhitespace(data, newCaret)) return;
    tn->setData(data);
    doc->selection()->collapse(tn, newCaret);
}

// Insert `text` at the current Selection. If the selection isn't collapsed,
// deletes the contents first. Caret ends up after the inserted text.
static void selectionInsertText(bro::dom::Document* doc, const std::string& text) {
    int off = 0;
    bool created = false;
    auto* tn = selectionCaretTextPosition(doc, off, created);
    if (!tn) return;
    tn->insertData(static_cast<size_t>(off), text);
    const int caret = off + static_cast<int>(text.size());
    doc->selection()->collapse(tn, caret);
    rebalanceAfterEdit(doc, tn, caret);
}
#include "util/time.h"
#include "util/log.h"
#include "util/platform.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace bro::engine {

// Keyboard "action" event dispatch lives in Engine::dispatchActionEventForKey
// (action_input.cpp) — shared with the mouse-button and gamepad paths.

// ---------------------------------------------------------------------------
// Input focus helpers
// ---------------------------------------------------------------------------

// Safe wrapper: SDL_GetModState() requires SDL_INIT_VIDEO. In --no-gpu
// headless mode no SDL video subsystem is initialized, so the call would
// dereference an internal NULL pointer and crash.  Return 0 (no modifiers)
// when there is no window.
//
// `heldModifierMask` ORs in modifiers held via simulated handleKeyDown()
// calls (see Engine::heldModifierMask_) — SDL_GetModState() only reflects
// the OS's real physical keyboard, which headless input simulation never
// touches, so without this a simulated keyDown(shift) + click() (e.g. a
// shift-click) would never see shiftKey on the resulting MouseEvent.
static int safeGetModState(platform::Window* window, int heldModifierMask) {
    return (window ? static_cast<int>(SDL_GetModState()) : 0) | heldModifierMask;
}

int Engine::currentModState() const {
    return safeGetModState(window_.get(), heldModifierMask_);
}

// Maps a modifier keycode to its SDL_KMOD_* bit (both left/right variants
// fold onto the same bit, matching SDL_GetModState()'s own behavior). Returns
// 0 for non-modifier keys.
int modifierBitForKeycode(int keycode) {
    switch (keycode) {
        case SDLK_LSHIFT: case SDLK_RSHIFT: return SDL_KMOD_SHIFT;
        case SDLK_LCTRL:  case SDLK_RCTRL:  return SDL_KMOD_CTRL;
        case SDLK_LALT:   case SDLK_RALT:   return SDL_KMOD_ALT;
        case SDLK_LGUI:   case SDLK_RGUI:   return SDL_KMOD_GUI;
        default: return 0;
    }
}

// Safe wrappers for SDL text input — no-ops when there is no window.
void safeStartTextInput(platform::Window* window) {
    if (window) SDL_StartTextInput(window->getSDLWindow());
}
void safeStopTextInput(platform::Window* window) {
    if (window) SDL_StopTextInput(window->getSDLWindow());
}

// Returns true if the element is a focusable text-editing control (input or textarea)
static bool isTextEditable(dom::Element* el) {
    return getElInput(el) || getElTextarea(el);
}

// A control that owns a text caret and selection: a <textarea>, or an <input>
// of a text-ish type. Excludes checkbox/radio/range/color/button inputs, where
// a press means something else entirely and a drag is not a text drag.
bool isCaretControl(dom::Element* el) {
    if (getElTextarea(el)) return true;
    if (auto* in = getElInput(el)) return in->isTextType(el);
    return false;
}

// Build a KeyboardEvent with all modifier fields set.
dom::KeyboardEvent makeKeyboardEvent(const char* type,
                                            int keycode, int scancode,
                                            int mod, bool repeat) {
    dom::KeyboardEvent evt(type);
    evt.setKey(sdlKeycodeToWebKey(keycode, mod));
    evt.setCode(sdlScancodeToWebCode(scancode));
    evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
    evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
    evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
    evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
    evt.setRepeat(repeat);
    evt.setIsTrusted(true);

    // Set location for left/right modifier keys
    if (scancode == SDL_SCANCODE_LSHIFT || scancode == SDL_SCANCODE_LCTRL ||
        scancode == SDL_SCANCODE_LALT || scancode == SDL_SCANCODE_LGUI)
        evt.setLocation(1); // DOM_KEY_LOCATION_LEFT
    else if (scancode == SDL_SCANCODE_RSHIFT || scancode == SDL_SCANCODE_RCTRL ||
             scancode == SDL_SCANCODE_RALT || scancode == SDL_SCANCODE_RGUI)
        evt.setLocation(2); // DOM_KEY_LOCATION_RIGHT
    else if (keycode >= SDLK_KP_DIVIDE && keycode <= SDLK_KP_EQUALS)
        evt.setLocation(3); // DOM_KEY_LOCATION_NUMPAD

    return evt;
}

// Element-relative offset coords live in replaced_elements.cpp as
// applyMouseOffset — shared by app and system paths.

// Convert SDL3 mouse button id (1=left, 2=middle, 3=right, 4=X1, 5=X2)
// into the DOM MouseEvent.button index (0=left, 1=middle, 2=right, 3=back, 4=forward).
// SDL and the DOM disagree on both the base index and the middle/right ordering.
int sdlToDomButton(int sdlButton) {
    switch (sdlButton) {
        case 1: return 0;  // SDL left   -> DOM primary
        case 2: return 1;  // SDL middle -> DOM auxiliary
        case 3: return 2;  // SDL right  -> DOM secondary
        case 4: return 3;  // SDL X1     -> DOM back
        case 5: return 4;  // SDL X2     -> DOM forward
        default: return sdlButton - 1;
    }
}

// MouseEvent.buttons bitmask values, keyed by DOM button index.
// Note that DOM swaps right (2) and middle (4) relative to a naive 1<<n encoding.
int domButtonMask(int domButton) {
    switch (domButton) {
        case 0: return 1;   // left
        case 1: return 4;   // middle
        case 2: return 2;   // right
        case 3: return 8;   // back
        case 4: return 16;  // forward
        default: return 0;
    }
}

// Build a MouseEvent with standard fields populated.
// `x, y` are screen-space; `contentTop` is the engine-reserved top inset
// (menu bar) so clientY/pageY are reported in the web-standard content space.
void populateMouseEvent(dom::MouseEvent& evt, float x, float y,
                               int button, int buttons,
                               float movementX, float movementY,
                               float scrollY, int mod,
                               float contentTop) {
    float cy = y - contentTop;
    evt.setClientX(static_cast<double>(x));
    evt.setClientY(static_cast<double>(cy));
    evt.setScreenX(static_cast<double>(x));
    evt.setScreenY(static_cast<double>(y));
    evt.setPageX(static_cast<double>(x));
    evt.setPageY(static_cast<double>(cy + scrollY));
    evt.setMovementX(static_cast<double>(movementX));
    evt.setMovementY(static_cast<double>(movementY));
    evt.setButton(button);
    evt.setButtons(buttons);
    evt.setCtrlKey((mod & SDL_KMOD_CTRL) != 0);
    evt.setShiftKey((mod & SDL_KMOD_SHIFT) != 0);
    evt.setAltKey((mod & SDL_KMOD_ALT) != 0);
    evt.setMetaKey((mod & SDL_KMOD_GUI) != 0);
    evt.setIsTrusted(true);
}

// ---------------------------------------------------------------------------
// Scrollbar hit testing helper
// ---------------------------------------------------------------------------

// findElementScrollbarHit lives in engine/overflow.h so system_panels.cpp
// and the app input handler share one implementation.

// ---------------------------------------------------------------------------
// Focus event dispatching
// ---------------------------------------------------------------------------

void Engine::dispatchFocusEvents(dom::Element* oldTarget, dom::Element* newTarget) {
    if (oldTarget == newTarget) return;

    (void)jsRuntime_; // focus events are dispatched via dispatchEvent

    // blur (non-bubbling) on old target
    if (oldTarget) {
        dom::FocusEvent blurEvt("blur", false, false);
        blurEvt.setRelatedTarget(newTarget);
        blurEvt.setIsTrusted(true);
        dispatchEvent(oldTarget, blurEvt);
    }

    // focus (non-bubbling) on new target
    if (newTarget) {
        dom::FocusEvent focusEvt("focus", false, false);
        focusEvt.setRelatedTarget(oldTarget);
        focusEvt.setIsTrusted(true);
        dispatchEvent(newTarget, focusEvt);
    }

    // focusout (bubbling) on old target
    if (oldTarget) {
        dom::FocusEvent focusoutEvt("focusout", true, false);
        focusoutEvt.setRelatedTarget(newTarget);
        focusoutEvt.setIsTrusted(true);
        dispatchEvent(oldTarget, focusoutEvt);
    }

    // focusin (bubbling) on new target
    if (newTarget) {
        dom::FocusEvent focusinEvt("focusin", true, false);
        focusinEvt.setRelatedTarget(oldTarget);
        focusinEvt.setIsTrusted(true);
        dispatchEvent(newTarget, focusinEvt);
    }
}

// ---------------------------------------------------------------------------
// Scroll event dispatching
// ---------------------------------------------------------------------------

void Engine::dispatchScrollEvent(dom::Element* el) {
    if (!el) return;
    dom::Event evt("scroll", false, false); // scroll doesn't bubble
    evt.setIsTrusted(true);
    dispatchEvent(el, evt);
}

// ---------------------------------------------------------------------------
// Mouse events
// ---------------------------------------------------------------------------

float Engine::overlayMouseY(float y) const {
    const Overlay* active = overlayMgr_.active();
    if (active && active->context() == OverlayContext::App) {
        return y - static_cast<float>(contentTop());
    }
    return y;
}

// ---------------------------------------------------------------------------
// Iframe input routing — a host mouse event over an <iframe> element is
// translated into the sub-document's own content space and dispatched through
// the same per-doc helpers the app and system panels use, against the iframe's
// isolated document/JS context/mouse state. Mirrors systemHandleMouse*.
// ---------------------------------------------------------------------------

dom::Element* Engine::iframeHitTest(IframeDoc* dp, float lx, float ly) {
    if (!dp || !dp->document) return nullptr;
    auto* root = dp->document->layoutRoot();
    if (!root) return nullptr;
    auto* node = htmlayout::layout::hitTest(root, lx, ly);
    auto* hit = layout::LayoutNodeAdapter::elementFor(node);
    if (!hit || hit == dp->document->documentElement()) return nullptr;
    return hit;
}

bool Engine::iframeHandleMouseDown(dom::Element* frameEl, float docX, float docY,
                                   int button, float movementX, float movementY, int mod) {
    if (!frameEl || !frameEl->iframeDoc()) return false;
    auto* dp = static_cast<IframeDoc*>(frameEl->iframeDoc());
    if (!dp->document || !dp->jsCtx) return false;
    dom::AbsoluteRect box = dom::absoluteContentBox(frameEl);
    float lx = docX - box.x, ly = docY - box.y;
    dom::Element* sub = iframeHitTest(dp, lx, ly);
    if (sub) {
        dom::MouseEvent evt("mousedown");
        populateMouseEvent(evt, lx, ly, button, pressedButtons_, movementX, movementY,
                           0.0f, mod, 0.0f);
        applyMouseOffset(evt, sub);
        ControlContext cctx{dp->document.get(), dp->jsCtx, renderer_.get(), window_.get(),
                            &uiDirty_, &overlayMgr_, OverlayContext::App, dp->boxW, dp->boxH};
        dispatchDocMousePress(cctx, dp->mouseState, sub, evt, lx, ly);
        if (jsRuntime_) jsRuntime_->executePendingJobs();
    }
    return true;  // the point is inside the iframe box — consume it
}

bool Engine::iframeHandleMouseUp(dom::Element* frameEl, float docX, float docY,
                                 int button, float movementX, float movementY, int mod) {
    if (!frameEl || !frameEl->iframeDoc()) return false;
    auto* dp = static_cast<IframeDoc*>(frameEl->iframeDoc());
    if (!dp->document || !dp->jsCtx) return false;
    dom::AbsoluteRect box = dom::absoluteContentBox(frameEl);
    float lx = docX - box.x, ly = docY - box.y;
    dom::Element* sub = iframeHitTest(dp, lx, ly);
    if (sub) {
        dom::MouseEvent upEvt("mouseup");
        populateMouseEvent(upEvt, lx, ly, button, pressedButtons_, movementX, movementY,
                           0.0f, mod, 0.0f);
        applyMouseOffset(upEvt, sub);
        ControlContext cctx{dp->document.get(), dp->jsCtx, renderer_.get(), window_.get(),
                            &uiDirty_, &overlayMgr_, OverlayContext::App, dp->boxW, dp->boxH};
        dispatchDocMouseRelease(cctx, dp->mouseState, sub, upEvt,
                                lx, ly, button, pressedButtons_, mod,
                                movementX, movementY, lx, ly,
                                util::currentTimeMs(),
                                inputConfig_.doubleClickThresholdMs,
                                inputConfig_.doubleClickDistancePx);
        if (jsRuntime_) jsRuntime_->executePendingJobs();
    }
    return true;
}

bool Engine::iframeHandleMouseMove(dom::Element* frameEl, float docX, float docY,
                                   float movementX, float movementY, int mod) {
    if (!frameEl || !frameEl->iframeDoc()) return false;
    auto* dp = static_cast<IframeDoc*>(frameEl->iframeDoc());
    if (!dp->document || !dp->jsCtx) return false;
    dom::AbsoluteRect box = dom::absoluteContentBox(frameEl);
    float lx = docX - box.x, ly = docY - box.y;
    dom::Element* sub = iframeHitTest(dp, lx, ly);

    // :hover restyle — mark the old and new targets dirty so the sub-doc's next
    // resolveStyles re-resolves the pseudo-class change. recordIframeLayers points
    // ElementRefAdapter at dp->hoveredElement before resolving the sub-doc.
    if (sub != dp->hoveredElement) {
        if (dp->hoveredElement) dp->hoveredElement->markDirty();
        if (sub) sub->markDirty();
        dp->hoveredElement = sub;
        uiDirty_ = true;
    }
    if (sub) {
        dom::MouseEvent moveEvt("mousemove");
        populateMouseEvent(moveEvt, lx, ly, 0, pressedButtons_, movementX, movementY,
                           0.0f, mod, 0.0f);
        applyMouseOffset(moveEvt, sub);
        js::dispatchDomEvent(dp->jsCtx, sub, moveEvt);
        if (jsRuntime_) jsRuntime_->executePendingJobs();
    }
    return true;
}

void Engine::handleMouseDown(float x, float y, int button) {
    // x, y = raw mouse position (window space).
    // Coordinate spaces, and the single boundary between them:
    //   window space  — raw SDL input; system panels and System-context
    //                   overlays live here.
    //   content space — window minus the engine-reserved inset
    //                   (y − contentTop()). The app layer surfaces, control
    //                   anchors (lastDrawPos_), and App-context overlays all
    //                   live here; the compositor adds the inset back exactly
    //                   once when placing app layers.
    //   document space — content plus scroll (for hit testing into the
    //                   scrolled document).
    // Translate once, at the boundary a consumer lives behind: overlayMouseY()
    // for the overlay manager, docX/docY for DOM hit tests. Never mix spaces.
    float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;
    uiDirty_ = true;

    // Keep the cursor-position bookkeeping current regardless of which branch
    // below consumes the event (several return early). Real SDL relative-mouse
    // input doesn't need this (movementX/Y come straight from the OS), but
    // headless's mouseMove(x, y) self-computes its delta as (x - lastMouseX_)
    // — only handleMouseMove used to maintain that pair, so any mousedown/up
    // with no intervening real mousemove left it stale (sometimes as far back
    // as (0, 0)), and the next simulated drag's first step jumped from that
    // stale baseline instead of the actual last cursor position. Capture the
    // pre-update position for this event's own movementX/Y below.
    const float prevMouseX = lastMouseX_, prevMouseY = lastMouseY_;
    lastMouseX_ = x;
    lastMouseY_ = y;

    // Convert SDL button id to DOM convention up front so every downstream
    // event sees the standard 0=left/1=middle/2=right indexing.
    button = sdlToDomButton(button);

    // Overlay manager sees every input event before the DOM so hover/click
    // can't leak through to elements underneath. (Same pattern in the other
    // handleMouse*/handleKey*/handleTextInput methods.) App-context overlays
    // are anchored in content space — overlayMouseY() translates once here.
    if (overlayMgr_.handleMouseDown(x, overlayMouseY(y), button)) {
        pressedButtons_ |= domButtonMask(button);
        // App-context overlays (dropdown popups) are base-only chrome.
        markAppBaseDirty();
        return;
    }

    // Forward to system overlay first — if it consumes, skip app handling
    if (systemHandleMouseDown(x, y, button)) {
        pressedButtons_ |= domButtonMask(button);
        return;
    }

    // Inspector picker mode: a click in the app viewport selects the hovered
    // element instead of dispatching to the app. Only the primary button picks;
    // other buttons fall through (so right-click context menus etc. still work
    // even when picker is on, though that's an unlikely combo).
    if (inspector_.pickerMode && inspector_.visible && button == 0) {
        dom::Element* hit = inspector_.pickerHover ? inspector_.pickerHover : hitTest(docX, docY);
        if (hit) inspectorPickElement(hit);
        inspectorSetPickerMode(false);
        // Notify the panel UI so it refreshes (selection + button state).
        for (auto& doc : systemDocs_) {
            if (doc.name != "inspector" || !doc.jsCtx) continue;
            JSValue global = JS_GetGlobalObject(doc.jsCtx);
            JSValue fn = JS_GetPropertyStr(doc.jsCtx, global, "__onInspectorChanged");
            if (JS_IsFunction(doc.jsCtx, fn)) {
                JSValue r = JS_Call(doc.jsCtx, fn, global, 0, nullptr);
                JS_FreeValue(doc.jsCtx, r);
            }
            JS_FreeValue(doc.jsCtx, fn);
            JS_FreeValue(doc.jsCtx, global);
        }
        pressedButtons_ |= domButtonMask(button);
        return;
    }

    // Engine-level 3D gizmo — sits between modal UI and DOM. Consumes only
    // when a handle is hit; otherwise falls through to DOM / canvas.
#if BRO_WITH_3D
    if (gizmoHandleMouseDown(docX, docY, button)) {
        pressedButtons_ |= domButtonMask(button);
        return;
    }
#endif

    // Update button bitmask (DOM convention: 1=left, 2=right, 4=middle, ...)
    pressedButtons_ |= domButtonMask(button);

    // "mouse:<button>" action bindings fire once the press has cleared the
    // engine consumers above (overlays, system panels, inspector, gizmo) —
    // the same rule as keyboard actions, which never fire for consumed
    // keydowns. The matching "up" is guaranteed by actionMouseDownMask_ in
    // handleMouseUp regardless of what consumes the release.
    dispatchMouseButtonAction(button, true);

    // --- Scrollbar interaction (before DOM hit testing) ---

    // Check viewport scrollbar (sits in the content area, below the menu bar)
    {
        float ct = static_cast<float>(contentTop());
        float vh = static_cast<float>(contentHeight());
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(viewportWidth_) - vs.width - vs.margin,
            ct, vh, documentHeight_, vh, scrollY_);
        if (viewportScrollbar_.hitTest(x, y, m)) {
            if (viewportScrollbar_.thumbHitTest(x, y, m)) {
                viewportScrollbar_.beginDrag(y, m);
                draggingViewportScrollbar_ = true;
            } else {
                // Click on track — page scroll
                scrollY_ = viewportScrollbar_.scrollToPosition(y,
                    documentHeight_, vh, m);
            }
            uiDirty_ = true;
            return; // consumed
        }
    }

    // Check element scrollbars. The app doc's scrollbar geometry (element
    // boxes offset by (0, -scrollY)) is content space, so fold the window→
    // content inset into the mouse y once here.
    if (document_ && document_->documentElement()) {
        float cy = y - static_cast<float>(contentTop());
        ScrollbarMetrics em;
        dom::Element* hitElem = findElementScrollbarHit(
            document_->documentElement(), x, cy,
            0.0f, -scrollY_, elementScrollbar_, em);
        if (hitElem) {
            if (elementScrollbar_.thumbHitTest(x, cy, em)) {
                elementScrollbar_.beginDrag(cy, em);
                scrollbarDragTarget_.assign(document_.get(), hitElem);
            } else {
                // Click on track — page scroll
                float viewH = hitElem->layoutBox().contentRect.height;
                float maxST = maxScrollTop(hitElem);
                float contentH = viewH + maxST;
                float newScroll = elementScrollbar_.scrollToPosition(cy,
                    contentH, viewH, em);
                float prev = hitElem->scrollTopValue();
                float clamped = std::clamp(newScroll, 0.0f, maxST);
                hitElem->setScrollTopValue(clamped);
                if (clamped != prev) dispatchScrollEvent(hitElem);
            }
            // Element scroll offset is applied at draw time, so a re-record
            // (not a relayout) reflects the new position.
            markAppBaseDirty();
            return; // consumed
        }
    }

    if (document_) {
        // A press moves the caret or the focus — either way an in-progress
        // IME composition commits first (browser behavior on caret
        // move/blur), so the preedit is finalized before the press re-seats
        // the caret or focuses another element.
        commitActiveComposition();

        dom::MouseEvent evt("mousedown");
        int mod = safeGetModState(window_.get(), heldModifierMask_);
        populateMouseEvent(evt, x, y, button, pressedButtons_,
                          x - prevMouseX, y - prevMouseY, scrollY_, mod, static_cast<float>(contentTop()));

        dom::Element* target = hitTest(docX, docY);
        // A press on an <iframe> is routed into its sub-document, not treated as
        // a click on the frame element itself.
        if (target && target->iframeDoc() &&
            iframeHandleMouseDown(target, docX, docY, button,
                                  x - prevMouseX, y - prevMouseY, mod)) {
            markAppBaseDirty();
            return;
        }
        if (target) applyMouseOffset(evt, target);

        // World-space HtmlNode hit test: if the click landed on a canvas
        // that owns a SceneGraph, ray-cast into the scene's HtmlNode
        // billboards. A hit consumes the event — the canvas itself does
        // not see mousedown in that case (parallels how a regular DOM
        // child element captures clicks before its parent).
#if BRO_WITH_3D
        scene::HtmlNode* hnHit = nullptr;
        dom::Element* hnEl = nullptr;
        float hnPxX = 0.0f, hnPxY = 0.0f;
        if (target && pickHtmlNodeUnderMouse(target, docX, docY,
                                              hnHit, hnEl, hnPxX, hnPxY)) {
            htmlNodeMouseDownNode_ = hnHit;
            htmlNodeMouseDownElement_.assign(hnHit->document(), hnEl);
            dispatchHtmlNodeMouseEvent("mousedown", hnEl, hnPxX, hnPxY,
                                        button, pressedButtons_, mod,
                                        x - prevMouseX, y - prevMouseY,
                                        /*bubbles=*/true);
            return;
        }
        htmlNodeMouseDownNode_ = nullptr;
        htmlNodeMouseDownElement_.reset();
#endif  // BRO_WITH_3D

        // A press over a draggable element arms a drag; the move that follows
        // decides whether it becomes one.
        if (button == 0) dragDrop_.arm(target, x, y);

        // App controls anchor and open overlays in content space, so the
        // ControlContext viewport is the content area and the focus point is
        // content-space (matches lastDrawPos_ comparisons in focusNewControl).
        ControlContext cctx{document_.get(), jsRuntime_->getContext(),
                           renderer_.get(), window_.get(), &uiDirty_,
                           &overlayMgr_, OverlayContext::App,
                           contentWidth(), contentHeight()};

        // What this press means for a text control's selection: place the caret
        // (single), take a word (double), take everything (triple), or extend
        // the existing selection from its anchor (shift). A left press on a text
        // control also arms drag-selection; the control keeps the anchor.
        const float focusX = x, focusY = y - static_cast<float>(contentTop());
        PressIntent intent;
        // Content space, the space the release path records its streak in — a
        // window-space y here would read contentTop() px off every press and
        // never match, so no press would ever count as a double.
        intent.ordinal = pressOrdinal(appMouseState_, target, focusX, focusY,
                                      util::currentTimeMs(),
                                      inputConfig_.doubleClickThresholdMs,
                                      inputConfig_.doubleClickDistancePx);
        intent.extend = (mod & SDL_KMOD_SHIFT) != 0;

        controlDragElement_.reset();
        if (button == 0 && isCaretControl(target)) {
            controlDragElement_.assign(document_.get(), target);
            controlDragIsPanel_ = false;
        }

        // pointerdown fires just before mousedown (web platform order).
        dispatchPointerAlias("pointerdown", target, evt);
        dispatchDocMousePress(cctx, appMouseState_, target, evt,
                              focusX, focusY, intent);
        jsRuntime_->executePendingJobs();
        // Track the (possibly re-seated) caret for the IME candidate window.
        updateTextInputArea();
        // A control press can reposition the native caret or toggle control
        // visual state without changing the DOM (e.g. clicking to move the
        // caret inside an already-focused input, where setActiveElement's
        // same-element early-out skips markDirty). That chrome lives in the
        // cached base, so force a re-record.
        markAppBaseDirty();

        // Mouse-driven text selection. Left button only; bail out if the
        // click landed on a text-editing control (input/textarea manage
        // their own caret via ElInput), a button-like control, or any
        // subtree with computed `user-select: none` (typical for app UI
        // chrome — sliders, scene canvas wrappers, etc. — where dragging
        // should pan/scrub rather than mark text).
        if (button == 0 && document_ && textMetrics_) {
            bool isEditableControl = false;
            if (target) {
                const std::string& tag = target->tagName();
                if (tag == "INPUT" || tag == "TEXTAREA" || tag == "SELECT" ||
                    tag == "BUTTON" || tag == "OPTION") {
                    isEditableControl = true;
                }
            }
            bool suppressed = target && isSelectionSuppressed(target);
            if (!isEditableControl && !suppressed) {
                // Scope the search to the editing host when the press landed
                // in one. A press that misses every run still resolves to the
                // NEAREST run, and unscoped that is measured across the whole
                // document — so clicking the blank right-hand part of a wide
                // contenteditable put the caret in whatever text was closest,
                // usually the line below it. The caret then sat outside the
                // host, invisible, and typing went nowhere.
                auto* editHost = editableHostOf(target);
                auto hit = layout::hitTestText(document_.get(), docX, docY,
                                               *textMetrics_, editHost);
                auto* sel = document_->selection();
                // Validate the hit textnode is still owned by the document.
                // Hit-testing can surface layout-cached pointers into detached
                // subtrees; binding a Range to one guarantees a dangling
                // endpoint the instant that subtree is freed.
                if (hit.textNode && document_->ownsNode(hit.textNode)) {
                    // The ordinal of *this* press. clickCount only advances on
                    // release, so reading it directly here lags by one and made
                    // double-click word-select fire on the third press.
                    int detail = intent.ordinal;
                    if (detail >= 3) {
                        // Triple-click: select the entire text node.
                        sel->setRange(hit.textNode, 0,
                                      hit.textNode,
                                      static_cast<int>(hit.textNode->length()),
                                      dom::Selection::Forward);
                        selectionDragging_ = false;
                    } else if (detail == 2) {
                        // Double-click: expand to word boundaries in the source
                        // string around the hit offset.
                        const std::string& s = hit.textNode->data();
                        int off = std::max(0, std::min(hit.srcOffset,
                            static_cast<int>(s.size())));
                        auto isWordChar = [](unsigned char c) {
                            return std::isalnum(c) || c == '_';
                        };
                        int lo = off;
                        while (lo > 0 && isWordChar(
                            static_cast<unsigned char>(s[lo - 1]))) lo--;
                        int hi = off;
                        while (hi < static_cast<int>(s.size()) &&
                               isWordChar(static_cast<unsigned char>(s[hi]))) hi++;
                        sel->setRange(hit.textNode, lo, hit.textNode, hi,
                                      dom::Selection::Forward);
                        selectionDragging_ = false;
                    } else {
                        sel->collapse(hit.textNode, hit.srcOffset);
                        selectionAnchorNode_.assign(document_.get(), hit.textNode);
                        selectionAnchorOffset_ = hit.srcOffset;
                        selectionDragging_ = true;
                        selectionPressX_ = docX;
                        selectionPressY_ = docY;
                        selectionPastThreshold_ = false;
                    }
                    // Selection highlight + caret are base-only chrome; force a
                    // re-record (no relayout) so the new selection paints.
                    markAppBaseDirty();
                } else if (target && document_->ownsNode(target) &&
                           inEditableHost(target)) {
                    // No text was hit, but the press landed inside an editable
                    // host — an empty contenteditable has no text node to hit
                    // at all, and a press in the padding below a host's text
                    // misses every run. Either way a browser gives you a
                    // caret; leaving rangeCount() at 0 means the next
                    // keystroke has nowhere to go and typing silently does
                    // nothing until script calls collapse().
                    //
                    // Element position, since there is no text node: the end
                    // of the pressed element's children, which is the start
                    // for an empty host and "after the content" for a press
                    // past it — what clicking below a paragraph does.
                    //
                    // No drag is armed: the drag anchor is a text-node handle
                    // and this position has no text node behind it. A press
                    // that misses every run has nothing to sweep a selection
                    // across anyway.
                    const int idx = static_cast<int>(target->childNodes().size());
                    sel->collapse(target, idx);
                    selectionDragging_ = false;
                    selectionAnchorNode_.reset();
                    markAppBaseDirty();
                } else {
                    // Click outside any text: clear selection.
                    sel->removeAllRanges();
                    selectionDragging_ = false;
                    selectionAnchorNode_.reset();
                    markAppBaseDirty();
                }
            }
        }
    }
}

void Engine::handleMouseUp(float x, float y, int button) {
    // x, y = window space. docX, docY = document space (see the coordinate-
    // space note in handleMouseDown).
    float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;
    uiDirty_ = true;

    // See the matching comment in handleMouseDown: keep this pair current so
    // headless's self-computed mouseMove delta always measures from the real
    // last cursor position, not a stale one left over from before a click.
    // Capture the pre-update position for this event's own movementX/Y below.
    const float prevMouseX = lastMouseX_, prevMouseY = lastMouseY_;
    lastMouseX_ = x;
    lastMouseY_ = y;

    // Match handleMouseDown: SDL -> DOM button index.
    button = sdlToDomButton(button);

    // Update button bitmask (DOM convention)
    pressedButtons_ &= ~domButtonMask(button);

    // Balance the "mouse:<button>" action pair before any consumer can eat
    // the release: fires only if this button dispatched an action "down".
    dispatchMouseButtonAction(button, false);

    if (overlayMgr_.handleMouseUp(x, overlayMouseY(y), button)) {
        markAppBaseDirty();
        return;
    }

    // End scrollbar drags FIRST, before any early-return consumer — the
    // mouseup that ends a drag must always terminate the drag regardless of
    // where the pointer happens to be, otherwise the thumb stays glued to
    // the cursor forever.
    if (viewportScrollbar_.isDragging()) {
        viewportScrollbar_.endDrag();
        draggingViewportScrollbar_ = false;
        uiDirty_ = true;
    }
    if (elementScrollbar_.isDragging()) {
        elementScrollbar_.endDrag();
        scrollbarDragTarget_.reset();
        if (scrollbarDragSystemDoc_) {
            systemDirty_ = true;
            scrollbarDragSystemDoc_ = nullptr;
        }
        uiDirty_ = true;
        // The mouseup that ends a scrollbar drag is purely for drag
        // termination — it shouldn't also be delivered to DOM listeners
        // underneath (where it would look like a random click).
        return;
    }

    // Forward to system overlay first
    if (systemHandleMouseUp(x, y, button)) {
        return;
    }

    // Gizmo consumes mouseUp only when the drag was active.
#if BRO_WITH_3D
    if (gizmoHandleMouseUp(docX, docY, button)) {
        return;
    }
#endif

    // Stop range slider dragging
    if (document_) {
        auto* activeEl = document_->activeElement();
        auto* input = getElInput(activeEl);
        if (input && input->isDragging()) {
            input->setDragging(false);
            dom::Event changeEvt("change");
            dispatchEvent(activeEl, changeEvt);
            uiDirty_ = true;
        }
    }

    if (button == 0) {
        // Terminate any in-progress selection drag regardless of where the
        // pointer released — next mousedown starts fresh. This covers both the
        // document's Selection and a drag inside a text control.
        selectionDragging_ = false;
        controlDragElement_.reset();
    }

    if (document_) {
        dom::Element* target = hitTest(docX, docY);
        int mod = safeGetModState(window_.get(), heldModifierMask_);
        float ct = static_cast<float>(contentTop());
        float movX = x - prevMouseX;
        float movY = y - prevMouseY;
        float clientY = y - ct;
        float pageY = clientY + scrollY_;

        // Release over an <iframe> routes into its sub-document (mouseup + click).
        if (target && target->iframeDoc() &&
            iframeHandleMouseUp(target, docX, docY, button, movX, movY, mod)) {
            markAppBaseDirty();
            return;
        }

        // Mirror the mousedown HtmlNode routing on release: if the press
        // landed on a HtmlNode and the release ray-casts to the same node,
        // dispatch mouseup + click; otherwise dispatch mouseup only on the
        // press target so handlers see a balanced down/up pair.
#if BRO_WITH_3D
        scene::HtmlNode* hnHit = nullptr;
        dom::Element* hnEl = nullptr;
        float hnPxX = 0.0f, hnPxY = 0.0f;
        bool hnReleaseHit = (target && pickHtmlNodeUnderMouse(target, docX, docY,
                                                                hnHit, hnEl, hnPxX, hnPxY));
        if (htmlNodeMouseDownNode_) {
            dom::Element* downEl = htmlNodeMouseDownElement_.get();
            if (hnReleaseHit && hnHit == htmlNodeMouseDownNode_) {
                dispatchHtmlNodeMouseEvent("mouseup", hnEl, hnPxX, hnPxY,
                                            button, pressedButtons_, mod,
                                            movX, movY, /*bubbles=*/true);
                if (button == 0 && hnEl == downEl) {
                    dispatchHtmlNodeMouseEvent("click", hnEl, hnPxX, hnPxY,
                                                button, pressedButtons_, mod,
                                                movX, movY, /*bubbles=*/true);
                }
            } else {
                dispatchHtmlNodeMouseEvent("mouseup", downEl,
                                            hnPxX, hnPxY,
                                            button, pressedButtons_, mod,
                                            movX, movY, /*bubbles=*/true);
            }
            htmlNodeMouseDownNode_ = nullptr;
            htmlNodeMouseDownElement_.reset();
            return;
        }
#endif  // BRO_WITH_3D

        dom::MouseEvent upEvt("mouseup");
        populateMouseEvent(upEvt, x, y, button, pressedButtons_,
                          movX, movY, scrollY_, mod, ct);
        if (target) applyMouseOffset(upEvt, target);

        ControlContext cctx{document_.get(), jsRuntime_->getContext(),
                           renderer_.get(), window_.get(), &uiDirty_,
                           &overlayMgr_, OverlayContext::App,
                           contentWidth(), contentHeight()};
        // A drag ends here: `drop` on whatever accepted it, then `dragend`.
        // A gesture that was a drag is not also a click, so the release is not
        // put through the click machinery at all.
        if (jsRuntime_ &&
            dragDrop_.finish(jsRuntime_->getContext(), target, x, y)) {
            jsRuntime_->executePendingJobs();
            markAppBaseDirty();
            return;
        }

        // pointerup fires just before mouseup (web platform order).
        dispatchPointerAlias("pointerup", target, upEvt);
        dispatchDocMouseRelease(cctx, appMouseState_, target, upEvt,
                                x, clientY, button, pressedButtons_, mod,
                                movX, movY, x, pageY,
                                util::currentTimeMs(),
                                inputConfig_.doubleClickThresholdMs,
                                inputConfig_.doubleClickDistancePx);
        if (jsRuntime_) jsRuntime_->executePendingJobs();
    }
}

void Engine::handleMouseMove(float x, float y, float xrel, float yrel) {
    // If the locked element was freed, release the lock. The handle resolves
    // to null once the node is destroyed, replacing the old isAlive() magic-
    // number canary (which had to read possibly-freed memory to answer).
    if (lockedElement_.held() && !lockedElement_.get()) exitPointerLock();

    // Pointer lock: OS cursor is pinned by SDL's relative mouse mode. SDL still
    // accumulates a virtual x/y in motion events, but we ignore it — clientX/Y
    // stays frozen at the lock position and movementX/Y carries the delta.
    if (dom::Element* locked = lockedElement_.get()) {
        if (document_) {
            int mod = safeGetModState(window_.get(), heldModifierMask_);
            dom::MouseEvent moveEvt("mousemove", true, true);
            populateMouseEvent(moveEvt, lockedMouseX_, lockedMouseY_, -1,
                               pressedButtons_, xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
            applyMouseOffset(moveEvt, locked);
            dispatchPointerAlias("pointermove", locked, moveEvt);
            dispatchEvent(locked, moveEvt);
            if (jsRuntime_) jsRuntime_->executePendingJobs();
        }
        // Real SDL relative-mouse-mode input ignores lastMouseX_/Y_ entirely
        // (xrel/yrel come straight from the OS). Headless's mouseMove(x, y)
        // has no real device delta, so it self-computes xrel/yrel as
        // (x - lastMouseX_) — that self-computation must keep tracking the
        // caller's last absolute position even while locked, or every
        // simulated move after lock-engage measures its delta against the
        // stale pre-lock position instead of the previous call.
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    // x, y = window space. docX, docY = document space (see the coordinate-
    // space note in handleMouseDown).
    float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;

    // Overlay manager sees mousemove first. While an overlay is active,
    // DOM hover is suppressed entirely — otherwise hovering elements under
    // the dropdown/picker would trigger :hover styles and JS handlers.
    bool overlayActive = overlayMgr_.hasActive();
    if (overlayActive && overlayMgr_.handleMouseMove(x, overlayMouseY(y))) {
        // Dropdown option highlight follows the pointer; it's base-only chrome.
        markAppBaseDirty();
    }

    // Drag-selection inside a text control: extend the control's own selection
    // to the pointer. The control kept the anchor from the press, so this only
    // has to move the caret end. Coordinates go in the control's draw space —
    // content space for the app document, window space for a system panel.
    //
    // This runs before the system-panel forward below: a press in a panel's text
    // field keeps the pointer inside that panel, and systemHandleMouseMove
    // consumes every move it hits, so a drag routed through it would never reach
    // the control. A drag holds the pointer the way a scrollbar drag does.
    if (auto* dragEl = controlDragElement_.get()) {
        float cx = x;
        float cy = controlDragIsPanel_ ? y : y - static_cast<float>(contentTop());
        if (auto* input = getElInput(dragEl)) {
            input->caretToPoint(cx, cy, /*extend=*/true);
        } else if (auto* ta = getElTextarea(dragEl)) {
            ta->caretToPoint(cx, cy, /*extend=*/true);
        }
        // Selection chrome lives in the cached base layer and no DOM changed,
        // so a re-record (not a relayout) is what makes the new range paint.
        if (controlDragIsPanel_) systemDirty_ = true;
        else markAppBaseDirty();

        // The page still has to see the drag. A browser extends the control's
        // selection AND keeps dispatching mousemove, which is what every
        // drag-to-scrub number field is built on — the three.js editor's
        // position/rotation/scale fields, lil-gui, dat.GUI: mousedown on an
        // <input>, then read document mousemove until mouseup. Consuming the
        // move outright left them with a press and a release and nothing in
        // between, so dragging a value did nothing at all.
        //
        // Only the app document gets this; a drag in a system panel's text
        // field is engine chrome with no page to notify. Hover bookkeeping
        // (mouseover/mouseout, :hover) stays frozen for the length of the
        // drag — the pointer belongs to the control until it is released.
        if (!controlDragIsPanel_ && document_) {
            if (dom::Element* target = hitTest(docX, docY)) {
                int mod = safeGetModState(window_.get(), heldModifierMask_);
                dom::MouseEvent moveEvt("mousemove", true, true);
                populateMouseEvent(moveEvt, x, y, -1, pressedButtons_,
                                   xrel, yrel, scrollY_, mod,
                                   static_cast<float>(contentTop()));
                applyMouseOffset(moveEvt, target);
                dispatchPointerAlias("pointermove", target, moveEvt);
                dispatchEvent(target, moveEvt);
                if (jsRuntime_) jsRuntime_->executePendingJobs();
            }
        }
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    // Forward to system overlay first. When the pointer is inside a visible
    // system panel (menu bar, modal card, modal backdrop), systemHandleMouseMove
    // returns true — consume the event so it doesn't bleed through to the app
    // behind the modal. When it returns false the pointer is outside any
    // system panel and the app handles the move normally. Exception: an
    // in-progress scrollbar drag must keep updating even when the pointer
    // strays outside the panel, so fall through in that case.
    if (isSystemVisible() && !elementScrollbar_.isDragging() &&
        systemHandleMouseMove(x, y)) {
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    // Inspector picker mode: hit-test the app document, update pickerHover so
    // the box-model overlay redraws on the new element, then suppress the rest
    // of the move (no app hover/JS dispatch while picking).
    if (inspector_.pickerMode && inspector_.visible) {
        dom::Element* hit = hitTest(docX, docY);
        if (hit != inspector_.pickerHover) {
            inspector_.pickerHover = hit;
            // Inspector box-model highlight is base-only chrome.
            markAppBaseDirty();
        }
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    // Gizmo mousemove — drives hover state always; consumes only while
    // actively dragging (returns true in that case).
#if BRO_WITH_3D
    if (gizmoHandleMouseMove(docX, docY)) {
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }
#endif

    // Mouse-driven text selection: while dragging, extend the selection's
    // focus to follow the pointer. The anchor is whatever was captured on
    // mousedown (selectionAnchor*).
    dom::TextNode* selAnchor = selectionAnchorNode_.get();
    if (selectionDragging_ && document_ && textMetrics_ && selAnchor) {
        // Require the pointer to travel past a small threshold before we begin
        // extending the selection. Without this, sub-pixel jitter during a
        // plain click registers as a drag and snaps the focus to whatever
        // text run hit-testing picks nearest — which inside a tiled/grid
        // layout is often a far-away cell, creating an apparent "selects the
        // whole page" bug from the user's perspective.
        if (!selectionPastThreshold_) {
            const float kThreshold = 4.0f;
            float dx = docX - selectionPressX_;
            float dy = docY - selectionPressY_;
            if (dx*dx + dy*dy < kThreshold * kThreshold) {
                // Still a potential click; leave the caret collapsed at anchor.
            } else {
                selectionPastThreshold_ = true;
            }
        }
        // Same scoping as the press: a drag that began inside an editing host
        // extends within it. Unscoped, dragging into the blank area beside the
        // text jumps the focus to the nearest unrelated run and the selection
        // swallows content the user never swept across.
        auto* dragHost = editableHostOf(selAnchor);
        auto hit = selectionPastThreshold_
            ? layout::hitTestText(document_.get(), docX, docY, *textMetrics_,
                                  dragHost)
            : layout::TextHit{};
        // Drop the drag if either endpoint's textnode is no longer live.
        // selectionAnchorNode_ was captured on mousedown and can be freed
        // by app code mid-drag (e.g. HUD rebuilds) even if the hit is fresh.
        if (hit.textNode && document_->ownsNode(hit.textNode) &&
            document_->ownsNode(selAnchor)) {
            auto* sel = document_->selection();
            // Compute direction: if focus is before anchor, backward. Use
            // comparePoint against a range collapsed at the anchor — probing
            // with setEnd doesn't work because Range::normalize clamps
            // reversed endpoints instead of swapping them.
            dom::Range probe;
            probe.setStart(selAnchor, selectionAnchorOffset_);
            bool backward = probe.comparePoint(hit.textNode, hit.srcOffset) < 0;
            if (backward) {
                sel->setRange(hit.textNode, hit.srcOffset,
                              selAnchor, selectionAnchorOffset_,
                              dom::Selection::Backward);
            } else {
                sel->setRange(selAnchor, selectionAnchorOffset_,
                              hit.textNode, hit.srcOffset,
                              dom::Selection::Forward);
            }
            // Re-record the base-only selection chrome (no relayout needed).
            markAppBaseDirty();
        }
    }

    // Viewport scrollbar drag
    if (viewportScrollbar_.isDragging()) {
        float ct = static_cast<float>(contentTop());
        float vh = static_cast<float>(contentHeight());
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(viewportWidth_) - vs.width - vs.margin,
            ct, vh, documentHeight_, vh, scrollY_);
        float maxScroll = std::max(0.0f, documentHeight_ - vh);
        scrollY_ = std::clamp(
            viewportScrollbar_.updateDrag(y, documentHeight_, vh, m),
            0.0f, maxScroll);
        uiDirty_ = true;
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    // Element scrollbar drag — works the same whether the target lives in
    // the app doc or a system panel; only the dirty-bit bookkeeping and
    // scroll event dispatch differ.
    if (elementScrollbar_.isDragging() && scrollbarDragTarget_) {
        auto* elem = scrollbarDragTarget_.get();
        float viewH = elem->layoutBox().contentRect.height;
        float maxST = maxScrollTop(elem);
        float contentH = viewH + maxST;

        auto& lbox = elem->layoutBox();
        float bh = lbox.fullHeight();
        auto m = elementScrollbar_.layout(0, 0, bh, contentH, viewH,
            elem->scrollTopValue());
        // beginDrag captured the mouse in the space the target's scrollbar
        // geometry lives in — content space for the app doc, window space
        // for system panels. Feed updateDrag the same space so the drag
        // delta stays exact.
        float dragY = scrollbarDragSystemDoc_
            ? y : y - static_cast<float>(contentTop());
        float newScroll = elementScrollbar_.updateDrag(dragY, contentH, viewH, m);
        float prev = elem->scrollTopValue();
        float clamped = std::clamp(newScroll, 0.0f, maxST);
        elem->setScrollTopValue(clamped);
        if (clamped != prev) {
            if (scrollbarDragSystemDoc_) {
                if (scrollbarDragSystemDoc_->document)
                    scrollbarDragSystemDoc_->document->markDirty();
                systemDirty_ = true;
            } else {
                dispatchScrollEvent(elem);
                // App element scroll: re-record the base to show the new offset.
                markAppBaseDirty();
            }
        }
        uiDirty_ = true;
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    // Viewport scrollbar hover
    {
        float ct = static_cast<float>(contentTop());
        float vh = static_cast<float>(contentHeight());
        auto& vs = viewportScrollbar_.style();
        auto m = viewportScrollbar_.layout(
            static_cast<float>(viewportWidth_) - vs.width - vs.margin,
            ct, vh, documentHeight_, vh, scrollY_);
        bool wasHovered = viewportScrollbar_.isHovered();
        viewportScrollbar_.setHovered(viewportScrollbar_.thumbHitTest(x, y, m));
        if (wasHovered != viewportScrollbar_.isHovered()) uiDirty_ = true;
    }

    // Element scrollbar hover (per-element tracking). Content-space geometry —
    // fold the inset into the mouse y once (same as the mousedown hit test).
    if (document_ && document_->documentElement()) {
        float cyEl = y - static_cast<float>(contentTop());
        ScrollbarMetrics em;
        dom::Element* hitElem = findElementScrollbarHit(
            document_->documentElement(), x, cyEl,
            0.0f, -scrollY_, elementScrollbar_, em);
        dom::Element* prevHovered = scrollbarHoveredElement_.get();
        if (hitElem && elementScrollbar_.thumbHitTest(x, cyEl, em)) {
            scrollbarHoveredElement_.assign(document_.get(), hitElem);
        } else {
            scrollbarHoveredElement_.reset();
        }
        if (prevHovered != scrollbarHoveredElement_.get()) uiDirty_ = true;
    }

    // Range slider dragging
    if (document_) {
        auto* activeEl = document_->activeElement();
        auto* rangeInput = getElInput(activeEl);
        if (rangeInput && rangeInput->isDragging()) {
            // lastDrawPos_ is content space. Only x/w are compared here, and
            // content x == window x (the engine reserves no left inset), so
            // the raw mouse x is already in the right space.
            auto dp = rangeInput->lastDrawPos();
            float thumbR = layout::ElInput::rangeThumbRadius(dp.h);
            float trackStart = dp.x + thumbR;
            float trackEnd = dp.x + dp.w - thumbR;
            float pct = (trackEnd > trackStart) ?
                std::clamp((x - trackStart) / (trackEnd - trackStart), 0.0f, 1.0f) : 0.0f;
            float mn = rangeInput->rangeMin(), mx = rangeInput->rangeMax();
            float val = mn + pct * (mx - mn);
            float step = rangeInput->rangeStep();
            if (step > 0) {
                val = mn + std::round((val - mn) / step) * step;
                val = std::clamp(val, mn, mx);
            }
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", static_cast<double>(val));
            activeEl->setAttribute("value", buf);
            dispatchInputEvent(activeEl);
            uiDirty_ = true;
        }
    }

    // Dispatch mousemove event (suppressed while any overlay is active)
    if (document_ && !overlayActive) {
        dom::Element* target = hitTest(docX, docY);

        // Over an <iframe>: route the move into its sub-document (sub-doc :hover
        // + mousemove). Host-side hover bookkeeping below still runs so the frame
        // element gets enter/leave and the sub-doc hover is cleared on exit.
        if (target && target->iframeDoc()) {
            iframeHandleMouseMove(target, docX, docY, xrel, yrel,
                                  safeGetModState(window_.get(), heldModifierMask_));
        }

        // Dispatch mouseover/mouseout when element changes (bubbling versions)
        dom::Element* prevHover = hoveredElement_.get();
        if (target != prevHover) {
            // Leaving an <iframe>: drop the sub-document's :hover so it doesn't
            // stay stuck highlighted after the pointer exits the frame.
            if (prevHover && prevHover->iframeDoc()) {
                auto* pdp = static_cast<IframeDoc*>(prevHover->iframeDoc());
                if (pdp->hoveredElement) {
                    pdp->hoveredElement->markDirty();
                    pdp->hoveredElement = nullptr;
                    uiDirty_ = true;
                }
            }
            int mod = safeGetModState(window_.get(), heldModifierMask_);

            // mouseout on previous element (bubbles)
            if (prevHover) {
                dom::MouseEvent outEvt("mouseout", true, true);
                populateMouseEvent(outEvt, x, y, -1, pressedButtons_,
                                  xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
                outEvt.setRelatedTarget(target);
                applyMouseOffset(outEvt, prevHover);
                dispatchEvent(prevHover, outEvt);
            }

            // mouseleave on previous element (doesn't bubble)
            if (prevHover) {
                dom::MouseEvent leaveEvt("mouseleave", false, false);
                populateMouseEvent(leaveEvt, x, y, -1, pressedButtons_,
                                  xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
                leaveEvt.setRelatedTarget(target);
                applyMouseOffset(leaveEvt, prevHover);
                dispatchEvent(prevHover, leaveEvt);
            }

            // mouseover on new element (bubbles)
            if (target) {
                dom::MouseEvent overEvt("mouseover", true, true);
                populateMouseEvent(overEvt, x, y, -1, pressedButtons_,
                                  xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
                overEvt.setRelatedTarget(prevHover);
                applyMouseOffset(overEvt, target);
                dispatchEvent(target, overEvt);
            }

            // mouseenter on new element (doesn't bubble)
            if (target) {
                dom::MouseEvent enterEvt("mouseenter", false, false);
                populateMouseEvent(enterEvt, x, y, -1, pressedButtons_,
                                  xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
                enterEvt.setRelatedTarget(prevHover);
                applyMouseOffset(enterEvt, target);
                dispatchEvent(target, enterEvt);
            }

            // Mark the elements whose :hover state flipped dirty for style
            // re-resolve — but only when the page actually has :hover rules.
            // With none, a hover-target change cannot alter any computed style,
            // so dirtying + a full base re-record on every mouse move is pure
            // waste (a 4.4k-element grid with no :hover cost ~16 ms/frame).
            // :hover applies up the ancestor chain, not just the leaf, so dirty
            // the whole changed path (see markHoverChainDirty) — otherwise
            // moving onto a child (a row's text span) leaves the parent row's
            // cached style stale and it fails to highlight. The JS dispatched
            // above can free the old target; re-fetch it through the handle.
            if (document_ && document_->cascade().usesHoverPseudo()) {
                markHoverChainDirty(document_->cascade(), hoveredElement_.get(), target);
                uiDirty_ = true;
            }
            hoveredElement_.assign(document_.get(), target);
        }

        // CSS `cursor` → OS cursor. Every move, not just hover changes: a
        // restyle (:hover rules, class flips) can change the computed cursor
        // without the hit target changing. Reads through the handle — the
        // hover-change JS above may have freed the raw target. Cheap when
        // nothing changed (string compare + Window::setCursor no-op).
        updateCursorFromHover(hoveredElement_.get());

        // World-space HtmlNode hover + move routing. Tracked in parallel
        // with hoveredElement_ — the canvas remains the outer-doc hover
        // target while the inner HtmlNode element gets enter/leave/over/
        // out/move events for its own hover state and listeners.
#if BRO_WITH_3D
        scene::HtmlNode* hnNode = nullptr;
        dom::Element* hnEl = nullptr;
        float hnPxX = 0.0f, hnPxY = 0.0f;
        bool hnHit = (target && pickHtmlNodeUnderMouse(target, docX, docY,
                                                        hnNode, hnEl, hnPxX, hnPxY));
        dom::Element* prevHnEl = hoveredHtmlElement_.get();
        if (hnEl != prevHnEl) {
            int mod = safeGetModState(window_.get(), heldModifierMask_);
            if (prevHnEl) {
                dispatchHtmlNodeMouseEvent("mouseout", prevHnEl,
                                            hnPxX, hnPxY, -1, pressedButtons_,
                                            mod, xrel, yrel, /*bubbles=*/true,
                                            hnEl);
                dispatchHtmlNodeMouseEvent("mouseleave", prevHnEl,
                                            hnPxX, hnPxY, -1, pressedButtons_,
                                            mod, xrel, yrel, /*bubbles=*/false,
                                            hnEl);
                if (auto* ph = hoveredHtmlElement_.get()) ph->markDirty();
            }
            if (hnEl) {
                dispatchHtmlNodeMouseEvent("mouseover", hnEl,
                                            hnPxX, hnPxY, -1, pressedButtons_,
                                            mod, xrel, yrel, /*bubbles=*/true,
                                            prevHnEl);
                dispatchHtmlNodeMouseEvent("mouseenter", hnEl,
                                            hnPxX, hnPxY, -1, pressedButtons_,
                                            mod, xrel, yrel, /*bubbles=*/false,
                                            prevHnEl);
                hnEl->markDirty();
            }
            hoveredHtmlElement_.assign(hnNode ? hnNode->document() : nullptr, hnEl);
            hoveredHtmlNode_ = hnNode;
            uiDirty_ = true;
        }
#endif  // BRO_WITH_3D

        // Always dispatch mousemove. Route into the inner HtmlNode if the
        // pointer is currently over one — otherwise normal DOM target.
#if BRO_WITH_3D
        if (hnHit) {
            int mod = safeGetModState(window_.get(), heldModifierMask_);
            dispatchHtmlNodeMouseEvent("mousemove", hnEl, hnPxX, hnPxY,
                                        -1, pressedButtons_, mod,
                                        xrel, yrel, /*bubbles=*/true);
        } else
#endif
        if (target) {
            int mod = safeGetModState(window_.get(), heldModifierMask_);
            dom::MouseEvent moveEvt("mousemove", true, true);
            populateMouseEvent(moveEvt, x, y, -1, pressedButtons_,
                              xrel, yrel, scrollY_, mod, static_cast<float>(contentTop()));
            applyMouseOffset(moveEvt, target);
            dispatchPointerAlias("pointermove", target, moveEvt);
            dispatchEvent(target, moveEvt);
        }

        // HTML5 drag and drop: a press that has travelled far enough over a
        // draggable element becomes a drag, and from then on this move feeds
        // dragenter / dragover / dragleave instead of doing anything else.
        // mousemove still fires above — a browser suppresses it during a drag,
        // but bro has app code (camera orbit, gizmos) that reads it, and a
        // page that never marks anything draggable never starts a drag.
        if (jsRuntime_)
            dragDrop_.update(jsRuntime_->getContext(), target, x, y, pressedButtons_);

        if (jsRuntime_) jsRuntime_->executePendingJobs();
    }

    lastMouseX_ = x;
    lastMouseY_ = y;
}

// ---------------------------------------------------------------------------
// CSS cursor → OS cursor
// ---------------------------------------------------------------------------

// Collapse a computed CSS `cursor` value onto a platform CursorShape.
// Handles the fallback-list form ("url(x.png), pointer" — custom images are
// not supported, so the last keyword wins, per the spec's fallback order).
// Unknown keywords and `auto` map to Default: bro's hit test targets
// elements, not text runs, so the browser "auto = I-beam over text"
// refinement doesn't apply — text controls get their I-beam from the UA
// stylesheet instead (default_styles.h).
platform::CursorShape cursorShapeFromCss(const std::string& value) {
    using platform::CursorShape;
    // Last comma-separated entry, trimmed + lowercased.
    size_t comma = value.find_last_of(',');
    std::string v = (comma == std::string::npos) ? value : value.substr(comma + 1);
    size_t b = v.find_first_not_of(" \t\r\n");
    size_t e = v.find_last_not_of(" \t\r\n");
    v = (b == std::string::npos) ? std::string() : v.substr(b, e - b + 1);
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (v.empty() || v == "auto" || v == "default") return CursorShape::Default;
    if (v == "pointer" || v == "hand")              return CursorShape::Pointer;
    if (v == "text" || v == "vertical-text")        return CursorShape::Text;
    // SDL3 has no grab/grabbing shape; MOVE (the four-arrow pan cursor) is
    // the closest native signal for "this can be dragged".
    if (v == "move" || v == "grab" || v == "grabbing" || v == "all-scroll")
        return CursorShape::Move;
    if (v == "crosshair" || v == "cell")            return CursorShape::Crosshair;
    if (v == "wait")                                return CursorShape::Wait;
    if (v == "progress")                            return CursorShape::Progress;
    if (v == "not-allowed" || v == "no-drop")       return CursorShape::NotAllowed;
    if (v == "ew-resize" || v == "e-resize" || v == "w-resize" || v == "col-resize")
        return CursorShape::ResizeEW;
    if (v == "ns-resize" || v == "n-resize" || v == "s-resize" || v == "row-resize")
        return CursorShape::ResizeNS;
    if (v == "nesw-resize" || v == "ne-resize" || v == "sw-resize")
        return CursorShape::ResizeNESW;
    if (v == "nwse-resize" || v == "nw-resize" || v == "se-resize")
        return CursorShape::ResizeNWSE;
    if (v == "none")                                return CursorShape::None;
    return CursorShape::Default;
}

const char* cursorShapeName(platform::CursorShape s) {
    using platform::CursorShape;
    switch (s) {
        case CursorShape::Default:    return "default";
        case CursorShape::Pointer:    return "pointer";
        case CursorShape::Text:       return "text";
        case CursorShape::Move:       return "move";
        case CursorShape::Crosshair:  return "crosshair";
        case CursorShape::Wait:       return "wait";
        case CursorShape::Progress:   return "progress";
        case CursorShape::NotAllowed: return "not-allowed";
        case CursorShape::ResizeEW:   return "ew-resize";
        case CursorShape::ResizeNS:   return "ns-resize";
        case CursorShape::ResizeNESW: return "nesw-resize";
        case CursorShape::ResizeNWSE: return "nwse-resize";
        case CursorShape::None:       return "none";
        case CursorShape::Count_:     break;
    }
    return "default";
}

void Engine::updateCursorFromHover(dom::Element* target) {
    std::string css;
    if (target) {
        // `cursor` is inherited, so the resolved cascade already carries any
        // ancestor's value down to the hit-test leaf.
        const auto& cs = target->computedStyle();
        auto it = cs.find("cursor");
        if (it != cs.end()) css = it->second;
    }
    platform::CursorShape shape = cursorShapeFromCss(css);
    resolvedCursor_ = cursorShapeName(shape);
    // Windowed only (the headless hidden window never shows a cursor), and
    // never under pointer lock — relative mouse mode already hides the OS
    // cursor and restores it on unlock; fighting it would flicker.
    if (displayMode_ == DisplayMode::Windowed && window_ && !lockedElement_.get()) {
        window_->setCursor(shape);
    }
}

// ---------------------------------------------------------------------------
// Pointer lock
// ---------------------------------------------------------------------------

bool Engine::requestPointerLock(dom::Element* target) {
    // Pointer lock routes through the app document; validate the JS-supplied
    // pointer against it (soundly — no freed-memory magic-number probe).
    if (!target || !document_ || !document_->isNodeLive(target)) return false;
    if (lockedElement_.get() == target) return true;

    lockedElement_.assign(document_.get(), target);
    lockedMouseX_ = lastMouseX_;
    lockedMouseY_ = lastMouseY_;

    if (window_) {
        SDL_SetWindowRelativeMouseMode(window_->getSDLWindow(), true);
    }

    // Notify listeners on document (document.addEventListener forwards to documentElement).
    if (document_ && document_->documentElement()) {
        dom::Event evt("pointerlockchange", true, false);
        evt.setIsTrusted(true);
        dispatchEvent(document_->documentElement(), evt);
        if (jsRuntime_) jsRuntime_->executePendingJobs();
    }
    return true;
}

// Call a JS global function with a single boolean argument. Silently no-ops
// if the function isn't defined yet (engine init orders JS wiring vs.
// visibility notifications in ways callers shouldn't have to reason about).
static void callJsBoolFn(JSContext* ctx, const char* name, bool arg) {
    if (!ctx) return;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, name);
    if (JS_IsFunction(ctx, fn)) {
        JSValue a = JS_NewBool(ctx, arg);
        JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &a);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, a);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
}

void Engine::setPageVisibility(bool visible) {
    if (!jsRuntime_) return;
    callJsBoolFn(jsRuntime_->getContext(), "__bro_set_visibility", visible);
    if (jsRuntime_) jsRuntime_->executePendingJobs();
}

void Engine::setFullscreenState(bool fullscreen) {
    if (!jsRuntime_) return;
    callJsBoolFn(jsRuntime_->getContext(), "__bro_set_fullscreen", fullscreen);
    if (jsRuntime_) jsRuntime_->executePendingJobs();
}

void Engine::exitPointerLock() {
    // held(), not get(): the lock must release (and relative mouse mode end)
    // even when the locked element has already been freed.
    if (!lockedElement_.held()) return;
    lockedElement_.reset();

    if (window_) {
        // Warp back to the pre-lock cursor position before releasing
        // relative mode — per SDL3 docs, this is how you pin the cursor
        // to a specific location on exit. Without it the OS cursor
        // reappears wherever SDL last placed it during capture, which
        // looks like the cursor "jumps" when the user releases the drag.
        SDL_WarpMouseInWindow(window_->getSDLWindow(), lockedMouseX_, lockedMouseY_);
        SDL_SetWindowRelativeMouseMode(window_->getSDLWindow(), false);
    }

    if (document_ && document_->documentElement()) {
        dom::Event evt("pointerlockchange", true, false);
        evt.setIsTrusted(true);
        dispatchEvent(document_->documentElement(), evt);
        if (jsRuntime_) jsRuntime_->executePendingJobs();
    }
}

// ---------------------------------------------------------------------------
// Keyboard events
// ---------------------------------------------------------------------------

// Helper: update input value and dispatch "input" event for v-model
void Engine::dispatchInputEvent(dom::Element* el, const std::string& data,
                                const std::string& inputType, bool isComposing) {
    if (!el) return;
    dom::InputEvent evt("input");
    evt.setData(data);
    evt.setInputType(inputType);
    evt.setIsComposing(isComposing);
    evt.setIsTrusted(true);
    dispatchEvent(el, evt);
    jsRuntime_->executePendingJobs();
    uiDirty_ = true;
}

/// Apply a KeyHandleResult from a control: dispatch events, handle unfocus.
void Engine::applyKeyResult(dom::Element* el, const layout::KeyHandleResult& r) {
    if (r.dispatchChange) {
        dom::Event changeEvt("change");
        dispatchEvent(el, changeEvt);
    }
    if (r.dispatchInput) {
        dispatchInputEvent(el, r.inputData, r.inputType);
    }
    if (r.unfocus) {
        // Escape leaves the field, and leaving a field that was typed in is
        // what `change` reports — the key does not put the old value back, so
        // what stands is an edit nobody has been told about yet.
        if (takeValueChange(el)) {
            dom::Event changeEvt("change");
            changeEvt.setIsTrusted(true);
            dispatchEvent(el, changeEvt);
        }
        dispatchFocusEvents(el, nullptr);
        // Keep the document's active element in sync with the control's focus
        // flag. Without this the field stays activeElement while its control
        // reports unfocused, so handleTextInput drops every keystroke and the
        // field appears dead until the user clicks elsewhere.
        if (document_) document_->setActiveElement(nullptr);
        safeStopTextInput(window_.get());
    }
    if (r.handled) {
        // The control's caret/selection and (for value edits) its text are
        // base-only chrome drawn from the control's state at record time. A
        // caret move (arrow keys, Home/End) changes no DOM, so without forcing
        // a base re-record the retained cache re-presents the old caret until
        // an unrelated restyle. (Value edits also markDirty via setAttribute,
        // making this a harmless superset for them.)
        markAppBaseDirty();
        // Keep the native IME candidate window tracking the caret.
        updateTextInputArea();
    }
}

// ---------------------------------------------------------------------------
// IME composition
// ---------------------------------------------------------------------------

bool Engine::compositionActive() {
    auto* el = document_ ? document_->activeElement() : nullptr;
    if (auto* input = getElInput(el); input && input->isComposing()) return true;
    if (auto* ta = getElTextarea(el); ta && ta->isComposing()) return true;
    if (editComp_.active) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Contenteditable composition — the DOM-splice counterpart of the controls'
// TextComposition. The preedit is provisional text inside the caret's text
// node (browser-observable via textContent), replaced on every update.
// ---------------------------------------------------------------------------

dom::TextNode* Engine::editableCompositionTarget() {
    if (!editComp_.active) return nullptr;
    auto* tn = editComp_.node.get();
    if (!document_ || !tn || !document_->ownsNode(tn)) {
        editComp_ = {};
        return nullptr;
    }
    // The preedit bytes must still sit where the composition put them —
    // script rewriting the node's data (textContent = ...) reuses the node,
    // so liveness alone can't catch it. A mismatch means script owns the
    // text now; the stale composition is dropped, and the pending commit
    // degrades to a plain insert at the caret (the controls behave the same
    // way after a .value write).
    const std::string& data = tn->data();
    if (editComp_.start < 0 || editComp_.length < 0 ||
        static_cast<size_t>(editComp_.start) + static_cast<size_t>(editComp_.length) > data.size() ||
        data.compare(static_cast<size_t>(editComp_.start),
                     static_cast<size_t>(editComp_.length),
                     editComp_.preedit) != 0) {
        editComp_ = {};
        return nullptr;
    }
    return tn;
}

bool Engine::editableCompositionUpdate(const std::string& text, int cursorCp,
                                       bool& wasComposing,
                                       std::string& replacedSel,
                                       dom::Element*& hostOut) {
    wasComposing = editComp_.active;
    replacedSel.clear();
    hostOut = nullptr;
    if (!document_) return false;
    auto* sel = document_->selection();
    if (!sel) return false;

    if (editComp_.active) {
        // Script can rip the preedit's node out — or rewrite its data —
        // mid-composition. Drop the stale composition rather than splice
        // into freed memory or over script-owned text (the contenteditable
        // analog of the controls dropping theirs on a .value write).
        auto* tn = editableCompositionTarget();
        if (!tn) {
            wasComposing = false;
            return false;
        }
        tn->replaceData(static_cast<size_t>(editComp_.start),
                        static_cast<size_t>(editComp_.length), text);
        editComp_.length = static_cast<int>(text.size());
        editComp_.preedit = text;
        sel->collapse(tn, editComp_.start +
                              layout::utf8ByteForCodepoint(text, cursorCp));
        hostOut = editComp_.host.get();
        document_->markDirty();
        return true;
    }

    // Starting a composition: the caret must sit in a contenteditable host.
    auto* focusNode = sel->rangeCount() > 0 ? sel->focusNode() : nullptr;
    auto* host = focusNode ? editableHostOf(focusNode) : nullptr;
    if (!host) return false;

    // compositionstart.data is the text the composition replaces — capture it
    // before the caret resolution deletes the selection.
    if (!sel->isCollapsed()) replacedSel = sel->toString();

    // Snapshot the host before the caret resolution deletes anything. This is
    // what a cancel restores (resurrecting a selection the composition
    // replaced, as the controls do by restoring their pre-composition value)
    // and what a commit records as its single undo entry.
    const std::string hostBefore = host->innerHTML();
    const auto compSelBefore = DomUndoStack::selectionOf(document_.get(), host);

    int off = 0;
    bool created = false;
    auto* tn = selectionCaretTextPosition(document_.get(), off, created);
    if (!tn) return false;

    editComp_.hostBefore = hostBefore;
    editComp_.selBefore = compSelBefore;
    editComp_.replacedSelection = !replacedSel.empty();
    editComp_.active = true;
    editComp_.node.assign(document_.get(), tn);
    editComp_.host.assign(document_.get(), host);
    editComp_.createdNode = created;
    editComp_.start = off;
    tn->insertData(static_cast<size_t>(off), text);
    editComp_.length = static_cast<int>(text.size());
    editComp_.preedit = text;
    sel->collapse(tn, off + layout::utf8ByteForCodepoint(text, cursorCp));
    hostOut = host;
    document_->markDirty();
    return true;
}

bool Engine::editableCompositionCommit(const std::string& text,
                                       dom::Element*& hostOut, bool cancel) {
    hostOut = nullptr;
    if (!editComp_.active) return false;
    auto* host = editComp_.host.get();
    auto* tn = editableCompositionTarget();
    if (!tn) return false;
    const int start = editComp_.start;
    const int length = editComp_.length;
    const bool createdNode = editComp_.createdNode;
    const std::string hostBefore = editComp_.hostBefore;
    const auto selBefore = editComp_.selBefore;
    const bool replacedSelection = editComp_.replacedSelection;
    editComp_ = {};

    // A canceled composition that had replaced a selection must resurrect it
    // — the deletion happened back at compositionstart, so undoing just the
    // preedit splice is not enough. Restoring the host's pre-composition
    // serialization puts the whole replaced range back, matching the controls
    // (which restore their pre-composition value wholesale). Only taken when
    // a selection was actually replaced: the common case rewrites nothing but
    // the preedit bytes, and re-parsing the host there would needlessly
    // destroy every node in it.
    if (cancel && replacedSelection && host) {
        host->setInnerHTML(hostBefore);
        host->markStructureDirty();
        DomUndoStack::restoreSelection(document_.get(), host, selBefore);
        document_->markDirty();
        hostOut = host;
        return true;
    }

    // ONE coherent splice: preedit range → committed text, recorded below as
    // a single discrete undo entry.
    tn->replaceData(static_cast<size_t>(start), static_cast<size_t>(length),
                    text);
    auto* sel = document_->selection();
    if (tn->length() == 0 && createdNode && tn->parentNode()) {
        // An empty commit into a node we created for the composition leaves
        // an empty text node behind — remove it and re-seat the caret where
        // the node sat, restoring the pre-composition DOM.
        auto* parent = tn->parentNode();
        const auto& kids = parent->childNodes();
        int idx = 0;
        for (size_t i = 0; i < kids.size(); ++i) {
            if (kids[i] == tn) { idx = static_cast<int>(i); break; }
        }
        parent->removeChild(tn);
        // Raw removeChild doesn't mark layout structure (see
        // selectionCaretTextPosition); rebuild the parent's layout children
        // so the adapter for the removed node goes away too.
        if (parent->nodeType() == dom::NodeType::Element)
            static_cast<dom::Element*>(parent)->markStructureDirty();
        if (sel) sel->collapse(parent, idx);
    } else if (sel) {
        sel->collapse(tn, start + static_cast<int>(text.size()));
    }

    // One discrete entry spanning pre-composition → committed, so a single
    // Ctrl+Z removes the whole committed run. A cancel records nothing: it
    // leaves the host as it found it.
    if (!cancel && host) {
        editUndo_.forHost(document_.get(), host)
            .recordStructural(host, hostBefore, host->innerHTML(), selBefore,
                              DomUndoStack::selectionOf(document_.get(), host),
                              util::currentTimeMs());
    }

    document_->markDirty();
    hostOut = host;
    return true;
}

bool Engine::editableCompositionCancel(dom::Element*& hostOut) {
    return editableCompositionCommit("", hostOut, /*cancel=*/true);
}

void Engine::dispatchCompositionEvent(dom::Element* el, const char* type,
                                      const std::string& data) {
    if (!el) return;
    // compositionstart is cancelable per spec; we dispatch it as such but do
    // not honor preventDefault (a cancel would have to abort the OS
    // composition, which SDL has no hook for).
    dom::CompositionEvent evt(type, true,
                              std::strcmp(type, "compositionstart") == 0);
    evt.setData(data);
    evt.setIsTrusted(true);
    dispatchEvent(el, evt);
    if (jsRuntime_) jsRuntime_->executePendingJobs();
}

void Engine::updateTextInputArea() {
    if (!window_ || !document_) return;
    auto* activeEl = document_->activeElement();
    float x = 0, y = 0, w = 0, h = 0;
    bool have = false;
    if (auto* input = getElInput(activeEl);
        input && input->isFocused() && input->isTextType(activeEl)) {
        have = input->caretRect(x, y, w, h);
    } else if (auto* ta = getElTextarea(activeEl); ta && ta->isFocused()) {
        have = ta->caretRect(x, y, w, h);
    } else if (textMetrics_) {
        // Contenteditable: the DOM Selection caret (which sits at the
        // composition cursor during a composition — editableCompositionUpdate
        // collapses it there), so the candidate window tracks the preedit.
        auto* sel = document_->selection();
        auto* range = (sel && sel->rangeCount() > 0) ? sel->getRangeAt(0)
                                                     : nullptr;
        auto* node = range ? range->startContainer() : nullptr;
        if (node && document_->ownsNode(node) && inEditableHost(node) &&
            node->nodeType() == dom::NodeType::Text) {
            auto* tn = static_cast<dom::TextNode*>(node);
            float cx = 0, cy = 0, chh = 0;
            if (layout::getCaretRect(document_.get(), tn, range->startOffset(),
                                     *textMetrics_, cx, cy, chh)) {
                // getCaretRect is transform-unaware document space: project
                // through the ancestor transform chain, then remove scroll so
                // the value below is content space like the control rects.
                dom::Element* ctxEl = nullptr;
                for (dom::Node* n = tn; n; n = n->parentNode()) {
                    if (n->nodeType() == dom::NodeType::Element) {
                        ctxEl = static_cast<dom::Element*>(n);
                        break;
                    }
                }
                auto pr = ctxEl ? dom::projectRectThroughAncestors(ctxEl, cx, cy,
                                                                   1.0f, chh)
                                : dom::AbsoluteRect{cx, cy, 1.0f, chh};
                x = pr.x;
                y = pr.y - scrollY_;
                w = pr.width;
                h = pr.height;
                have = true;
            }
        }
    }
    if (!have) return;
    // Control caret rects are in app content space; SDL wants window
    // coordinates (points), so fold the engine's top inset back in.
    SDL_Rect rect;
    rect.x = static_cast<int>(std::lround(x));
    rect.y = static_cast<int>(std::lround(y + static_cast<float>(contentTop())));
    rect.w = static_cast<int>(std::lround(std::max(1.0f, w)));
    rect.h = static_cast<int>(std::lround(std::max(1.0f, h)));
    SDL_SetTextInputArea(window_->getSDLWindow(), &rect, 0);
}

void Engine::handleTextEditing(const std::string& text, int start,
                               int /*length*/) {
    if (!document_) return;
    // Overlays (color-picker hex field) take raw text input only — no
    // composition rendering there; the eventual TEXT_INPUT commit still
    // lands via handleTextInput.
    if (overlayMgr_.hasActive()) return;

    auto* activeEl = document_->activeElement();
    auto* input = getElInput(activeEl);
    auto* ta = getElTextarea(activeEl);
    const bool inputOk = input && input->isFocused();
    const bool taOk = !inputOk && ta && ta->isFocused();
    if (!inputOk && !taOk) {
        // No focused control — maybe the DOM Selection caret sits in a
        // contenteditable host. Same Chrome-shaped event order as the
        // controls, targeted at the host element.
        if (text.empty()) {
            if (!editComp_.active) return;
            dom::Element* host = nullptr;
            if (!editableCompositionCancel(host)) return;
            dispatchCompositionEvent(host, "compositionupdate", "");
            dispatchInputEvent(host, "", "insertCompositionText", true);
            dispatchCompositionEvent(host, "compositionend", "");
        } else {
            bool wasComposing = false;
            std::string replacedSel;
            dom::Element* host = nullptr;
            if (!editableCompositionUpdate(text, start, wasComposing,
                                           replacedSel, host)) return;
            if (!wasComposing)
                dispatchCompositionEvent(host, "compositionstart", replacedSel);
            dispatchCompositionEvent(host, "compositionupdate", text);
            dispatchInputEvent(host, text, "insertCompositionText", true);
        }
        markAppBaseDirty();
        uiDirty_ = true;
        updateTextInputArea();
        return;
    }
    const bool wasComposing = inputOk ? input->isComposing() : ta->isComposing();

    if (text.empty()) {
        // Empty editing event = the composition ended without commit.
        if (!wasComposing) return;
        layout::KeyHandleResult r = inputOk ? input->compositionCancel(activeEl)
                                            : ta->compositionCancel(activeEl);
        if (!r.handled) return;
        // Chrome's observable cancel order: compositionupdate("") → input →
        // compositionend("").
        dispatchCompositionEvent(activeEl, "compositionupdate", "");
        dispatchInputEvent(activeEl, "", "insertCompositionText", true);
        dispatchCompositionEvent(activeEl, "compositionend", "");
        markAppBaseDirty();
        uiDirty_ = true;
        updateTextInputArea();
        return;
    }

    // compositionstart.data is the text the composition replaces — capture
    // the selection before the first update deletes it.
    std::string replacedSel;
    if (!wasComposing)
        replacedSel = inputOk ? input->selectedText() : ta->selectedText();

    layout::KeyHandleResult r =
        inputOk ? input->compositionUpdate(activeEl, text, start)
                : ta->compositionUpdate(activeEl, text, start);
    if (!r.handled) return;

    if (!wasComposing)
        dispatchCompositionEvent(activeEl, "compositionstart", replacedSel);
    dispatchCompositionEvent(activeEl, "compositionupdate", text);
    dispatchInputEvent(activeEl, text, "insertCompositionText", true);
    markAppBaseDirty();
    uiDirty_ = true;
    updateTextInputArea();
}

void Engine::commitActiveComposition() {
    if (!document_) return;
    auto* activeEl = document_->activeElement();
    layout::KeyHandleResult r;
    std::string data;
    if (auto* input = getElInput(activeEl);
        input && input->isFocused() && input->isComposing()) {
        data = input->compositionText();
        r = input->compositionCommit(activeEl, data);
    } else if (auto* ta = getElTextarea(activeEl);
               ta && ta->isFocused() && ta->isComposing()) {
        data = ta->compositionText();
        r = ta->compositionCommit(activeEl, data);
    } else if (editComp_.active) {
        // Contenteditable composition: finalize the preedit in place.
        data = editComp_.preedit;
        dom::Element* host = nullptr;
        if (!editableCompositionCommit(data, host)) return;
        dispatchCompositionEvent(host, "compositionupdate", data);
        dispatchInputEvent(host, data, "insertCompositionText", true);
        dispatchCompositionEvent(host, "compositionend", data);
        markAppBaseDirty();
        uiDirty_ = true;
        updateTextInputArea();
        return;
    } else {
        return;
    }
    if (!r.handled) return;
    dispatchCompositionEvent(activeEl, "compositionupdate", data);
    dispatchInputEvent(activeEl, data, "insertCompositionText", true);
    dispatchCompositionEvent(activeEl, "compositionend", data);
    markAppBaseDirty();
    uiDirty_ = true;
    updateTextInputArea();
}

void Engine::handleProgrammaticFocus(dom::Document* doc, dom::Element* oldEl,
                                     dom::Element* newEl) {
    // App document only: iframe and system-panel controls never own the
    // window's IME state.
    if (!doc || !document_ || doc != document_.get()) return;

    commitActiveComposition();

    // A script moving focus off an edited field reports the edit, exactly as a
    // click elsewhere does — the caller (js_element_focus / js_element_blur)
    // dispatches blur after this, so the order is the same too.
    if (takeValueChange(oldEl)) {
        dom::ElementHandle keepNew(document_.get(), newEl);
        dom::Event changeEvt("change");
        changeEvt.setIsTrusted(true);
        dispatchEvent(oldEl, changeEvt);
        newEl = keepNew.get();
    }
    armValueChange(newEl);

    if (auto* prevInput = getElInput(oldEl)) prevInput->setFocused(false);
    if (auto* prevTa = getElTextarea(oldEl)) prevTa->setFocused(false);

    auto* newInput = getElInput(newEl);
    auto* newTa = getElTextarea(newEl);
    if (newInput) {
        newInput->setFocused(true);
        if (newInput->isTextType(newEl)) {
            std::string v = newEl->getAttribute("value");
            newInput->setCursorPos(static_cast<int>(v.size()));
            safeStartTextInput(window_.get());
        } else {
            safeStopTextInput(window_.get());
        }
    } else if (newTa) {
        newTa->setFocused(true);
        std::string v = newEl->hasAttribute("value")
                            ? newEl->getAttribute("value")
                            : newEl->textContent();
        newTa->setCursorPos(static_cast<int>(v.size()));
        safeStartTextInput(window_.get());
    } else if (newEl && inEditableHost(newEl)) {
        // A contenteditable host still takes raw text input (commits insert
        // via the DOM Selection); keep SDL text input running for it.
        safeStartTextInput(window_.get());
    } else {
        safeStopTextInput(window_.get());
    }

    updateTextInputArea();
    markAppBaseDirty();
    uiDirty_ = true;
}

// System hotkeys are GLOBAL: the perf HUD and the settings modal toggle
// whichever bro window currently has keyboard focus, so a user working in a
// secondary window can still open them (they render on the main window). Both
// the app-document key path and the per-host one (window_host_input.cpp) run
// this before anything window-specific.
bool Engine::handleGlobalHotkey(int keycode, int mod, bool repeat) {
    if (repeat || !settings_) return false;
    const std::string webKey = sdlKeycodeToWebKey(keycode, mod);
    const std::string action = settings_->getActionForKey(webKey);
    if (action == "system_toggle_perf") {
        toggleSystemPerf();
        uiDirty_ = true;
        return true;
    }
    if (action == "system_toggle_settings") {
        toggleSystemSettings();
        uiDirty_ = true;
        return true;
    }
    return false;
}

void Engine::handleKeyDown(int keycode, int scancode, int mod, bool repeat) {
    heldModifierMask_ |= modifierBitForKeycode(keycode);
    // Physical held-key set for polled action state (actionStrength /
    // actionPressed). Maintained before any consumption branch below so it
    // tracks the keyboard itself, and keyed by keycode so the release always
    // clears the entry recorded at press time (the webKey can differ if a
    // modifier was released in between).
    heldKeys_[keycode] = sdlKeycodeToWebKey(keycode, mod);

    if (overlayMgr_.handleKeyDown(keycode, mod)) {
        uiDirty_ = true;
        return;
    }

    // Esc cancels inspector picker mode without dismissing the panel itself.
    if (inspector_.pickerMode && keycode == SDLK_ESCAPE && !repeat) {
        inspectorSetPickerMode(false);
        for (auto& doc : systemDocs_) {
            if (doc.name != "inspector" || !doc.jsCtx) continue;
            JSValue global = JS_GetGlobalObject(doc.jsCtx);
            JSValue fn = JS_GetPropertyStr(doc.jsCtx, global, "__onInspectorChanged");
            if (JS_IsFunction(doc.jsCtx, fn)) {
                JSValue r = JS_Call(doc.jsCtx, fn, global, 0, nullptr);
                JS_FreeValue(doc.jsCtx, r);
            }
            JS_FreeValue(doc.jsCtx, fn);
            JS_FreeValue(doc.jsCtx, global);
        }
        uiDirty_ = true;
        return;
    }

    // While the settings modal is open, route keys to its panels first.
    // Modal means modal: app keystrokes are fully suppressed until the modal
    // closes. Esc closes the modal unless a panel listener preventDefaulted
    // the event (e.g. the input panel cancelling a rebind capture). The
    // user's configured system_toggle_settings hotkey also still closes it.
    if (systemSettingsVisible_) {
        bool prevented = systemHandleKeyDown(keycode, scancode, mod, repeat);
        if (!prevented && !repeat) {
            if (keycode == SDLK_ESCAPE) {
                toggleSystemSettings();
                uiDirty_ = true;
            } else if (settings_) {
                std::string webKey = sdlKeycodeToWebKey(keycode, mod);
                if (settings_->getActionForKey(webKey) == "system_toggle_settings") {
                    toggleSystemSettings();
                    uiDirty_ = true;
                }
            }
        }
        return;
    }

    // System hotkeys (perf HUD, settings modal).
    if (handleGlobalHotkey(keycode, mod, repeat)) return;

    if (!document_) return;

    // A caret-moving or command key arriving while an IME composition is in
    // progress commits the preedit first (the browser's blur/caret-move
    // behavior — provisional text is never stranded). Real OS IMEs consume
    // these keys during composition, so in practice this only fires for
    // headless-injected or stray events. Plain character keys pass through
    // untouched: during a real composition SDL still delivers their raw
    // keydowns alongside the TEXT_EDITING stream.
    {
        const bool caretOrCommandKey =
            util::hasPrimaryMod(mod) ||
            keycode == SDLK_LEFT || keycode == SDLK_RIGHT ||
            keycode == SDLK_UP || keycode == SDLK_DOWN ||
            keycode == SDLK_HOME || keycode == SDLK_END ||
            keycode == SDLK_PAGEUP || keycode == SDLK_PAGEDOWN ||
            keycode == SDLK_BACKSPACE || keycode == SDLK_DELETE ||
            keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER ||
            keycode == SDLK_TAB || keycode == SDLK_ESCAPE;
        if (caretOrCommandKey) commitActiveComposition();
    }

    // Tab key: dispatch to JS first; only advance focus if not prevented
    if (keycode == SDLK_TAB) {
        auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
        dom::Element* target = document_->activeElement();
        if (!target) target = document_->body();
        if (target) dispatchEvent(target, evt);

        if (!evt.defaultPrevented()) {
            advanceFocus((mod & SDL_KMOD_SHIFT) != 0);
            uiDirty_ = true;
        }
        return;
    }

    // Clipboard: ⌘-C / ⌘-X / ⌘-V on macOS, Ctrl equivalents elsewhere.
    if (util::hasPrimaryMod(mod) &&
        (keycode == SDLK_C || keycode == SDLK_X || keycode == SDLK_V)) {

        auto* activeEl = document_->activeElement();
        dom::Element* target = activeEl ? activeEl : document_->body();

        if (keycode == SDLK_V) {
            // Paste: read system clipboard, dispatch paste event
            char* clipText = SDL_GetClipboardText();
            std::string text = clipText ? clipText : "";
            SDL_free(clipText);

            dom::ClipboardEvent pasteEvt("paste", true, true);
            pasteEvt.setClipboardText(text);
            if (!text.empty()) {
                pasteEvt.addItem({"text/plain", {}, text});
            }
            // Pull any image formats the system has. SDL3 normalizes CF_DIB/CF_DIBV5
            // to "image/bmp" and the Windows CF_PNG format to "image/png" for us.
            for (const char* mime : {"image/png", "image/bmp", "image/jpeg"}) {
                if (!SDL_HasClipboardData(mime)) continue;
                size_t n = 0;
                void* p = SDL_GetClipboardData(mime, &n);
                if (p && n > 0) {
                    auto* bp = static_cast<const uint8_t*>(p);
                    pasteEvt.addItem({mime, std::vector<uint8_t>(bp, bp + n), ""});
                }
                if (p) SDL_free(p);
            }
            pasteEvt.setIsTrusted(true);
            dispatchEvent(target, pasteEvt);

            // If not prevented and no form field consumes it, try inserting
            // into a contenteditable host via the Selection.
            if (!pasteEvt.defaultPrevented() && !text.empty()) {
                bool handledByForm = false;
                if (activeEl) {
                    auto* input = getElInput(activeEl);
                    auto* ta = getElTextarea(activeEl);
                    handledByForm = (input && input->isFocused()) ||
                                    (ta && ta->isFocused());
                }
                if (!handledByForm) {
                    auto* sel = document_->selection();
                    if (sel && sel->rangeCount() > 0) {
                        auto* fn = sel->focusNode();
                        if (fn && inEditableHost(fn)) {
                            auto* host = editableHostOf(fn);
                            runEditableMutation(document_.get(), jsRuntime_.get(), fn,
                                "insertFromPaste", text,
                                [&] {
                                    // A paste always stands alone, and may
                                    // replace a multi-node selection.
                                    EditUndoScope undo(&editUndo_, document_.get(), host,
                                                       nullptr, DomUndoStack::Kind::Discrete);
                                    selectionInsertText(document_.get(), text);
                                    undo.commit();
                                });
                            uiDirty_ = true;
                        }
                    }
                }
            }
            if (!pasteEvt.defaultPrevented() && !text.empty() && activeEl) {
                layout::KeyHandleResult r;
                // pasteText, not handleTextInput: a paste is a discrete undo
                // entry (and reports inputType "insertFromPaste").
                if (auto* input = getElInput(activeEl); input && input->isFocused()) {
                    r = input->pasteText(activeEl, text);
                } else if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
                    r = textarea->pasteText(activeEl, text);
                }
                if (r.handled) {
                    applyKeyResult(activeEl, r);
                    uiDirty_ = true;
                }
            }
        } else {
            // Copy or Cut: first try the focused input/textarea, which copies
            // its *selected* text (a collapsed caret copies nothing, as in a
            // browser). Otherwise fall back to the document's Selection so
            // users can copy text they highlighted outside form fields.
            std::string text;
            bool fromFormField = false;
            if (activeEl) {
                if (auto* input = getElInput(activeEl); input && input->isFocused()) {
                    text = input->selectedText();
                    fromFormField = true;
                } else if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
                    text = textarea->selectedText();
                    fromFormField = true;
                }
            }
            if (!fromFormField) {
                if (auto* sel = document_->selection(); sel && !sel->isCollapsed()) {
                    text = sel->toString();
                }
            }

            std::string evtType = (keycode == SDLK_C) ? "copy" : "cut";
            dom::ClipboardEvent clipEvt(evtType, true, true);
            clipEvt.setClipboardText(text);
            clipEvt.setIsTrusted(true);
            dispatchEvent(target, clipEvt);

            if (!clipEvt.defaultPrevented() && !text.empty()) {
                SDL_SetClipboardText(text.c_str());

                // Cut: remove the selected range from the field (the whole value
                // only if the whole value was selected).
                if (keycode == SDLK_X && fromFormField && activeEl) {
                    bool cut = false;
                    if (auto* input = getElInput(activeEl)) {
                        cut = input->cutSelection(activeEl);
                    } else if (auto* textarea = getElTextarea(activeEl)) {
                        cut = textarea->cutSelection(activeEl);
                    }
                    if (cut) {
                        dom::InputEvent inputEvt("input", true, false);
                        inputEvt.setInputType("deleteByCut");
                        inputEvt.setIsTrusted(true);
                        dispatchEvent(activeEl, inputEvt);
                        if (activeEl->document()) activeEl->document()->markDirty();
                        uiDirty_ = true;
                    }
                } else if (keycode == SDLK_X && !fromFormField) {
                    // Cut from a DOM Selection inside contenteditable.
                    auto* sel = document_->selection();
                    if (sel && sel->rangeCount() > 0 && !sel->isCollapsed()) {
                        auto* fn = sel->focusNode();
                        if (fn && inEditableHost(fn)) {
                            auto* host = editableHostOf(fn);
                            runEditableMutation(document_.get(), jsRuntime_.get(), fn,
                                "deleteByCut", "",
                                [&] {
                                    EditUndoScope undo(&editUndo_, document_.get(), host,
                                                       nullptr, DomUndoStack::Kind::Discrete);
                                    auto* range = sel->getRangeAt(0);
                                    if (!range) return;
                                    dom::Node* after = nullptr; int afterOff = 0;
                                    deleteRangeContents(document_.get(), *range, after, afterOff);
                                    if (after) sel->collapse(after, afterOff);
                                    undo.commit();
                                });
                            uiDirty_ = true;
                        }
                    }
                }
            }
        }
        return;
    }

    // Delegate to the active control
    auto* activeEl = document_->activeElement();
    layout::KeyHandleResult result;

    if (auto* input = getElInput(activeEl); input && input->isFocused()) {
        result = input->handleKeyDown(activeEl, keycode, mod);
    } else if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
        result = textarea->handleKeyDown(activeEl, keycode, mod);
    }

    if (result.handled) {
        applyKeyResult(activeEl, result);
        // Still dispatch keydown event for JS listeners
        auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
        evt.setIsComposing(compositionActive());
        dispatchEvent(activeEl, evt);
        return;
    }

    // ----------------------------------------------------------------------
    // DOM Selection keyboard navigation. Fires when no form field handled
    // the key. Moves the focus endpoint of the active Selection; Shift
    // extends the selection instead of collapsing it.
    // ----------------------------------------------------------------------
    if (document_) {
        auto* sel = document_->selection();
        if (sel && sel->rangeCount() > 0) {
            bool shift = (mod & SDL_KMOD_SHIFT) != 0;
            bool ctrl = util::hasPrimaryMod(mod);
            bool handled = false;

            auto moveFocus = [&](dom::Node* n, int off) {
                if (shift) {
                    sel->extend(n, off);
                } else {
                    sel->collapse(n, off);
                }
            };

            dom::Node* focusN = sel->focusNode();
            int focusO = sel->focusOffset();
            auto* focusText = (focusN && focusN->nodeType() == dom::NodeType::Text)
                ? static_cast<dom::TextNode*>(focusN) : nullptr;

            // -----------------------------------------------------------
            // Contenteditable editing: Backspace / Delete / Enter, plus
            // cut via Ctrl+X (copy lands in the earlier clipboard block).
            // Only fires when the focus endpoint sits in an editable host.
            // -----------------------------------------------------------
            bool editable = focusN && inEditableHost(focusN);
            dom::Element* editHost = editable ? editableHostOf(focusN) : nullptr;

            // Undo / redo, mirroring the controls: primary+Z undoes,
            // primary+Y or primary+shift+Z redoes. Handled before the editing
            // branches (and returning early) so the recording sites below
            // never see a history move as a fresh edit.
            if (editable && ctrl && (keycode == SDLK_Z || keycode == SDLK_Y)) {
                // Handled whether or not there was anything to step to:
                // Ctrl+Z over an empty history is still Ctrl+Z, and must not
                // fall through to the editing branches below.
                editHistoryStep(/*redo=*/(keycode == SDLK_Y) || shift);
                handled = true;
            } else if (editable && (keycode == SDLK_BACKSPACE || keycode == SDLK_DELETE)) {
                editDeleteAtCaret(/*backward=*/keycode == SDLK_BACKSPACE);
                handled = true;
            } else if (editable && keycode == SDLK_RETURN) {
                editInsertLineBreak();
                handled = true;
            } else if (ctrl && keycode == SDLK_A) {
                // Ctrl+A selects the containing contenteditable host's
                // children, or the body's — not gated on `editable`, so it
                // works over ordinary text too.
                editSelectAll();
                handled = true;
            } else if (focusText) {
                const std::string& data = focusText->data();
                int len = static_cast<int>(data.size());
                // Whole code points, for the same reason deletion steps them:
                // `data` is UTF-8 and `focusO` indexes its bytes, so ±1 lands
                // inside a multi-byte character — where the UTF-16 conversion
                // at the JS boundary snaps back and the caret looks stuck.
                if (keycode == SDLK_LEFT) {
                    if (focusO > 0) {
                        moveFocus(focusText, layout::utf8Prev(data, focusO));
                        handled = true;
                    }
                } else if (keycode == SDLK_RIGHT) {
                    if (focusO < len) {
                        moveFocus(focusText, layout::utf8Next(data, focusO));
                        handled = true;
                    }
                } else if (keycode == SDLK_HOME) {
                    moveFocus(focusText, 0);
                    handled = true;
                } else if (keycode == SDLK_END) {
                    moveFocus(focusText, len);
                    handled = true;
                }
            }

            if (handled) {
                // Caret/selection moves (arrows, Home/End, Ctrl+A) change no
                // DOM but move base-only selection chrome; force a re-record.
                // DOM-mutating branches above (Backspace/Delete/Enter) already
                // markDirty via their edits, so this is a harmless superset.
                markAppBaseDirty();
                auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
                // Same target rule as every other keydown: the focused
                // element. The caret's own node is where the *edit* happened,
                // not where the key was aimed — an arrow press with a text
                // field focused but nothing to move in it used to be
                // delivered to whatever element the document Selection
                // happened to sit in, so the field never saw its own key.
                if (dom::Element* target = document_->activeElement())
                    dispatchEvent(target, evt);
                return;
            }
        }
    }

    // Default: dispatch keydown at the focused element. A key nothing above
    // consumed still belongs to whatever has focus — that is where the web
    // aims it, and where a widget listens for its own keys. (Document::
    // activeElement falls back to <body> when nothing is focused, so an
    // unfocused page behaves exactly as it did.) The event bubbles from there,
    // so document- and body-level listeners still see every key.
    auto evt = makeKeyboardEvent("keydown", keycode, scancode, mod, repeat);
    evt.setIsComposing(compositionActive());
    dom::Element* target = document_->activeElement();
    if (target) {
        dispatchEvent(target, evt);
    }

    // Dispatch action event if key is bound to an action
    dispatchActionEventForKey(sdlKeycodeToWebKey(keycode, mod), "down", 1.0f);
}

void Engine::handleKeyUp(int keycode, int scancode, int mod, bool repeat) {
    heldModifierMask_ &= ~modifierBitForKeycode(keycode);
    heldKeys_.erase(keycode);

    if (systemSettingsVisible_) {
        systemHandleKeyUp(keycode, scancode, mod, repeat);
        return;
    }
    if (!document_) return;

    // Dispatch keyup at the focused element, whatever it is — the same rule
    // keydown follows, and the one the web states: focus decides the target,
    // not whether the element happens to be a text control. It falls back to
    // <body> on its own when nothing is focused.
    auto evt = makeKeyboardEvent("keyup", keycode, scancode, mod, repeat);
    evt.setIsComposing(compositionActive());

    dom::Element* target = document_->activeElement();
    if (target) {
        dispatchEvent(target, evt);
    }

    // Dispatch action event if key is bound to an action
    dispatchActionEventForKey(sdlKeycodeToWebKey(keycode, mod), "up", 0.0f);
}

// Filter out control characters (tab, etc.) that shouldn't be inserted as text
bool isControlChar(const std::string& text) {
    if (text.empty()) return true;
    unsigned char c = static_cast<unsigned char>(text[0]);
    // Allow printable ASCII and multi-byte UTF-8 sequences
    if (text.size() == 1 && c < 0x20 && c != '\n') return true; // control chars except newline
    if (text.size() == 1 && c == 0x7f) return true; // DEL
    return false;
}

void Engine::handleTextInput(const std::string& text) {
    if (!document_) return;

    // Filter control characters for all inputs
    if (isControlChar(text)) return;

    if (overlayMgr_.handleTextInput(text)) {
        uiDirty_ = true;
        return;
    }

    auto* activeEl = document_->activeElement();
    layout::KeyHandleResult result;

    // TEXT_INPUT while a composition is in progress is the IME commit:
    // replace the preedit with the committed text (one undo entry) and close
    // the composition with Chrome's observable order — compositionupdate →
    // input(insertCompositionText) → compositionend.
    {
        layout::KeyHandleResult commit;
        if (auto* textarea = getElTextarea(activeEl);
            textarea && textarea->isFocused() && textarea->isComposing()) {
            commit = textarea->compositionCommit(activeEl, text);
        } else if (auto* input = getElInput(activeEl);
                   input && input->isFocused() && input->isComposing()) {
            commit = input->compositionCommit(activeEl, text);
        }
        if (commit.handled) {
            dispatchCompositionEvent(activeEl, "compositionupdate", text);
            dispatchInputEvent(activeEl, text, "insertCompositionText", true);
            dispatchCompositionEvent(activeEl, "compositionend", text);
            markAppBaseDirty();
            uiDirty_ = true;
            updateTextInputArea();
            return;
        }
        // Contenteditable composition: this TEXT_INPUT is its commit.
        if (editComp_.active) {
            dom::Element* host = nullptr;
            if (editableCompositionCommit(text, host)) {
                dispatchCompositionEvent(host, "compositionupdate", text);
                dispatchInputEvent(host, text, "insertCompositionText", true);
                dispatchCompositionEvent(host, "compositionend", text);
                markAppBaseDirty();
                uiDirty_ = true;
                updateTextInputArea();
                return;
            }
        }
    }

    if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
        result = textarea->handleTextInput(activeEl, text);
    } else if (auto* input = getElInput(activeEl); input && input->isFocused()) {
        result = input->handleTextInput(activeEl, text);
    }

    if (result.handled) {
        applyKeyResult(activeEl, result);
        return;
    }

    // No form-field consumer — maybe the selection is inside a
    // contenteditable host. Fire beforeinput → mutate → input.
    editInsertTextAtSelection(text);
}

// ---------------------------------------------------------------------------
// contenteditable edit primitives
//
// The bodies below were lifted verbatim out of handleKeyDown/handleTextInput
// so that execCommand() runs the SAME code a key press runs rather than a
// second implementation of it. Editing a DOM tree has enough corners —
// cross-node deletes, whitespace re-encoding, which edits coalesce in undo —
// that two implementations would diverge silently, and the divergence would
// show up as script-driven edits corrupting a tree that typed edits handle.
//
// Each re-derives (selection, focus node, offset, host) instead of taking
// them as parameters. That costs nothing, and it means a caller cannot hand
// in a stale caret: script may have moved the Selection between the
// beforeinput handler and the edit.
// ---------------------------------------------------------------------------

namespace {
// The (selection, focus, host) tuple every edit primitive starts from.
// `host` is null when the caret isn't inside a contenteditable.
struct EditCaret {
    bro::dom::Selection* sel = nullptr;
    bro::dom::Node* node = nullptr;
    int offset = 0;
    bro::dom::TextNode* text = nullptr;
    bro::dom::Element* host = nullptr;
    explicit operator bool() const { return sel && node && host; }
};

EditCaret editCaretOf(bro::dom::Document* doc) {
    EditCaret c;
    if (!doc) return c;
    auto* sel = doc->selection();
    if (!sel || sel->rangeCount() == 0) return c;
    auto* focusN = sel->focusNode();
    if (!focusN || !inEditableHost(focusN)) return c;
    c.sel = sel;
    c.node = focusN;
    c.offset = sel->focusOffset();
    c.text = (focusN->nodeType() == bro::dom::NodeType::Text)
                 ? static_cast<bro::dom::TextNode*>(focusN) : nullptr;
    c.host = editableHostOf(focusN);
    return c;
}
} // namespace

bool Engine::editDeleteAtCaret(bool backward) {
    const EditCaret c = editCaretOf(document_.get());
    if (!c) return false;
    const std::string inputType =
        backward ? "deleteContentBackward" : "deleteContentForward";
    runEditableMutation(document_.get(), jsRuntime_.get(), c.node, inputType, "",
        [&] {
            auto* range = c.sel->getRangeAt(0);
            if (!range) return;
            if (!range->collapsed()) {
                // A selection delete can span nodes — structural.
                EditUndoScope undo(&editUndo_, document_.get(), c.host,
                                   nullptr, DomUndoStack::Kind::Discrete);
                dom::Node* after = nullptr; int afterOff = 0;
                deleteRangeContents(document_.get(), *range, after, afterOff);
                if (after) c.sel->collapse(after, afterOff);
                undo.commit();
                return;
            }
            // Collapsed: delete one character backward/forward within the
            // current text node when possible. That is confined to one node's
            // data, so it records as a mergeable splice and consecutive
            // presses coalesce.
            //
            // The character to delete may not be in the caret's own node — at
            // offset 0, or at the end of a text node, it is in the
            // neighbouring leaf. Resolve it first, then pick the undo scope,
            // because a delete that empties an inline or crosses a node is
            // structural and a plain splice inside one node is not.
            //
            // One code point, not one byte: offsets here index UTF-8 bytes, so
            // a ±1 step would chop a multi-byte character in half and leave
            // invalid UTF-8 in the tree. resolveDeleteTarget steps with
            // utf8Prev/utf8Next, the same helpers <input>/<textarea> delete
            // with.
            const EditDeleteTarget target =
                resolveDeleteTarget(c.node, c.offset, c.host, backward ? -1 : 1);
            if (!target.text && !target.remove) return;

            const bool sameNodeSplice =
                target.text && target.text == c.text &&
                static_cast<int>(target.text->length()) >
                    (target.end - target.start);
            EditUndoScope undo(
                &editUndo_, document_.get(), c.host,
                sameNodeSplice ? target.text : nullptr,
                sameNodeSplice
                    ? (backward ? DomUndoStack::Kind::Backspace
                                : DomUndoStack::Kind::DeleteForward)
                    : DomUndoStack::Kind::Discrete);

            dom::Node* caretNode = nullptr;
            int caretOff = 0;
            if (target.remove) {
                auto* p = target.remove->parentNode();
                const int idx = indexInParent(target.remove);
                if (!p) return;
                p->removeChild(target.remove);
                caretNode = p;
                caretOff = idx < 0 ? 0 : idx;
            } else {
                target.text->deleteData(target.start,
                                        target.end - target.start);
                caretNode = target.text;
                caretOff = target.start;
            }
            if (!sameNodeSplice)
                pruneAndMerge(c.host, caretNode, caretOff);
            if (caretNode) c.sel->collapse(caretNode, caretOff);
            undo.commit();
        });
    uiDirty_ = true;
    return true;
}

bool Engine::editInsertLineBreak() {
    const EditCaret c = editCaretOf(document_.get());
    if (!c) return false;
    // Inserts a <br> element — v1 treats contenteditable as plaintext-only
    // (no block splitting on Enter).
    runEditableMutation(document_.get(), jsRuntime_.get(), c.node,
        "insertLineBreak", "\n",
        [&] {
            // Enter inserts an element and splits a text node — always
            // structural, always its own entry.
            EditUndoScope undo(&editUndo_, document_.get(), c.host,
                               nullptr, DomUndoStack::Kind::Discrete);
            auto* range = c.sel->getRangeAt(0);
            if (!range) return;
            if (!range->collapsed()) {
                dom::Node* after = nullptr; int afterOff = 0;
                deleteRangeContents(document_.get(), *range, after, afterOff);
                if (after) c.sel->collapse(after, afterOff);
            }
            auto* br = document_->createElement("BR");
            range = c.sel->getRangeAt(0);
            if (range) range->insertNode(br);
            // Move caret past the <br>. For a text-node caret, insertNode
            // splits the text; the caret now sits immediately after the <br>
            // in its parent.
            if (br->parentNode()) {
                auto* p = br->parentNode();
                const auto& kids = p->childNodes();
                for (size_t i = 0; i < kids.size(); ++i) {
                    if (kids[i] == br) {
                        c.sel->collapse(p, static_cast<int>(i + 1));
                        break;
                    }
                }
            }
            undo.commit();
        });
    uiDirty_ = true;
    return true;
}

bool Engine::editInsertTextAtSelection(const std::string& text) {
    const EditCaret c = editCaretOf(document_.get());
    if (!c) return false;
    runEditableMutation(document_.get(), jsRuntime_.get(), c.node,
        "insertText", text,
        [&] {
            auto* r = c.sel->getRangeAt(0);
            if (!r) return;
            if (!r->collapsed()) {
                // Typing over a selection replaces a range that can span
                // nodes, and stands alone — the same rule the controls apply.
                EditUndoScope undo(&editUndo_, document_.get(), c.host,
                                   nullptr, DomUndoStack::Kind::Discrete);
                selectionInsertText(document_.get(), text);
                undo.commit();
                return;
            }
            // Collapsed caret: resolve it to a text node BEFORE opening the
            // scope. Minting an empty text node at an element boundary is not
            // itself an edit (an empty text node serializes to nothing), so
            // doing it first lets every plain keystroke record the same cheap
            // splice — and a run coalesces whether or not the host started
            // empty.
            int off = 0;
            bool created = false;
            auto* tn = selectionCaretTextPosition(document_.get(), off, created);
            if (!tn) return;
            EditUndoScope undo(&editUndo_, document_.get(), c.host, tn,
                               DomUndoStack::Kind::Typing);
            tn->insertData(static_cast<size_t>(off), text);
            const int caret = off + static_cast<int>(text.size());
            document_->selection()->collapse(tn, caret);
            // Inside the undo scope: the re-encoding is part of the same
            // keystroke, so undo takes the whole thing back rather than
            // leaving stray U+00A0.
            rebalanceAfterEdit(document_.get(), tn, caret);
            undo.commit();
        });
    uiDirty_ = true;
    return true;
}

bool Engine::editHistoryStep(bool redo) {
    const EditCaret c = editCaretOf(document_.get());
    if (!c) return false;
    // find(), not forHost(): asking whether an untouched host can undo must
    // not mint a history for it.
    auto* stack = editUndo_.find(c.host);
    if (!stack) return false;
    if (redo ? !stack->canRedo() : !stack->canUndo()) return false;
    const char* inputType = redo ? "historyRedo" : "historyUndo";
    runEditableMutation(document_.get(), jsRuntime_.get(), c.node, inputType, "",
        [&] {
            if (redo) stack->redo(document_.get(), c.host);
            else stack->undo(document_.get(), c.host);
        });
    uiDirty_ = true;
    return true;
}

bool Engine::editSelectAll() {
    if (!document_) return false;
    auto* sel = document_->selection();
    if (!sel) return false;
    // The caret's contenteditable host if there is one, else the body — the
    // rule Ctrl+A follows, which is deliberately not gated on editability.
    dom::Node* host = (sel->rangeCount() > 0) ? sel->focusNode() : nullptr;
    while (host && host->nodeType() != dom::NodeType::Element)
        host = host->parentNode();
    auto* el = static_cast<dom::Element*>(host);
    while (el && !el->hasAttribute("contenteditable"))
        el = el->parentElement();
    dom::Node* target = el ? static_cast<dom::Node*>(el)
                           : static_cast<dom::Node*>(document_->body());
    if (!target) return false;
    sel->selectAllChildren(target);
    uiDirty_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// document.execCommand
//
// SCOPE: the commands whose primitives this build actually has. Everything
// here maps onto an edit the keyboard can already perform, which is the line
// that decides what is in and what is out:
//
//   in    insertText, insertLineBreak/insertParagraph, delete, forwardDelete,
//         undo, redo, selectAll, copy, cut, paste
//   out   bold/italic/underline/foreColor/… — these wrap content in inline
//         elements or style it, and contenteditable here is plaintext-v1: it
//         has no inline-formatting model to hang them on. They report
//         unsupported rather than silently doing nothing, so a caller can
//         feature-detect with queryCommandSupported() instead of discovering
//         it from a no-op.
//
// DIVERGENCE FROM BROWSERS, deliberate: browsers refuse execCommand("paste")
// (and often cut/copy) from script, because a web page reading the user's
// clipboard without a gesture is a privilege escalation. bro is an app
// runtime, not a web sandbox — the app IS the trusted party, and
// bro.window's clipboard read/write is already exposed to it unconditionally.
// Refusing here would buy no safety and would only make the keyboard path and
// the scripted path disagree.
// ---------------------------------------------------------------------------

namespace {
// Canonical command name: browsers match case-insensitively, and accept a
// legacy "cmd_"-free spelling only.
std::string normalizeCommand(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char ch : name)
        out.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(ch))));
    return out;
}
} // namespace

bool Engine::queryCommandSupported(const std::string& name) const {
    const std::string cmd = normalizeCommand(name);
    return cmd == "inserttext" || cmd == "insertlinebreak" ||
           cmd == "insertparagraph" || cmd == "delete" ||
           cmd == "forwarddelete" || cmd == "undo" || cmd == "redo" ||
           cmd == "selectall" || cmd == "copy" || cmd == "cut" ||
           cmd == "paste";
}

bool Engine::queryCommandEnabled(const std::string& name) {
    if (!queryCommandSupported(name)) return false;
    const std::string cmd = normalizeCommand(name);
    // selectAll needs no editable caret — it falls back to the body.
    if (cmd == "selectall") return document_ && document_->body() != nullptr;
    const EditCaret c = editCaretOf(document_.get());
    if (!c) return false;
    // undo/redo are only enabled with something to step to, which is the one
    // case where "supported and editable" still isn't enough.
    if (cmd == "undo" || cmd == "redo") {
        auto* stack = editUndo_.find(c.host);
        if (!stack) return false;
        return (cmd == "redo") ? stack->canRedo() : stack->canUndo();
    }
    // copy/cut need a non-collapsed selection to have anything to take.
    if (cmd == "copy" || cmd == "cut") return !c.sel->isCollapsed();
    return true;
}

bool Engine::execCommand(const std::string& name, bool /*showUI*/,
                         const std::string& value) {
    if (!document_) return false;
    const std::string cmd = normalizeCommand(name);

    if (cmd == "inserttext") {
        // An empty insertion is a no-op, not a failure to run the command,
        // but it must not fire beforeinput/input for a zero-length edit.
        if (value.empty()) return editCaretOf(document_.get()) ? true : false;
        return editInsertTextAtSelection(value);
    }
    // insertParagraph is distinct from insertLineBreak in a browser (a new
    // block vs a <br>), but plaintext-v1 has no block splitting, so both
    // land on the <br> Enter inserts. Named separately anyway: callers
    // feature-detect the name, and the day blocks arrive this is the seam
    // where they diverge.
    if (cmd == "insertlinebreak" || cmd == "insertparagraph")
        return editInsertLineBreak();
    if (cmd == "delete") return editDeleteAtCaret(/*backward=*/true);
    if (cmd == "forwarddelete") return editDeleteAtCaret(/*backward=*/false);
    if (cmd == "undo") return editHistoryStep(/*redo=*/false);
    if (cmd == "redo") return editHistoryStep(/*redo=*/true);
    if (cmd == "selectall") return editSelectAll();
    if (cmd == "copy") {
        // Same source the Ctrl+C path reads, and the same system clipboard it
        // writes: a scripted copy has to leave the clipboard in the state the
        // key press would, or a following paste sees stale text.
        const std::string text = simulateCopy();
        if (text.empty()) return false;
        SDL_SetClipboardText(text.c_str());
        return true;
    }
    if (cmd == "cut") {
        const std::string text = simulateCut();
        if (text.empty()) return false;
        SDL_SetClipboardText(text.c_str());
        return true;
    }
    if (cmd == "paste") {
        char* clip = SDL_GetClipboardText();
        const std::string text = clip ? clip : "";
        SDL_free(clip);
        if (text.empty()) return false;
        if (!editCaretOf(document_.get())) return false;
        simulatePaste(text);
        return true;
    }
    return false;  // unsupported command
}

// ---------------------------------------------------------------------------
// Tab focus navigation
// ---------------------------------------------------------------------------

void Engine::advanceFocus(bool reverse) {
    if (!document_) return;

    // Tab away mid-composition commits the preedit (never strands it).
    commitActiveComposition();

    // Build list of focusable elements in DOM order
    std::vector<dom::Element*> focusable;
    auto* body = document_->body();
    if (!body) return;

    // Collect all elements via querySelectorAll for common focusable tags
    auto inputs = body->querySelectorAll("input");
    auto textareas = body->querySelectorAll("textarea");
    auto selects = body->querySelectorAll("select");
    auto buttons = body->querySelectorAll("button");

    // Merge into a single list — we need DOM order, so collect all elements
    // and filter. Use a simple recursive walk.
    std::function<void(dom::Node*)> walk = [&](dom::Node* node) {
        if (!node) return;
        if (node->nodeType() == dom::NodeType::Element) {
            auto* el = static_cast<dom::Element*>(node);
            std::string tag = el->tagName();
            for (auto& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool isFocusable = (tag == "input" || tag == "textarea" || tag == "select" || tag == "button");
            if (isFocusable) {
                // Skip hidden inputs
                auto* inp = getElInput(el);
                if (inp && inp->inputType(el) == layout::ElInput::InputType::Hidden)
                    isFocusable = false;
                // Skip disabled
                if (el->getAttribute("disabled") == "true" || el->attributes().count("disabled"))
                    isFocusable = false;
            }
            if (isFocusable) focusable.push_back(el);
        }
        for (auto* child : node->childNodes()) walk(child);
    };
    walk(body);

    if (focusable.empty()) return;

    // Find current active element
    auto* activeEl = document_->activeElement();
    int currentIdx = -1;
    for (int i = 0; i < static_cast<int>(focusable.size()); ++i) {
        if (focusable[i] == activeEl) { currentIdx = i; break; }
    }

    // Compute next index
    int nextIdx;
    if (reverse) {
        nextIdx = (currentIdx <= 0) ? static_cast<int>(focusable.size()) - 1 : currentIdx - 1;
    } else {
        nextIdx = (currentIdx < 0 || currentIdx >= static_cast<int>(focusable.size()) - 1) ? 0 : currentIdx + 1;
    }

    auto* nextEl = focusable[nextIdx];

    // Unfocus current
    if (activeEl) {
        // Tab out of a field that was typed in is a departure like any other,
        // and reports the edit before blur. The listener may redraw and take
        // the element Tab was heading for with it, so that one is held across
        // the dispatch — there is nothing to focus if it is gone.
        if (takeValueChange(activeEl)) {
            dom::ElementHandle keepNext(document_.get(), nextEl);
            dom::Event changeEvt("change");
            changeEvt.setIsTrusted(true);
            dispatchEvent(activeEl, changeEvt);
            nextEl = keepNext.get();
            if (!nextEl) { uiDirty_ = true; return; }
        }
        auto* prevInput = getElInput(activeEl);
        if (prevInput) prevInput->setFocused(false);
        auto* prevTa = getElTextarea(activeEl);
        if (prevTa) prevTa->setFocused(false);
        // Tab-advancing away from a <select> dismisses any open dropdown.
        if (getElSelect(activeEl)) overlayMgr_.close();
    }

    // Focus next
    document_->setActiveElement(nextEl);
    dispatchFocusEvents(activeEl, nextEl);
    armValueChange(nextEl);

    auto* newInput = getElInput(nextEl);
    auto* newTa = getElTextarea(nextEl);

    if (newInput) {
        newInput->setFocused(true);
        if (newInput->isTextType(nextEl)) {
            std::string v = nextEl->getAttribute("value");
            newInput->setCursorPos(static_cast<int>(v.size()));
            safeStartTextInput(window_.get());
        } else {
            safeStopTextInput(window_.get());
        }
    } else if (newTa) {
        newTa->setFocused(true);
        std::string v = nextEl->getAttribute("value");
        newTa->setCursorPos(static_cast<int>(v.size()));
        safeStartTextInput(window_.get());
    } else {
        safeStopTextInput(window_.get());
    }

    updateTextInputArea();
    uiDirty_ = true;
}

// ---------------------------------------------------------------------------
// Mouse wheel
// ---------------------------------------------------------------------------

void Engine::handleWheel(float x, float y, float dx, float dy) {
    if (!document_) return;

    if (overlayMgr_.handleWheel(x, overlayMouseY(y), dx, dy)) {
        uiDirty_ = true;
        return;
    }

    // System panels (menu bar, modals) take wheel input first. Scrolls a
    // panel-local overflow box if the pointer is over one, and fully swallows
    // the event while a modal is open so the app behind doesn't scroll.
    if (isSystemVisible() && systemHandleWheel(x, y, dx, dy)) {
        uiDirty_ = true;
        return;
    }

    float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;
    dom::Element* target = hitTest(docX, docY);

    // Convert raw SDL wheel delta to pixels once; reuse below for default
    // scroll and the JS wheel event. See util::wheelDeltaToPixels for why
    // this needs to distinguish classic ticks from precise/trackpad input.
    const float pxPerTick = inputConfig_.scrollSpeed;
    const float pxX = util::wheelDeltaToPixels(dx, pxPerTick);
    const float pxY = util::wheelDeltaToPixels(dy, pxPerTick);
    // Vertical-only default scroll: on macOS some trackpad configurations
    // deliver vertical gestures through the X channel. Use the dominant
    // axis so the engine's built-in scrolling matches native app behavior.
    const float pxV = util::wheelDeltaToPixels(
        util::verticalWheelDelta(dx, dy), pxPerTick);

    // Dispatch wheel event to JS
    if (target) {
        dom::WheelEvent wheelEvt("wheel", true, true);
        int mod = safeGetModState(window_.get(), heldModifierMask_);
        populateMouseEvent(wheelEvt, x, y, -1, pressedButtons_,
                          x - lastMouseX_, y - lastMouseY_, scrollY_, mod, static_cast<float>(contentTop()));
        // DOM convention: positive deltaY = scroll toward bottom of content.
        // SDL convention: positive wheel.y = scroll up (classic wheel-up).
        // Negate so the JS wheel event matches the browser contract.
        wheelEvt.setDeltaX(static_cast<double>(-pxX));
        wheelEvt.setDeltaY(static_cast<double>(-pxY));
        wheelEvt.setDeltaZ(0.0);
        wheelEvt.setDeltaMode(dom::WheelEvent::DOM_DELTA_PIXEL);
        applyMouseOffset(wheelEvt, target);
        dispatchEvent(target, wheelEvt);

        // If JS called preventDefault(), don't do default scrolling
        if (wheelEvt.defaultPrevented()) {
            if (jsRuntime_) jsRuntime_->executePendingJobs();
            return;
        }
    }

    // Check if mouse is over a focused textarea
    auto* activeEl = document_->activeElement();
    auto* textarea = getElTextarea(activeEl);
    if (textarea && textarea->isFocused()) {
        float scroll = textarea->scrollY() - pxV;
        scroll = std::max(scroll, 0.0f);
        textarea->setScrollY(scroll);
        markAppBaseDirty();
        return;
    }

    // Also allow scrolling textarea under mouse cursor (not just active one)
    auto* hoverTa = getElTextarea(target);
    if (hoverTa) {
        float scroll = hoverTa->scrollY() - pxV;
        scroll = std::max(scroll, 0.0f);
        hoverTa->setScrollY(scroll);
        markAppBaseDirty();
        return;
    }

    // Check if target or an ancestor is a scrollable overflow element.
    // Walk up the composed tree and, matching browser scroll chaining, let the
    // wheel fall through to the next scrollable ancestor whenever the current
    // element can't move in the wheel's direction — either because its content
    // fits (no scrollbar) or because it's already pinned at that edge. Without
    // this a nested overflow box (a fits-content region like an expanded
    // reasoning fold, or a list scrolled to its limit) would swallow the wheel
    // and the outer scroller never moved — scrolling felt "stuck" over those
    // regions while the gutter, sitting directly over the outer scroller,
    // worked fine.
    {
        auto* el = target;
        while (el) {
            std::string ov = getOverflowY(el->computedStyle());
            if (overflowScrollable(ov)) {
                float maxST = maxScrollTop(el);
                if (maxST > 0.0f) {
                    float prevScroll = el->scrollTopValue();
                    // pxV > 0 scrolls toward the top (scrollTop decreases);
                    // pxV < 0 scrolls toward the bottom (scrollTop increases).
                    // Only consume the wheel if there is room to move that way,
                    // otherwise chain to a scrollable ancestor.
                    const bool canScroll = (pxV > 0.0f) ? (prevScroll > 0.5f)
                                                        : (prevScroll < maxST - 0.5f);
                    if (canScroll) {
                        float newScroll = std::clamp(prevScroll - pxV, 0.0f, maxST);
                        el->setScrollTopValue(newScroll);
                        if (newScroll != prevScroll) {
                            dispatchScrollEvent(el);
                        }
                        markAppBaseDirty();
                        return;
                    }
                    // Pinned at this edge — fall through to an ancestor.
                }
                // maxST <= 0: content fits, this box isn't scrollable at all.
            }
            // overflow:hidden, non-scrollable, or at-edge: keep walking up so
            // the wheel reaches a scrollable ancestor (matches browser behavior).
            el = composedParent(el);
        }
    }

    // Viewport scrolling — push into the smoothing residual rather than
    // mutating scrollY_ directly. drainWheelSmoothing() eases it in over
    // the next few frames, which turns irregular macOS momentum events
    // into steady deceleration. The residual is unclamped here; the
    // drain clamps against the live document height each frame (so late
    // re-layouts don't leave us stuck past the bottom).
    wheelResidualY_ -= pxV;
    uiDirty_ = true;
}

void Engine::drainWheelSmoothing(float frameDtSec) {
    if (wheelResidualY_ == 0.0f) return;

    // Exponential ease: each frame apply a fraction of the residual.
    // Higher rate = snappier response / more momentum jitter passing
    // through; lower rate = smoother but floatier. ~60 gives ~63%/frame
    // at 60 fps — settles in 2–3 frames, nearly imperceptible lag on
    // steady swipes while taming irregular momentum tails.
    constexpr float kSmoothRate = 60.0f;
    float t = 1.0f - std::exp(-frameDtSec * kSmoothRate);
    if (t > 1.0f) t = 1.0f;

    // Snap the residual to scrollY_ once it's small enough to avoid
    // infinitely shrinking float tails.
    float apply = wheelResidualY_ * t;
    if (std::abs(wheelResidualY_) < 0.5f) {
        apply = wheelResidualY_;
        wheelResidualY_ = 0.0f;
    } else {
        wheelResidualY_ -= apply;
    }

    if (!document_) { wheelResidualY_ = 0.0f; return; }

    float maxScroll = std::max(0.0f, documentHeight_ - static_cast<float>(contentHeight()));
    float prevScroll = scrollY_;
    scrollY_ = std::clamp(scrollY_ + apply, 0.0f, maxScroll);
    if (scrollY_ == 0.0f || scrollY_ == maxScroll) {
        // Hit an edge — discard remaining residual so we don't fight it.
        wheelResidualY_ = 0.0f;
    }
    if (scrollY_ != prevScroll) {
        if (document_->documentElement()) {
            dispatchScrollEvent(document_->documentElement());
        }
        uiDirty_ = true;
    }
}

// ---------------------------------------------------------------------------
// File/text drop handling
// ---------------------------------------------------------------------------

void Engine::handleDropFile(const std::vector<std::string>& paths, float x, float y) {
    if (!document_) return;
    if (paths.empty()) return;

    // Use provided coordinates, fall back to last mouse position
    float dropX = (x >= 0) ? x : lastMouseX_;
    float dropY = (y >= 0) ? y : lastMouseY_;
    float docX = dropX, docY = dropY - static_cast<float>(contentTop()) + scrollY_;
    dom::Element* target = hitTest(docX, docY);
    if (!target) target = document_->body();
    if (!target) return;

    // Dispatch dragenter, dragover, then drop — one triple for the whole
    // gesture, each event carrying every dropped path in dataTransfer.files.
    for (const char* type : { "dragenter", "dragover", "drop" }) {
        dom::DragEvent evt(type, true, true);
        for (const auto& p : paths) evt.addFile(p);
        evt.setIsTrusted(true);
        dispatchEvent(target, evt);
    }
}

void Engine::handleDropText(const std::string& text, float x, float y) {
    if (!document_) return;

    float dropX = (x >= 0) ? x : lastMouseX_;
    float dropY = (y >= 0) ? y : lastMouseY_;
    float docX = dropX, docY = dropY - static_cast<float>(contentTop()) + scrollY_;
    dom::Element* target = hitTest(docX, docY);
    if (!target) target = document_->body();
    if (!target) return;

    dom::DragEvent enterEvt("dragenter", true, true);
    enterEvt.setDataText(text);
    enterEvt.setIsTrusted(true);
    dispatchEvent(target, enterEvt);

    dom::DragEvent overEvt("dragover", true, true);
    overEvt.setDataText(text);
    overEvt.setIsTrusted(true);
    dispatchEvent(target, overEvt);

    dom::DragEvent dropEvt("drop", true, true);
    dropEvt.setDataText(text);
    dropEvt.setIsTrusted(true);
    dispatchEvent(target, dropEvt);
}

// ---------------------------------------------------------------------------
// Clipboard simulation (for headless testing)
// ---------------------------------------------------------------------------

void Engine::simulatePaste(const std::string& text) {
    if (!document_) return;

    auto* activeEl = document_->activeElement();
    dom::Element* target = activeEl ? activeEl : document_->body();

    dom::ClipboardEvent pasteEvt("paste", true, true);
    pasteEvt.setClipboardText(text);
    if (!text.empty()) {
        pasteEvt.addItem({"text/plain", {}, text});
    }
    pasteEvt.setIsTrusted(true);
    dispatchEvent(target, pasteEvt);

    // If not prevented, insert into focused input/textarea. pasteText, not
    // handleTextInput: a paste is a discrete undo entry ("insertFromPaste").
    if (!pasteEvt.defaultPrevented() && !text.empty() && activeEl) {
        layout::KeyHandleResult r;
        if (auto* input = getElInput(activeEl); input && input->isFocused()) {
            r = input->pasteText(activeEl, text);
        } else if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
            r = textarea->pasteText(activeEl, text);
        }
        if (r.handled) {
            applyKeyResult(activeEl, r);
            uiDirty_ = true;
            return;
        }
    }

    // No form field took it — fall through to a contenteditable selection,
    // the same way the real Ctrl+V path does.
    if (pasteEvt.defaultPrevented() || text.empty()) return;
    auto* sel = document_->selection();
    if (!sel || sel->rangeCount() == 0) return;
    auto* fn = sel->focusNode();
    auto* host = fn ? editableHostOf(fn) : nullptr;
    if (!host) return;
    runEditableMutation(document_.get(), jsRuntime_.get(), fn,
        "insertFromPaste", text,
        [&] {
            EditUndoScope undo(&editUndo_, document_.get(), host, nullptr,
                               DomUndoStack::Kind::Discrete);
            selectionInsertText(document_.get(), text);
            undo.commit();
        });
    uiDirty_ = true;
}

std::string Engine::simulateCopy() {
    if (!document_) return "";

    auto* activeEl = document_->activeElement();
    dom::Element* target = activeEl ? activeEl : document_->body();

    // A focused field copies its *selected* text — a collapsed caret copies
    // nothing, as in a browser. Mirrors the Ctrl+C path in handleKeyDown.
    std::string text;
    bool fromFormField = false;
    if (activeEl) {
        if (auto* input = getElInput(activeEl); input && input->isFocused()) {
            text = input->selectedText();
            fromFormField = true;
        } else if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
            text = textarea->selectedText();
            fromFormField = true;
        }
    }
    // No focused form field: a DOM Selection inside a contenteditable host
    // copies instead, the same source simulateCut() takes its text from.
    // Copy reads and never mutates, so there is no cut/paste counterpart to
    // the deletion those two go on to do.
    if (!fromFormField) {
        auto* sel = document_->selection();
        if (sel && sel->rangeCount() > 0 && !sel->isCollapsed()) {
            auto* fn = sel->focusNode();
            if (fn && editableHostOf(fn)) text = sel->toString();
        }
    }

    dom::ClipboardEvent clipEvt("copy", true, true);
    clipEvt.setClipboardText(text);
    clipEvt.setIsTrusted(true);
    dispatchEvent(target, clipEvt);

    return text;
}

std::string Engine::simulateCut() {
    if (!document_) return "";

    auto* activeEl = document_->activeElement();
    dom::Element* target = activeEl ? activeEl : document_->body();

    // Cuts the selected range only — see simulateCopy. Mirrors Ctrl+X.
    std::string text;
    bool fromFormField = false;
    if (activeEl) {
        if (auto* input = getElInput(activeEl); input && input->isFocused()) {
            text = input->selectedText();
            fromFormField = true;
        } else if (auto* textarea = getElTextarea(activeEl); textarea && textarea->isFocused()) {
            text = textarea->selectedText();
            fromFormField = true;
        }
    }
    // No focused form field: a DOM Selection inside a contenteditable host
    // cuts instead, as it does on the real Ctrl+X path.
    auto* sel = document_->selection();
    dom::Element* ceHost = nullptr;
    dom::Node* ceFocus = nullptr;
    if (!fromFormField && sel && sel->rangeCount() > 0 && !sel->isCollapsed()) {
        ceFocus = sel->focusNode();
        ceHost = ceFocus ? editableHostOf(ceFocus) : nullptr;
        if (ceHost) text = sel->toString();
    }

    dom::ClipboardEvent clipEvt("cut", true, true);
    clipEvt.setClipboardText(text);
    clipEvt.setIsTrusted(true);
    dispatchEvent(target, clipEvt);

    if (!clipEvt.defaultPrevented() && !text.empty() && activeEl) {
        bool cut = false;
        if (auto* input = getElInput(activeEl)) {
            cut = input->cutSelection(activeEl);
        } else if (auto* textarea = getElTextarea(activeEl)) {
            cut = textarea->cutSelection(activeEl);
        }
        if (cut) {
            dom::InputEvent inputEvt("input", true, false);
            inputEvt.setInputType("deleteByCut");
            inputEvt.setIsTrusted(true);
            dispatchEvent(activeEl, inputEvt);
            if (activeEl->document()) activeEl->document()->markDirty();
            uiDirty_ = true;
        }
    }

    if (!clipEvt.defaultPrevented() && !text.empty() && ceHost) {
        runEditableMutation(document_.get(), jsRuntime_.get(), ceFocus,
            "deleteByCut", "",
            [&] {
                EditUndoScope undo(&editUndo_, document_.get(), ceHost, nullptr,
                                   DomUndoStack::Kind::Discrete);
                auto* range = sel->getRangeAt(0);
                if (!range) return;
                dom::Node* after = nullptr; int afterOff = 0;
                deleteRangeContents(document_.get(), *range, after, afterOff);
                if (after) sel->collapse(after, afterOff);
                undo.commit();
            });
        uiDirty_ = true;
    }

    return text;
}

// ---------------------------------------------------------------------------
// World-space HtmlNode mouse routing
// ---------------------------------------------------------------------------

#if BRO_WITH_3D
scene::SceneGraph* Engine::sceneGraphForElement(const dom::Element* el) const {
    if (!el) return nullptr;
    for (auto& sg : sceneGraphs_) {
        if (sg.element == el && sg.graph) return sg.graph.get();
    }
    return nullptr;
}
#endif  // BRO_WITH_3D

bool Engine::elementAbsoluteOrigin(dom::Element* el, float& outX, float& outY) const {
    if (!el) return false;
    dom::AbsolutePoint p = dom::absoluteContentOrigin(el);
    outX = p.x;
    outY = p.y;
    return true;
}

#if BRO_WITH_3D
bool Engine::pickHtmlNodeUnderMouse(dom::Element* canvasEl, float docX, float docY,
                                    scene::HtmlNode*& outNode, dom::Element*& outEl,
                                    float& outLocalPxX, float& outLocalPxY) {
    outNode = nullptr;
    outEl = nullptr;
    auto* sg = sceneGraphForElement(canvasEl);
    if (!sg) return false;

    float originX = 0.0f, originY = 0.0f;
    if (!elementAbsoluteOrigin(canvasEl, originX, originY)) return false;
    const float canvasLocalX = docX - originX;
    const float canvasLocalY = docY - originY;

    scene::SceneGraph::HtmlNodePick pick;
    if (!sg->pickHtmlNode(canvasLocalX, canvasLocalY, pick)) return false;
    if (!pick.node) return false;

    auto* doc = pick.node->document();
    if (!doc) return false;
    auto* root = doc->layoutRoot();
    if (!root) return false;

    auto* layoutNode = htmlayout::layout::hitTest(root, pick.localPxX, pick.localPxY);
    auto* hitEl = layout::LayoutNodeAdapter::elementFor(layoutNode);
    if (!hitEl) hitEl = doc->documentElement();
    if (!hitEl) return false;

    outNode = pick.node;
    outEl = hitEl;
    outLocalPxX = pick.localPxX;
    outLocalPxY = pick.localPxY;
    return true;
}

void Engine::dispatchHtmlNodeMouseEvent(const std::string& type,
                                        dom::Element* target,
                                        float localPxX, float localPxY,
                                        int button, int pressedButtons, int mods,
                                        float movX, float movY, bool bubbles,
                                        dom::Element* relatedTarget) {
    if (!target) return;
    dom::MouseEvent evt(type, bubbles, /*cancelable=*/true);
    evt.setIsTrusted(true);
    evt.setClientX(static_cast<double>(localPxX));
    evt.setClientY(static_cast<double>(localPxY));
    evt.setScreenX(static_cast<double>(localPxX));
    evt.setScreenY(static_cast<double>(localPxY));
    evt.setPageX(static_cast<double>(localPxX));
    evt.setPageY(static_cast<double>(localPxY));
    evt.setMovementX(static_cast<double>(movX));
    evt.setMovementY(static_cast<double>(movY));
    evt.setButton(button);
    evt.setButtons(pressedButtons);
    evt.setShiftKey((mods & SDL_KMOD_SHIFT) != 0);
    evt.setCtrlKey ((mods & SDL_KMOD_CTRL ) != 0);
    evt.setAltKey  ((mods & SDL_KMOD_ALT  ) != 0);
    evt.setMetaKey ((mods & SDL_KMOD_GUI  ) != 0);
    if (relatedTarget) evt.setRelatedTarget(relatedTarget);

    // offsetX/Y is the same as clientX/Y here — the inner document's layout
    // origin matches the raster surface origin.
    evt.setOffsetX(static_cast<double>(localPxX));
    evt.setOffsetY(static_cast<double>(localPxY));

    // Pointer parity: fire the matching pointer event just before the mouse one
    // so listeners inside HtmlNode documents see pointerdown/up/move too.
    const char* pointerType = (type == "mousedown") ? "pointerdown"
                            : (type == "mouseup")   ? "pointerup"
                            : (type == "mousemove") ? "pointermove"
                            : nullptr;
    if (pointerType) dispatchPointerAlias(pointerType, target, evt);

    dispatchEvent(target, evt);
    if (jsRuntime_) jsRuntime_->executePendingJobs();
}
#endif  // BRO_WITH_3D

} // namespace bro::engine
