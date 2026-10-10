#pragma once

// Image: the decode behind `.src` (host_image.cpp) and the <img> element class
// (host_element_image.cpp).

#include "embed/embed.h"
#include "bronze_host/host_class.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>

#include <functional>

namespace bro::dom {
class Document;
class Element;
}  // namespace bro::dom

namespace bro::render {
struct DecodedImage;
class ImageRequest;
}  // namespace bro::render

namespace bro::engine {
class Engine;
}  // namespace bro::engine

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// ---------------------------------------------------------------------------
// Image (host_image.cpp)
// ---------------------------------------------------------------------------

void installImageGlobal();

// One image element, as the document's element factory spells it:
// createElement('img') and createElementNS(ns, 'img') both answer one of these,
// and so does `new Image()`.
Value makeImageValue();

// The image behind an Image value, or nullptr for anything that is not one: a
// view of the <img> element's load (layout/image_loading.h), refreshed on
// every lookup. The pixels are HOST memory — the shared decoded image the
// store made off the page thread, held by reference — NOT heap bytes, so
// unlike embed::typedArrayInfo's pointer this one survives a bronze
// allocation. That is what lets the texture upload path read width/height
// through embed calls and only then hand the pixels to the WebGL context.
struct HostImage {
    uint32_t tag = kHostImageTag;  // must be first — see the tag note in host_class.h
    std::string src;
    int width = 0;   // natural size (upright: image-orientation from-image)
    int height = 0;
    // The decoded picture once the load settled as a success; null before and
    // for a broken image. RGBA8 straight alpha, top-down.
    std::shared_ptr<const render::DecodedImage> pixels;
    bool complete = false;  // the load settled, either way
    bool ok = false;        // ... and it settled as a success
    const uint8_t* rgba() const;  // null without pixels
    // The pixels as a Skia image (no copy), made once per picture.
    sk_sp<SkImage> skImage() const;
    mutable sk_sp<SkImage> skImage_;
    mutable const render::DecodedImage* skImageOf_ = nullptr;
};
const HostImage* hostImageOf(Value v);

// Start loading `src` into the <img> `el` (layout::loadImageElement): the
// decode runs off the page thread and `load` / `error` follow as a task.
// Shared by `new Image()` and an <img> element, because a texture must not
// depend on which of the two the page built. A relative `src` resolves
// against the element's document's base path — an <img> in a system panel or
// an <iframe> names a file beside ITS markup, not the app's.
void loadHostImage(dom::Element* el, const std::string& src);

// Settle pending decode() promises and createImageBitmap(blob) promises whose
// decodes finished (host_image.cpp). The image frame pump: once a frame,
// after layout::pumpImageLoads.
void pumpHostImageDecodes();

// A createImageBitmap(blob) decode: `onDone` runs on the main thread from
// pumpHostImageDecodes when the store request settles.
void whenHostDecodeSettles(std::shared_ptr<render::ImageRequest> req, std::function<void()> onDone);

// Register pumpHostImageDecodes as an engine frame pump (once), and note the
// calling thread as the one whose decodes it settles. Headless engines also
// make their paints wait for decodes (deterministic screenshots).
void installHostImagePump(engine::Engine& engine);
// Whether the caller is that thread. A worker realm (its own thread, its own
// loop) decodes synchronously instead: it is already off the page thread.
bool onHostImageMainThread();

// The one decoder every image byte stream in this layer goes through: the
// bitmap codecs (broimage), then WebP, then SVG rasterized at its intrinsic
// size. Answers straight-alpha RGBA8, top-down; false with `err` set when no
// decoder accepted the bytes (host_image.cpp).
bool decodeHostImageBytes(const uint8_t* bytes, size_t len,
                          int& outW, int& outH, std::vector<uint8_t>& outRgba,
                          std::string* err);

// The <img> half of the element surface (host_element_image.cpp). `Image` is
// an element CLASS here, so the members live on its prototype and an img
// wrapper is born on that instead of on Element's — which is why the handle
// comes from here rather than from host_element.cpp's element class.
Value makeImageElementHandle(dom::Element* el);
void primeImageFromMarkup(dom::Element* el);

}  // namespace bro::bronze_host
