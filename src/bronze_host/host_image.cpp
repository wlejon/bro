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
// thread — exactly what bro's own Image binding does (src/js/image_bindings.cpp)
// — so no bronze value is ever produced, touched, or freed off the main thread,
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

#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"  // ObjectBuilder, argAt

#include "js/asset_path.h"
#include "util/object_url.h"
#include "util/log.h"

#include "broimage/decode.h"

#include <string>
#include <utility>

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

// The decode, shared. `new Image()` and `document.createElement('img')` differ
// in what OBJECT they hand the program — a bare handle, or an element that can
// be appended — and in nothing else: one src is resolved by one set of rules
// and decoded by one decoder, so a texture cannot depend on which spelling the
// page used.
void loadHostImage(HostImage& image, const std::string& src) {
    HostImage* img = &image;
    img->src = src;
    img->width = 0;
    img->height = 0;
    img->rgba.clear();
    img->complete = false;
    img->ok = false;

    std::string err;
    if (src.rfind("http://", 0) == 0 || src.rfind("https://", 0) == 0) {
        // The network belongs to brokit, which is QuickJS-native; there is no
        // bronze-side fetch to route this through (see host_xhr.cpp).
        err = "http(s) image URLs are not fetched by the bronze host image path";
        LOG_ERROR("bronze_host: Image.src = %s needs a network fetch this layer "
                  "does not provide", src.c_str());
    } else if (std::vector<uint8_t> inline_; util::inlineURLBytes(src, inline_)) {
        // A `blob:` or `data:` URL carries its own bytes — there is no path to
        // resolve and no disk to touch. Handled here rather than after the
        // path resolution, because resolveAssetPath would turn `blob:bro/7`
        // into a filename under the app directory and the decode would fail
        // with a message about a missing file that was never meant to exist.
        //
        // The table is util::object_url.h's, which is the process's ONE table:
        // a URL minted by an interpreted script on the page resolves here, and
        // one minted by URL.createObjectURL in compiled code resolves in the
        // page's markup (host_file.cpp says why).
        broimage::Image decoded;
        if (broimage::decode_memory(inline_.data(), inline_.size(), decoded, &err)) {
            img->width = decoded.width;
            img->height = decoded.height;
            img->rgba = std::move(decoded.pixels);
            img->ok = true;
            LOG_INFO("bronze_host: Image loaded from an inline URL (%dx%d)",
                     img->width, img->height);
        } else {
            LOG_WARN("bronze_host: Image inline-URL decode failed (%s)", err.c_str());
        }
    } else {
        // The shared app-path rules every bro binding uses (js/asset_path.h):
        // drive-qualified passes through, a leading slash resolves against the
        // engine mounts, anything else is relative to the app directory.
        const std::string path = js::resolveAssetPath(src);
        broimage::Image decoded;
        if (broimage::decode_file(path, decoded, &err)) {
            img->width = decoded.width;
            img->height = decoded.height;
            img->rgba = std::move(decoded.pixels);
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
