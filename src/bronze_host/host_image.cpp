// `new Image()` and the load path three.js's TextureLoader drives.
//
// WHAT THIS MODELS, and nothing more: three.js r160's ImageLoader builds its
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
#include "util/log.h"

#include "broimage/decode.h"

#include <string>
#include <utility>

namespace bro::bronze_host {

namespace {

// Mid-collection, and the whole of its job: free host memory. The struct holds
// no ev::Persistent precisely so that this is true (host_internal.h says why).
void hostImageDtor(void* p) { delete static_cast<HostImage*>(p); }

HostImage* mutableHostImage(Value v) {
    auto* img = static_cast<HostImage*>(ev::handleData(v));
    if (!img || img->tag != kHostImageTag) return nullptr;
    return img;
}

// Republish the decode result onto the properties JS reads. Each setProperty
// allocates and may move the object, so the receiver rides in the Persistent
// and every call stores its post-call address back.
void publishImageState(ev::Persistent& self, const HostImage& img) {
    Value w = ev::fromDouble(img.width);
    self.set(ev::setProperty(self.get(), "width", w));
    Value h = ev::fromDouble(img.height);
    self.set(ev::setProperty(self.get(), "height", h));
    // naturalWidth/naturalHeight are the same numbers here: nothing in this
    // layer scales an image, so the intrinsic size and the used size cannot
    // differ. three.js reads `width`/`height`; loaders in the wild read the
    // natural pair.
    Value nw = ev::fromDouble(img.width);
    self.set(ev::setProperty(self.get(), "naturalWidth", nw));
    Value nh = ev::fromDouble(img.height);
    self.set(ev::setProperty(self.get(), "naturalHeight", nh));
    Value complete = ev::fromBool(img.complete);
    self.set(ev::setProperty(self.get(), "complete", complete));
}

Value imageSrcGetter(Value thisValue, std::span<const Value>) {
    const HostImage* img = hostImageOf(thisValue);
    return ev::fromUtf8(img ? img->src : std::string());
}

Value imageSrcSetter(Value thisValue, std::span<const Value> a) {
    // Re-root the receiver before anything allocates: `thisValue` is a plain
    // copy, current only at entry (embed.h).
    ev::Persistent self(thisValue);

    Value v = argAt(a, 0);
    if (ev::isObject(v)) return ev::throwTypeError("Image.src must be a string");
    const std::string src = ev::toUtf8(v);  // ALLOCATES for a non-string input

    // The payload is HOST memory, so the pointer survives the allocation above
    // — but it must be read from the receiver's CURRENT address, which is why
    // it is fetched here and not before.
    HostImage* img = mutableHostImage(self.get());
    if (!img) return ev::throwTypeError("Image.src: the receiver is not an Image");

    img->src = src;
    img->width = 0;
    img->height = 0;
    img->rgba.clear();
    img->complete = false;
    img->ok = false;

    std::string err;
    if (src.rfind("data:", 0) == 0) {
        // three.js's ImageLoader special-cases data: URLs (it skips crossOrigin
        // for them), so an app can genuinely hand us one. Decoding it needs a
        // base64 reader this layer does not have; refuse by name rather than
        // report a missing file.
        err = "data: URLs are not decoded by the bronze host image path";
        LOG_ERROR("bronze_host: Image.src = data: URL is not supported");
    } else if (src.rfind("http://", 0) == 0 || src.rfind("https://", 0) == 0) {
        // The network belongs to brokit, which is QuickJS-native; there is no
        // bronze-side fetch to route this through (see host_xhr.cpp).
        err = "http(s) image URLs are not fetched by the bronze host image path";
        LOG_ERROR("bronze_host: Image.src = %s needs a network fetch this layer "
                  "does not provide", src.c_str());
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
    publishImageState(self, *img);

    const bool loaded = img->ok;
    ev::Persistent target(self.get());
    postHostTask([target, loaded]() {
        // A copy of the Persistent rides in the closure and is an independent
        // root (embed.h), released when the task is destroyed on the main
        // thread after it runs — so nothing but the task itself keeps the image
        // alive between the decode and the event.
        dispatchHostEvent(target, loaded ? "load" : "error");
    });
    return ev::undefined();
}

}  // namespace

// ---------------------------------------------------------------------------
// The image value
// ---------------------------------------------------------------------------

const HostImage* hostImageOf(Value v) { return mutableHostImage(v); }

Value makeImageValue() {
    auto* img = new HostImage();
    // The handle cell is a plain object as far as the program is concerned, so
    // ObjectBuilder decorates it exactly as it decorates a createObject() one.
    ObjectBuilder b(ev::makeHandle(img, hostImageDtor));

    // Data properties first, so the shape is fixed before any of them is
    // rewritten by a load: publishImageState assigns the same four names, which
    // is a shape hit only if they were not there to begin with.
    {
        Value zero = ev::fromDouble(0);
        b.set("width", zero);
    }
    {
        Value zero = ev::fromDouble(0);
        b.set("height", zero);
    }
    {
        Value zero = ev::fromDouble(0);
        b.set("naturalWidth", zero);
    }
    {
        Value zero = ev::fromDouble(0);
        b.set("naturalHeight", zero);
    }
    {
        Value no = ev::fromBool(false);
        b.set("complete", no);
    }
    // The handler slots, present and null so an assignment finds a data
    // property rather than creating one — and so `image.onload` reads null
    // rather than undefined, which is what the web answers for an unset one.
    {
        Value nul = ev::null();
        b.set("onload", nul);
    }
    {
        Value nul = ev::null();
        b.set("onerror", nul);
    }
    // Stored and ignored: there is no network here, so there is no origin to be
    // cross. three.js assigns it on every ImageLoader load.
    {
        Value nul = ev::null();
        b.set("crossOrigin", nul);
    }

    b.accessor("src", imageSrcGetter, imageSrcSetter);

    b.def("addEventListener", 2, [](Value thisValue, std::span<const Value> a) {
        ev::Persistent self(thisValue);
        Value typeV = argAt(a, 0);
        if (ev::isObject(typeV)) return ev::undefined();
        const std::string type = ev::toUtf8(typeV);
        addHostListener(self, type, argAt(a, 1));
        return ev::undefined();
    });
    b.def("removeEventListener", 2, [](Value thisValue, std::span<const Value> a) {
        ev::Persistent self(thisValue);
        Value typeV = argAt(a, 0);
        if (ev::isObject(typeV)) return ev::undefined();
        const std::string type = ev::toUtf8(typeV);
        removeHostListener(self, type, argAt(a, 1));
        return ev::undefined();
    });

    return b.get();
}

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------

void installImageGlobal() {
    // `new Image()` reaches this through bronze_construct, which builds a plain
    // instance, runs the body with it as the receiver, and then REPLACES the
    // instance with whatever object the body returns (rt_object.cpp). So the
    // value the program gets is the one built here, and `Image()` without `new`
    // answers the same thing.
    //
    // The consequence, named because it is observable: `img instanceof Image`
    // is false, since the returned object's prototype is not Image.prototype.
    // Nothing on three.js's texture path asks — its one `instanceof
    // HTMLImageElement` sits inside the resizeImage branch that a WebGL2
    // context with an in-range texture never enters — and giving the answer
    // properly needs a way to build an object on a chosen prototype, which the
    // embed API does not have.
    Value ctor = ev::makeFunction(
        [](Value, std::span<const Value>) {
            // `new Image(width, height)` may pass dimensions. They set the
            // element's layout box, not the decode, and this layer has no
            // layout box for an image — accepted and ignored.
            return makeImageValue();
        },
        0);
    ev::registerGlobal("Image", ctor);
}

}  // namespace bro::bronze_host
