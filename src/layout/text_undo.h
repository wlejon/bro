#pragma once

// Per-control text-editing undo/redo history, shared by ElInput and ElTextarea.
//
// Every entry is a single contiguous splice — {pos, removed, inserted} — plus
// the selection before and after the edit, computed as the common-prefix/suffix
// delta between the value before and after a completed edit. Deltas (not full
// value snapshots) so a long document being appended to costs one character per
// entry, not a copy of the whole value; every edit the controls perform is one
// contiguous splice, so the delta is always exact.
//
// Coalescing: a run of plain character insertions at the caret merges into one
// entry, as does a run of backspaces or a run of forward-deletes. A run is
// broken by anything that moves the caret or selection between edits
// (arrow keys, mouse, JS setSelectionRange), by focus loss, by a pause longer
// than kCoalesceWindowMs, or by a Discrete edit (paste, cut, typing over a
// selection, spinner steps, Enter in a textarea) — those always stand alone.
//
// Offsets are byte offsets into the UTF-8 value, matching the controls'
// selectionStart/selectionEnd convention, so an undo restores the exact
// selection bytes the edit started from.

#include <algorithm>
#include <string>
#include <vector>

namespace bro::layout {

class TextUndoStack {
public:
    // How an edit came about — drives coalescing. Discrete never merges.
    enum class Kind { Typing, Backspace, DeleteForward, Discrete };

    // A control selection: anchor (pinned end) + caret (moving end), bytes.
    struct Sel { int anchor = 0; int caret = 0; };

    // Record one completed edit as the delta between `before` and `after`.
    // No-op when the value didn't change. Any new edit drops the redo tail.
    void record(const std::string& before, Sel selBefore,
                const std::string& after, Sel selAfter,
                Kind kind, double nowMs) {
        // Contiguous delta via common prefix/suffix.
        const size_t bn = before.size(), an = after.size();
        const size_t pmax = std::min(bn, an);
        size_t p = 0;
        while (p < pmax && before[p] == after[p]) ++p;
        size_t sfx = 0;
        const size_t smax = pmax - p;
        while (sfx < smax && before[bn - 1 - sfx] == after[an - 1 - sfx]) ++sfx;
        std::string removed = before.substr(p, bn - sfx - p);
        std::string inserted = after.substr(p, an - sfx - p);
        if (removed.empty() && inserted.empty()) return;

        // A fresh edit invalidates everything that was undone.
        if (cursor_ < entries_.size()) {
            for (size_t i = cursor_; i < entries_.size(); ++i)
                bytes_ -= entries_[i].removed.size() + entries_[i].inserted.size();
            entries_.resize(cursor_);
        }

        const int pos = static_cast<int>(p);
        if (coalesce_ && !entries_.empty() && kind != Kind::Discrete &&
            entries_.back().kind == kind && nowMs - lastEditMs_ <= kCoalesceWindowMs) {
            Entry& e = entries_.back();
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
            if (kind == Kind::DeleteForward && inserted.empty() && pos == e.pos) {
                e.removed += removed;
                e.selAfter = selAfter;
                bytes_ += removed.size();
                lastEditMs_ = nowMs;
                return;
            }
        }

        bytes_ += removed.size() + inserted.size();
        entries_.push_back(Entry{pos, std::move(removed), std::move(inserted),
                                 selBefore, selAfter, kind});
        cursor_ = entries_.size();
        coalesce_ = true;
        lastEditMs_ = nowMs;

        // Caps: oldest entries drop first. A single oversized entry is kept —
        // dropping it would leave the user with no undo at all.
        while (entries_.size() > 1 &&
               (entries_.size() > kMaxEntries || bytes_ > kMaxBytes)) {
            bytes_ -= entries_.front().removed.size() + entries_.front().inserted.size();
            entries_.erase(entries_.begin());
            --cursor_;
        }
    }

    // Revert the newest undoable edit in `val`, restoring the pre-edit
    // selection into `sel`. False (and no writes) when there is nothing to
    // undo. If `val` no longer matches the entry — an out-of-band write that
    // bypassed clear() — the whole history is dropped instead of corrupting
    // the value.
    bool undo(std::string& val, Sel& sel) {
        coalesce_ = false;
        if (cursor_ == 0) return false;
        const Entry& e = entries_[cursor_ - 1];
        if (e.pos < 0 || static_cast<size_t>(e.pos) + e.inserted.size() > val.size() ||
            val.compare(static_cast<size_t>(e.pos), e.inserted.size(), e.inserted) != 0) {
            clear();
            return false;
        }
        val.replace(static_cast<size_t>(e.pos), e.inserted.size(), e.removed);
        sel = e.selBefore;
        --cursor_;
        return true;
    }

    // Re-apply the newest undone edit, restoring its post-edit selection.
    bool redo(std::string& val, Sel& sel) {
        coalesce_ = false;
        if (cursor_ >= entries_.size()) return false;
        const Entry& e = entries_[cursor_];
        if (e.pos < 0 || static_cast<size_t>(e.pos) + e.removed.size() > val.size() ||
            val.compare(static_cast<size_t>(e.pos), e.removed.size(), e.removed) != 0) {
            clear();
            return false;
        }
        val.replace(static_cast<size_t>(e.pos), e.removed.size(), e.inserted);
        sel = e.selAfter;
        ++cursor_;
        return true;
    }

    bool canUndo() const { return cursor_ > 0; }
    bool canRedo() const { return cursor_ < entries_.size(); }

    // The next edit starts a new entry even if it would otherwise merge.
    void breakCoalescing() { coalesce_ = false; }

    // Drop the whole history (programmatic value writes).
    void clear() {
        entries_.clear();
        cursor_ = 0;
        bytes_ = 0;
        coalesce_ = false;
    }

private:
    struct Entry {
        int pos = 0;                    // byte offset of the splice
        std::string removed;            // text the edit removed at pos
        std::string inserted;           // text the edit inserted at pos
        Sel selBefore;                  // selection to restore on undo
        Sel selAfter;                   // selection to restore on redo
        Kind kind = Kind::Discrete;
    };

    static constexpr size_t kMaxEntries = 200;
    static constexpr size_t kMaxBytes = 1u << 20;   // ~1 MB of delta text
    static constexpr double kCoalesceWindowMs = 1000.0;

    std::vector<Entry> entries_;
    size_t cursor_ = 0;     // entries_[0..cursor_) undoable, [cursor_..) redoable
    size_t bytes_ = 0;      // total removed+inserted bytes across entries_
    bool coalesce_ = false; // the last record() may accept a merge
    double lastEditMs_ = 0.0;
};

// IME composition (preedit) state carried by a text control while the user
// composes. The preedit lives INSIDE the value as provisional text (browser
// behavior: `.value` shows it and input events fire during composition),
// replaced on every TEXT_EDITING update, finalized on commit and removed on
// cancel. No undo entries are recorded while composing; the commit records
// ONE discrete entry spanning pre-composition state → committed state, so a
// single Ctrl+Z removes the whole committed run (and a cancel leaves no
// entry at all). Offsets are byte offsets into the UTF-8 value, matching the
// controls' selection convention.
struct TextComposition {
    bool active = false;
    int start = 0;            // byte offset of the preedit in the value
    int length = 0;           // byte length of the current preedit
    std::string preedit;      // current preedit text (value[start, start+length))
    std::string beforeVal;    // value at composition start (undo snapshot)
    TextUndoStack::Sel selBefore{};  // selection at composition start
};

} // namespace bro::layout
