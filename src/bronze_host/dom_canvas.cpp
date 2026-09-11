#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_canvas2d.h"
#include "bronze_host/host_internal.h"

#include "engine/engine.h"
#include "dom/document.h"
#include "dom/element.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bro::bronze_host {

namespace {

struct CanvasState {
    dom::Element* el = nullptr;
    webgl::WebGL2RenderingContext* glCtx = nullptr;
    ev::Persistent jsObj;
    ev::Persistent glObj;
    ev::Persistent ctx2dObj;
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
        if (auto* eng = hostEngine()) {
            eng->flushLayoutForRead(cs->el->document());
        }
        auto& box = cs->el->layoutBox();
        double w = box.contentRect.width > 0 ? box.contentRect.width : canvasWidthOf(cs);
        double h = box.contentRect.height > 0 ? box.contentRect.height : canvasHeightOf(cs);
        double x = box.contentRect.x;
        double y = box.contentRect.y;
        ObjectBuilder r;
        r.set("left", ev::fromDouble(x));
        r.set("top", ev::fromDouble(y));
        r.set("right", ev::fromDouble(x + w));
        r.set("bottom", ev::fromDouble(y + h));
        r.set("width", ev::fromDouble(w));
        r.set("height", ev::fromDouble(h));
        r.set("x", ev::fromDouble(x));
        r.set("y", ev::fromDouble(y));
        return r.get();
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
                if (cs->glCtx) cs->glCtx->resize(w, cs->glCtx->canvasHeight());
            } else if (name == "height") {
                int h = std::atoi(val.c_str());
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
            if (ev::isObject(cs->ctx2dObj.get())) return cs->ctx2dObj.get();
            Value ctx2d = makeCanvas2DContextValue(cs->jsObj.get());
            cs->ctx2dObj.set(ctx2d);
            return ctx2d;
        }
        if (type != "webgl2" && type != "webgl") return ev::null();
        if (cs->hasGl) return cs->glObj.get();
        auto* eng = hostEngine();
        if (!eng) return ev::null();
        webgl::WebGL2RenderingContext* ctx = eng->createWebGL2Context(cs->el);
        if (!ctx) return ev::null();
        cs->glCtx = ctx;
        Value glValue = createGlContextValue(ctx, cs->jsObj.get());
        cs->glObj.set(glValue);
        cs->hasGl = true;
        return cs->glObj.get();
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
