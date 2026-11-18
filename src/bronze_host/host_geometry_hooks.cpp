// `__bro_geometry` — the host half of the geometry interfaces. DOMRect,
// DOMPoint, DOMMatrix, DOMQuad, the legacy SVGMatrix / SVGPoint / SVGRect and
// SVG elements' getBBox / getCTM / getScreenCTM are JavaScript
// (js/geometry.js); the SVG geometry they read is the layout's
// (layout/svg_geometry.h), and this is the door:
//
//   svgBBox(el)          [x, y, width, height] in the element's user space,
//                        or undefined outside an <svg> subtree
//   svgCTM(el, screen)   [a, b, c, d, e, f] to the outermost <svg>'s
//                        viewport, or (screen) to client coordinates
//
// The module puts its factories on the same object, and the host's rect and
// matrix results are made through them (makeHostRectValue,
// makeHostMatrix2DValue) so getBoundingClientRect answers a real DOMRect:
//
//   rect(x, y, w, h)             a DOMRect
//   rectList(array)              a DOMRectList over DOMRects
//   matrix2d(a, b, c, d, e, f)   a 2D DOMMatrix

#include "bronze_host/host_builder.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_js_modules.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"

#include "dom/document.h"
#include "dom/element.h"
#include "engine/engine.h"
#include "layout/svg_geometry.h"

#include <span>

namespace bro::bronze_host {

namespace {

Value numbersArray(const double* v, size_t n) {
    return hostArrayOf(n, [v](size_t i) { return ev::fromDouble(v[i]); });
}

// A factory the module put on `__bro_geometry`, called with `args`; the
// fallback when the module has not run (or threw).
template <typename Fallback>
Value callFactory(const char* name, std::span<const Value> args, Fallback fallback) {
    ev::GlobalValue hooks = ev::globalValue("__bro_geometry");
    if (hooks.found && ev::isObject(hooks.value)) {
        Value fn = ev::getProperty(hooks.value, name);
        if (ev::isFunction(fn)) {
            ev::CallResult r = ev::call(fn, ev::undefined(), args);
            if (!r.thrown) return r.value;
        }
    }
    return fallback();
}

}  // namespace

void installGeometryHooks() {
    ObjectBuilder b;
    b.def("svgBBox", 1, [](Value, std::span<const Value> a) {
        dom::Element* el = hostElementOf(argAt(a, 0));
        engine::Engine* eng = hostEngine();
        if (!el || !eng) return ev::undefined();
        eng->flushLayoutForRead(el->document());
        float r[4];
        if (!layout::svgElementBBox(el, r, eng->renderer())) return ev::undefined();
        const double d[4] = {r[0], r[1], r[2], r[3]};
        return numbersArray(d, 4);
    });
    b.def("svgCTM", 2, [](Value, std::span<const Value> a) {
        dom::Element* el = hostElementOf(argAt(a, 0));
        const bool screen = boolAt(a, 1);
        engine::Engine* eng = hostEngine();
        if (!el || !eng) return ev::undefined();
        eng->flushLayoutForRead(el->document());
        double m[6];
        if (!layout::svgElementCTM(el, screen, m)) return ev::undefined();
        if (screen) m[5] -= viewportScrollOf(el);
        return numbersArray(m, 6);
    });
    ev::registerGlobal("__bro_geometry", b.get());
}

Value makeHostRectValue(double x, double y, double w, double h) {
    const Value args[4] = {ev::fromDouble(x), ev::fromDouble(y), ev::fromDouble(w), ev::fromDouble(h)};
    return callFactory("rect", args, [&] {
        ObjectBuilder r;
        r.set("x", ev::fromDouble(x));
        r.set("y", ev::fromDouble(y));
        r.set("width", ev::fromDouble(w));
        r.set("height", ev::fromDouble(h));
        return r.get();
    });
}

Value makeHostRectListValue(Value rects) {
    const ev::Persistent list(rects);
    const Value args[1] = {list.get()};
    return callFactory("rectList", args, [&] { return list.get(); });
}

Value makeHostMatrix2DValue(double a, double b, double c, double d, double e, double f) {
    const Value args[6] = {ev::fromDouble(a), ev::fromDouble(b), ev::fromDouble(c),
                           ev::fromDouble(d), ev::fromDouble(e), ev::fromDouble(f)};
    return callFactory("matrix2d", args, [&] {
        const double m[6] = {a, b, c, d, e, f};
        return numbersArray(m, 6);
    });
}

}  // namespace bro::bronze_host
