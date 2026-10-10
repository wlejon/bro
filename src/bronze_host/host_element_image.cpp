// `Image` — which on the web is `HTMLImageElement`, one constructor under two
// names — as an ELEMENT class rather than a decoder that resembles one.
//
// three.js's ImageLoader builds its image with
// `document.createElementNS('http://www.w3.org/1999/xhtml', 'img')`, assigns
// `crossOrigin` and `src`, and waits for a `load` event; WebGLTextures then
// reads `image.width`/`image.height` and hands the element straight to
// texImage2D. Nothing on that path ever appends the image, which is why it
// could be served by a bare host handle with no node behind it — and why
// serving it that way looked right until a page appended one. The three.js
// editor appends several, and `appendChild` on something that is not a
// dom::Node is a TypeError with nowhere to go from there.
//
// So there is ONE img here and it is an element: `new Image()` creates a
// detached `<img>` and hands back its ordinary element wrapper, and
// `document.createElement('img')` hands back the wrapper for the element the
// document made. Both are born on this class's prototype, which chains to
// `Element.prototype` — so an img is `instanceof Image`, `instanceof Element`
// and a node, all three, and the two spellings produce the same shape rather
// than two shapes a library must not tell apart.
//
// The decoded pixels live in the node's registry entry (HostNodeState), which
// is what lets `hostImageOf` answer for an img element and lets
// webgl_textures.cpp keep asking one question.

#include "bronze_host/host_builder.h"  // ObjectBuilder, argAt
#include "bronze_host/host_class.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_image.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_html_interfaces.h"
#include "bronze_host/host_node_sweep.h"

#include "dom/document.h"
#include "dom/element.h"
#include "engine/engine.h"
#include "layout/image_loading.h"

#include <memory>
#include <string>

namespace bro::bronze_host {

namespace {

HostClass g_imageClass;

// The image state behind this receiver, refreshed from its element's load
// (hostImageOf). An <img> that has never been given a `src` still has to
// answer `width` and `complete`, so the state exists from the first read.
const HostImage* imageStateOf(Value self) {
    return hostImageOf(self);
}

Value sizeMember(Value self, int HostImage::*field) {
    const HostImage* img = imageStateOf(self);
    return ev::fromDouble(img ? img->*field : 0);
}

Value imageSrcGetter(Value self, std::span<const Value>) {
    HostNodeState* st = hostNodeStateOfValue(self);
    return ev::fromUtf8(st && st->el ? st->el->getAttribute("src") : std::string());
}

Value imageSrcSetter(Value self, std::span<const Value> a) {
    // Re-root before anything allocates: `self` is a plain copy, current only
    // at entry (embed.h's NativeFn contract).
    ev::Persistent receiver(self);
    Value v = argAt(a, 0);
    if (ev::isObject(v)) return ev::throwTypeError("img.src must be a string");
    const std::string src = ev::toUtf8(v);  // ALLOCATES for a non-string input

    // Host pointers, read from the receiver's CURRENT address.
    HostNodeState* st = hostNodeStateOfValue(receiver.get());
    if (!st || !st->el) return ev::throwTypeError("img.src: the receiver is not an img");

    // The attribute, then the load: a stat and a header read here, the
    // decode on the store's threads (layout/image_loading.h). Starting it
    // here rather than at the next layout pass is what makes a detached image
    // — three.js's ImageLoader never appends one — load at all, and what
    // records the src as loaded, so the layout walk does not start it twice.
    st->el->setAttribute("src", src);
    loadHostImage(st->el, src);
    return ev::undefined();
}

}  // namespace

void decorateImageProto(ObjectBuilder& b) {
    b.accessor("src", imageSrcGetter, imageSrcSetter);

    // The four numbers three.js reads. `width`/`height` are the used size and
    // `naturalWidth`/`naturalHeight` the intrinsic one; nothing in this layer
    // scales an image, so they are the same pair twice rather than a second
    // stored value that could disagree with the first.
    b.accessor(
        "width",
        [](Value self, std::span<const Value>) { return sizeMember(self, &HostImage::width); },
        nullptr);
    b.accessor(
        "height",
        [](Value self, std::span<const Value>) { return sizeMember(self, &HostImage::height); },
        nullptr);
    b.accessor(
        "naturalWidth",
        [](Value self, std::span<const Value>) { return sizeMember(self, &HostImage::width); },
        nullptr);
    b.accessor(
        "naturalHeight",
        [](Value self, std::span<const Value>) { return sizeMember(self, &HostImage::height); },
        nullptr);

    b.accessor(
        "complete",
        [](Value self, std::span<const Value>) {
            const HostImage* img = imageStateOf(self);
            return ev::fromBool(img && img->complete);
        },
        nullptr);

    // decode(): the promise a loader awaits before it uses the pixels —
    // resolved when the decode off the page thread has landed (now, when it
    // already has), rejected with an EncodingError for a broken image or one
    // whose src changed before it landed.
    b.def("decode", 0, [](Value self, std::span<const Value>) {
        ev::Persistent p{ev::createPromise()};
        HostNodeState* st = hostNodeStateOfValue(self);
        auto settle = [p](bool ok) {
            if (ok) {
                ev::resolvePromise(p.get(), ev::undefined());
                return;
            }
            // Rooted: the Error lookup may allocate (builtins build lazily).
            Rooted reason(ev::fromUtf8("EncodingError: the image could not be decoded"));
            Value ctor = ev::globalValue("Error").value;
            if (ev::isFunction(ctor)) {
                const Value msg = reason.get();
                ev::CallResult made = ev::construct(ctor, std::span<const Value>(&msg, 1));
                if (!made.thrown) reason.set(made.value);
            }
            ev::rejectPromise(p.get(), reason.get());
        };
        if (!st || !st->el) {
            settle(false);
            return p.get();
        }
        // The settle runs from the frame pump (or now), on the main thread.
        layout::whenImageSettled(st->el, settle);
        return p.get();
    });

    // Stored and ignored: there is no network here, so there is no origin to be
    // cross. three.js assigns it on every ImageLoader load, and the assignment
    // lands as an own property over this default, which is what the web does
    // too.
    {
        Value nul = ev::null();
        b.set("crossOrigin", nul);
    }
}

namespace {

// `new Image()` — a detached <img>, which is exactly what the web's
// [[HTMLConstructor]] produces. The optional width/height arguments set the
// element's LAYOUT box, not the decode, and this layer has no layout box for a
// detached image: accepted and ignored, as they were before.
Value imageConstructor(Value, std::span<const Value>) {
    engine::Engine* engine = hostEngine();
    dom::Document* doc = engine ? engine->document() : nullptr;
    if (!doc) return ev::throwError("new Image(): the engine has no document");
    dom::Element* el = doc->createElement("img");
    if (!el) return ev::throwError("new Image(): the document refused an <img>");
    HostNodeState* st = hostNodeStateFor(el);
    if (st) st->fromImageConstructor = true;
    return hostElementValue(el);
}

}  // namespace

const HostClass& htmlImageElementClass() {
    return g_imageClass;
}

void installImageGlobal() {
    g_imageClass.install("HTMLImageElement", 0, imageConstructor, decorateImageProto);
    g_imageClass.alias("Image");
    g_imageClass.inherit(htmlElementHostClass());
}

Value makeImageElementHandle(dom::Element* el) {
    HostNodeState* st = hostNodeStateFor(el);
    return g_imageClass.make(st, stateHandleDtor(st));
}

void primeImageFromMarkup(dom::Element* el) {
    // An <img> parsed from the page's own markup already carries its src, and
    // nothing will assign it again. Starting its load here, once, is what
    // makes the size true before the program's first read (from the header)
    // and the decode under way — the same answer the markup path gives on the
    // web. The layout walk sees the src already loaded and leaves it.
    const std::string src = el->getAttribute("src");
    if (src.empty() || src == el->imageProbedSrc()) return;
    loadHostImage(el, src);
}

}  // namespace bro::bronze_host
