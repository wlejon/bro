#include "layout/el_input.h"
#include "layout/control_text.h"
#include "layout/draw_traversal.h"
#include "layout/pseudo_style.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "render/renderer.h"
#include "util/platform.h"
#include "util/time.h"

#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bro::layout {

using bromath::Color;
using bromath::cfromColor8;

ElInput::ElInput(render::Renderer* renderer)
    : renderer_(renderer) {}

std::string ElInput::getAttr(const std::string& name) const {
    return elem_ ? elem_->getAttribute(name) : "";
}

ElInput::InputType ElInput::inputType(dom::Element* el) const {
    auto* e = el ? el : elem_;
    if (!e) return InputType::Text;
    std::string type = e->getAttribute("type");
    if (type.empty()) return InputType::Text;
    if (type == "password") return InputType::Password;
    if (type == "button")   return InputType::Button;
    if (type == "submit")   return InputType::Submit;
    if (type == "reset")    return InputType::Reset;
    if (type == "checkbox") return InputType::Checkbox;
    if (type == "radio")    return InputType::Radio;
    if (type == "range")    return InputType::Range;
    if (type == "number")   return InputType::Number;
    if (type == "color")    return InputType::Color;
    if (type == "hidden")   return InputType::Hidden;
    if (type == "email")    return InputType::Email;
    if (type == "tel")      return InputType::Tel;
    if (type == "url")      return InputType::Url;
    if (type == "search")   return InputType::Search;
    return InputType::Text;
}

bool ElInput::isTextType(dom::Element* el) const {
    auto t = inputType(el);
    return t == InputType::Text || t == InputType::Password ||
           t == InputType::Email || t == InputType::Tel ||
           t == InputType::Url || t == InputType::Search ||
           t == InputType::Number;
}

bool ElInput::isButtonType(dom::Element* el) const {
    auto t = inputType(el);
    return t == InputType::Button || t == InputType::Submit || t == InputType::Reset;
}

float ElInput::rangeMin() const {
    std::string a = getAttr("min");
    return a.empty() ? 0.0f : static_cast<float>(atof(a.c_str()));
}

float ElInput::rangeMax() const {
    std::string a = getAttr("max");
    return a.empty() ? 100.0f : static_cast<float>(atof(a.c_str()));
}

float ElInput::rangeStep() const {
    std::string a = getAttr("step");
    float v = a.empty() ? 0.0f : static_cast<float>(atof(a.c_str()));
    return v > 0 ? v : 1.0f;
}

float ElInput::rangeValue() const {
    std::string v = getAttr("value");
    if (!v.empty()) return static_cast<float>(atof(v.c_str()));
    return (rangeMin() + rangeMax()) / 2.0f;
}

void ElInput::setRangeValue(float v) {
    float mn = rangeMin(), mx = rangeMax(), st = rangeStep();
    v = std::clamp(v, mn, mx);
    if (st > 0) {
        v = mn + std::round((v - mn) / st) * st;
        v = std::clamp(v, mn, mx);
    }
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
        r.handled = true;
    } else if (keycode == SDLK_ESCAPE) {
        setFocused(false);
        r.handled = true;
        r.unfocus = true;
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

void ElInput::getContentSize(float& w, float& h, float maxWidth) {
    auto t = inputType(nullptr);
    if (t == InputType::Hidden) { w = 0; h = 0; return; }

    // Read dimensions from computed style (set by UA stylesheet)
    if (elem_) {
        auto& style = elem_->computedStyle();
        auto wIt = style.find("width");
        auto hIt = style.find("height");
        if (wIt != style.end() && !wIt->second.empty() && wIt->second != "auto") {
            char* end = nullptr;
            float v = std::strtof(wIt->second.c_str(), &end);
            if (end != wIt->second.c_str() && v > 0) w = v;
        }
        if (hIt != style.end() && !hIt->second.empty() && hIt->second != "auto") {
            char* end = nullptr;
            float v = std::strtof(hIt->second.c_str(), &end);
            if (end != hIt->second.c_str() && v > 0) h = v;
        }
        if (w > 0 && h > 0) return;
    }

    // Fallback defaults if style didn't provide dimensions
    if (t == InputType::Checkbox || t == InputType::Radio) { w = 13; h = 13; return; }
    if (t == InputType::Range) { w = 160; h = 20; return; }
    if (t == InputType::Color) { w = 44; h = 24; return; }
    if (isButtonType(nullptr)) {
        // Button-type inputs (submit/reset/button) shrink to fit their label
        // just like <button> — content width = label text width, content
        // height = one line. The UA button box model (padding 1px 6px,
        // 2px border, border-box) then wraps it, matching Chromium exactly.
        float labelW = 0.f;
        float lineH = 16.0f;
        if (elem_ && renderer_) {
            auto fr = getFontRef();
            std::string val = elem_->getAttribute("value");
            if (!val.empty()) labelW = renderer_->measureText(val, fr).width;
            auto lm = render::LineMetrics::from(renderer_->measureText("M", fr));
            if (lm.lineHeight() > 0) lineH = lm.lineHeight();
        }
        w = labelW;
        h = lineH;
        return;
    }

    // Text-like inputs: width derives from the `size` attribute (default 20)
    // times the font's average lowercase advance, plus a fixed decoration
    // allowance — matching Chromium's default text-field metrics (size=20 at
    // 16px sans → 195px content; at 13.333px → 169px). Height is one line.
    int sizeAttr = 20;
    {
        std::string s = getAttr("size");
        if (!s.empty()) { int v = atoi(s.c_str()); if (v > 0) sizeAttr = v; }
    }
    float avgChar = 8.0f;
    float lineH = 16.0f;
    if (renderer_) {
        auto fr = getFontRef();
        float alpha = renderer_->measureText("abcdefghijklmnopqrstuvwxyz", fr).width;
        if (alpha > 0) avgChar = alpha / 26.0f;
        auto lm = render::LineMetrics::from(renderer_->measureText("M", fr));
        float lh = lm.lineHeight();
        if (lh > 0) lineH = lh;
    }
    w = std::round(sizeAttr * avgChar + 38.4f);
    // Don't overflow a narrow containing box (preserves the prior clamp for
    // inputs placed in tight columns).
    if (maxWidth > 0 && maxWidth < 200 && maxWidth < w) w = maxWidth;
    h = lineH;
}

render::FontRef ElInput::getFontRef() const {
    if (!elem_) return {std::string_view{"Arial"}, 16.0f, 400, false};

    auto& style = elem_->computedStyle();

    std::string_view family = "Arial";
    auto it = style.find("font-family");
    if (it != style.end() && !it->second.empty()) family = it->second;

    float size = 16.0f;
    auto sit = style.find("font-size");
    if (sit != style.end()) {
        char* end = nullptr;
        float v = std::strtof(sit->second.c_str(), &end);
        if (end != sit->second.c_str() && v > 0) size = v;
    }

    return render::FontRef{family, size, 400, false};
}

void ElInput::draw(render::Renderer* renderer,
                   const htmlayout::layout::LayoutBox& box,
                   const htmlayout::css::ComputedStyle& /*style*/,
                   float offsetX, float offsetY,
                   float docOffsetX, float docOffsetY) {
    if (!renderer || !elem_) return;

    // Use the caller's renderer (may differ from construction renderer,
    // e.g. raster thread has its own SkiaRenderer). Leave it set — the
    // raster thread is idle when the main thread uses control methods,
    // so no race condition.
    renderer_ = renderer;

    float x = box.contentRect.x + offsetX;
    float y = box.contentRect.y + offsetY;
    float w = box.contentRect.width;
    float h = box.contentRect.height;

    if (w <= 0 || h <= 0) return;

    // lastDrawPos_ feeds mouse-drag math in Engine::handleMouseMove and the
    // overlay anchors in focusNewControl, which compare against cursor
    // coordinates translated into this pass's surface space — so it needs
    // the ancestor-transform-projected rect (same fix as canvas/webgl/scene
    // layers in DrawTraversal), not the raw pre-transform layout position,
    // or a range slider under a zoomed/panned ancestor drags at the wrong
    // screen-to-value ratio entirely. absoluteContentBox() is document-space;
    // the caller's doc→surface offset (just −scroll for the app document)
    // lands this in app content space (window space for system panels).
    docOffsetX_ = docOffsetX;
    docOffsetY_ = docOffsetY;
    lastDrawPos_ = contentBox_();

    auto t = inputType(nullptr);
    if (t == InputType::Hidden) return;

    switch (t) {
        case InputType::Checkbox: drawCheckbox_(x, y, w, h); break;
        case InputType::Radio:    drawRadio_(x, y, w, h); break;
        case InputType::Range:    drawRange_(x, y, w, h); break;
        case InputType::Color:    drawColor_(x, y, w, h); break;
        default: drawText_(x, y, w, h); break;
    }
}

std::string ElInput::displayText_() const {
    std::string val = getAttr("value");
    if (inputType(nullptr) == InputType::Password) {
        return std::string(val.size(), '*');
    }
    return val;
}

float ElInput::textWidth_(float w) const {
    if (inputType(nullptr) == InputType::Number) {
        return std::max(0.0f, w - kSpinButtonWidth);
    }
    return w;
}

ElInput::DrawPos ElInput::contentBox_() const {
    if (!elem_) return {0, 0, 0, 0};
    auto r = dom::absoluteContentBox(elem_);
    return {r.x + docOffsetX_, r.y + docOffsetY_, r.width, r.height};
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

void ElInput::drawText_(float x, float y, float w, float h) {
    std::string val = getAttr("value");
    std::string placeholder = getAttr("placeholder");
    std::string text;
    bool isPlaceholder = false;

    if (!val.empty()) {
        text = displayText_();
    } else if (!focused_ && !placeholder.empty()) {
        text = placeholder;
        isPlaceholder = true;
    }

    // ::placeholder styling from the cascade (color/opacity/font-*). Empty
    // when no rule targets it — the legacy gray paint applies unchanged.
    htmlayout::css::ComputedStyle phStyle;
    if (isPlaceholder) phStyle = resolveStyledPseudo(elem_, "placeholder");

    render::FontRef fontRef = getFontRef();
    if (!phStyle.empty()) applyPseudoFont(phStyle, fontRef);
    auto lm = render::LineMetrics::from(renderer_->measureText("M", fontRef));
    float textY = lm.baselineY(y, h);

    // Distance from the text's left edge to the caret. Both password and plain
    // text draw one display glyph per value byte-run, so a byte prefix of `val`
    // is a glyph prefix of `text` — measuring `text` works for both (measuring
    // `val` under a password would place the caret against the wrong glyphs).
    const int cpos = std::clamp(sel_.caret, 0, static_cast<int>(val.size()));
    float caretOffset = 0.0f;
    if (!isPlaceholder && cpos > 0 && cpos <= static_cast<int>(text.size())) {
        caretOffset = caretXInRun(text, 0, text.size(),
                                  static_cast<size_t>(cpos), fontRef, renderer_);
    }

    // Scroll the text under the fixed box so the caret stays visible once the
    // value is wider than the content area — otherwise typing past the right
    // edge clips the caret away and you lose your place. Only a focused field
    // scrolls; an unfocused one always shows its text from the start.
    const float availW = textWidth_(w);
    if (!focused_ || isPlaceholder) {
        scrollX_ = 0.0f;
    } else {
        float fullW = text.empty() ? 0.0f
                                   : renderer_->measureText(text, fontRef).width;
        // Leave a pixel for the caret itself so it isn't half-clipped at the edge.
        if (caretOffset - scrollX_ < 0.0f) {
            scrollX_ = caretOffset;
        } else if (caretOffset - scrollX_ > availW - 1.0f) {
            scrollX_ = caretOffset - availW + 1.0f;
        }
        scrollX_ = std::clamp(scrollX_, 0.0f, std::max(0.0f, fullW - availW + 1.0f));
    }
    float drawX = x - scrollX_;

    renderer_->save();
    renderer_->setClip(x, y, availW, h);

    // Selection wash, behind the text. Measured against the drawn glyphs, so a
    // password field highlights its mask rather than the raw value's widths.
    // ::selection may restyle the wash (background-color) and the selected
    // glyphs (color, repainted after the main text run below).
    htmlayout::css::ComputedStyle selStyle;
    int selS = 0, selE = 0;
    bool hasSelBand = false;
    if (focused_ && !isPlaceholder && hasSelection() && !text.empty()) {
        selS = std::clamp(sel_.start(), 0, static_cast<int>(text.size()));
        selE = std::clamp(sel_.end(), 0, static_cast<int>(text.size()));
        if (selE > selS) {
            hasSelBand = true;
            selStyle = resolveStyledPseudo(elem_, "selection");
            float sx = drawX + caretXInRun(text, 0, text.size(), static_cast<size_t>(selS), fontRef, renderer_);
            float ex = drawX + caretXInRun(text, 0, text.size(), static_cast<size_t>(selE), fontRef, renderer_);
            float top = textY - lm.ascent;
            renderer_->fillRect(sx, top, ex - sx, lm.lineHeight(),
                                selectionWash(selStyle, accentColor_()));
        }
    }

    if (!text.empty()) {
        // Use the element's computed color for text (respects app themes)
        bromath::Color textColor = cfromColor8({0, 0, 0, 255});
        if (elem_) {
            auto& style = elem_->computedStyle();
            auto cIt = style.find("color");
            if (cIt != style.end() && !cIt->second.empty()) {
                bromath::Color parsed;
                if (DrawTraversal::tryParseColor(cIt->second, parsed)) {
                    textColor = parsed;
                }
            }
        }
        bromath::Color color = isPlaceholder ? cfromColor8({128, 128, 128, 180})
                                            : textColor;
        if (isPlaceholder && !phStyle.empty()) {
            // ::placeholder color (inherits the input's color when the rule
            // doesn't set one) with its opacity applied on top.
            pseudoColor(phStyle, "color", color);
            color.a *= pseudoOpacity(phStyle);
        }
        renderer_->drawText(text, drawX, textY, fontRef, color);

        // ::selection color: repaint the selected glyph run over the wash.
        // Drawn as its own run starting at the wash's left edge — the same
        // prefix measurement the wash used, so the glyphs land on themselves.
        // Skipped when the resolved color matches the base text color (a rule
        // that only sets background-color inherits the element's color), so
        // the common case doesn't double-draw anti-aliased edges.
        bromath::Color selColor;
        if (hasSelBand && !selStyle.empty() &&
            pseudoColor(selStyle, "color", selColor) &&
            (selColor.r != color.r || selColor.g != color.g ||
             selColor.b != color.b || selColor.a != color.a)) {
            float sx = drawX + caretXInRun(text, 0, text.size(), static_cast<size_t>(selS),
                                          fontRef, renderer_);
            renderer_->drawText(
                std::string_view(text).substr(static_cast<size_t>(selS),
                                              static_cast<size_t>(selE - selS)),
                sx, textY, fontRef, selColor);
        }
    }

    if (focused_ && isTextType(nullptr)) {
        // Use computed color for cursor too
        bromath::Color cursorColor = cfromColor8({0, 0, 0, 255});
        if (elem_) {
            auto& style = elem_->computedStyle();
            auto cIt = style.find("color");
            if (cIt != style.end() && !cIt->second.empty()) {
                bromath::Color parsed;
                if (DrawTraversal::tryParseColor(cIt->second, parsed)) {
                    cursorColor = parsed;
                }
            }
        }
        float cursorX = drawX + caretOffset;
        float cursorTop = textY - lm.ascent;
        float cursorBottom = cursorTop + lm.lineHeight();
        renderer_->drawLine(cursorX, cursorTop, cursorX, cursorBottom, cursorColor, 1.0f);

        // IME preedit: thin underline under the whole provisional run, just
        // below the baseline (the caret above marks the composition cursor).
        // Password fields underline the mask — its bytes map 1:1 to the value.
        if (comp_.active && comp_.length > 0 && !isPlaceholder) {
            const int n = static_cast<int>(text.size());
            const int ps = std::clamp(comp_.start, 0, n);
            const int pe = std::clamp(comp_.start + comp_.length, ps, n);
            if (pe > ps) {
                float ux0 = drawX + caretXInRun(text, 0, text.size(), static_cast<size_t>(ps),
                                               fontRef, renderer_);
                float ux1 = drawX + caretXInRun(text, 0, text.size(), static_cast<size_t>(pe),
                                               fontRef, renderer_);
                float uy = textY + 2.0f;
                renderer_->drawLine(ux0, uy, ux1, uy, cursorColor, 1.0f);
            }
        }
    }

    // The spin buttons sit outside the text's clip — they own the right edge.
    renderer_->restore();

    if (inputType(nullptr) == InputType::Number) {
        float btnW = kSpinButtonWidth;
        float bx = x + w - btnW;
        renderer_->drawLine(bx, y, bx, y + h, cfromColor8({180, 180, 180, 255}), 1.0f);
        renderer_->drawLine(bx, y + h / 2, bx + btnW, y + h / 2, cfromColor8({180, 180, 180, 255}), 1.0f);

        float cx = bx + btnW / 2;
        render::PointF upPts[3] = {
            {cx - 4, y + h / 4 + 2}, {cx + 4, y + h / 4 + 2}, {cx, y + h / 4 - 2}
        };
        renderer_->drawPolygon(std::span<const render::PointF>(upPts, 3),
                              cfromColor8({80, 80, 80, 255}), cfromColor8({0, 0, 0, 0}), 0.0f);

        render::PointF downPts[3] = {
            {cx - 4, y + h * 3 / 4 - 2}, {cx + 4, y + h * 3 / 4 - 2}, {cx, y + h * 3 / 4 + 2}
        };
        renderer_->drawPolygon(std::span<const render::PointF>(downPts, 3),
                              cfromColor8({80, 80, 80, 255}), cfromColor8({0, 0, 0, 0}), 0.0f);
    }
}

// color-scheme, per CSS Color Adjustment: an element whose computed
// color-scheme includes "dark" gets dark-rendered UA control chrome.
// htmlayout registers the property as inherited, so `body { color-scheme:
// dark }` is enough to theme every form control in an app.
bool ElInput::darkScheme_() const {
    if (!elem_) return false;
    auto& style = elem_->computedStyle();
    auto it = style.find("color-scheme");
    return it != style.end() && it->second.find("dark") != std::string::npos;
}

// CSS accent-color for the "accent parts" of a control: the checked
// checkbox/radio fill and the range fill + thumb. Windows-blue when unset.
Color ElInput::accentColor_() const {
    Color accent = cfromColor8({0, 120, 215, 255});
    if (elem_) {
        auto& style = elem_->computedStyle();
        auto it = style.find("accent-color");
        if (it != style.end() && !it->second.empty() && it->second != "auto") {
            accent = DrawTraversal::parseColor(it->second);
        }
    }
    return accent;
}

void ElInput::drawCheckbox_(float x, float y, float w, float h) {
    float sz = std::min(w, h);
    float bx = x + (w - sz) / 2;
    float by = y + (h - sz) / 2;

    bool dark = darkScheme_();
    bool checked = elem_ && elem_->hasAttribute("checked");
    Color accent = accentColor_();
    Color box = dark ? cfromColor8({43, 47, 56, 255}) : cfromColor8({255, 255, 255, 255});
    Color border = dark ? cfromColor8({110, 118, 130, 255}) : cfromColor8({118, 118, 118, 255});

    // Checked box fills with the accent (Chromium's accent-color behavior);
    // the mark contrasts against that fill, not the scheme.
    renderer_->fillRect(bx, by, sz, sz, checked ? accent : box);
    if (!checked) renderer_->drawRect(bx, by, sz, sz, border);

    if (focused_) {
        Color ring = {accent.r, accent.g, accent.b, 128.0f / 255.0f};
        renderer_->drawRect(bx - 1, by - 1, sz + 2, sz + 2, ring);
    }

    if (checked) {
        Color mark = (accent.r + accent.g + accent.b > 1.5f)
            ? cfromColor8({20, 20, 20, 255}) : cfromColor8({255, 255, 255, 255});
        float pad = sz * 0.2f;
        float x1 = bx + pad, y1 = by + sz * 0.5f;
        float x2 = bx + sz * 0.4f, y2 = by + sz - pad;
        float x3 = bx + sz - pad, y3 = by + pad;
        renderer_->drawLine(x1, y1, x2, y2, mark, 2.0f);
        renderer_->drawLine(x2, y2, x3, y3, mark, 2.0f);
    }
}

void ElInput::drawRadio_(float x, float y, float w, float h) {
    float sz = std::min(w, h);
    float r = sz / 2;
    float cx = x + w / 2, cy = y + h / 2;

    bool dark = darkScheme_();
    bool checked = elem_ && elem_->hasAttribute("checked");
    Color accent = accentColor_();
    Color box = dark ? cfromColor8({43, 47, 56, 255}) : cfromColor8({255, 255, 255, 255});
    Color border = dark ? cfromColor8({110, 118, 130, 255}) : cfromColor8({118, 118, 118, 255});

    renderer_->drawCircle(cx, cy, r, box, checked ? accent : border, checked ? 2.0f : 1.0f);
    if (focused_) {
        Color ring = {accent.r, accent.g, accent.b, 128.0f / 255.0f};
        renderer_->drawCircle(cx, cy, r + 1, cfromColor8({0, 0, 0, 0}), ring, 1.0f);
    }
    if (checked) {
        renderer_->drawCircle(cx, cy, r * 0.45f, accent, cfromColor8({0, 0, 0, 0}), 0.0f);
    }
}

float ElInput::rangeThumbRadius(float h) {
    // Scale with element height, but keep the thumb inside the hit box
    // (diameter <= h). 0.4 * h gives ~8 at the default 20px height.
    float r = std::clamp(h * 0.4f, 2.0f, 10.0f);
    return std::min(r, h * 0.5f);
}

float ElInput::rangeTrackHeight(float h) {
    return std::clamp(h * 0.2f, 2.0f, 6.0f);
}

void ElInput::drawRange_(float x, float y, float w, float h) {
    float trackH = rangeTrackHeight(h);
    float trackY = y + (h - trackH) / 2;
    float thumbR = rangeThumbRadius(h);
    float trackPad = thumbR;

    // Accent color — honor CSS accent-color for the filled track and thumb,
    // falling back to Windows-blue when unset.
    Color accent = accentColor_();
    // Darken the accent ~18% in linear space (was uint8 *0.82 — equivalent
    // multiplicative scale, now correctly applied in linear-light).
    bromath::Color accentDark = {
        accent.r * 0.82f, accent.g * 0.82f, accent.b * 0.82f, accent.a
    };
    bromath::Color focusRing = {accent.r, accent.g, accent.b, 128.0f/255.0f};

    bool dark = darkScheme_();
    Color trackBg = dark ? cfromColor8({52, 58, 68, 255}) : cfromColor8({200, 200, 200, 255});
    renderer_->fillRoundRect(x + trackPad, trackY, w - trackPad * 2, trackH,
                            2, 2, trackBg);

    float mn = rangeMin(), mx = rangeMax();
    float val = rangeValue();
    float span = w - trackPad * 2;
    float pct = (mx > mn) ? (val - mn) / (mx - mn) : 0.0f;
    pct = std::clamp(pct, 0.0f, 1.0f);
    float thumbX = x + trackPad + pct * span;
    float thumbY = y + h / 2;

    // Fill origin: a signed range (min < 0 < max) is a bipolar control — its
    // resting point is 0, not the left edge, so the accent fill grows from the
    // zero position toward the thumb in either direction. A slider at 0 shows
    // no fill at all, which is exactly the "this control is neutral" signal.
    // One-signed ranges keep the usual fill-from-min.
    float fillFrom = x + trackPad;
    if (mn < 0.0f && mx > 0.0f) {
        float zeroPct = (0.0f - mn) / (mx - mn);
        fillFrom = x + trackPad + zeroPct * span;
        // A faint zero tick so the resting point stays visible while dragging.
        Color tick = dark ? cfromColor8({110, 118, 130, 255}) : cfromColor8({140, 140, 140, 255});
        renderer_->fillRect(fillFrom - 0.5f, trackY - 2, 1, trackH + 4, tick);
    }
    float fx0 = std::min(fillFrom, thumbX), fx1 = std::max(fillFrom, thumbX);
    if (fx1 > fx0) renderer_->fillRoundRect(fx0, trackY, fx1 - fx0, trackH, 2, 2, accent);

    bromath::Color thumbFill = dragging_ ? accentDark : accent;
    Color thumbRim = dark ? cfromColor8({16, 18, 24, 255}) : cfromColor8({255, 255, 255, 255});
    renderer_->drawCircle(thumbX, thumbY, thumbR, thumbFill, thumbRim, 1.5f);

    if (focused_) {
        renderer_->drawCircle(thumbX, thumbY, thumbR + 2, cfromColor8({0, 0, 0, 0}), focusRing, 1.5f);
    }
}

void ElInput::drawColor_(float x, float y, float w, float h) {
    std::string val = getAttr("value");
    bromath::Color swatch = cfromColor8({0, 0, 0, 255});
    if (val.size() == 7 && val[0] == '#') {
        swatch = bromath::cfromHex(val.c_str());
    }

    float pad = 3.0f;
    renderer_->drawRect(x, y, w, h, cfromColor8({118, 118, 118, 255}));
    renderer_->fillRect(x + pad, y + pad, w - pad * 2, h - pad * 2, swatch);
    if (focused_) {
        renderer_->drawRect(x - 1, y - 1, w + 2, h + 2, cfromColor8({0, 120, 215, 255}));
    }
}

} // namespace bro::layout
