#pragma once

// Undo/redo history for contenteditable hosts — the DOM-tree counterpart of
// layout::TextUndoStack, deliberately sharing its Kind taxonomy and its
// coalescing rules so the two editing surfaces behave identically to a user.
//
// WHY A SECOND STACK AND NOT THE SAME ONE
// ---------------------------------------
// TextUndoStack addresses one flat UTF-8 value by byte offset. A
// contenteditable edit is a splice into a *tree*: it can span text nodes,
// split or merge them, insert or remove elements, and leave the caret in a
// different node than it started in. So the entry has to carry DOM positions.
// Everything else — the Kind taxonomy, the merge rules, the coalescing
// window, the refuse-rather-than-corrupt policy on a mismatched restore — is
// deliberately identical, because a user who types in a <textarea> and then
// in a contenteditable div must get the same undo granularity in both.
//
// ENTRY REPRESENTATION: two shapes, chosen per edit site
// ------------------------------------------------------
// 1. TextSplice — {node path, byte pos, removed, inserted}. Used when the
//    whole edit lives inside ONE text node's data: plain typing, a backspace
//    or forward-delete inside a node, an IME commit that doesn't restructure.
//    Cost is proportional to the characters actually typed, exactly like
//    TextUndoStack, and it merges under the same three rules. This is the
//    hot path, and it is the only shape that ever coalesces.
//
// 2. Structural — {host, innerHTML before, innerHTML after}. Used when the
//    edit changes the shape of the tree: Enter (<br> insertion, which splits
//    a text node), paste, cut, typing over a selection, a composition that
//    replaced a selection. These are ALL Kind::Discrete, which by definition
//    never coalesces — so the snapshot cost is paid exactly where merging
//    would not have helped anyway, and never once per keystroke.
//
//    Rejected alternatives: a mutation-record list would have to intercept
//    every DOM write, including the ones script makes between edits, and
//    replaying inverse operations across those is exactly the ambiguity this
//    design refuses. A subtree snapshot for *every* edit (no shape 1) would
//    make one keystroke cost a serialization of the whole host.
//
// POSITIONS: child-index paths, not node pointers
// -----------------------------------------------
// Restoring a Structural entry re-parses the host's children, so the nodes an
// entry recorded are gone by the time a later entry is restored. Both shapes
// therefore address nodes as a chain of child indices from the host, plus an
// offset within the resolved container (byte offset in a text node, child
// index in an element) — the same convention Range uses. A path that no
// longer resolves is treated as a failed restore, never as a guess.
//
// WHEN SCRIPT MUTATES THE TREE UNDER A PENDING ENTRY
// --------------------------------------------------
// The stack REFUSES and drops its whole history. Before touching anything it
// verifies that the DOM still looks the way the entry left it: a TextSplice
// checks that its path resolves to a text node whose bytes at `pos` are
// still exactly `inserted`; a Structural entry checks that the host still
// serializes to `afterHTML`. On any mismatch it calls clear() and reports
// false, changing nothing. Silently splicing into a tree script has moved
// underneath us would corrupt the document, which is strictly worse than the
// user finding undo unavailable. This mirrors TextUndoStack::undo(), which
// drops its history the same way when the value no longer matches.
//
// COALESCING IS SELF-POLICING
// ---------------------------
// TextUndoStack needs an explicit breakCoalescing() call at every site that
// moves the caret. There is no such chokepoint for contenteditable — script,
// arrow keys and mouse all move a shared dom::Selection. So instead of
// hunting for hooks, an entry only merges when the selection the edit STARTED
// from is exactly where the previous edit LEFT the caret. An arrow key, a
// click, or a script selection change all fail that test automatically, which
// reproduces the controls' behavior without depending on being notified.

#include "dom/document.h"
#include "dom/element.h"
#include "dom/node_handle.h"
#include "dom/selection.h"
#include "dom/text_node.h"
#include "layout/text_undo.h"

#include <algorithm>
#include <string>
#include <vector>

namespace bro::engine {

class DomUndoStack {
public:
    // The taxonomy is shared with the controls on purpose — see the header
    // comment. Typing/Backspace/DeleteForward may merge; Discrete never does.
    using Kind = layout::TextUndoStack::Kind;

    // A DOM position as a child-index path from the host plus an offset in
    // the resolved container. `valid` distinguishes "no position recorded"
    // from "the host itself, offset 0".
    struct Pos {
        std::vector<int> path;
        int offset = 0;
        bool valid = false;

        bool operator==(const Pos& o) const {
            return valid == o.valid && offset == o.offset && path == o.path;
        }
        bool operator!=(const Pos& o) const { return !(*this == o); }
    };

    // Anchor (pinned end) + focus (moving end), matching dom::Selection.
    struct Sel {
        Pos anchor, focus;
        bool operator==(const Sel& o) const {
            return anchor == o.anchor && focus == o.focus;
        }
        bool operator!=(const Sel& o) const { return !(*this == o); }
    };

    // --- Position helpers -------------------------------------------------

    // The child-index path from `host` down to `node`. Invalid when `node`
    // is not `host` or a descendant of it.
    static Pos posOf(dom::Element* host, dom::Node* node, int offset) {
        Pos p;
        if (!host || !node) return p;
        std::vector<int> rev;
        dom::Node* cur = node;
        while (cur && cur != host) {
            dom::Node* parent = cur->parentNode();
            if (!parent) return p;               // detached — not addressable
            const auto& kids = parent->childNodes();
            int idx = -1;
            for (size_t i = 0; i < kids.size(); ++i) {
                if (kids[i] == cur) { idx = static_cast<int>(i); break; }
            }
            if (idx < 0) return p;
            rev.push_back(idx);
            cur = parent;
        }
        if (cur != host) return p;               // node was outside the host
        p.path.assign(rev.rbegin(), rev.rend());
        p.offset = offset;
        p.valid = true;
        return p;
    }

    // Walk a path back to a live node, or nullptr if it no longer resolves.
    static dom::Node* resolve(dom::Element* host, const Pos& p) {
        if (!host || !p.valid) return nullptr;
        dom::Node* cur = host;
        for (int idx : p.path) {
            if (!cur) return nullptr;
            const auto& kids = cur->childNodes();
            if (idx < 0 || idx >= static_cast<int>(kids.size())) return nullptr;
            cur = kids[idx];
        }
        return cur;
    }

    // Put the selection back where `s` recorded it. A path that no longer
    // resolves leaves the selection alone rather than pointing it somewhere
    // arbitrary.
    static void restoreSelection(dom::Document* doc, dom::Element* host,
                                 const Sel& s) {
        if (!doc || !host) return;
        auto* sel = doc->selection();
        if (!sel || !s.anchor.valid || !s.focus.valid) return;
        dom::Node* a = resolve(host, s.anchor);
        dom::Node* f = resolve(host, s.focus);
        if (!a || !f) return;
        sel->setRange(a, s.anchor.offset, f, s.focus.offset);
    }

    // Read the document's current selection as a host-relative Sel. Invalid
    // when there is no selection or it lies outside `host`.
    static Sel selectionOf(dom::Document* doc, dom::Element* host) {
        Sel s;
        if (!doc || !host) return s;
        auto* sel = doc->selection();
        if (!sel || sel->rangeCount() == 0) return s;
        s.anchor = posOf(host, sel->anchorNode(), sel->anchorOffset());
        s.focus  = posOf(host, sel->focusNode(), sel->focusOffset());
        return s;
    }

    // --- Recording --------------------------------------------------------

    // A text-local edit: `tn` is the node, `beforeData`/`afterData` its data
    // either side of the edit. The contiguous delta between them is the
    // entry, computed the same way TextUndoStack computes it.
    void recordTextEdit(dom::Element* host, dom::TextNode* tn,
                        const std::string& beforeData,
                        const std::string& afterData,
                        const Sel& selBefore, const Sel& selAfter,
                        Kind kind, double nowMs) {
        if (!host || !tn) return;
        const Pos nodePos = posOf(host, tn, 0);
        if (!nodePos.valid) return;

        size_t p = 0, sfx = 0;
        splice(beforeData, afterData, p, sfx);
        std::string removed = beforeData.substr(p, beforeData.size() - sfx - p);
        std::string inserted = afterData.substr(p, afterData.size() - sfx - p);
        if (removed.empty() && inserted.empty()) return;

        dropRedoTail();

        const int pos = static_cast<int>(p);
        if (canMerge(kind, selBefore, nowMs)) {
            Entry& e = entries_.back();
            if (e.isText && e.nodePath == nodePos.path && e.kind == kind) {
                if (kind == Kind::Typing && removed.empty() &&
                    pos == e.pos + static_cast<int>(e.inserted.size())) {
                    e.inserted += inserted;
                    e.selAfter = selAfter;
                    bytes_ += inserted.size();
                    lastEditMs_ = nowMs;
                    return;
                }
                if (kind == Kind::Backspace && inserted.empty() &&
                    pos + static_cast<int>(removed.size()) == e.pos) {
                    e.pos = pos;
                    e.removed.insert(0, removed);
                    e.selAfter = selAfter;
                    bytes_ += removed.size();
                    lastEditMs_ = nowMs;
                    return;
                }
                if (kind == Kind::DeleteForward && inserted.empty() &&
                    pos == e.pos) {
                    e.removed += removed;
                    e.selAfter = selAfter;
                    bytes_ += removed.size();
                    lastEditMs_ = nowMs;
                    return;
                }
            }
        }

        Entry e;
        e.isText = true;
        e.kind = kind;
        e.nodePath = nodePos.path;
        e.pos = pos;
        e.removed = std::move(removed);
        e.inserted = std::move(inserted);
        e.selBefore = selBefore;
        e.selAfter = selAfter;
        push(std::move(e), nowMs);
    }

    // A structural edit: the host's serialized children either side of it.
    // Always Discrete — these never merge with anything.
    void recordStructural(dom::Element* host,
                          const std::string& beforeHTML,
                          const std::string& afterHTML,
                          const Sel& selBefore, const Sel& selAfter,
                          double nowMs) {
        if (!host || beforeHTML == afterHTML) return;
        dropRedoTail();
        Entry e;
        e.isText = false;
        e.kind = Kind::Discrete;
        e.beforeHTML = beforeHTML;
        e.afterHTML = afterHTML;
        e.selBefore = selBefore;
        e.selAfter = selAfter;
        push(std::move(e), nowMs);
    }

    // --- Applying ---------------------------------------------------------

    // Revert the newest undoable edit in `host`, restoring the pre-edit
    // selection. False (changing nothing) when there is nothing to undo, or
    // when the DOM no longer matches the entry — in which case the whole
    // history is dropped rather than corrupting the tree.
    bool undo(dom::Document* doc, dom::Element* host) {
        if (cursor_ == 0) return false;
        const Entry& e = entries_[cursor_ - 1];
        if (!apply(doc, host, e, /*forward=*/false)) return false;
        --cursor_;
        return true;
    }

    // Re-apply the newest undone edit, restoring its post-edit selection.
    bool redo(dom::Document* doc, dom::Element* host) {
        if (cursor_ >= entries_.size()) return false;
        const Entry& e = entries_[cursor_];
        if (!apply(doc, host, e, /*forward=*/true)) return false;
        ++cursor_;
        return true;
    }

    bool canUndo() const { return cursor_ > 0; }
    bool canRedo() const { return cursor_ < entries_.size(); }

    // The next edit starts a new entry even if it would otherwise merge.
    // Selection comparison already covers caret moves; this is for the
    // events that don't move the caret, like focus leaving the host.
    void breakCoalescing() { coalesce_ = false; }

    void clear() {
        entries_.clear();
        cursor_ = 0;
        bytes_ = 0;
        coalesce_ = false;
    }

private:
    struct Entry {
        bool isText = false;
        Kind kind = Kind::Discrete;

        // TextSplice
        std::vector<int> nodePath;
        int pos = 0;
        std::string removed, inserted;

        // Structural
        std::string beforeHTML, afterHTML;

        Sel selBefore, selAfter;

        size_t weight() const {
            return removed.size() + inserted.size() +
                   beforeHTML.size() + afterHTML.size();
        }
    };

    static constexpr size_t kMaxEntries = 200;
    static constexpr size_t kMaxBytes = 1u << 20;
    static constexpr double kCoalesceWindowMs = 1000.0;

    // Common prefix/suffix of two strings — the contiguous splice between
    // them. Same computation TextUndoStack::record does.
    static void splice(const std::string& before, const std::string& after,
                       size_t& prefix, size_t& suffix) {
        const size_t bn = before.size(), an = after.size();
        const size_t pmax = std::min(bn, an);
        size_t p = 0;
        while (p < pmax && before[p] == after[p]) ++p;
        size_t sfx = 0;
        const size_t smax = pmax - p;
        while (sfx < smax && before[bn - 1 - sfx] == after[an - 1 - sfx]) ++sfx;
        prefix = p;
        suffix = sfx;
    }

    // A merge is allowed only when the edit starts exactly where the previous
    // one left the caret — see the header comment on self-policing.
    bool canMerge(Kind kind, const Sel& selBefore, double nowMs) const {
        return coalesce_ && !entries_.empty() && kind != Kind::Discrete &&
               nowMs - lastEditMs_ <= kCoalesceWindowMs &&
               entries_.back().selAfter == selBefore;
    }

    void dropRedoTail() {
        if (cursor_ >= entries_.size()) return;
        for (size_t i = cursor_; i < entries_.size(); ++i)
            bytes_ -= entries_[i].weight();
        entries_.resize(cursor_);
    }

    void push(Entry&& e, double nowMs) {
        bytes_ += e.weight();
        entries_.push_back(std::move(e));
        cursor_ = entries_.size();
        coalesce_ = true;
        lastEditMs_ = nowMs;
        // Oldest entries drop first. A single oversized entry is kept —
        // dropping it would leave the user with no undo at all.
        while (entries_.size() > 1 &&
               (entries_.size() > kMaxEntries || bytes_ > kMaxBytes)) {
            bytes_ -= entries_.front().weight();
            entries_.erase(entries_.begin());
            --cursor_;
        }
    }

    // Undo and redo are the same operation in opposite directions.
    bool apply(dom::Document* doc, dom::Element* host, const Entry& e,
               bool forward) {
        coalesce_ = false;
        if (!doc || !host) return false;

        const std::string& expect = e.isText ? (forward ? e.removed : e.inserted)
                                             : (forward ? e.beforeHTML : e.afterHTML);
        const std::string& want   = e.isText ? (forward ? e.inserted : e.removed)
                                             : (forward ? e.afterHTML : e.beforeHTML);

        if (e.isText) {
            Pos np;
            np.path = e.nodePath;
            np.valid = true;
            dom::Node* n = resolve(host, np);
            if (!n || n->nodeType() != dom::NodeType::Text) { clear(); return false; }
            auto* tn = static_cast<dom::TextNode*>(n);
            const std::string& data = tn->data();
            if (e.pos < 0 ||
                static_cast<size_t>(e.pos) + expect.size() > data.size() ||
                data.compare(static_cast<size_t>(e.pos), expect.size(), expect) != 0) {
                clear();
                return false;
            }
            tn->replaceData(static_cast<size_t>(e.pos), expect.size(), want);
        } else {
            if (host->innerHTML() != expect) { clear(); return false; }
            host->setInnerHTML(want);
            host->markStructureDirty();
        }

        restoreSelection(doc, host, forward ? e.selAfter : e.selBefore);
        doc->markDirty();
        return true;
    }

    std::vector<Entry> entries_;
    size_t cursor_ = 0;
    size_t bytes_ = 0;
    bool coalesce_ = false;
    double lastEditMs_ = 0.0;
};

// Per-host histories. contenteditable has no per-element C++ object to hang
// a stack off (unlike ElInput/ElTextarea), so the engine keeps a small set
// keyed by host element. Tabbing between two editable divs must not throw
// either one's history away — a <textarea> keeps its history across a focus
// round-trip, and the two surfaces have to match. Least-recently-used hosts
// are evicted past kMaxHosts; a dead host's entry is reclaimed on the next
// lookup.
class DomUndoHistories {
public:
    DomUndoStack& forHost(dom::Document* doc, dom::Element* host) {
        reap();
        for (size_t i = 0; i < hosts_.size(); ++i) {
            if (hosts_[i].host.get() == host) {
                touch(i);
                return hosts_.front().stack;
            }
        }
        if (hosts_.size() >= kMaxHosts) hosts_.pop_back();
        hosts_.insert(hosts_.begin(), Slot{});
        hosts_.front().host.assign(doc, host);
        return hosts_.front().stack;
    }

    // The stack for `host` if one already exists, else nullptr — for the
    // key handler, which must not mint a history just by asking.
    DomUndoStack* find(dom::Element* host) {
        reap();
        for (auto& s : hosts_)
            if (s.host.get() == host) return &s.stack;
        return nullptr;
    }

    void clear() { hosts_.clear(); }

private:
    struct Slot {
        dom::ElementHandle host;
        DomUndoStack stack;
    };
    static constexpr size_t kMaxHosts = 8;

    void reap() {
        hosts_.erase(std::remove_if(hosts_.begin(), hosts_.end(),
                                    [](Slot& s) { return !s.host.get(); }),
                     hosts_.end());
    }

    void touch(size_t i) {
        if (i == 0) return;
        std::rotate(hosts_.begin(), hosts_.begin() + i, hosts_.begin() + i + 1);
    }

    std::vector<Slot> hosts_;
};

} // namespace bro::engine
