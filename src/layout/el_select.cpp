#include "layout/el_select.h"
#include "layout/draw_traversal.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/text_node.h"
#include "dom/node.h"
#include "render/renderer.h"

#include <algorithm>
#include <cstring>

namespace bro::layout {

using bromath::cfromColor8;

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

        // Spec: an option's value is its text only when there is NO value
        // attribute. An explicit value="" stays "" — placeholder options
        // (<option value="">pick one…</option>) rely on select.value === ''.
        if (!child->hasAttribute("value")) opt.value = opt.text;
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

render::FontRef ElSelect::getFontRef() const {
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

void ElSelect::getContentSize(float& w, float& h) {
    if (!renderer_) {
        w = 120;
        h = 20;
        return;
    }
    render::FontRef fontRef = getFontRef();
    auto lm = render::LineMetrics::from(renderer_->measureText("M", fontRef));

    auto opts = getOptions();
    float maxW = 50.0f;
    for (auto& opt : opts) {
        if (!opt.text.empty()) {
            auto tm = renderer_->measureText(opt.text, fontRef);
            maxW = std::max(maxW, tm.width);
        }
    }
    w = maxW + 20; // extra space for dropdown arrow
    h = lm.lineHeight();
}

void ElSelect::draw(render::Renderer* renderer,
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

    // Store the border-box position for dropdown placement and hit testing.
    // This matches getBoundingClientRect and ensures the dropdown aligns with
    // the visible element bounds (including padding/border). Must go through
    // the ancestor-transform-projected rect (same fix as canvas/webgl/scene
    // layers and the range-slider), or a select under a zoomed/panned
    // ancestor opens its dropdown at the raw pre-transform layout position.
    // absoluteBorderBox() is document-space; the caller's doc→surface offset
    // (just −scroll for the app document) lands this in the pass's surface
    // space — app content space, where App overlays anchor and receive input.
    auto screenRect = dom::absoluteBorderBox(elem_);
    lastDrawPos_ = {screenRect.x + docOffsetX, screenRect.y + docOffsetY,
                    screenRect.width, screenRect.height};

    render::FontRef fontRef = getFontRef();
    auto lm = render::LineMetrics::from(renderer_->measureText("M", fontRef));

    // Respect CSS `color` for selected-option text and the dropdown arrow,
    // so dark themes can read the selection. Falls back to black if unset.
    bromath::Color color = cfromColor8({0, 0, 0, 255});
    if (elem_) {
        auto& style = elem_->computedStyle();
        auto cIt = style.find("color");
        if (cIt != style.end() && !cIt->second.empty()) {
            bromath::Color parsed;
            if (DrawTraversal::tryParseColor(cIt->second, parsed)) {
                color = parsed;
            }
        }
    }
    float textY = lm.baselineY(y, h);

    renderer_->save();
    renderer_->setClip(x, y, w, h);

    auto opts = getOptions();
    int idx = std::clamp(selectedIndex_, 0, std::max(0, static_cast<int>(opts.size()) - 1));
    if (!opts.empty() && idx < static_cast<int>(opts.size())) {
        renderer_->drawText(opts[idx].text, x, textY, fontRef, color);
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
                          color, cfromColor8({0, 0, 0, 0}), 0.0f);

    renderer_->restore();
}

} // namespace bro::layout
