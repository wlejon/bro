#include "layout/el_textarea.h"
#include "layout/control_text.h"
#include "layout/draw_traversal.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "render/renderer.h"
#include "util/platform.h"

#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::layout {

using bromath::cfromColor8;

ElTextarea::ElTextarea(render::Renderer* renderer)
    : renderer_(renderer) {}

std::string ElTextarea::getAttr(const std::string& name) const {
    return elem_ ? elem_->getAttribute(name) : "";
}

// Textarea text storage: typed/edited text lives in the "value" attribute
// once any edit has occurred; before that, the initial content from HTML
// (e.g. `<textarea>foo</textarea>`) lives in textContent. Readers must check
// the attribute first, then fall back to textContent.
static std::string readCurrentValue(dom::Element* el) {
    if (!el) return "";
    if (el->hasAttribute("value")) return el->getAttribute("value");
    return el->textContent();
}

int ElTextarea::rows() const {
    std::string r = getAttr("rows");
    if (!r.empty()) {
        int v = atoi(r.c_str());
        if (v > 0) return v;
    }
    return 2;
}

int ElTextarea::cols() const {
    std::string c = getAttr("cols");
    if (!c.empty()) {
        int v = atoi(c.c_str());
        if (v > 0) return v;
    }
    return 20;
}

render::FontRef ElTextarea::getFontRef() const {
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

// ---------------------------------------------------------------------------
// Soft wrapping
// ---------------------------------------------------------------------------
//
// A textarea soft-wraps like `white-space: pre-wrap` (wrap="soft"): explicit
// '\n's are hard breaks, and any logical line wider than the content box is
// wrapped at word boundaries (a single word longer than the box is broken
// between characters). Both rendering and cursor navigation run off the same
// visual-line list so the caret, scrolling, and up/down agree with what's drawn.

namespace {
struct VisLine { size_t start; size_t end; };  // byte range [start,end); end excludes any '\n'
}

// Break `text` into visual lines wrapped to `maxWidth`. maxWidth <= 0 disables
// wrapping (hard newlines only). Uses `r`/`fr` to measure — must be a live renderer.
static std::vector<VisLine> buildVisualLines(const std::string& text, float maxWidth,
                                             const render::FontRef& fr, render::Renderer* r) {
    std::vector<VisLine> out;
    auto measure = [&](size_t a, size_t b) -> float {
        if (b <= a || !r) return 0.0f;
        return r->measureText(text.substr(a, b - a), fr).width;
    };
    const size_t n = text.size();
    size_t lineStart = 0;
    for (size_t p = 0; p <= n; ++p) {
        if (p != n && text[p] != '\n') continue;
        // Logical line [lineStart, p).
        if (maxWidth <= 0 || measure(lineStart, p) <= maxWidth) {
            out.push_back({lineStart, p});
        } else {
            size_t segStart = lineStart;
            size_t i = lineStart;
            while (i < p) {
                size_t wordStart = i;
                while (wordStart < p && text[wordStart] == ' ') ++wordStart;
                size_t wordEnd = wordStart;
                while (wordEnd < p && text[wordEnd] != ' ') ++wordEnd;
                if (wordEnd == wordStart) { i = p; break; }  // trailing spaces
                if (measure(segStart, wordEnd) > maxWidth && wordStart > segStart) {
                    out.push_back({segStart, i});  // i = start of the spaces before this word
                    segStart = wordStart;
                    i = wordStart;
                    continue;
                }
                if (measure(segStart, wordEnd) > maxWidth && segStart == wordStart) {
                    // Single word wider than the whole line — hard-break between chars.
                    size_t j = wordStart + 1;
                    while (j < wordEnd && measure(segStart, j + 1) <= maxWidth) ++j;
                    out.push_back({segStart, j});
                    segStart = j;
                    i = j;
                    continue;
                }
                i = wordEnd;
            }
            out.push_back({segStart, p});
        }
        lineStart = p + 1;
    }
    if (out.empty()) out.push_back({0, 0});
    return out;
}

// Visual line the caret sits on. Last matching line wins, which naturally puts
// the caret at the START of the next line on a soft-wrap boundary (both lines
// touch at that offset) while keeping it at the END of the line before a hard
// '\n' (the next line starts one byte later, so it doesn't match).
static int caretVisualLine(const std::vector<VisLine>& vls, int pos) {
    int idx = 0;
    for (int k = 0; k < static_cast<int>(vls.size()); ++k) {
        if (pos >= static_cast<int>(vls[k].start) && pos <= static_cast<int>(vls[k].end)) idx = k;
        else if (pos < static_cast<int>(vls[k].start)) break;
    }
    return idx;
}

// ---------------------------------------------------------------------------
// Key handling
// ---------------------------------------------------------------------------

// Helper: compute line and column from cursor position
static void cursorLineCol(const std::string& val, int pos, int& line, int& col) {
    line = 0; col = 0;
    for (int i = 0; i < pos; ++i) {
        if (val[i] == '\n') { ++line; col = 0; } else { ++col; }
    }
}

// Helper: find start and length of a given line number
static int lineStart(const std::string& val, int targetLine) {
    int curLine = 0;
    for (int i = 0; i <= static_cast<int>(val.size()); ++i) {
        if (curLine == targetLine) return i;
        if (i < static_cast<int>(val.size()) && val[i] == '\n') ++curLine;
    }
    return static_cast<int>(val.size());
}

static int lineLength(const std::string& val, int start) {
    int len = 0;
    for (int i = start; i < static_cast<int>(val.size()) && val[i] != '\n'; ++i)
        ++len;
    return len;
}

KeyHandleResult ElTextarea::handleKeyDown(dom::Element* el, int keycode, int mod) {
    KeyHandleResult r;
    std::string val = readCurrentValue(el);
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
        }
        r.handled = true;
    } else if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
        std::string discarded;
        deleteSelection_(val, discarded);   // Enter replaces a selection
        int at = sel_.caret;
        val.insert(at, 1, '\n');
        setCursorPos(at + 1);
        el->setAttribute("value", val);
        r.handled = true;
        r.dispatchInput = true;
        r.inputData = "\n";
        r.inputType = "insertLineBreak";
    } else if (keycode == SDLK_LEFT) {
        // An unshifted arrow against a selection collapses to that edge rather
        // than stepping — the selection itself was the movement.
        if (!shift && hasSelection()) setCursorPos(sel_.start());
        else moveCaret(utf8Prev(val, pos));
        r.handled = true;
    } else if (keycode == SDLK_RIGHT) {
        if (!shift && hasSelection()) setCursorPos(sel_.end());
        else moveCaret(utf8Next(val, pos));
        r.handled = true;
    } else if (keycode == SDLK_UP || keycode == SDLK_DOWN) {
        // Move by VISUAL line so wrapped rows behave like the browser. Fall back
        // to hard-newline lines if the box hasn't been drawn yet (no wrap width).
        if (wrapWidth_ > 0 && renderer_) {
            auto vls = buildVisualLines(val, wrapWidth_, getFontRef(), renderer_);
            int li = caretVisualLine(vls, pos);
            int col = pos - static_cast<int>(vls[li].start);
            int target = li + (keycode == SDLK_UP ? -1 : 1);
            if (target >= 0 && target < static_cast<int>(vls.size())) {
                int tlen = static_cast<int>(vls[target].end - vls[target].start);
                moveCaret(static_cast<int>(vls[target].start) + std::min(col, tlen));
            }
        } else {
            int line, col;
            cursorLineCol(val, pos, line, col);
            if (keycode == SDLK_UP) {
                if (line > 0) {
                    int prev = lineStart(val, line - 1);
                    moveCaret(prev + std::min(col, lineLength(val, prev)));
                }
            } else {
                int nextStart = -1, curLine = 0;
                for (int i = 0; i < static_cast<int>(val.size()); ++i) {
                    if (val[i] == '\n') { if (curLine == line) { nextStart = i + 1; break; } ++curLine; }
                }
                if (nextStart >= 0) moveCaret(nextStart + std::min(col, lineLength(val, nextStart)));
            }
        }
        r.handled = true;
    } else if (keycode == SDLK_HOME) {
        // Start of the current VISUAL line.
        if (wrapWidth_ > 0 && renderer_) {
            auto vls = buildVisualLines(val, wrapWidth_, getFontRef(), renderer_);
            moveCaret(static_cast<int>(vls[caretVisualLine(vls, pos)].start));
        } else {
            int ls = pos;
            while (ls > 0 && val[ls - 1] != '\n') --ls;
            moveCaret(ls);
        }
        r.handled = true;
    } else if (keycode == SDLK_END) {
        // End of the current VISUAL line.
        if (wrapWidth_ > 0 && renderer_) {
            auto vls = buildVisualLines(val, wrapWidth_, getFontRef(), renderer_);
            moveCaret(static_cast<int>(vls[caretVisualLine(vls, pos)].end));
        } else {
            int le = pos;
            while (le < static_cast<int>(val.size()) && val[le] != '\n') ++le;
            moveCaret(le);
        }
        r.handled = true;
    } else if (keycode == SDLK_ESCAPE) {
        setFocused(false);
        r.handled = true;
        r.unfocus = true;
    } else if (util::hasPrimaryMod(mod) && keycode == SDLK_A) {
        selectAll();
        r.handled = true;
    }

    return r;
}

KeyHandleResult ElTextarea::handleTextInput(dom::Element* el, const std::string& text) {
    KeyHandleResult r;
    if (!focused_) return r;

    std::string val = readCurrentValue(el);
    sel_.clampTo(static_cast<int>(val.size()));

    // Typing over a selection replaces it.
    std::string discarded;
    deleteSelection_(val, discarded);

    int pos = sel_.caret;
    val.insert(pos, text);
    setCursorPos(pos + static_cast<int>(text.size()));
    el->setAttribute("value", val);

    r.handled = true;
    r.dispatchInput = true;
    r.inputData = text;
    r.inputType = "insertText";
    return r;
}

void ElTextarea::getContentSize(float& w, float& h) {
    if (!renderer_) {
        w = 173;
        h = 40;
        return;
    }
    render::FontRef fr = getFontRef();
    // Column advance: grid-fit the measured glyph advance to whole pixels the
    // way the browser hints its default monospace font, so `cols` lands on the
    // same integer column width Chromium uses. Reserve a vertical-scrollbar
    // gutter (~15px) inside the content box, as browsers do for a textarea.
    float charW = renderer_->measureText("0", fr).width;
    float colW = std::round(charW);
    // Rows height uses the browser's "normal" line box. Blink (and htmlayout's
    // metrics adapter) rounds the font ascent, descent and leading to integers
    // *independently* before summing — round(ascent)+round(descent)+round(gap)
    // — so a rows="N" textarea is exactly N of those line boxes tall.
    auto lm = render::LineMetrics::from(renderer_->measureText("M", fr));
    float lineH = std::round(lm.ascent) + std::round(lm.descent) + std::round(lm.leading);
    w = cols() * colW + 15.0f;
    h = rows() * lineH;
}

bromath::Color ElTextarea::accentColor_() const {
    bromath::Color accent = cfromColor8({0, 120, 215, 255});
    if (elem_) {
        auto& style = elem_->computedStyle();
        auto it = style.find("accent-color");
        if (it != style.end() && !it->second.empty() && it->second != "auto") {
            accent = DrawTraversal::parseColor(it->second);
        }
    }
    return accent;
}

ElTextarea::DrawPos ElTextarea::contentBox() const {
    if (!elem_) return {0, 0, 0, 0};
    // Ancestor-transform-projected, same as ElInput — a textarea under a zoomed
    // or panned ancestor must hit-test where it visibly is.
    auto r = dom::absoluteContentBox(elem_);
    return {r.x + docOffsetX_, r.y + docOffsetY_, r.width, r.height};
}

// Caret for a click. The draw pass lays the visual lines out from the content
// box's top-left, shifted up by scrollY_, so the inverse is: which row does y
// fall in, then which offset within that row's text run does x fall on.
int ElTextarea::caretIndexFromPoint(float px, float py) {
    if (!renderer_ || !elem_) return sel_.caret;

    std::string val = readCurrentValue(elem_);
    if (val.empty()) return 0;

    render::FontRef fr = getFontRef();
    auto lm = render::LineMetrics::from(renderer_->measureText("M", fr));
    float lineHeight = lm.lineHeight();
    if (lineHeight <= 0.0f) return sel_.caret;

    DrawPos box = contentBox();

    // Wrap against exactly what the frame drew. Before the first paint there is
    // no recorded wrap width — the content width is what draw() will use.
    float wrapW = wrapWidth_ > 0.0f ? wrapWidth_ : box.w;
    auto vls = buildVisualLines(val, wrapW, fr, renderer_);

    float relY = (py - box.y) + scrollY_;
    int line = static_cast<int>(std::floor(relY / lineHeight));
    line = std::clamp(line, 0, static_cast<int>(vls.size()) - 1);

    float relX = px - box.x;
    return caretOffsetForX(val, vls[line].start, vls[line].end, relX, fr, renderer_);
}

void ElTextarea::caretToPoint(float px, float py, bool extend) {
    int idx = caretIndexFromPoint(px, py);
    if (extend) sel_.caret = idx;   // anchor stays pinned where the drag began
    else sel_.collapseTo(idx);
}

void ElTextarea::selectWordAtPoint(float px, float py) {
    std::string val = readCurrentValue(elem_);
    int lo = 0, hi = 0;
    wordBoundsAt(val, caretIndexFromPoint(px, py), lo, hi);
    sel_.set(lo, hi);
}

void ElTextarea::setSelectionRange(int start, int end) {
    const std::string val = readCurrentValue(elem_);
    const int len = static_cast<int>(val.size());
    start = std::clamp(start, 0, len);
    end = std::clamp(end, 0, len);
    if (end <= start) {
        sel_.collapseTo(utf8SnapBack(val, start));
    } else {
        sel_.set(utf8SnapBack(val, start), utf8SnapFwd(val, end));
    }
}

void ElTextarea::selectAll() {
    sel_.set(0, static_cast<int>(readCurrentValue(elem_).size()));
}

std::string ElTextarea::selectedText() const {
    if (sel_.collapsed()) return "";
    std::string val = readCurrentValue(elem_);
    int len = static_cast<int>(val.size());
    int s = std::clamp(sel_.start(), 0, len);
    int e = std::clamp(sel_.end(), 0, len);
    return val.substr(s, e - s);
}

bool ElTextarea::cutSelection(dom::Element* el) {
    if (!el || sel_.collapsed()) return false;
    std::string val = readCurrentValue(el);
    sel_.clampTo(static_cast<int>(val.size()));
    std::string removed;
    if (!deleteSelection_(val, removed)) return false;
    el->setAttribute("value", val);
    return true;
}

bool ElTextarea::deleteSelection_(std::string& val, std::string& removed) {
    if (sel_.collapsed()) return false;
    int len = static_cast<int>(val.size());
    // Erase whole characters — see ElInput::deleteSelection_.
    int s = utf8SnapBack(val, std::clamp(sel_.start(), 0, len));
    int e = utf8SnapFwd(val, std::clamp(sel_.end(), 0, len));
    if (s == e) return false;
    removed = val.substr(s, e - s);
    val.erase(s, e - s);
    sel_.collapseTo(s);
    return true;
}

void ElTextarea::draw(render::Renderer* renderer,
                      const htmlayout::layout::LayoutBox& box,
                      const htmlayout::css::ComputedStyle& /*style*/,
                      float offsetX, float offsetY,
                      float docOffsetX, float docOffsetY) {
    if (!renderer || !elem_) return;

    // Use the caller's renderer (raster thread has its own)
    renderer_ = renderer;

    float x = box.contentRect.x + offsetX;
    float y = box.contentRect.y + offsetY;
    float w = box.contentRect.width;
    float h = box.contentRect.height;

    if (w <= 0 || h <= 0) return;

    // Remember this pass's doc→surface translation so contentBox() can put the
    // control back in the space the engine's mouse coordinates arrive in.
    docOffsetX_ = docOffsetX;
    docOffsetY_ = docOffsetY;

    std::string val = readCurrentValue(elem_);
    std::string placeholder = getAttr("placeholder");
    std::string text;
    bool isPlaceholder = false;

    if (!val.empty()) {
        text = val;
    } else if (!focused_ && !placeholder.empty()) {
        text = placeholder;
        isPlaceholder = true;
    }

    render::FontRef fontRef = getFontRef();
    auto lm = render::LineMetrics::from(renderer_->measureText("M", fontRef));
    float lineHeight = lm.lineHeight();

    float contentH = h;

    // Soft-wrap to the content width, and remember that width so cursor
    // navigation (up/down/home/end) wraps against exactly what this frame drew.
    wrapWidth_ = w;
    auto vls = buildVisualLines(text, w, fontRef, renderer_);
    auto lineStr = [&](const VisLine& v) { return text.substr(v.start, v.end - v.start); };

    if (focused_) {
        int cpos = std::clamp(sel_.caret, 0, static_cast<int>(text.size()));
        int cursorLine = caretVisualLine(vls, cpos);
        float cursorY = cursorLine * lineHeight;
        if (cursorY < scrollY_) scrollY_ = cursorY;
        else if (cursorY + lineHeight > scrollY_ + contentH)
            scrollY_ = cursorY + lineHeight - contentH;
        float maxScroll = std::max(0.0f, static_cast<float>(vls.size()) * lineHeight - contentH);
        scrollY_ = std::clamp(scrollY_, 0.0f, maxScroll);
    }

    renderer_->save();
    renderer_->setClip(x, y, w, h);

    // Honor the element's computed `color` for the value text (so app themes
    // with light text on dark fields read correctly) and the caret. Fall back
    // to black only when no color is set. The placeholder is always a muted
    // gray regardless of the theme color.
    bromath::Color textColor = cfromColor8({0, 0, 0, 255});
    if (elem_) {
        auto& style = elem_->computedStyle();
        auto cIt = style.find("color");
        if (cIt != style.end() && !cIt->second.empty()) {
            bromath::Color parsed;
            if (DrawTraversal::tryParseColor(cIt->second, parsed)) textColor = parsed;
        }
    }
    bromath::Color color = isPlaceholder ? cfromColor8({128, 128, 128, 180})
                                        : textColor;

    float baseX = x;
    float baseY = y - scrollY_;

    // Selection wash, behind the text. A selection spanning rows paints one band
    // per visual line: the intersection of the selected range with that line.
    // Rows fully inside the range extend a little past their last glyph to show
    // the swallowed newline, as browsers do.
    if (focused_ && !isPlaceholder && hasSelection()) {
        const int selS = std::clamp(sel_.start(), 0, static_cast<int>(text.size()));
        const int selE = std::clamp(sel_.end(), 0, static_cast<int>(text.size()));
        const bromath::Color wash = selectionFill(accentColor_());
        const float breakW = renderer_->measureText(" ", fontRef).width;
        for (int i = 0; i < static_cast<int>(vls.size()); ++i) {
            float lineY = baseY + i * lineHeight;
            if (lineY + lineHeight < y) continue;
            if (lineY > y + h) break;

            int ls = static_cast<int>(vls[i].start);
            int le = static_cast<int>(vls[i].end);
            int a = std::max(selS, ls);
            int b = std::min(selE, le);
            if (a > b) continue;                    // line outside the selection
            if (a == b && !(selS <= ls && selE > le)) continue;  // nothing on this row

            float ax = baseX + runWidthTo(text, vls[i].start, static_cast<size_t>(a),
                                          fontRef, renderer_);
            float bx = baseX + runWidthTo(text, vls[i].start, static_cast<size_t>(b),
                                          fontRef, renderer_);
            // The line break itself is selected: carry the band past the text.
            if (selE > le) bx += breakW;
            if (bx > ax) renderer_->fillRect(ax, lineY, bx - ax, lineHeight, wash);
        }
    }

    for (int i = 0; i < static_cast<int>(vls.size()); ++i) {
        float lineY = baseY + i * lineHeight;
        if (lineY + lineHeight < y) continue;
        if (lineY > y + h) break;
        if (vls[i].end > vls[i].start) {
            renderer_->drawText(lineStr(vls[i]), baseX, lineY + lm.ascent, fontRef, color);
        }
    }

    if (focused_) {
        int cpos = std::clamp(sel_.caret, 0, static_cast<int>(val.size()));
        int cursorLine = caretVisualLine(vls, cpos);

        float cursorX = baseX + runWidthTo(text, vls[cursorLine].start,
                                           static_cast<size_t>(cpos), fontRef, renderer_);

        float cursorTop = baseY + cursorLine * lineHeight;
        float cursorBottom = cursorTop + lineHeight;
        renderer_->drawLine(cursorX, cursorTop, cursorX, cursorBottom, textColor, 1.0f);
    }

    renderer_->restore();
}

} // namespace bro::layout
