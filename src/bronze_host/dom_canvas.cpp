#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_canvas2d.h"
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
    ev::Persistent jsObj;
    ev::Persistent glObj;
    ev::Persistent ctx2dObj;
    ev::Persistent sceneObj;
    bool hasGl = false;
};

std::vector<std::unique_ptr<CanvasState>> s_canvases;

int attributeOr(dom::Element* el, const char* name, int fallback) {
    const std::string& v = el->getAttribute(name);
    return v.empty() ? fallback : std::atoi(v.c_str());
}

int canvasWidthOf(CanvasState* cs) {
    if (cs->glCtx) return cs->glCtx->canvasWidth();
    return attributeOr(cs->el, "width", 300);
}

int canvasHeightOf(CanvasState* cs) {
    if (cs->glCtx) return cs->glCtx->canvasHeight();
    return attributeOr(cs->el, "height", 150);
}

Value makeCanvasValue(dom::Element* el) {
    auto owned = std::make_unique<CanvasState>();
    CanvasState* cs = owned.get();
    cs->el = el;
    s_canvases.push_back(std::move(owned));

    ObjectBuilder b(makeElementHandleObject(el));
    installElementCore(b, el);

    b.accessor("width",
               [cs](Value, std::span<const Value>) {
                   return ev::fromDouble(canvasWidthOf(cs));
               },
                [cs](Value, std::span<const Value> a) {
                    int w = i32At(a, 0);
                    cs->el->setAttribute("width", std::to_string(w));
                    if (auto* cScene = static_cast<canvas::CanvasScene*>(cs->el->canvasScene())) {
                        cScene->setIntrinsicWidth(w);
                        cScene->reset();
                    }
                    if (cs->glCtx) cs->glCtx->resize(w, cs->glCtx->canvasHeight());
                    return ev::undefined();
                });
    b.accessor("height",
               [cs](Value, std::span<const Value>) {
                   return ev::fromDouble(canvasHeightOf(cs));
               },
               [cs](Value, std::span<const Value> a) {
                   int h = i32At(a, 0);
                   cs->el->setAttribute("height", std::to_string(h));
                   if (auto* cScene = static_cast<canvas::CanvasScene*>(cs->el->canvasScene())) {
                        cScene->setIntrinsicHeight(h);
                        cScene->reset();
                    }
                   if (cs->glCtx) cs->glCtx->resize(cs->glCtx->canvasWidth(), h);
                   return ev::undefined();
               });

    b.accessor("clientWidth",
               [cs](Value, std::span<const Value>) {
                   if (auto* eng = hostEngine()) {
                       eng->flushLayoutForRead(cs->el->document());
                   }
                   auto& box = cs->el->layoutBox();
                   double w = box.contentRect.width;
                   return ev::fromDouble(w > 0 ? w : canvasWidthOf(cs));
               },
               nullptr);
    b.accessor("clientHeight",
               [cs](Value, std::span<const Value>) {
                   if (auto* eng = hostEngine()) {
                       eng->flushLayoutForRead(cs->el->document());
                   }
                   auto& box = cs->el->layoutBox();
                   double h = box.contentRect.height;
                   return ev::fromDouble(h > 0 ? h : canvasHeightOf(cs));
               },
               nullptr);

    b.def("getBoundingClientRect", 0, [cs](Value, std::span<const Value>) {
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
    b.def("setAttribute", 2, [cs](Value, std::span<const Value> a) {
        Value nameV = argAt(a, 0);
        Value valV = argAt(a, 1);
        if (!ev::isObject(nameV) && !ev::isUndefined(nameV)) {
            std::string name = ev::toUtf8(nameV);
            std::string val = (!ev::isObject(valV) && !ev::isUndefined(valV)) ? ev::toUtf8(valV) : "";
            cs->el->setAttribute(name, val);
            if (name == "width") {
                int w = std::atoi(val.c_str());
                if (auto* cScene = static_cast<canvas::CanvasScene*>(cs->el->canvasScene())) {
                    cScene->setIntrinsicWidth(w);
                    cScene->reset();
                }
                if (cs->glCtx) cs->glCtx->resize(w, cs->glCtx->canvasHeight());
            } else if (name == "height") {
                int h = std::atoi(val.c_str());
                if (auto* cScene = static_cast<canvas::CanvasScene*>(cs->el->canvasScene())) {
                    cScene->setIntrinsicHeight(h);
                    cScene->reset();
                }
                if (cs->glCtx) cs->glCtx->resize(cs->glCtx->canvasWidth(), h);
            }
        }
        return ev::undefined();
    });
    b.def("getContext", 1, [cs](Value, std::span<const Value> a) {
        Value typeV = argAt(a, 0);
        if (ev::isObject(typeV)) return ev::null();
        std::string type = ev::toUtf8(typeV);
        if (type == "2d") {
            if (auto* eng = hostEngine()) {
                eng->createCanvasContext(cs->el);
            }
            if (ev::isObject(cs->ctx2dObj.get())) return cs->ctx2dObj.get();
            Value ctx2d = makeCanvas2DContextValue(cs->jsObj.get(), cs->el);
            cs->ctx2dObj.set(ctx2d);
            return ctx2d;
        }
        if (type == "scene") {
#if BRO_WITH_3D
            if (isChildRealm()) return ev::null();
            auto* eng = hostEngine();
            if (!eng) return ev::null();
            dom::Document* curDoc = currentHostDocument();
            if (curDoc && (eng->isWindowHostDocument(curDoc) || eng->isIframeDocument(curDoc))) return ev::null();
            if (cs->el->sceneGraph() != nullptr && ev::isObject(cs->sceneObj.get())) return cs->sceneObj.get();
            scene::SceneGraph* sg = eng->createSceneContext(cs->el);
            if (!sg) return ev::null();
            Value scn = createSceneGraphValue(sg, cs->el);
            cs->sceneObj.set(scn);
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
        if (cs->hasGl) return cs->glObj.get();
        webgl::WebGL2RenderingContext* ctx = eng->createWebGL2Context(cs->el);
        if (!ctx) return ev::null();
        cs->glCtx = ctx;
        Value glValue = createGlContextValue(ctx, cs->jsObj.get());
        cs->glObj.set(glValue);
        cs->hasGl = true;
        return cs->glObj.get();
    });

    b.def("toDataURL", 2, [cs](Value, std::span<const Value> a) -> Value {
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

    b.def("toBlob", 3, [cs](Value, std::span<const Value> a) -> Value {
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
    cs->jsObj.set(built);
    noteHostElementValue(el, built);
    return built;
}

}  // namespace

Value makeCanvasElementValue(dom::Element* el) {
    return makeCanvasValue(el);
}

}  // namespace bro::bronze_host
