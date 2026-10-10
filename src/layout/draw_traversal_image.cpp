// Images for painting: an <img>'s picture, a CSS url() for a background or a
// border-image. All of them come from the shared decoded-image store
// (render/image_store.h), decoded off the page thread once per source: the
// traversal only asks for a request and paints what is ready. An animated
// GIF / WebP paints the frame its timeline is on (render/animated_image.h),
// and asks for its next frame only when what it paints is on screen.

#include "layout/draw_traversal_internal.h"
#include "layout/image_loading.h"
#include "dom/element_geometry.h"
#include "render/animated_image.h"
#include "render/image_source.h"
#include "render/image_store.h"

#include <algorithm>

namespace bro::layout {

namespace {

// Headless paints wait for a decode rather than painting the gap (a
// screenshot is then deterministic); a windowed one paints what is ready and
// repaints when the rest lands.
std::shared_ptr<const render::DecodedImage> readyImage(const std::shared_ptr<render::ImageRequest>& req) {
    if (!req) return nullptr;
    if (!req->settled() && paintWaitsForImages()) req->wait(10000.0);
    return req->image();
}

}  // namespace

bool DrawTraversal::boxOnScreen(dom::Element* elem) const {
    if (!elem || viewportW_ <= 0 || viewportH_ <= 0) return true;
    const dom::AbsoluteRect b = dom::absoluteBorderBox(elem);
    float x0 = b.x + rootOffsetX_, y0 = b.y + rootOffsetY_;
    float x1 = x0 + b.width, y1 = y0 + b.height;
    x0 = std::max(x0, 0.0f);
    y0 = std::max(y0, static_cast<float>(viewportTop_));
    x1 = std::min(x1, static_cast<float>(viewportW_));
    y1 = std::min(y1, static_cast<float>(viewportTop_ + viewportH_));
    float cx = 0, cy = 0, cw = 0, ch = 0;
    if (currentClipRect(cx, cy, cw, ch)) {
        x0 = std::max(x0, cx);
        y0 = std::max(y0, cy);
        x1 = std::min(x1, cx + cw);
        y1 = std::min(y1, cy + ch);
    }
    return x1 > x0 && y1 > y0;
}

std::shared_ptr<const render::DecodedImage> DrawTraversal::currentFrame(
        std::shared_ptr<const render::DecodedImage> img, dom::Element* elem) const {
    if (!img || !img->animation) return img;
    auto frame = img->animation->frameAt(render::imageAnimationClock(), boxOnScreen(elem), paintWaitsForImages());
    return frame ? frame : img;
}

std::shared_ptr<const render::DecodedImage> DrawTraversal::paintImage(const std::string& url, bool oriented,
                                                                      dom::Element* forElem) {
    if (url.empty()) return nullptr;
    const std::string memoKey = oriented ? url : url + "\x01n";
    std::shared_ptr<render::ImageRequest> req;
    if (auto it = imageRequests_.find(memoKey); it != imageRequests_.end()) req = it->second.lock();
    if (!req) {
        const render::ImageSource source = render::resolveImageSource(url, basePath_, oriented);
        req = render::requestImage(source);
        if (!req) return nullptr;  // nothing to read: broken
        imageRequests_[memoKey] = req;
    }
    return currentFrame(readyImage(req), forElem);
}

std::shared_ptr<const render::DecodedImage> DrawTraversal::elementPaintImage(dom::Element* elem, bool oriented) {
    std::shared_ptr<render::ImageRequest> req = elem->imageRequest();
    if (!req) {
        // An <img> whose load has not started (markup the replaced-element
        // pass has not reached): the painter asks for its src itself.
        return paintImage(elem->getAttribute("src"), oriented, elem);
    }
    auto img = readyImage(req);
    // image-orientation: none on a photo that carries a turn: the pixels as
    // stored, a request of their own.
    if (img && !oriented && img->orientation != 1 && !img->isSvg)
        return paintImage(elem->getAttribute("src"), /*oriented=*/false, elem);
    return currentFrame(std::move(img), elem);
}

}  // namespace bro::layout
