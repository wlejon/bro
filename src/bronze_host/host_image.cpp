// The image DECODE as script sees it, and the one lookup that finds a decoded
// image behind a value. The `Image` constructor, its prototype and the <img>
// element that carries both live in host_element_image.cpp — an image is an
// element here, so its surface belongs with the element surface.
//
// WHAT THIS MODELS: three.js's ImageLoader builds its element with
// `document.createElementNS('http://www.w3.org/1999/xhtml','img')`, attaches
// `load` and `error` listeners, sets `crossOrigin`, assigns `src`, and
// removes its listeners from inside them. WebGLTextures then reads
// `image.width` and `image.height` and hands the element straight to the
// DOM-source texImage2D/texSubImage2D overloads (webgl_textures.cpp). A
// gallery sets `src` on a few hundred recycled <img>s and awaits `decode()`.
//
// THREADING. The decode is the shared store's (render/image_store.h), on its
// decoder threads: `src =` returns after a stat and a header read, and the
// element's load (layout/image_loading.h) settles on the frame the pixels
// land. Only bytes cross threads. Every bronze value — a decode() promise, a
// createImageBitmap promise — is made, settled and freed on the main thread,
// by pumpHostImageDecodes below, which runs from the engine's frame pump.
//
// A src some element (or the painter) already decoded is not decoded again:
// the store hands back the same request, ready, and the image is complete as
// soon as `src` is set (its `load` is still a task, as on the web).

#include "bronze_host/host_runtime.h"
#include "bronze_host/host_image.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_builder.h"  // ObjectBuilder, argAt

#include "dom/document.h"
#include "dom/element.h"
#include "engine/engine.h"
#include "layout/image_loading.h"
#include "render/animated_image.h"
#include "render/image_store.h"
#include "render/shared_pixels_image.h"
#include "util/log.h"

#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bro::bronze_host {

// ---------------------------------------------------------------------------
// The image value
// ---------------------------------------------------------------------------

const uint8_t* HostImage::rgba() const {
    return (pixels && !pixels->rgba.empty()) ? pixels->rgba.data() : nullptr;
}

sk_sp<SkImage> HostImage::skImage() const {
    if (!rgba()) return nullptr;
    if (!skImage_ || skImageOf_ != pixels.get()) {
        skImage_ = render::makeSharedPixelsImage(render::sharedPixelsOf(pixels));
        skImageOf_ = pixels.get();
    }
    return skImage_;
}

// The image behind a value, refreshed from its element's load. There is one
// shape to find it in — an <img> element, whose state hangs off its node
// registry entry — and `new Image()` produces that same shape, so
// webgl_textures.cpp asks this one question and never has to know which
// spelling built the image.
const HostImage* hostImageOf(Value v) {
    HostNodeState* st = hostNodeStateOfValue(v);
    if (!st || !st->el) return nullptr;
    const std::string& tag = st->el->tagName();
    if (tag != "img" && tag != "IMG") return st->image.get();
    if (!st->image) st->image = std::make_unique<HostImage>();
    HostImage& img = *st->image;
    dom::Element* el = st->el;
    img.src = el->getAttribute("src");
    img.width = el->imageNaturalWidth();
    img.height = el->imageNaturalHeight();
    img.complete = el->imageComplete();
    img.ok = el->imageOk();
    const auto& req = el->imageRequest();
    img.pixels = (img.complete && img.ok && req) ? req->image() : nullptr;
    // An animated GIF / WebP: the frame it shows now (a canvas drawImage or
    // a texture upload takes the current frame, as in browsers).
    if (img.pixels && img.pixels->animation) {
        if (auto frame = img.pixels->animation->frameAt(render::imageAnimationClock(), false, false))
            img.pixels = std::move(frame);
    }
    if (img.pixels && !img.pixels->rgba.empty()) {
        // The pixels' own size: an SVG without an intrinsic one has none.
        img.width = img.pixels->width;
        img.height = img.pixels->height;
    }
    return &img;
}

// ---------------------------------------------------------------------------
// The decoder ladder
// ---------------------------------------------------------------------------

bool decodeHostImageBytes(const uint8_t* bytes, size_t len,
                          int& outW, int& outH, std::vector<uint8_t>& outRgba,
                          std::string* err) {
    render::DecodedImage img;
    std::string decErr;
    if (!render::decodeImageData(bytes, len, /*orient=*/true, img, decErr) || img.rgba.empty()) {
        if (err) *err = decErr.empty() ? std::string("SVG markup did not rasterize (no intrinsic size?)") : decErr;
        return false;
    }
    outW = img.width;
    outH = img.height;
    outRgba = std::move(img.rgba);
    return true;
}

void loadHostImage(dom::Element* el, const std::string& src) {
    if (!el) return;
    layout::loadImageElement(el, src);
}

// ---------------------------------------------------------------------------
// Settling decodes on the main thread
// ---------------------------------------------------------------------------

namespace {

struct PendingDecode {
    std::shared_ptr<render::ImageRequest> req;
    std::function<void()> onDone;
};

std::vector<PendingDecode>& pendingDecodes() {
    static std::vector<PendingDecode> list;
    return list;
}

}  // namespace

namespace {
std::thread::id g_imageMainThread;
bool g_imagePumpInstalled = false;
}  // namespace

void installHostImagePump(engine::Engine& engine) {
    g_imageMainThread = std::this_thread::get_id();
    if (g_imagePumpInstalled) return;
    g_imagePumpInstalled = true;
    // An engine frame pump, not the pause-gated frame seam: a picture that
    // finished decoding lands (and repaints) while bro.time is paused too.
    // Its load events and decode() settles are tasks and promise jobs; the
    // promise jobs drain here, in the frame that produced them.
    // Animated images run on the engine's clock (bro.time's, virtual under
    // headless advanceTime); one whose next frame is due repaints every
    // document, and painting it again schedules the frame after.
    render::setImageAnimationClockSource([&engine] { return engine.timeNowMs(); });
    engine.addFramePump([&engine] {
        pumpHostImageDecodes();
        const double now = engine.timeNowMs();
        render::setImageAnimationClock(now);
        if (render::takeDueImageAnimations(now)) dom::Document::markAllPaintDirty();
        if (ev::microtasksPending()) ev::drainMicrotasks();
    });
    // The pending callbacks hold rooted promises: they go before the runtime.
    engine.addShutdownHook([] {
        pendingDecodes().clear();
        render::setImageAnimationClockSource(nullptr);
    });
    // Headless settles loads deterministically: its paints wait for the
    // decodes they need rather than painting the gap.
    if (engine.displayMode() == engine::DisplayMode::Headless) layout::setPaintWaitsForImages(true);
}

bool onHostImageMainThread() {
    return std::this_thread::get_id() == g_imageMainThread;
}

void whenHostDecodeSettles(std::shared_ptr<render::ImageRequest> req, std::function<void()> onDone) {
    if (!req || !onDone) return;
    pendingDecodes().push_back(PendingDecode{std::move(req), std::move(onDone)});
}

void pumpHostImageDecodes() {
    layout::pumpImageLoads();
    auto& list = pendingDecodes();
    if (list.empty()) return;
    std::vector<std::function<void()>> done;
    for (auto it = list.begin(); it != list.end();) {
        if (it->req->settled()) {
            done.push_back(std::move(it->onDone));
            it = list.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& fn : done) fn();
}

}  // namespace bro::bronze_host
