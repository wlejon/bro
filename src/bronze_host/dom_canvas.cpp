#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_canvas2d.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_window_open.h"

#include "engine/engine.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "canvas/canvas_scene.h"
#include "webgl/webgl2_context.h"
#include "broimage/encode.h"
#include "util/string_utils.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if BRO_WITH_3D
#include "scene/scene_graph.h"
#endif

namespace bro::bronze_host {

#if BRO_WITH_3D
Value createSceneGraphValue(scene::SceneGraph* sg, dom::Element* canvas);
#endif

namespace {

struct CanvasState {
    dom::Element* el = nullptr;
    webgl::WebGL2RenderingContext* glCtx = nullptr;
    bool hasGl = false;
    // The context type this canvas was first asked for. A canvas has ONE
    // context mode for its life (HTML: getContext with a different type on a
    // canvas already in a mode answers null), and the QuickJS-era cache was
    // keyed on exactly that rule. Without it `getContext('webgl') ||
    // getContext('2d')` gives one element both a WebGL drawing buffer and a
    // CanvasScene, and the compositor draws both.
    std::string contextType;
};

// The drawing-buffer resize behind `canvas.width = w` / `canvas.height = h`
// and the width/height attributes. The bitmap of a 2D canvas follows the
// intrinsic size (a zero is a real, empty bitmap there); a WebGL drawing
// buffer keeps its size on a zero, as the old binding guarded, because a 0x0
// FBO is a GL error every following draw repeats.
void resizeBacking(CanvasState* cs, int w, int h, bool widthChanged) {
    if (!cs || !cs->el) return;
    // A bitmaprenderer canvas displays the ImageBitmap it was handed at that
    // bitmap's own size; its width/height attributes do not resize or clear
    // it.
    if (cs->contextType == "bitmaprenderer") return;
    if (auto* cScene = static_cast<canvas::CanvasScene*>(cs->el->canvasScene())) {
        if (widthChanged) cScene->setIntrinsicWidth(w); else cScene->setIntrinsicHeight(h);
        cScene->reset();
    }
    if (cs->glCtx) {
        const int curW = cs->glCtx->canvasWidth();
        const int curH = cs->glCtx->canvasHeight();
        if (widthChanged) {
            if (w > 0 && w != curW) cs->glCtx->resize(w, curH);
        } else {
            if (h > 0 && h != curH) cs->glCtx->resize(curW, h);
        }
    }
}

std::unordered_map<dom::Element*, std::unique_ptr<CanvasState>> s_canvases;

CanvasState* canvasStateFor(dom::Element* el) {
    if (!el) return nullptr;
    auto it = s_canvases.find(el);
    return it != s_canvases.end() ? it->second.get() : nullptr;
}

int attributeOr(dom::Element* el, const char* name, int fallback) {
    if (!el) return fallback;
    const std::string& v = el->getAttribute(name);
    return v.empty() ? fallback : std::atoi(v.c_str());
}

int canvasWidthOf(CanvasState* cs) {
    if (!cs || !cs->el) return 300;
    if (cs->glCtx) return cs->glCtx->canvasWidth();
    return attributeOr(cs->el, "width", 300);
}

int canvasHeightOf(CanvasState* cs) {
    if (!cs || !cs->el) return 150;
    if (cs->glCtx) return cs->glCtx->canvasHeight();
    return attributeOr(cs->el, "height", 150);
}

// The canvas's bitmap as it is displayed, at ITS size — which is not always
// the width/height attributes: a bitmaprenderer canvas shows the ImageBitmap
// it was handed at that bitmap's size, and an attribute-less 2D canvas
// follows its layout box. The CanvasScene's own size is the one its surface
// has, so the snapshot is taken at exactly that. False with no pixels.
bool readCanvasBitmap(CanvasState* cs, std::vector<uint8_t>& out, int& w, int& h) {
    w = h = 0;
    if (!cs || !cs->el) return false;
    if (cs->glCtx) {
        if (!cs->glCtx->readCanvasPixels(out)) return false;
        w = cs->glCtx->canvasWidth();
        h = cs->glCtx->canvasHeight();
        return w > 0 && h > 0;
    }
    auto* scene = static_cast<canvas::CanvasScene*>(cs->el->canvasScene());
    if (!scene) return false;
    w = scene->width();
    h = scene->height();
    if (w <= 0 || h <= 0) return false;
    const uint8_t* p = scene->snapshotPixels(w, h);
    if (!p) return false;
    out.assign(p, p + static_cast<size_t>(w) * h * 4);
    return true;
}

// PNG, or JPEG when asked for (alpha composited onto black, as browsers do);
// an unknown type is PNG (HTML: "image/png" is the fallback).
bool encodeCanvasBitmap(const std::vector<uint8_t>& px, int w, int h, const std::string& type,
                        double quality, std::vector<uint8_t>& bytes, std::string& outType) {
    bytes.clear();
    outType = "image/png";
    if (type == "image/jpeg" || type == "image/jpg") {
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
        for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
            const uint8_t alpha = px[i * 4 + 3];
            for (int c = 0; c < 3; ++c) {
                rgb[i * 3 + c] = static_cast<uint8_t>((px[i * 4 + c] * alpha + 127) / 255);
            }
        }
        int q = (quality > 0.0 && quality <= 1.0) ? static_cast<int>(quality * 100.0 + 0.5) : 92;
        if (q < 1) q = 1;
        if (broimage::encode_jpeg_memory(bytes, rgb.data(), w, h, 3, q)) {
            outType = "image/jpeg";
        }
    }
    if (outType == "image/png") {
        bytes.clear();
        if (!broimage::encode_png_memory(bytes, px.data(), w, h, 4)) bytes.clear();
    }
    return !bytes.empty();
}

// ImageBitmapRenderingContext — getContext('bitmaprenderer'). The canvas gets
// an ordinary CanvasScene, and transferFromImageBitmap replaces its whole
// bitmap with the ImageBitmap's pixels (recorded as a putImageData, so it
// travels through the same command stream to the canvas worker as any 2D
// draw) and detaches the ImageBitmap, which is the "transfer".
HostClass g_bitmapRendererClass;

Value makeBitmapRendererContextValue(const ev::Persistent& canvasRoot, dom::Element* el) {
    static bool installed = false;
    if (!installed) {
        installed = true;
        g_bitmapRendererClass.install("ImageBitmapRenderingContext", 0, nullptr, nullptr);
    }
    ObjectBuilder b;
    b.set("canvas", canvasRoot.get());
    b.def("transferFromImageBitmap", 1, [el](Value, std::span<const Value> a) -> Value {
        CanvasState* cs = canvasStateFor(el);
        if (!cs || !cs->el) return ev::undefined();
        auto* scene = static_cast<canvas::CanvasScene*>(cs->el->canvasScene());
        Value arg = a.empty() ? ev::undefined() : a[0];
        if (ev::isNull(arg) || ev::isUndefined(arg)) {
            // null: the output bitmap becomes transparent black at the
            // canvas's own size.
            if (scene) {
                scene->setIntrinsicSize(canvasWidthOf(cs), canvasHeightOf(cs));
                scene->reset();
            }
            return ev::undefined();
        }
        HostImageBitmap* bmp = hostImageBitmapOfMut(arg);
        if (!bmp) {
            return ev::throwTypeError(
                "ImageBitmapRenderingContext.transferFromImageBitmap: argument is not an ImageBitmap");
        }
        if (bmp->closed) {
            return ev::throwValue(hostMakeDomError("InvalidStateError",
                "ImageBitmapRenderingContext.transferFromImageBitmap: the ImageBitmap is detached"));
        }
        if (scene && bmp->width > 0 && bmp->height > 0 && !bmp->pixels.empty()) {
            scene->setIntrinsicSize(bmp->width, bmp->height);
            scene->reset();
            scene->putImageData(bmp->pixels.data(), bmp->width, bmp->height, 0, 0);
        }
        bmp->closed = true;
        bmp->width = 0;
        bmp->height = 0;
        bmp->image = nullptr;
        bmp->pixels.clear();
        return ev::undefined();
    });
    Value proto = g_bitmapRendererClass.prototype();
    return ev::setPrototype(b.get(), proto);
}

}  // namespace

void cleanupCanvasForElement(dom::Element* el) {
    if (!el) return;
    s_canvases.erase(el);
}

void clearHostCanvases() {
    s_canvases.clear();
}

namespace {

Value makeCanvasValue(dom::Element* el) {
    auto& slot = s_canvases[el];
    if (!slot) {
        slot = std::make_unique<CanvasState>();
        slot->el = el;
    }
    CanvasState* cs = slot.get();

    ObjectBuilder b(makeElementHandleObject(el));
    installElementCore(b, el);

    b.accessor("width",
               [el](Value, std::span<const Value>) {
                   CanvasState* cs = canvasStateFor(el);
                   return ev::fromDouble(canvasWidthOf(cs));
               },
               [el](Value, std::span<const Value> a) {
                   CanvasState* cs = canvasStateFor(el);
                   if (!cs || !cs->el) return ev::undefined();
                   int w = i32At(a, 0);
                   cs->el->setAttribute("width", std::to_string(w));
                   resizeBacking(cs, w, 0, /*widthChanged=*/true);
                   return ev::undefined();
               });
    b.accessor("height",
               [el](Value, std::span<const Value>) {
                   CanvasState* cs = canvasStateFor(el);
                   return ev::fromDouble(canvasHeightOf(cs));
               },
               [el](Value, std::span<const Value> a) {
                   CanvasState* cs = canvasStateFor(el);
                   if (!cs || !cs->el) return ev::undefined();
                   int h = i32At(a, 0);
                   cs->el->setAttribute("height", std::to_string(h));
                   resizeBacking(cs, 0, h, /*widthChanged=*/false);
                   return ev::undefined();
               });

    b.accessor("clientWidth",
               [el](Value, std::span<const Value>) {
                   CanvasState* cs = canvasStateFor(el);
                   if (!cs || !cs->el) return ev::fromDouble(0);
                   if (auto* eng = hostEngine()) {
                       eng->flushLayoutForRead(cs->el->document());
                   }
                   auto& box = cs->el->layoutBox();
                   double w = box.contentRect.width;
                   return ev::fromDouble(w > 0 ? w : canvasWidthOf(cs));
               },
               nullptr);
    b.accessor("clientHeight",
               [el](Value, std::span<const Value>) {
                   CanvasState* cs = canvasStateFor(el);
                   if (!cs || !cs->el) return ev::fromDouble(0);
                   if (auto* eng = hostEngine()) {
                       eng->flushLayoutForRead(cs->el->document());
                   }
                   auto& box = cs->el->layoutBox();
                   double h = box.contentRect.height;
                   return ev::fromDouble(h > 0 ? h : canvasHeightOf(cs));
               },
               nullptr);

    b.def("getBoundingClientRect", 0, [el](Value, std::span<const Value>) {
        CanvasState* cs = canvasStateFor(el);
        if (!cs || !cs->el) return makeHostRectValue(0, 0, 0, 0);
        dom::AbsoluteRect r = borderBoxOf(cs->el);
        ObjectBuilder bRect;
        bRect.set("left", ev::fromDouble(r.x));
        bRect.set("top", ev::fromDouble(r.y));
        bRect.set("right", ev::fromDouble(r.x + r.width));
        bRect.set("bottom", ev::fromDouble(r.y + r.height));
        bRect.set("width", ev::fromDouble(r.width));
        bRect.set("height", ev::fromDouble(r.height));
        bRect.set("x", ev::fromDouble(r.x));
        bRect.set("y", ev::fromDouble(r.y));
        return bRect.get();
    });
    b.def("setAttribute", 2, [el](Value, std::span<const Value> a) {
        CanvasState* cs = canvasStateFor(el);
        if (!cs || !cs->el) return ev::undefined();
        Value nameV = argAt(a, 0);
        if (!ev::isObject(nameV) && !ev::isUndefined(nameV)) {
            std::string name = ev::toUtf8(nameV);
            // Read after the name's conversion, which allocates for a
            // non-string: the argument slot is current, a copy taken before
            // it would not be.
            Value valV = argAt(a, 1);
            std::string val = (!ev::isObject(valV) && !ev::isUndefined(valV)) ? ev::toUtf8(valV) : "";
            cs->el->setAttribute(name, val);
            if (name == "width") {
                resizeBacking(cs, std::atoi(val.c_str()), 0, /*widthChanged=*/true);
            } else if (name == "height") {
                resizeBacking(cs, 0, std::atoi(val.c_str()), /*widthChanged=*/false);
            }
        }
        return ev::undefined();
    });
    b.def("getContext", 1, [el](Value thisVal, std::span<const Value> a) {
        // `thisVal` is a plain copy, current only at entry, and almost every
        // call below allocates: root it first. The canvas object and each
        // context value built here live in Persistents, read back with get()
        // right where they are used.
        ev::Persistent thisRoot(thisVal);
        CanvasState* cs = canvasStateFor(el);
        if (!cs || !cs->el) return ev::null();
        Value typeV = argAt(a, 0);
        if (ev::isObject(typeV)) return ev::null();
        std::string type = ev::toUtf8(typeV);
        if (!cs->contextType.empty() && cs->contextType != type) return ev::null();
        ev::Persistent canvasRoot(ev::isObject(thisRoot.get()) ? thisRoot.get() : ev::undefined());
        if (!ev::isObject(canvasRoot.get())) canvasRoot.set(hostElementValue(cs->el));
        if (type == "2d") {
            Value existing = ev::getProperty(canvasRoot.get(), "__bro_ctx2d__");
            if (ev::isObject(existing)) return existing;
            if (auto* eng = hostEngine()) {
                eng->createCanvasContext(cs->el);
            }
            ev::Persistent ctxRoot(makeCanvas2DContextValue(canvasRoot.get(), cs->el));
            ev::setProperty(canvasRoot.get(), "__bro_ctx2d__", ctxRoot.get());
            cs->contextType = type;
            return ctxRoot.get();
        }
        if (type == "bitmaprenderer") {
            Value existing = ev::getProperty(canvasRoot.get(), "__bro_ctxbmp__");
            if (ev::isObject(existing)) return existing;
            if (auto* eng = hostEngine()) {
                eng->createCanvasContext(cs->el);
            }
            ev::Persistent ctxRoot(makeBitmapRendererContextValue(canvasRoot, cs->el));
            ev::setProperty(canvasRoot.get(), "__bro_ctxbmp__", ctxRoot.get());
            cs->contextType = type;
            return ctxRoot.get();
        }
        if (type == "scene") {
#if BRO_WITH_3D
            if (isChildRealm()) return ev::null();
            auto* eng = hostEngine();
            if (!eng) return ev::null();
            dom::Document* curDoc = currentHostDocument();
            if (curDoc && (eng->isWindowHostDocument(curDoc) || eng->isIframeDocument(curDoc))) return ev::null();
            Value existing = ev::getProperty(canvasRoot.get(), "__bro_scene__");
            if (cs->el->sceneGraph() != nullptr && ev::isObject(existing)) return existing;
            scene::SceneGraph* sg = eng->createSceneContext(cs->el);
            if (!sg) return ev::null();
            ev::Persistent scnRoot(createSceneGraphValue(sg, cs->el));
            ev::setProperty(canvasRoot.get(), "__bro_scene__", scnRoot.get());
            cs->contextType = type;
            return scnRoot.get();
#else
            return ev::null();
#endif
        }
        if (type != "webgl2" && type != "webgl") return ev::null();
        if (isChildRealm()) return ev::null();
        auto* eng = hostEngine();
        if (!eng) return ev::null();
        dom::Document* curDoc = currentHostDocument();
        if (curDoc && (eng->isWindowHostDocument(curDoc) || eng->isIframeDocument(curDoc))) return ev::null();
        Value existing = ev::getProperty(canvasRoot.get(), "__bro_gl__");
        if (cs->hasGl && ev::isObject(existing)) return existing;
        webgl::WebGL2RenderingContext* ctx = eng->createWebGL2Context(cs->el);
        if (!ctx) return ev::null();
        cs->glCtx = ctx;
        ev::Persistent glRoot(createGlContextValue(ctx, canvasRoot.get()));
        ev::setProperty(canvasRoot.get(), "__bro_gl__", glRoot.get());
        cs->hasGl = true;
        cs->contextType = type;
        return glRoot.get();
    });

    b.def("toDataURL", 2, [el](Value, std::span<const Value> a) -> Value {
        CanvasState* cs = canvasStateFor(el);
        if (!cs || !cs->el) return ev::fromUtf8("data:,");
        std::string type = "image/png";
        double quality = -1.0;
        if (!a.empty() && ev::isString(a[0])) {
            type = util::toLower(ev::toUtf8(a[0]));
        }
        if (a.size() > 1 && ev::isNumber(a[1])) {
            quality = ev::toDouble(a[1]);
        }
        std::vector<uint8_t> pixels;
        int w = 0, h = 0;
        std::vector<uint8_t> bytes;
        std::string outType;
        if (!readCanvasBitmap(cs, pixels, w, h) ||
            !encodeCanvasBitmap(pixels, w, h, type, quality, bytes, outType)) {
            return ev::fromUtf8("data:,");
        }
        std::string url = "data:" + outType + ";base64," + util::base64Encode(bytes.data(), bytes.size());
        return ev::fromUtf8(url);
    });

    b.def("toBlob", 3, [el](Value, std::span<const Value> a) -> Value {
        CanvasState* cs = canvasStateFor(el);
        if (!cs || !cs->el) return ev::undefined();
        if (a.empty() || !ev::isFunction(a[0])) {
            return ev::throwTypeError("toBlob requires a callback function");
        }
        // Everything from the encode to the promise allocates; the callback
        // and each value built on the way live in Persistents.
        ev::Persistent callback(a[0]);
        std::string type = "image/png";
        double quality = -1.0;
        if (a.size() > 1 && ev::isString(a[1])) {
            type = util::toLower(ev::toUtf8(a[1]));
        }
        if (a.size() > 2 && ev::isNumber(a[2])) {
            quality = ev::toDouble(a[2]);
        }
        std::vector<uint8_t> pixels;
        int w = 0, h = 0;
        ev::Persistent blobVal(ev::null());
        std::vector<uint8_t> bytes;
        std::string outType;
        if (readCanvasBitmap(cs, pixels, w, h) &&
            encodeCanvasBitmap(pixels, w, h, type, quality, bytes, outType)) {
            {
                ev::Persistent blobCtor(ev::globalValue("Blob").value);
                if (ev::isFunction(blobCtor.get())) {
                    ev::Persistent ab(ev::createArrayBuffer(std::span<const uint8_t>(bytes.data(), bytes.size())));
                    ev::Persistent parts(ev::makeArray(0));
                    parts.set(ev::setElement(parts.get(), 0, ab.get()));
                    ObjectBuilder opts;
                    opts.set("type", ev::fromUtf8(outType));
                    const Value ctorArgs[2] = { parts.get(), opts.get() };
                    auto res = ev::construct(blobCtor.get(), std::span<const Value>(ctorArgs, 2));
                    if (!res.thrown) {
                        blobVal.set(res.value);
                    }
                }
            }
        }
        ev::Persistent promise(ev::createPromise());
        ev::resolvePromise(promise.get(), blobVal.get());
        Value thenFn = ev::getProperty(promise.get(), "then");
        if (ev::isFunction(thenFn)) {
            const Value thenArgs[1] = { callback.get() };
            ev::call(thenFn, promise.get(), std::span<const Value>(thenArgs, 1));
        }
        return ev::undefined();
    });

    Value built = b.get();
    noteHostElementValue(el, built);
    return built;
}

}  // namespace

Value makeCanvasElementValue(dom::Element* el) {
    return makeCanvasValue(el);
}

}  // namespace bro::bronze_host
