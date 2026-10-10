// Images for painting: an <img>'s picture, a CSS url() for a background or a
// border-image. All of them come from the shared decoded-image store
// (render/image_store.h), decoded off the page thread once per source: the
// traversal only asks for a request and paints what is ready.

#include "layout/draw_traversal_internal.h"
#include "layout/image_loading.h"
#include "render/image_source.h"
#include "render/image_store.h"

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

std::shared_ptr<const render::DecodedImage> DrawTraversal::paintImage(const std::string& url, bool oriented) {
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
    return readyImage(req);
}

std::shared_ptr<const render::DecodedImage> DrawTraversal::elementPaintImage(dom::Element* elem, bool oriented) {
    std::shared_ptr<render::ImageRequest> req = elem->imageRequest();
    if (!req) {
        // An <img> whose load has not started (markup the replaced-element
        // pass has not reached): the painter asks for its src itself.
        return paintImage(elem->getAttribute("src"), oriented);
    }
    auto img = readyImage(req);
    // image-orientation: none on a photo that carries a turn: the pixels as
    // stored, a request of their own.
    if (img && !oriented && img->orientation != 1 && !img->isSvg)
        return paintImage(elem->getAttribute("src"), /*oriented=*/false);
    return img;
}

}  // namespace bro::layout
