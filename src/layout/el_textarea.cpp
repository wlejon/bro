#include "layout/el_textarea.h"
#include "dom/element.h"
#include "render/renderer.h"

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
    return renderer_->createFont(family, size, 400, false);
}

void ElTextarea::getContentSize(float& w, float& h) {
    uint64_t fontHandle = getFontHandle();
    if (fontHandle && renderer_) {
        auto tm = renderer_->measureText("M", fontHandle);
        float charW = tm.width;
        float lineH = tm.height;
        w = charW * cols() + 8;
        h = lineH * rows() + 4;
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
    if (!renderer_ || !elem_) return;

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

    auto tm = renderer_->measureText("M", fontHandle);
    float lineHeight = tm.height;
    float ascent = lineHeight * 0.8f;

    float padX = 4.0f, padY = 2.0f;
    float contentH = h - padY * 2;

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

    float baseX = x + padX;
    float baseY = y + padY - scrollY_;

    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        float lineY = baseY + i * lineHeight;
        if (lineY + lineHeight < y) continue;
        if (lineY > y + h) break;
        if (!lines[i].empty()) {
            renderer_->drawText(lines[i], baseX, lineY + ascent, fontHandle, color);
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
