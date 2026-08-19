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
// gl_textures.cpp keep asking one question.

#include "bronze_host/gl_internal.h"  // ObjectBuilder, argAt
#include "bronze_host/host_internal.h"

#include "dom/document.h"
#include "dom/element.h"
#include "engine/engine.h"

#include <memory>
#include <string>

namespace bro::bronze_host {

namespace {

HostClass g_imageClass;

// The image state behind this receiver, minted on first use. An <img> that has
// never been given a `src` still has to answer `width` and `complete`, so the
// state exists from the first read rather than from the first load.
HostImage* imageStateOf(Value self) {
    HostNodeState* st = hostNodeStateOfValue(self);
    if (!st || !st->el) return nullptr;
    if (!st->image) st->image = std::make_unique<HostImage>();
    return st->image.get();
}

Value sizeMember(Value self, int HostImage::*field) {
    HostImage* img = imageStateOf(self);
    return ev::fromDouble(img ? img->*field : 0);
}

Value imageSrcGetter(Value self, std::span<const Value>) {
    HostImage* img = imageStateOf(self);
    return ev::fromUtf8(img ? img->src : std::string());
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
    if (!st->image) st->image = std::make_unique<HostImage>();
    HostImage& img = *st->image;

    loadHostImage(img, src);

    // The attribute and the natural size too. This element is in a real
    // document, so if it is ever laid out the painter must find the picture
    // that was just decoded rather than probing the file a second time.
    st->el->setAttribute("src", src);
    if (img.ok) st->el->setImageNaturalSize(src, img.width, img.height);

    // Deferred, and for the reason the load path has always given: on the web
    // this event is a queued task, never synchronous with the assignment, and
    // firing it from inside the setter re-enters compiled code with the
    // caller's own load() still on the stack. Dispatched AT THE ELEMENT
    // through the engine, so an interpreted listener on the same node hears it
    // as well — which is the whole point of the image being a node.
    dom::Element* target = st->el;
    const bool loaded = img.ok;
    postHostTask([target, loaded]() {
        engine::Engine* engine = hostEngine();
        if (!engine) return;
        dom::Event evt(loaded ? "load" : "error", false, false);
        engine->dispatchElementEvent(target, evt);
    });
    return ev::undefined();
}

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
            HostImage* img = imageStateOf(self);
            return ev::fromBool(img && img->complete);
        },
        nullptr);

    // Stored and ignored: there is no network here, so there is no origin to be
    // cross. three.js assigns it on every ImageLoader load, and the assignment
    // lands as an own property over this default, which is what the web does
    // too.
    {
        Value nul = ev::null();
        b.set("crossOrigin", nul);
    }
}

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
    return hostElementValue(el);
}

}  // namespace

void installImageGlobal() {
    // AFTER installElementGlobals, and the only class in this layer that
    // depends on that order: the prototype below is chained onto
    // Element.prototype, which has to exist first.
    g_imageClass.install("Image", 0, imageConstructor, decorateImageProto);
    g_imageClass.alias("HTMLImageElement");
    g_imageClass.inherit(elementHostClass());
}

Value makeImageElementHandle(dom::Element* el) {
    return g_imageClass.make(hostNodeStateFor(el), [](void*) {});
}

void primeImageFromMarkup(dom::Element* el) {
    // An <img> parsed from the page's own markup already carries its src, and
    // nothing will assign it again. Decoding here, once, is what makes
    // `complete` and the size true before the program's first read — the same
    // answer the markup path gives on the web.
    const std::string src = el->getAttribute("src");
    if (src.empty()) return;
    HostNodeState* st = hostNodeStateFor(el);
    if (!st) return;
    if (!st->image) st->image = std::make_unique<HostImage>();
    loadHostImage(*st->image, src);
}

}  // namespace bro::bronze_host
