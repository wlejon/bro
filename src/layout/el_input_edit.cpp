// Text editing for <input>: keys, typing/paste, IME composition, caret
// movement and hit-testing, selection, and the undo history behind them.
#include "layout/el_input.h"
#include "layout/control_text.h"
#include "dom/element.h"
#include "util/platform.h"
#include "util/time.h"

#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bro::layout {

void ElInput::armChange(dom::Element* el) {
    change_.arm(el ? el->getAttribute("value") : std::string());
}

void ElInput::spliceValue(dom::Element* el, const std::string& value,
                          int selStart, int selEnd) {
    if (!el) return;
    const std::string before = el->getAttribute("value");
    const TextUndoStack::Sel selBefore{sel_.anchor, sel_.caret};
    comp_ = {};   // the script owns the value now
    el->setAttribute("value", value);
    setSelectionRange(selStart, selEnd);
    undo_.record(before, selBefore, value, {sel_.anchor, sel_.caret},
                 TextUndoStack::Kind::Discrete, util::currentTimeMs());
    armChange(el);
}

bool ElInput::takeChange(dom::Element* el) {
    if (!el || !isTextType(el)) return false;
    return change_.take(el->getAttribute("value"));
}

// ---------------------------------------------------------------------------
// Key handling
// ---------------------------------------------------------------------------

KeyHandleResult ElInput::handleKeyDown(dom::Element* el, int keycode, int mod) {
    KeyHandleResult r;
    auto itype = inputType(el);

    // Checkbox/radio: space toggles
    if ((itype == InputType::Checkbox || itype == InputType::Radio)
        && keycode == SDLK_SPACE) {
        if (itype == InputType::Checkbox) {
            if (el->hasAttribute("checked"))
                el->removeAttribute("checked");
            else
                el->setAttribute("checked", "");
        } else {
            el->setAttribute("checked", "");
        }
        r.handled = true;
        r.dispatchChange = true;
        r.dispatchInput = true;
        return r;
    }

    // Range: arrow keys adjust value
    if (itype == InputType::Range) {
        if (keycode == SDLK_LEFT || keycode == SDLK_DOWN) {
            float v = std::clamp(rangeValue() - rangeStep(), rangeMin(), rangeMax());
            char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
            el->setAttribute("value", buf);
            r.handled = true;
            r.dispatchInput = true;
            return r;
        }
        if (keycode == SDLK_RIGHT || keycode == SDLK_UP) {
            float v = std::clamp(rangeValue() + rangeStep(), rangeMin(), rangeMax());
            char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
            el->setAttribute("value", buf);
            r.handled = true;
            r.dispatchInput = true;
            return r;
        }
    }

    // Non-text types: nothing more to handle
    if (!isTextType(el)) return r;

    // Text editing
    std::string val = el->getAttribute("value");
    const int len = static_cast<int>(val.size());
    sel_.clampTo(len);
    const int pos = sel_.caret;
    const bool shift = (mod & SDL_KMOD_SHIFT) != 0;

    // Shift moves the caret end only, leaving the anchor pinned — that is what
    // grows a selection. Without shift the caret and anchor move together.
    auto moveCaret = [&](int to) {
        if (shift) sel_.caret = to;
        else sel_.collapseTo(to);
    };

    // Undo / redo: primary+Z undoes; primary+Y or primary+shift+Z redoes.
    // Handled before the editing branches (and returning early) so the
    // recording chokepoint at the bottom never sees these as fresh edits.
    // Empty-stack undo and spent redo are handled no-ops.
    if (util::hasPrimaryMod(mod) && (keycode == SDLK_Z || keycode == SDLK_Y)) {
        const bool isRedo = (keycode == SDLK_Y) || shift;
        std::string v = val;
        TextUndoStack::Sel s{sel_.anchor, sel_.caret};
        if (isRedo ? undo_.redo(v, s) : undo_.undo(v, s)) {
            el->setAttribute("value", v);
            sel_.set(s.anchor, s.caret);
            sel_.clampTo(static_cast<int>(v.size()));
            r.dispatchInput = true;
            r.inputType = isRedo ? "historyRedo" : "historyUndo";
        }
        r.handled = true;
        return r;
    }

    // Snapshot for the history recorder at the bottom. `kind` drives
    // coalescing: only the single-character delete paths set a mergeable kind;
    // everything else (selection deletes, spinner steps) stands alone.
    const std::string beforeVal = val;
    const TextUndoStack::Sel selBefore{sel_.anchor, sel_.caret};
    TextUndoStack::Kind kind = TextUndoStack::Kind::Discrete;

    if (keycode == SDLK_BACKSPACE) {
        if (deleteSelection_(val, r.inputData)) {
            el->setAttribute("value", val);
            r.dispatchInput = true;
            r.inputType = "deleteContentBackward";
        } else if (pos > 0) {
            int prev = utf8Prev(val, pos);
            r.inputData = val.substr(prev, pos - prev);
            val.erase(prev, pos - prev);
            setCursorPos(prev);
            el->setAttribute("value", val);
            r.dispatchInput = true;
            r.inputType = "deleteContentBackward";
            kind = TextUndoStack::Kind::Backspace;
        }
        r.handled = true;
    } else if (keycode == SDLK_DELETE) {
        if (deleteSelection_(val, r.inputData)) {
            el->setAttribute("value", val);
            r.dispatchInput = true;
            r.inputType = "deleteContentForward";
        } else if (pos < len) {
            int next = utf8Next(val, pos);
            r.inputData = val.substr(pos, next - pos);
            val.erase(pos, next - pos);
            el->setAttribute("value", val);
            r.dispatchInput = true;
            r.inputType = "deleteContentForward";
            kind = TextUndoStack::Kind::DeleteForward;
        }
        r.handled = true;
    } else if (keycode == SDLK_LEFT) {
        // An unshifted arrow against a selection collapses to that edge rather
        // than stepping — the selection itself was the movement.
        if (!shift && hasSelection()) setCursorPos(sel_.start());
        else moveCaret(caretStepPrev_(val, pos));
        r.handled = true;
    } else if (keycode == SDLK_RIGHT) {
        if (!shift && hasSelection()) setCursorPos(sel_.end());
        else moveCaret(caretStepNext_(val, pos));
        r.handled = true;
    } else if (keycode == SDLK_HOME) {
        moveCaret(0);
        r.handled = true;
    } else if (keycode == SDLK_END) {
        moveCaret(len);
        r.handled = true;
    } else if (itype == InputType::Number &&
               (keycode == SDLK_UP || keycode == SDLK_DOWN)) {
        float v = val.empty() ? 0.0f : static_cast<float>(atof(val.c_str()));
        float step = rangeStep();
        v += (keycode == SDLK_UP) ? step : -step;
        std::string minStr = el->getAttribute("min");
        std::string maxStr = el->getAttribute("max");
        if (!minStr.empty()) v = std::max(v, rangeMin());
        if (!maxStr.empty()) v = std::min(v, rangeMax());
        char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
        el->setAttribute("value", buf);
        setCursorPos(static_cast<int>(strlen(buf)));
        r.handled = true;
        r.dispatchInput = true;
    } else if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
        // Enter does NOT blur a single-line text input (matches browsers).
        // Mark it handled so the keydown is still delivered to this element's
        // own listeners — form/app code commonly submits on Enter — while
        // focus is retained so the user can keep typing afterward. Blurring
        // here previously left the control unfocused but still the document's
        // activeElement, wedging all further text entry until a click.
        //
        // It does commit, though, which is the other half of what browsers do
        // with it: Enter in a text field is somebody saying they mean it, and
        // an application that only learns the value when the field is left
        // would sit there having been told and doing nothing. takeChange()
        // moves the baseline, so the blur that eventually follows is silent.
        r.dispatchChange = takeChange(el);
        r.handled = true;
    } else if (keycode == SDLK_ESCAPE) {
        // Escape does nothing to a text field. A browser leaves the value,
        // the caret and the focus exactly where they were — there is no
        // "revert" on a plain input, and the field is not a dialog to
        // dismiss. Blurring here instead ran the blur commit, so pressing
        // Escape to back out of a half-typed edit *applied* it: the one
        // gesture a user makes when they have changed their mind was the one
        // that committed. Left unhandled so the keydown still reaches the
        // page, which is where an app's own "escape closes my panel" lives.
    } else if (util::hasPrimaryMod(mod) && keycode == SDLK_A) {
        selectAll();
        r.handled = true;
    }

    // Single recording chokepoint for every value edit above. A handled key
    // that changed nothing (caret moves, Ctrl+A, Escape) breaks coalescing
    // instead — the caret is no longer where the run left it.
    const std::string afterVal = el->getAttribute("value");
    if (afterVal != beforeVal) {
        undo_.record(beforeVal, selBefore, afterVal,
                     {sel_.anchor, sel_.caret}, kind, util::currentTimeMs());
    } else if (r.handled) {
        undo_.breakCoalescing();
    }

    return r;
}

KeyHandleResult ElInput::handleTextInput(dom::Element* el, const std::string& text) {
    return insertText_(el, text, /*fromPaste=*/false);
}

KeyHandleResult ElInput::pasteText(dom::Element* el, const std::string& text) {
    return insertText_(el, text, /*fromPaste=*/true);
}

KeyHandleResult ElInput::insertText_(dom::Element* el, const std::string& text,
                                     bool fromPaste) {
    KeyHandleResult r;
    if (!focused_ || !isTextType(el)) return r;

    // Number type: only allow numeric characters. Filtering happens before
    // history recording — a rejected keystroke records nothing.
    if (inputType(el) == InputType::Number) {
        for (char c : text) {
            if (!((c >= '0' && c <= '9') || c == '-' || c == '.' ||
                  c == 'e' || c == 'E' || c == '+'))
                return r;
        }
    }

    std::string val = el->getAttribute("value");
    sel_.clampTo(static_cast<int>(val.size()));

    const std::string beforeVal = val;
    const TextUndoStack::Sel selBefore{sel_.anchor, sel_.caret};

    // Typing over a selection replaces it.
    std::string discarded;
    const bool replaced = deleteSelection_(val, discarded);

    int pos = sel_.caret;
    val.insert(pos, text);
    setCursorPos(pos + static_cast<int>(text.size()));
    el->setAttribute("value", val);

    // A plain character insertion coalesces with the run before it; a paste
    // or a type-over-selection replace is a discrete history step.
    undo_.record(beforeVal, selBefore, val, {sel_.anchor, sel_.caret},
                 (fromPaste || replaced) ? TextUndoStack::Kind::Discrete
                                         : TextUndoStack::Kind::Typing,
                 util::currentTimeMs());

    r.handled = true;
    r.dispatchInput = true;
    r.inputData = text;
    r.inputType = fromPaste ? "insertFromPaste" : "insertText";
    return r;
}

// ---------------------------------------------------------------------------
// IME composition
// ---------------------------------------------------------------------------

KeyHandleResult ElInput::compositionUpdate(dom::Element* el,
                                           const std::string& text,
                                           int cursorCp) {
    KeyHandleResult r;
    if (!focused_ || !el || !isTextType(el)) return r;
    // Number inputs take no composition — the eventual raw TEXT_INPUT commit
    // still runs through insertText_'s numeric filter.
    if (inputType(el) == InputType::Number) return r;

    std::string val = el->getAttribute("value");
    sel_.clampTo(static_cast<int>(val.size()));

    if (!comp_.active) {
        // First update: snapshot for the single undo entry the commit will
        // record, and delete any active selection (part of that same entry).
        comp_.beforeVal = val;
        comp_.selBefore = {sel_.anchor, sel_.caret};
        std::string discarded;
        deleteSelection_(val, discarded);
        comp_.start = sel_.caret;
        comp_.active = true;
        // A composition never merges into a preceding typing run.
        undo_.breakCoalescing();
    } else {
        val.erase(static_cast<size_t>(comp_.start),
                  static_cast<size_t>(comp_.length));
    }

    val.insert(static_cast<size_t>(comp_.start), text);
    comp_.length = static_cast<int>(text.size());
    comp_.preedit = text;
    // Caret at the IME's composition cursor within the preedit.
    sel_.collapseTo(comp_.start + utf8ByteForCodepoint(text, cursorCp));
    el->setAttribute("value", val);

    r.handled = true;
    r.dispatchInput = true;
    r.inputData = text;
    r.inputType = "insertCompositionText";
    return r;
}

KeyHandleResult ElInput::compositionCommit(dom::Element* el,
                                           const std::string& text) {
    KeyHandleResult r;
    if (!comp_.active || !el) return r;

    std::string val = el->getAttribute("value");
    // Guard against out-of-band writes that bypassed clearHistory().
    const int len = static_cast<int>(val.size());
    comp_.start = std::clamp(comp_.start, 0, len);
    comp_.length = std::clamp(comp_.length, 0, len - comp_.start);

    val.erase(static_cast<size_t>(comp_.start),
              static_cast<size_t>(comp_.length));
    val.insert(static_cast<size_t>(comp_.start), text);
    sel_.collapseTo(comp_.start + static_cast<int>(text.size()));
    el->setAttribute("value", val);

    // ONE discrete entry: pre-composition state → committed state. record()
    // no-ops when nothing actually changed (e.g. an empty commit over what
    // was already there).
    undo_.record(comp_.beforeVal, comp_.selBefore, val,
                 {sel_.anchor, sel_.caret},
                 TextUndoStack::Kind::Discrete, util::currentTimeMs());
    comp_ = {};

    r.handled = true;
    r.dispatchInput = true;
    r.inputData = text;
    r.inputType = "insertCompositionText";
    return r;
}

KeyHandleResult ElInput::compositionCancel(dom::Element* el) {
    KeyHandleResult r;
    if (!comp_.active || !el) return r;

    el->setAttribute("value", comp_.beforeVal);
    sel_.set(comp_.selBefore.anchor, comp_.selBefore.caret);
    sel_.clampTo(static_cast<int>(comp_.beforeVal.size()));
    comp_ = {};

    r.handled = true;
    r.dispatchInput = true;
    r.inputData = "";
    r.inputType = "insertCompositionText";
    return r;
}

// ---------------------------------------------------------------------------
// Caret and selection
// ---------------------------------------------------------------------------

int ElInput::caretStepPrev_(const std::string& val, int pos) const {
    if (!renderer_ || inputType(nullptr) == InputType::Password)
        return utf8Prev(val, pos);
    size_t lo = 0, hi = 0;
    logicalLineBounds(val, pos, lo, hi);
    const int step = clusterPrev(val, lo, hi, pos, getFontRef(), renderer_);
    // A caret at the line's first byte has no cluster behind it within the
    // line; stepping off the front of a line onto the newline before it is a
    // character step, not a cluster one.
    return step < pos ? step : utf8Prev(val, pos);
}

int ElInput::caretStepNext_(const std::string& val, int pos) const {
    if (!renderer_ || inputType(nullptr) == InputType::Password)
        return utf8Next(val, pos);
    size_t lo = 0, hi = 0;
    logicalLineBounds(val, pos, lo, hi);
    const int step = clusterNext(val, lo, hi, pos, getFontRef(), renderer_);
    // Likewise at the end of a line: the next caret site is across the
    // newline, which no cluster covers.
    return step > pos ? step : utf8Next(val, pos);
}

bool ElInput::caretRect(float& x, float& y, float& w, float& h) {
    if (!renderer_ || !elem_ || !isTextType(nullptr)) return false;
    DrawPos box = contentBox_();
    if (box.w <= 0 || box.h <= 0) return false;
    std::string disp = displayText_();
    const int cpos = std::clamp(sel_.caret, 0, static_cast<int>(disp.size()));
    float off = caretXInRun(disp, 0, disp.size(), static_cast<size_t>(cpos), getFontRef(),
                           renderer_);
    x = box.x + off - scrollX_;
    y = box.y;
    w = 1.0f;
    h = box.h;
    return true;
}

int ElInput::caretIndexFromPoint(float px, float /*py*/) {
    if (!renderer_ || !isTextType(nullptr)) return sel_.caret;

    std::string disp = displayText_();
    // Text is drawn from the content-box left edge, shifted left by scrollX_.
    float rel = (px - contentBox_().x) + scrollX_;
    int idx = caretOffsetForX(disp, 0, disp.size(), rel, getFontRef(), renderer_);

    if (inputType(nullptr) == InputType::Password) {
        // The mask is one '*' per value *byte*, so a display index is already a
        // value byte index — but it can land inside a multi-byte character.
        // Snap back to that character's first byte.
        std::string val = getAttr("value");
        int len = static_cast<int>(val.size());
        idx = std::clamp(idx, 0, len);
        while (idx > 0 && idx < len && !isUtf8Boundary(val, static_cast<size_t>(idx))) --idx;
    }
    return idx;
}

void ElInput::caretToPoint(float px, float py, bool extend) {
    if (!isTextType(nullptr)) return;
    undo_.breakCoalescing();   // mouse caret/selection change ends the run
    int idx = caretIndexFromPoint(px, py);
    if (extend) sel_.caret = idx;   // anchor stays pinned where the drag began
    else sel_.collapseTo(idx);
}

void ElInput::selectWordAtPoint(float px, float py) {
    if (!isTextType(nullptr)) return;
    undo_.breakCoalescing();
    std::string val = getAttr("value");
    int lo = 0, hi = 0;
    wordBoundsAt(val, caretIndexFromPoint(px, py), lo, hi);
    sel_.set(lo, hi);
}

void ElInput::setSelectionRange(int start, int end) {
    undo_.breakCoalescing();   // programmatic selection change ends the run
    const std::string val = getAttr("value");
    const int len = static_cast<int>(val.size());
    start = std::clamp(start, 0, len);
    end = std::clamp(end, 0, len);
    if (end <= start) {
        int p = utf8SnapBack(val, start);
        sel_.collapseTo(p);
    } else {
        sel_.set(utf8SnapBack(val, start), utf8SnapFwd(val, end));
    }
}

void ElInput::selectAll() {
    undo_.breakCoalescing();
    sel_.set(0, static_cast<int>(getAttr("value").size()));
}

std::string ElInput::selectedText() const {
    if (sel_.collapsed()) return "";
    std::string val = getAttr("value");
    int len = static_cast<int>(val.size());
    int s = std::clamp(sel_.start(), 0, len);
    int e = std::clamp(sel_.end(), 0, len);
    return val.substr(s, e - s);
}

bool ElInput::cutSelection(dom::Element* el) {
    if (!el || sel_.collapsed()) return false;
    std::string val = el->getAttribute("value");
    sel_.clampTo(static_cast<int>(val.size()));
    const std::string beforeVal = val;
    const TextUndoStack::Sel selBefore{sel_.anchor, sel_.caret};
    std::string removed;
    if (!deleteSelection_(val, removed)) return false;
    el->setAttribute("value", val);
    // Cut is always its own history entry; undo restores the cut text AND
    // the selection that covered it.
    undo_.record(beforeVal, selBefore, val, {sel_.anchor, sel_.caret},
                 TextUndoStack::Kind::Discrete, util::currentTimeMs());
    return true;
}

bool ElInput::deleteSelection_(std::string& val, std::string& removed) {
    if (sel_.collapsed()) return false;
    int len = static_cast<int>(val.size());
    // Erase whole characters. The range is boundary-aligned by every path that
    // sets it, but this is the one op that can leave invalid UTF-8 behind if it
    // ever isn't, so it pays for the guard.
    int s = utf8SnapBack(val, std::clamp(sel_.start(), 0, len));
    int e = utf8SnapFwd(val, std::clamp(sel_.end(), 0, len));
    if (s == e) return false;
    removed = val.substr(s, e - s);
    val.erase(s, e - s);
    sel_.collapseTo(s);
    return true;
}

} // namespace bro::layout
