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
    std::string w = elem_->getAttribute("width");
    if (!w.empty()) intrinsicWidth_ = std::strtof(w.c_str(), nullptr);
    std::string h = elem_->getAttribute("height");
    if (!h.empty()) intrinsicHeight_ = std::strtof(h.c_str(), nullptr);
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
