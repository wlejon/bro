#include "layout/el_select.h"
#include "dom/element.h"
#include "dom/text_node.h"
#include "dom/node.h"
#include "render/renderer.h"

#include <algorithm>
#include <cstring>

namespace bro::layout {

ElSelect::ElSelect(render::Renderer* renderer)
    : renderer_(renderer) {}

std::vector<ElSelect::Option> ElSelect::getOptions() const {
    std::vector<Option> opts;
    if (!elem_) return opts;

    // Walk DOM children looking for <option> elements
    for (auto* child : elem_->children()) {
        if (child->tagName() != "OPTION") continue;

        Option opt;
        opt.value = child->getAttribute("value");

        // Get text content
        std::string text = child->textContent();
        // Trim whitespace
        while (!text.empty() && (text.front() == ' ' || text.front() == '\n' || text.front() == '\t'))
            text.erase(text.begin());
        while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\t'))
            text.pop_back();
        opt.text = text;

        if (opt.value.empty()) opt.value = opt.text;
        opts.push_back(std::move(opt));
    }
    return opts;
}

void ElSelect::initSelectedIndex() {
    if (!elem_) return;
    int idx = 0;
    for (auto* child : elem_->children()) {
        if (child->tagName() != "OPTION") continue;
        if (child->hasAttribute("selected")) {
            selectedIndex_ = idx;
            return;
        }
        ++idx;
    }
}

uint64_t ElSelect::getFontHandle() const {
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

void ElSelect::getContentSize(float& w, float& h) {
    uint64_t fontHandle = getFontHandle();
    if (fontHandle && renderer_) {
        auto lm = render::LineMetrics::from(renderer_->measureText("M", fontHandle));

        auto opts = getOptions();
        float maxW = 50.0f;
        for (auto& opt : opts) {
            if (!opt.text.empty()) {
                auto tm = renderer_->measureText(opt.text, fontHandle);
                maxW = std::max(maxW, tm.width);
            }
        }
        w = maxW + 20; // extra space for dropdown arrow
        h = lm.lineHeight();
    } else {
        w = 120;
        h = 20;
    }
}

void ElSelect::draw(render::Renderer* renderer,
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

    // Store the border-box position for dropdown placement and hit testing.
    // This matches getBoundingClientRect and ensures the dropdown aligns with
    // the visible element bounds (including padding/border).
    lastDrawPos_ = {
        x - box.padding.left - box.border.left,
        y - box.padding.top - box.border.top,
        box.fullWidth(),
        box.fullHeight()
    };

    uint64_t fontHandle = getFontHandle();
    if (!fontHandle) return;

    auto lm = render::LineMetrics::from(renderer_->measureText("M", fontHandle));

    render::Color color = {0, 0, 0, 255};
    float textY = lm.baselineY(y, h);

    renderer_->save();
    renderer_->setClip(x, y, w, h);

    auto opts = getOptions();
    int idx = std::clamp(selectedIndex_, 0, std::max(0, static_cast<int>(opts.size()) - 1));
    if (!opts.empty() && idx < static_cast<int>(opts.size())) {
        renderer_->drawText(opts[idx].text, x, textY, fontHandle, color);
    }

    // Dropdown arrow
    float arrowX = x + w - 16.0f;
    float arrowY = y + h / 2.0f;
    render::PointF arrowPts[3] = {
        {arrowX, arrowY - 3.0f},
        {arrowX + 8.0f, arrowY - 3.0f},
        {arrowX + 4.0f, arrowY + 3.0f}
    };
    renderer_->drawPolygon(std::span<const render::PointF>(arrowPts, 3),
                          color, {0, 0, 0, 0}, 0.0f);

    renderer_->restore();
}

float ElSelect::dropdownLineHeight() const {
    uint64_t fontHandle = getFontHandle();
    if (!fontHandle || !renderer_) return 20.0f;
    auto lm = render::LineMetrics::from(renderer_->measureText("M", fontHandle));
    return lm.lineHeight() + 8.0f; // line height + vertical padding
}

void ElSelect::drawDropdown() {
    if (!open_ || !renderer_ || !elem_) return;

    auto opts = getOptions();
    if (opts.empty()) return;

    uint64_t fontHandle = getFontHandle();
    if (!fontHandle) return;

    auto lm = render::LineMetrics::from(renderer_->measureText("M", fontHandle));
    float lineHeight = lm.lineHeight() + 8.0f; // add vertical padding per item
    float padX = 6.0f;
    float padY = 4.0f; // vertical padding within each item

    render::Color color = {0, 0, 0, 255};

    float dropX = lastDrawPos_.x;
    float dropY = lastDrawPos_.y + lastDrawPos_.h;
    float dropW = lastDrawPos_.w;
    float dropH = lineHeight * static_cast<float>(opts.size()) + 2.0f;

    renderer_->save();
    renderer_->resetClip();

    renderer_->fillRect(dropX, dropY, dropW, dropH, {255, 255, 255, 255});
    renderer_->drawRect(dropX, dropY, dropW, dropH, {118, 118, 118, 255});

    for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
        float itemY = dropY + 1.0f + i * lineHeight;
        if (i == highlightedIndex_) {
            renderer_->fillRect(dropX + 1, itemY, dropW - 2, lineHeight, {0, 120, 215, 255});
            renderer_->drawText(opts[i].text, dropX + padX, itemY + padY + lm.ascent,
                               fontHandle, {255, 255, 255, 255});
        } else {
            renderer_->drawText(opts[i].text, dropX + padX, itemY + padY + lm.ascent,
                               fontHandle, color);
        }
    }

    renderer_->restore();
}

} // namespace bro::layout
