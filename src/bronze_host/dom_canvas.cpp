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
    return ev::setPrototype(b.get(), g_bitmapRendererClass.prototype());
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
        Value valV = argAt(a, 1);
        if (!ev::isObject(nameV) && !ev::isUndefined(nameV)) {
            std::string name = ev::toUtf8(nameV);
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
        CanvasState* cs = canvasStateFor(el);
        if (!cs || !cs->el) return ev::null();
        Value typeV = argAt(a, 0);
        if (ev::isObject(typeV)) return ev::null();
        std::string type = ev::toUtf8(typeV);
        if (!cs->contextType.empty() && cs->contextType != type) return ev::null();
        Value canvasObj = ev::isObject(thisVal) ? thisVal : hostElementValue(cs->el);
        if (type == "2d") {
            Value existing = ev::getProperty(canvasObj, "__bro_ctx2d__");
            if (ev::isObject(existing)) return existing;
            if (auto* eng = hostEngine()) {
                eng->createCanvasContext(cs->el);
            }
            Value ctx2d = makeCanvas2DContextValue(canvasObj, cs->el);
            ev::setProperty(canvasObj, "__bro_ctx2d__", ctx2d);
            cs->contextType = type;
            return ctx2d;
        }
        if (type == "bitmaprenderer") {
            Value existing = ev::getProperty(canvasObj, "__bro_ctxbmp__");
            if (ev::isObject(existing)) return existing;
            // Rooted: every call below allocates, and a raw Value does not
            // survive a collection.
            ev::Persistent canvasRoot(canvasObj);
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
            Value existing = ev::getProperty(canvasObj, "__bro_scene__");
            if (cs->el->sceneGraph() != nullptr && ev::isObject(existing)) return existing;
            scene::SceneGraph* sg = eng->createSceneContext(cs->el);
            if (!sg) return ev::null();
            Value scn = createSceneGraphValue(sg, cs->el);
            ev::setProperty(canvasObj, "__bro_scene__", scn);
            cs->contextType = type;
            return scn;
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
        Value existing = ev::getProperty(canvasObj, "__bro_gl__");
        if (cs->hasGl && ev::isObject(existing)) return existing;
        webgl::WebGL2RenderingContext* ctx = eng->createWebGL2Context(cs->el);
        if (!ctx) return ev::null();
        cs->glCtx = ctx;
        Value glValue = createGlContextValue(ctx, canvasObj);
        ev::setProperty(canvasObj, "__bro_gl__", glValue);
        cs->hasGl = true;
        cs->contextType = type;
        return glValue;
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
        std::vector<uint8_t> owned;
        const uint8_t* px = nullptr;
        int w = 0, h = 0;
        if (cs->glCtx) {
            if (cs->glCtx->readCanvasPixels(owned)) {
                px = owned.data();
                w = cs->glCtx->canvasWidth();
                h = cs->glCtx->canvasHeight();
            }
        } else if (auto* scene = static_cast<canvas::CanvasScene*>(cs->el->canvasScene())) {
            int cw = canvasWidthOf(cs);
            int ch = canvasHeightOf(cs);
            const uint8_t* p = scene->snapshotPixels(cw, ch);
            if (p) {
                owned.assign(p, p + static_cast<size_t>(cw) * ch * 4);
                px = owned.data();
                w = cw;
                h = ch;
            }
        }
        if (!px || w <= 0 || h <= 0) {
            return ev::fromUtf8("data:,");
        }
        std::vector<uint8_t> bytes;
        std::string outType = "image/png";
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
            if (!broimage::encode_png_memory(bytes, px, w, h, 4) || bytes.empty()) {
                return ev::fromUtf8("data:,");
            }
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
        Value callback = a[0];
        std::string type = "image/png";
        double quality = -1.0;
        if (a.size() > 1 && ev::isString(a[1])) {
            type = util::toLower(ev::toUtf8(a[1]));
        }
        if (a.size() > 2 && ev::isNumber(a[2])) {
            quality = ev::toDouble(a[2]);
        }
        std::vector<uint8_t> owned;
        const uint8_t* px = nullptr;
        int w = 0, h = 0;
        if (cs->glCtx) {
            if (cs->glCtx->readCanvasPixels(owned)) {
                px = owned.data();
                w = cs->glCtx->canvasWidth();
                h = cs->glCtx->canvasHeight();
            }
        } else if (auto* scene = static_cast<canvas::CanvasScene*>(cs->el->canvasScene())) {
            int cw = canvasWidthOf(cs);
            int ch = canvasHeightOf(cs);
            const uint8_t* p = scene->snapshotPixels(cw, ch);
            if (p) {
                owned.assign(p, p + static_cast<size_t>(cw) * ch * 4);
                px = owned.data();
                w = cw;
                h = ch;
            }
        }

        Value blobVal = ev::null();
        if (px && w > 0 && h > 0) {
            std::vector<uint8_t> bytes;
            std::string outType = "image/png";
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
                broimage::encode_png_memory(bytes, px, w, h, 4);
            }
            if (!bytes.empty()) {
                Value ab = ev::createArrayBuffer(std::span<const uint8_t>(bytes.data(), bytes.size()));
                Value blobCtor = ev::globalValue("Blob").value;
                if (ev::isFunction(blobCtor)) {
                    Value parts = ev::parseJson("[]").value;
                    ev::setElement(parts, 0, ab);
                    ObjectBuilder opts;
                    opts.set("type", ev::fromUtf8(outType));
                    const Value ctorArgs[2] = { parts, opts.get() };
                    auto res = ev::construct(blobCtor, std::span<const Value>(ctorArgs, 2));
                    if (!res.thrown) {
                        blobVal = res.value;
                    }
                }
            }
        }
        Value promise = ev::createPromise();
        ev::resolvePromise(promise, blobVal);
        Value thenFn = ev::getProperty(promise, "then");
        if (ev::isFunction(thenFn)) {
            const Value thenArgs[1] = { callback };
            ev::call(thenFn, promise, std::span<const Value>(thenArgs, 1));
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
