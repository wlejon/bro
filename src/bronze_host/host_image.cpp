// The image DECODE, and the one lookup that finds a decoded image behind a
// value. The `Image` constructor, its prototype and the <img> element that
// carries both live in host_element_image.cpp — an image is an element here,
// so its surface belongs with the element surface. What is left in this file
// is the part that has nothing to do with either: given a src, produce pixels.
//
// WHAT THIS MODELS, and nothing more: three.js's ImageLoader builds its
// element with `document.createElementNS('http://www.w3.org/1999/xhtml','img')`,
// attaches `load` and `error` listeners, sets `crossOrigin`, assigns `src`, and
// removes its listeners from inside them. WebGLTextures then reads `image.width`
// and `image.height` and hands the element straight to the DOM-source
// texImage2D/texSubImage2D overloads (gl_textures.cpp). That list is the whole
// contract; everything else an HTMLImageElement has on the web is deliberately
// absent, because a stub for it would fail somewhere further from here.
//
// THREADING, stated because it is the question an image path usually raises:
// there is no thread. broimage::decode_file is synchronous and runs on the main
// thread — so no bronze value is ever produced, touched, or freed off the main thread,
// and there is no cross-thread queue to get the memory ordering wrong on. If a
// later chunk moves the decode onto bro's async job machinery, the decoded
// BYTES may cross threads but the bronze side must not: the completion has to
// arrive through postHostTask, which is drained on the frame seam, and every
// embed call must stay on this side of it.
//
// WHAT IS DEFERRED ANYWAY: the load/error event. It is posted as a host task
// and fires on the next frame rather than from inside the `src` setter. On the
// web the event is a queued task, never synchronous with the assignment — and
// firing it here would re-enter compiled code from a property setter, with
// three.js's TextureLoader still inside `load()` and its Texture not yet
// returned to the caller.
//
// THE DECODER LADDER is one function, decodeHostImageBytes, and every byte
// stream in this layer goes through it: an <img> src (file or inline URL) and
// createImageBitmap(blob) alike. broimage first (PNG/JPEG/GIF/BMP/...), WebP
// second (bro's own decoder — the pinned Skia carries no libwebp), and SVG
// last, rasterized at its intrinsic size — so a vector icon is as usable a
// drawImage or texImage2D source as a bitmap one, which is what the paint path
// already makes it for markup.

#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"  // ObjectBuilder, argAt

#include "dom/document.h"
#include "dom/element.h"
#include "engine/engine.h"
#include "util/asset_path.h"
#include "util/object_url.h"
#include "util/remote_asset.h"
#include "util/log.h"

#include "broimage/decode.h"
#include "svg/svg_renderer.h"

#if BRO_WITH_WEBP
#include "render/webp_image.h"
#endif

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bro::bronze_host {


// ---------------------------------------------------------------------------
// The image value
// ---------------------------------------------------------------------------

// The decoded image behind a value, or nullptr. There is one shape to find it
// in — an <img> element, whose pixels hang off its node registry entry — and
// `new Image()` produces that same shape, so gl_textures.cpp asks this one
// question and never has to know which spelling built the image.
const HostImage* hostImageOf(Value v) {
    HostNodeState* st = hostNodeStateOfValue(v);
    return st ? st->image.get() : nullptr;
}

// ---------------------------------------------------------------------------
// The decoder ladder
// ---------------------------------------------------------------------------

bool decodeHostImageBytes(const uint8_t* bytes, size_t len,
                          int& outW, int& outH, std::vector<uint8_t>& outRgba,
                          std::string* err) {
    if (!bytes || len == 0) {
        if (err) *err = "empty image data";
        return false;
    }
    std::string decErr;
#if BRO_WITH_WEBP
    // WebP before the bitmap codecs, as the <img> path always had it: bro's
    // own decoder answers the same pixels on every platform, where a Skia
    // built with libwebp would otherwise take the bytes on one OS and not
    // another.
    {
        int ww = 0, hh = 0;
        std::vector<uint8_t> rgba;
        if (render::decodeWebP(bytes, len, ww, hh, rgba)) {
            outW = ww;
            outH = hh;
            outRgba = std::move(rgba);
            return true;
        }
    }
#endif
    broimage::Image decoded;
    if (broimage::decode_memory(bytes, len, decoded, &decErr)) {
        outW = decoded.width;
        outH = decoded.height;
        outRgba = std::move(decoded.pixels);
        return true;
    }
    if (svg::looksLikeSvg(reinterpret_cast<const char*>(bytes), len)) {
        int ww = 0, hh = 0;
        std::vector<uint8_t> rgba;
        if (svg::rasterizeSvgMarkup(reinterpret_cast<const char*>(bytes), len,
                                    0, 0, ww, hh, rgba)) {
            outW = ww;
            outH = hh;
            outRgba = std::move(rgba);
            return true;
        }
        decErr = "SVG markup did not rasterize (no intrinsic size?)";
    }
    if (err) *err = decErr;
    return false;
}

namespace {

// Resolve an <img> src the way the old per-realm resolver did: a
// drive-qualified path passes through, a leading slash goes to the engine
// mounts, and anything else is relative to the DOCUMENT that carries the
// element — its base path when it has one, the app directory otherwise
// (util::resolveAssetPath, which is also what handles the mounts).
std::string resolveImageSrc(const std::string& src, const dom::Document* doc) {
    if (src.size() >= 2 && src[1] == ':') return src;
    if (!src.empty() && (src[0] == '/' || src[0] == '\\')) return util::resolveAssetPath(src);
    if (doc && !doc->basePath().empty()) {
        std::string path = doc->basePath();
        if (path.back() != '/' && path.back() != '\\') path += '/';
        return path + src;
    }
    return util::resolveAssetPath(src);
}

bool readFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    out.assign(s.begin(), s.end());
    return true;
}

}  // namespace

// The decode, shared. `new Image()` and `document.createElement('img')` differ
// in what OBJECT they hand the program — a bare handle, or an element that can
// be appended — and in nothing else: one src is resolved by one set of rules
// and decoded by one decoder, so a texture cannot depend on which spelling the
// page used.
void loadHostImage(HostImage& image, const std::string& src, const dom::Document* doc,
                   dom::Element* target) {
    HostImage* img = &image;
    img->src = src;
    img->width = 0;
    img->height = 0;
    img->rgba.clear();
    img->complete = false;
    img->ok = false;

    if (util::isHttpUrl(src)) {
        auto token = std::make_shared<uint64_t>(++img->loadId);
        img->activeLoadToken = token;
        uint64_t thisLoadId = *token;
        std::weak_ptr<uint64_t> weakToken = token;

        std::thread([&image, src, thisLoadId, weakToken, target]() {
            std::string body = util::fetchRemoteCached(src);
            int w = 0, h = 0;
            std::vector<uint8_t> rgba;
            std::string decErr;
            bool success = false;
            if (!body.empty()) {
                success = decodeHostImageBytes(reinterpret_cast<const uint8_t*>(body.data()),
                                               body.size(), w, h, rgba, &decErr);
            }

            postHostTask([&image, src, thisLoadId, weakToken, target, success, w, h,
                          rgba = std::move(rgba)]() mutable {
                auto locked = weakToken.lock();
                if (!locked || *locked != thisLoadId) {
                    return;
                }

                image.complete = true;
                image.ok = success;
                if (success) {
                    image.width = w;
                    image.height = h;
                    image.rgba = std::move(rgba);
                    LOG_INFO("bronze_host: Remote image loaded %s (%dx%d)", src.c_str(), w, h);
                } else {
                    LOG_WARN("bronze_host: Remote image load failed %s", src.c_str());
                }

                auto promises = std::move(image.pendingDecodePromises);
                image.pendingDecodePromises.clear();
                for (auto& p : promises) {
                    if (success) {
                        ev::resolvePromise(p.get(), ev::undefined());
                    } else {
                        // Rooted: the Error lookup may allocate (builtins build lazily).
                        Rooted reason(ev::fromUtf8("EncodingError: the remote image could not be decoded"));
                        Value ctor = ev::globalValue("Error").value;
                        if (ev::isFunction(ctor)) {
                            const Value msg = reason.get();
                            ev::CallResult made = ev::construct(ctor, std::span<const Value>(&msg, 1));
                            if (!made.thrown) reason.set(made.value);
                        }
                        ev::rejectPromise(p.get(), reason.get());
                    }
                }

                if (target) {
                    if (success) {
                        target->setImageNaturalSize(src, w, h);
                    }
                    dom::Event evt(success ? "load" : "error", false, false);
                    if (auto* eng = hostEngine()) {
                        eng->dispatchElementEvent(target, evt);
                    }
                }
            });
        }).detach();
        return;
    }

    std::string err;
    if (std::vector<uint8_t> inline_; util::inlineURLBytes(src, inline_)) {
        // A `blob:` or `data:` URL carries its own bytes — there is no path to
        // resolve and no disk to touch. Handled here rather than after the
        // path resolution, because resolveAssetPath would turn `blob:bro/7`
        // into a filename under the app directory and the decode would fail
        // with a message about a missing file that was never meant to exist.
        //
        // The table is util::object_url.h's, which is the process's ONE table:
        // a URL minted anywhere on the page resolves here, including
        // one minted by URL.createObjectURL (host_file.cpp says why).
        if (decodeHostImageBytes(inline_.data(), inline_.size(),
                                 img->width, img->height, img->rgba, &err)) {
            img->ok = true;
            LOG_INFO("bronze_host: Image loaded from an inline URL (%dx%d)",
                     img->width, img->height);
        } else {
            LOG_WARN("bronze_host: Image inline-URL decode failed (%s)", err.c_str());
        }
    } else {
        const std::string path = resolveImageSrc(src, doc);
        std::vector<uint8_t> bytes;
        if (!readFileBytes(path, bytes)) {
            LOG_WARN("bronze_host: Image load failed %s (cannot open)", path.c_str());
        } else if (decodeHostImageBytes(bytes.data(), bytes.size(),
                                        img->width, img->height, img->rgba, &err)) {
            img->ok = true;
            LOG_INFO("bronze_host: Image loaded %s (%dx%d)", src.c_str(), img->width,
                     img->height);
        } else {
            // A failed decode is a BROKEN image, not a 1x1 white one: broimage
            // hands back a white fallback pixel and adopting it would make a
            // missing texture indistinguishable from a real one. Per HTML a
            // broken image has zero natural dimensions and no pixels, so the
            // texture upload path sees an empty buffer and no-ops.
            LOG_WARN("bronze_host: Image load failed %s (%s)", path.c_str(), err.c_str());
        }
    }

    img->complete = true;  // the load settled, success or not
}



}  // namespace bro::bronze_host
