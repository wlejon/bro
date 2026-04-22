#include "layout/el_textarea.h"
#include "dom/element.h"
#include "render/renderer.h"
#include "util/platform.h"

#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cstring>

namespace bro::layout {

ElTextarea::ElTextarea(render::Renderer* renderer)
    : renderer_(renderer) {}

std::string ElTextarea::getAttr(const std::string& name) const {
    return elem_ ? elem_->getAttribute(name) : "";
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

uint64_t ElTextarea::getFontHandle() const {
    if (!elem_ || !renderer_) return 0;
    if (cachedFontHandle_) return cachedFontHandle_;

    auto& style = elem_->computedStyle();
    std::string family = "Arial";
    auto it = style.find("font-family");
    if (it != style.end() && !it->second.empty()) family = it->second;
    float size = 16.0f;
    auto sit = style.find("font-size");
    if (sit != style.end()) {
        char* end = nullptr;
        float v = std::strtof(sit->second.c_str(), &end);
        if (end != sit->second.c_str() && v > 0) size = v;
    }
    cachedFontHandle_ = renderer_->createFont(family, size, 400, false);
    return cachedFontHandle_;
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
    std::string val = el->getAttribute("value");
    int pos = std::clamp(cursorPos_, 0, static_cast<int>(val.size()));

    if (keycode == SDLK_BACKSPACE) {
        if (pos > 0) {
            r.inputData = val.substr(pos - 1, 1);
            val.erase(pos - 1, 1);
            setCursorPos(pos - 1);
            el->setAttribute("value", val);
            r.dispatchInput = true;
            r.inputType = "deleteContentBackward";
        }
        r.handled = true;
    } else if (keycode == SDLK_DELETE) {
        if (pos < static_cast<int>(val.size())) {
            r.inputData = val.substr(pos, 1);
            val.erase(pos, 1);
            el->setAttribute("value", val);
            r.dispatchInput = true;
            r.inputType = "deleteContentForward";
        }
        r.handled = true;
    } else if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
        val.insert(pos, 1, '\n');
        setCursorPos(pos + 1);
        el->setAttribute("value", val);
        r.handled = true;
        r.dispatchInput = true;
        r.inputData = "\n";
        r.inputType = "insertLineBreak";
    } else if (keycode == SDLK_LEFT) {
        if (pos > 0) setCursorPos(pos - 1);
        r.handled = true;
    } else if (keycode == SDLK_RIGHT) {
        if (pos < static_cast<int>(val.size())) setCursorPos(pos + 1);
        r.handled = true;
    } else if (keycode == SDLK_UP) {
        int line, col;
        cursorLineCol(val, pos, line, col);
        if (line > 0) {
            int prev = lineStart(val, line - 1);
            setCursorPos(prev + std::min(col, lineLength(val, prev)));
        }
        r.handled = true;
    } else if (keycode == SDLK_DOWN) {
        int line, col;
        cursorLineCol(val, pos, line, col);
        // Find next line
        int nextStart = -1;
        int curLine = 0;
        for (int i = 0; i < static_cast<int>(val.size()); ++i) {
            if (val[i] == '\n') {
                if (curLine == line) { nextStart = i + 1; break; }
                ++curLine;
            }
        }
        if (nextStart >= 0) {
            setCursorPos(nextStart + std::min(col, lineLength(val, nextStart)));
        }
        r.handled = true;
    } else if (keycode == SDLK_HOME) {
        // Start of current line
        int ls = pos;
        while (ls > 0 && val[ls - 1] != '\n') --ls;
        setCursorPos(ls);
        r.handled = true;
    } else if (keycode == SDLK_END) {
        // End of current line
        int le = pos;
        while (le < static_cast<int>(val.size()) && val[le] != '\n') ++le;
        setCursorPos(le);
        r.handled = true;
    } else if (keycode == SDLK_ESCAPE) {
        setFocused(false);
        r.handled = true;
        r.unfocus = true;
    } else if (util::hasPrimaryMod(mod) && keycode == SDLK_A) {
        setCursorPos(static_cast<int>(val.size()));
        r.handled = true;
    }

    return r;
}

KeyHandleResult ElTextarea::handleTextInput(dom::Element* el, const std::string& text) {
    KeyHandleResult r;
    if (!focused_) return r;

    std::string val = el->getAttribute("value");
    int pos = std::clamp(cursorPos_, 0, static_cast<int>(val.size()));
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
    uint64_t fontHandle = getFontHandle();
    if (fontHandle && renderer_) {
        auto tm = renderer_->measureText("M", fontHandle);
        float charW = tm.width;
        float lineH = tm.height;
        w = charW * cols();
        h = lineH * rows();
    } else {
        w = 173;
        h = 40;
    }
}

static std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
        if (start == text.size()) lines.push_back("");
    }
    if (lines.empty()) lines.push_back("");
    return lines;
}

static std::pair<int, int> posToLineCol(const std::string& text, int pos) {
    int line = 0, col = 0;
    for (int i = 0; i < pos && i < static_cast<int>(text.size()); ++i) {
        if (text[i] == '\n') { ++line; col = 0; }
        else { ++col; }
    }
    return {line, col};
}

void ElTextarea::draw(render::Renderer* renderer,
                      const htmlayout::layout::LayoutBox& box,
                      const htmlayout::css::ComputedStyle& /*style*/,
                      float offsetX, float offsetY) {
    if (!renderer || !elem_) return;

    // Use the caller's renderer (raster thread has its own)
    if (renderer != renderer_) {
        renderer_ = renderer;
        cachedFontHandle_ = 0;
    }

    float x = box.contentRect.x + offsetX;
    float y = box.contentRect.y + offsetY;
    float w = box.contentRect.width;
    float h = box.contentRect.height;

    if (w <= 0 || h <= 0) return;

    std::string val = getAttr("value");
    std::string placeholder = getAttr("placeholder");
    std::string text;
    bool isPlaceholder = false;

    if (!val.empty()) {
        text = val;
    } else if (!focused_ && !placeholder.empty()) {
        text = placeholder;
        isPlaceholder = true;
    }

    uint64_t fontHandle = getFontHandle();
    if (!fontHandle) return;

    auto lm = render::LineMetrics::from(renderer_->measureText("M", fontHandle));
    float lineHeight = lm.lineHeight();

    float contentH = h;

    auto lines = splitLines(text);

    if (focused_) {
        auto [cursorLine, cursorCol] = posToLineCol(text, std::clamp(cursorPos_, 0, static_cast<int>(text.size())));
        float cursorY = cursorLine * lineHeight;
        if (cursorY < scrollY_) scrollY_ = cursorY;
        else if (cursorY + lineHeight > scrollY_ + contentH)
            scrollY_ = cursorY + lineHeight - contentH;
        float maxScroll = std::max(0.0f, static_cast<float>(lines.size()) * lineHeight - contentH);
        scrollY_ = std::clamp(scrollY_, 0.0f, maxScroll);
    }

    renderer_->save();
    renderer_->setClip(x, y, w, h);

    render::Color color = isPlaceholder ? render::Color{128, 128, 128, 180}
                                        : render::Color{0, 0, 0, 255};

    float baseX = x;
    float baseY = y - scrollY_;

    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        float lineY = baseY + i * lineHeight;
        if (lineY + lineHeight < y) continue;
        if (lineY > y + h) break;
        if (!lines[i].empty()) {
            renderer_->drawText(lines[i], baseX, lineY + lm.ascent, fontHandle, color);
        }
    }

    if (focused_) {
        std::string valStr = val;
        int cpos = std::clamp(cursorPos_, 0, static_cast<int>(valStr.size()));
        auto [cursorLine, cursorCol] = posToLineCol(valStr, cpos);

        float cursorX = baseX;
        if (cursorCol > 0 && cursorLine < static_cast<int>(lines.size())) {
            std::string beforeCursor = lines[cursorLine].substr(0, cursorCol);
            auto ctm = renderer_->measureText(beforeCursor, fontHandle);
            cursorX += ctm.width;
        }

        float cursorTop = baseY + cursorLine * lineHeight;
        float cursorBottom = cursorTop + lineHeight;
        renderer_->drawLine(cursorX, cursorTop, cursorX, cursorBottom, {0, 0, 0, 255}, 1.0f);
    }

    renderer_->restore();
}

} // namespace bro::layout
