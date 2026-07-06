#include "layout/el_svg.h"
#include "dom/element.h"
#include "render/renderer.h"

#include <cstdlib>
#include <string>

namespace bro::layout {

ElSvg::ElSvg(render::Renderer* renderer)
    : renderer_(renderer) {}

void ElSvg::parseAttributes() {
    if (!elem_) return;

    // Fixed-length width/height attributes give the intrinsic dimensions.
    // Percentages are viewport-relative, not intrinsic — ignore them here.
    auto attrLength = [&](const char* name) -> float {
        const std::string& v = elem_->getAttribute(name);
        if (v.empty() || v.find('%') != std::string::npos) return -1.0f;
        char* end = nullptr;
        float f = std::strtof(v.c_str(), &end);
        return (end == v.c_str() || f < 0) ? -1.0f : f;
    };
    float attrW = attrLength("width");
    float attrH = attrLength("height");

    // The intrinsic aspect ratio falls back to the viewBox when one or both
    // dimensions are missing (SVG 2 / CSS sizing of replaced elements).
    // gumbo lowercases attribute names at parse time.
    float vbW = -1.0f, vbH = -1.0f;
    {
        std::string vb = elem_->getAttribute("viewBox");
        if (vb.empty()) vb = elem_->getAttribute("viewbox");
        if (!vb.empty()) {
            const char* p = vb.c_str();
            float nums[4];
            int n = 0;
            while (*p && n < 4) {
                while (*p && (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
                char* end = nullptr;
                float f = std::strtof(p, &end);
                if (end == p) break;
                nums[n++] = f;
                p = end;
            }
            if (n == 4 && nums[2] > 0 && nums[3] > 0) { vbW = nums[2]; vbH = nums[3]; }
        }
    }
    bool hasVb = vbW > 0 && vbH > 0;

    if (attrW >= 0 && attrH >= 0) {
        intrinsicWidth_ = attrW;
        intrinsicHeight_ = attrH;
    } else if (attrW >= 0) {
        intrinsicWidth_ = attrW;
        intrinsicHeight_ = hasVb ? attrW * vbH / vbW : 150.0f;
    } else if (attrH >= 0) {
        intrinsicHeight_ = attrH;
        intrinsicWidth_ = hasVb ? attrH * vbW / vbH : 300.0f;
    } else if (hasVb) {
        intrinsicWidth_ = 300.0f;
        intrinsicHeight_ = 300.0f * vbH / vbW;
    } else {
        intrinsicWidth_ = 300.0f;
        intrinsicHeight_ = 150.0f;
    }
}

void ElSvg::getContentSize(float& w, float& h) {
    w = intrinsicWidth_;
    h = intrinsicHeight_;
}

void ElSvg::draw(render::Renderer* renderer,
                 dom::Element* elem,
                 const htmlayout::layout::LayoutBox& box,
                 float offsetX, float offsetY) {
    if (!renderer || !elem) return;

    float x = box.contentRect.x + offsetX;
    float y = box.contentRect.y + offsetY;
    float w = box.contentRect.width;
    float h = box.contentRect.height;
    if (w <= 0 || h <= 0) return;

    // Serialize at record time — DrawTraversal records into a CommandBuffer
    // that the raster thread replays without DOM access. The recording
    // renderer copies the markup bytes into its arena.
    std::string markup = elem->outerHTML();
    if (markup.empty()) return;
    renderer->drawSvgMarkup(markup.data(), markup.size(), x, y, w, h);
}

} // namespace bro::layout
