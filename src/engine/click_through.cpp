// Transparent, click-through windows: the engine's half. The window system
#include "engine/replaced_elements.h"
// composites a transparent window's premultiplied frames over the desktop
// (the frame's base is cleared to transparent: engine_frame_composite.cpp);
// a click-through window passes pointer input beneath it outside its input
// region, which comes from the page's own hit test (hit_region.h) and is
// recomputed after each frame that laid out or painted, so a still page
// costs nothing.
#include "engine/engine.h"
#include "engine/hit_region.h"

#include "dom/document.h"
#include "dom/element.h"
#include "layout/box.h"
#include "layout/layout_node_adapter.h"
#include "platform/window.h"
#include "render/vulkan_swapchain.h"
#include "util/log.h"
#include "util/time.h"

#include <algorithm>
#include <cmath>

namespace bro::engine {

namespace {

struct EdgeWalk {
    std::vector<float> xs, ys;
    std::vector<platform::WindowRect> boxes;  // candidate element border boxes, window coords
    dom::Element* html = nullptr;
    dom::Element* body = nullptr;
    float viewportScrollY = 0.0f;
    float toWindowY = 0.0f;  // window y = doc y + toWindowY
    int width = 0, height = 0;

    void addRect(float x, float y, float w, float h) {
        if (w <= 0.0f || h <= 0.0f) return;
        const float wy = y + toWindowY;
        if (x >= width || wy >= height || x + w <= 0.0f || wy + h <= 0.0f) return;
        xs.push_back(x);
        xs.push_back(x + w);
        ys.push_back(wy);
        ys.push_back(wy + h);
    }

    // A box an element could be hit in (document space), as a candidate.
    void addBox(float x, float y, float w, float h) {
        if (w <= 0.0f || h <= 0.0f) return;
        const float wy = y + toWindowY;
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(wy));
        boxes.push_back({x0, y0, static_cast<int>(std::ceil(x + w)) - x0, static_cast<int>(std::ceil(wy + h)) - y0});
    }

    // The node's boxes, in document space, as hitTestRecursive places them.
    void walk(htmlayout::layout::LayoutNode* node, float offsetX, float offsetY, bool underFixedCB) {
        if (!node || node->isTextNode()) return;
        const auto& box = node->box;
        dom::Element* el = layout::LayoutNodeAdapter::elementFor(node);
        if (el && !underFixedCB) {
            const auto& style = el->computedStyle();
            auto it = style.find("position");
            if (it != style.end() && it->second == "fixed") {
                float px = 0.0f, py = 0.0f;
                for (auto* p = node->parent(); p; p = p->parent()) {
                    px += p->box.contentRect.x;
                    py += p->box.contentRect.y;
                }
                offsetX = px;
                offsetY = py + viewportScrollY;
            }
        }
        const auto& hb = box.hitBounds;
        if (hb.width == 0.0f && hb.height == 0.0f) return;  // display: none
        // The subtree's reach (transforms folded in), then the node's own box.
        if (hb.width > 0.0f) addRect(hb.x + offsetX, hb.y + offsetY, hb.width, hb.height);
        const float absX = box.contentRect.x + offsetX;
        const float absY = box.contentRect.y + offsetY;
        const float bx = absX - box.padding.left - box.border.left;
        const float by = absY - box.padding.top - box.border.top;
        addRect(bx, by, box.fullWidth(), box.fullHeight());
        if (el && el != html && el != body) addBox(bx, by, box.fullWidth(), box.fullHeight());
        bool transformed = false;
        if (el) {
            const auto& style = el->computedStyle();
            auto t = style.find("transform");
            transformed = t != style.end() && !t->second.empty() && t->second != "none";
        }
        // A transform moves the box and everything in it: where they can be
        // hit is the subtree's projected reach.
        if (transformed && hb.width > 0.0f) addBox(hb.x + offsetX, hb.y + offsetY, hb.width, hb.height);
        const bool fixedCB = underFixedCB || transformed;
        const float childX = absX - node->scrollLeftPx();
        const float childY = absY - node->scrollTopPx();
        for (auto* child : node->children()) walk(child, childX, childY, fixedCB);
    }
};

}  // namespace

bool Engine::windowTransparent() const {
    if (!graphicsConfig_.transparent) return false;
    if (displayMode_ == DisplayMode::Headless) return true;
    if (displayMode_ != DisplayMode::Windowed || !window_ || !window_->isTransparent()) return false;
    return vulkanSwapchain_ && vulkanSwapchain_->transparent();
}

bool Engine::takesPointerAt(float x, float y) {
    if (!document_ || !document_->documentElement()) return false;
    if (overlayMgr_->hasActive()) return true;  // an open dropdown closes on a click anywhere
    if (y < static_cast<float>(contentTop())) return true;  // the menu bar
    if (isSystemVisible()) {
        SystemDocument* doc = nullptr;
        if (systemHoverHit(x, y, doc)) return true;
    }
    const float docX = x, docY = y - static_cast<float>(contentTop()) + scrollY_;
    dom::Element* hit = hitTest(docX, docY);
    return hit && hit != document_->documentElement() && hit != document_->body();
}

std::vector<platform::WindowRect> Engine::computeInputRegion() {
    std::vector<platform::WindowRect> rects;
    const int w = viewportWidth_, h = viewportHeight_;
    if (!document_ || !document_->documentElement() || w <= 0 || h <= 0) return rects;
    flushLayoutForRead(document_.get());
    if (overlayMgr_->hasActive() || isSystemVisible()) {
        rects.push_back({0, 0, w, h});
        return rects;
    }
    auto* root = document_->layoutRoot();
    if (!root) return rects;

    EdgeWalk walk;
    walk.html = document_->documentElement();
    walk.body = document_->body();
    walk.viewportScrollY = scrollY_;
    walk.toWindowY = static_cast<float>(contentTop()) - scrollY_;
    walk.width = w;
    walk.height = h;
    walk.walk(root, 0.0f, 0.0f, false);
    const int top = contentTop();
    if (top > 0) {
        walk.ys.push_back(static_cast<float>(top));
        walk.boxes.push_back({0, 0, w, top});  // the menu bar
    }

    auto takes = [this](float x, float y) { return takesPointerAt(x, y); };
    if (computeHitRegion(w, h, walk.xs, walk.ys, takes, rects, kHitRegionMaxCells, &walk.boxes)) return rects;

    // Too fine a grid to test cell by cell (a large page): every element's
    // box takes input, which errs toward catching input over passing it.
    static bool warned = false;
    if (!warned) {
        warned = true;
        LOG_WARN("click-through: %zu boxes are too many to test cell by cell; the input region "
                 "is their union", walk.boxes.size());
    }
    for (const auto& b : walk.boxes) {
        const int x0 = std::max(0, b.x), y0 = std::max(0, b.y);
        const int x1 = std::min(w, b.x + b.w), y1 = std::min(h, b.y + b.h);
        if (x1 > x0 && y1 > y0) rects.push_back({x0, y0, x1 - x0, y1 - y0});
    }
    return rects;
}

void Engine::updateInputRegion() {
    inputRegionStale_ = false;
    if (!clickThrough_ || !window_) return;
    // A window that follows the pointer asks about one point: where it is.
    if (!window_->wantsInputRegion()) {
        window_->refreshInputTest();
        return;
    }
    // A region costs a hit test per cell of the page's box grid: well under
    // a millisecond for an overlay, tens for a page of hundreds of
    // positioned boxes. One that costs more than kRegionBudgetMs is redone
    // at most every kRegionSpacing times its cost while the page keeps
    // changing; the last change is caught up once it stops (windowIdle
    // waits for it).
    constexpr double kRegionBudgetMs = 2.0;
    constexpr double kRegionSpacing = 8.0;
    const double now = util::currentTimeMs();
    if (inputRegionCostMs_ > kRegionBudgetMs && now - inputRegionAtMs_ < inputRegionCostMs_ * kRegionSpacing) {
        inputRegionStale_ = true;
        return;
    }
    auto rects = computeInputRegion();
    inputRegionAtMs_ = util::currentTimeMs();
    inputRegionCostMs_ = inputRegionAtMs_ - now;
    if (rects == inputRegion_) return;
    inputRegion_ = std::move(rects);
    window_->setInputRegion(inputRegion_);
}

bool Engine::setClickThrough(bool on) {
    if (!window_ || (displayMode_ != DisplayMode::Windowed && displayMode_ != DisplayMode::Headless)) return false;
    if (on == clickThrough_) return true;
    if (on) window_->setInputTest([this](float x, float y) { return takesPointerAt(x, y); });
    if (!window_->setClickThrough(on)) return false;
    clickThrough_ = on;
    inputRegion_.clear();
    if (on && window_->wantsInputRegion()) {
        inputRegion_ = computeInputRegion();
        window_->setInputRegion(inputRegion_);
    }
    return true;
}

}  // namespace bro::engine
