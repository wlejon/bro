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

namespace bro::dom {
class Document;
class Element;
}  // namespace bro::dom

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

// The decode result behind an Image value, or nullptr for anything that is not
// one. The bytes are HOST memory (a std::vector owned by the value's handle
// cell), NOT heap bytes — so unlike embed::typedArrayInfo's pointer this one
// survives a bronze allocation and stays valid until the value is collected.
// That is what lets the texture upload path read width/height through embed
// calls and only then hand the pixels to GL.
struct HostImage {
    uint32_t tag = kHostImageTag;  // must be first — see the tag note in host_class.h
    std::string src;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // RGBA8, top-down (row 0 = top); empty if broken
    bool complete = false;      // the load settled, either way
    bool ok = false;            // ... and it settled as a success
    uint64_t loadId = 0;
    std::shared_ptr<uint64_t> activeLoadToken;
    std::vector<ev::Persistent> pendingDecodePromises;
};
const HostImage* hostImageOf(Value v);

// Resolve `src` and decode it into `img`, leaving `img.complete` true either
// way and `img.ok` true only on success. Shared by `new Image()` and by an
// <img> element, because a texture must not depend on which of the two the
// page happened to build (host_image.cpp). A relative `src` resolves against
// `doc`'s own base path when one is given — an <img> in a system panel or an
// <iframe> names a file beside ITS markup, not the app's — and against the
// app directory otherwise.
void loadHostImage(HostImage& img, const std::string& src, const dom::Document* doc = nullptr,
                   dom::Element* target = nullptr);

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
