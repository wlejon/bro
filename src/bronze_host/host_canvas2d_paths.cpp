#include "bronze_host/host_canvas2d_paths.h"
#include "bronze_host/host_canvas_path2d.h"
#include "canvas/canvas_scene.h"

#include <cmath>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

bool parseSingleRadius(Value v, SkVector& out, std::string& err) {
    if (ev::isNumber(v)) {
        double d = ev::toDouble(v);
        if (d < 0.0) {
            err = "Failed to execute 'roundRect': radius must be non-negative";
            return false;
        }
        float f = static_cast<float>(d);
        out.set(f, f);
        return true;
    }
    if (ev::isObject(v)) {
        Value xv = ev::getProperty(v, "x");
        Value yv = ev::getProperty(v, "y");
        double xd = ev::isUndefined(xv) ? 0.0 : ev::toDouble(xv);
        double yd = ev::isUndefined(yv) ? 0.0 : ev::toDouble(yv);
        if (xd < 0.0 || yd < 0.0) {
            err = "Failed to execute 'roundRect': radius must be non-negative";
            return false;
        }
        out.set(static_cast<float>(xd), static_cast<float>(yd));
        return true;
    }
    out.set(0.0f, 0.0f);
    return true;
}

}  // namespace

bool parseRoundRectRadii(Value v, SkVector radii[4], std::string& err) {
    if (ev::isUndefined(v) || ev::isNull(v)) {
        for (int i = 0; i < 4; ++i) radii[i].set(0.0f, 0.0f);
        return true;
    }
    if (ev::isNumber(v)) {
        SkVector r;
        if (!parseSingleRadius(v, r, err)) return false;
        for (int i = 0; i < 4; ++i) radii[i] = r;
        return true;
    }
    if (ev::isObject(v)) {
        Value isArrFn = ev::getProperty(ev::globalValue("Array").value, "isArray");
        bool isArr = false;
        if (ev::isFunction(isArrFn)) {
            isArr = ev::toBool(ev::call(isArrFn, ev::undefined(), std::span<const Value>(&v, 1)).value);
        }
        if (isArr) {
            int len = static_cast<int>(ev::toDouble(ev::getProperty(v, "length")));
            if (len == 0 || len > 4) {
                err = "Failed to execute 'roundRect': 1 to 4 radii required";
                return false;
            }
            SkVector items[4];
            for (int i = 0; i < len; ++i) {
                std::string idxStr = std::to_string(i);
                Value itemVal = ev::getProperty(v, idxStr.c_str());
                if (!parseSingleRadius(itemVal, items[i], err)) return false;
            }
            if (len == 1) {
                radii[0] = radii[1] = radii[2] = radii[3] = items[0];
            } else if (len == 2) {
                radii[0] = items[0]; // top-left
                radii[1] = items[1]; // top-right
                radii[2] = items[0]; // bottom-right
                radii[3] = items[1]; // bottom-left
            } else if (len == 3) {
                radii[0] = items[0]; // top-left
                radii[1] = items[1]; // top-right
                radii[2] = items[2]; // bottom-right
                radii[3] = items[1]; // bottom-left
            } else if (len == 4) {
                radii[0] = items[0]; // top-left
                radii[1] = items[1]; // top-right
                radii[2] = items[2]; // bottom-right
                radii[3] = items[3]; // bottom-left
            }
            return true;
        } else {
            SkVector r;
            if (!parseSingleRadius(v, r, err)) return false;
            for (int i = 0; i < 4; ++i) radii[i] = r;
            return true;
        }
    }
    for (int i = 0; i < 4; ++i) radii[i].set(0.0f, 0.0f);
    return true;
}

void installCanvas2DPaths(ObjectBuilder& b, dom::Element* el) {
    b.def("beginPath", 0, [el](Value, std::span<const Value>) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->beginPath();
        }
        return ev::undefined();
    });

    b.def("closePath", 0, [el](Value, std::span<const Value>) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->closePath();
        }
        return ev::undefined();
    });

    b.def("fill", 2, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            HostCanvasPath2D* path = !a.empty() ? hostCanvasPath2DOf(a[0]) : nullptr;
            std::string fillRule = "nonzero";
            if (path) {
                if (a.size() > 1 && ev::isString(a[1])) fillRule = ev::toUtf8(a[1]);
                cs->fill(path->snapshot(), fillRule);
            } else {
                if (!a.empty() && ev::isString(a[0])) fillRule = ev::toUtf8(a[0]);
                cs->fill(fillRule);
            }
        }
        return ev::undefined();
    });

    b.def("stroke", 1, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            HostCanvasPath2D* path = !a.empty() ? hostCanvasPath2DOf(a[0]) : nullptr;
            if (path) {
                cs->stroke(path->snapshot());
            } else {
                cs->stroke();
            }
        }
        return ev::undefined();
    });

    b.def("clip", 2, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            HostCanvasPath2D* path = !a.empty() ? hostCanvasPath2DOf(a[0]) : nullptr;
            std::string fillRule = "nonzero";
            if (path) {
                if (a.size() > 1 && ev::isString(a[1])) fillRule = ev::toUtf8(a[1]);
                cs->clip(path->snapshot(), fillRule);
            } else {
                if (!a.empty() && ev::isString(a[0])) fillRule = ev::toUtf8(a[0]);
                cs->clip(fillRule);
            }
        }
        return ev::undefined();
    });

    b.def("reset", 0, [el](Value, std::span<const Value>) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->reset();
        }
        return ev::undefined();
    });

    b.def("fillRect", 4, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            float x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            float w = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
            float h = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
            cs->fillRect(x, y, w, h);
        }
        return ev::undefined();
    });

    b.def("strokeRect", 4, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            float x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            float w = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
            float h = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
            cs->strokeRect(x, y, w, h);
        }
        return ev::undefined();
    });

    b.def("clearRect", 4, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            float x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            float w = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
            float h = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
            cs->clearRect(x, y, w, h);
        }
        return ev::undefined();
    });

    b.def("moveTo", 2, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene() && a.size() >= 2) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->moveTo(static_cast<float>(ev::toDouble(a[0])), static_cast<float>(ev::toDouble(a[1])));
        }
        return ev::undefined();
    });

    b.def("lineTo", 2, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene() && a.size() >= 2) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->lineTo(static_cast<float>(ev::toDouble(a[0])), static_cast<float>(ev::toDouble(a[1])));
        }
        return ev::undefined();
    });

    b.def("rect", 4, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene() && a.size() >= 4) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->rect(static_cast<float>(ev::toDouble(a[0])), static_cast<float>(ev::toDouble(a[1])),
                     static_cast<float>(ev::toDouble(a[2])), static_cast<float>(ev::toDouble(a[3])));
        }
        return ev::undefined();
    });

    b.def("roundRect", 4, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.size() < 4) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        float x = static_cast<float>(ev::toDouble(a[0]));
        float y = static_cast<float>(ev::toDouble(a[1]));
        float w = static_cast<float>(ev::toDouble(a[2]));
        float h = static_cast<float>(ev::toDouble(a[3]));
        SkVector radii[4];
        std::string err;
        Value rVal = a.size() > 4 ? a[4] : ev::undefined();
        if (!parseRoundRectRadii(rVal, radii, err)) {
            return ev::throwRangeError(err);
        }
        cs->roundRect(x, y, w, h, radii);
        return ev::undefined();
    });

    b.def("arc", 6, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.size() < 5) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        float cx = static_cast<float>(ev::toDouble(a[0]));
        float cy = static_cast<float>(ev::toDouble(a[1]));
        float r  = static_cast<float>(ev::toDouble(a[2]));
        float sa = static_cast<float>(ev::toDouble(a[3]));
        float ea = static_cast<float>(ev::toDouble(a[4]));
        bool acw = a.size() >= 6 ? ev::toBool(a[5]) : false;
        cs->arc(cx, cy, r, sa, ea, acw);
        return ev::undefined();
    });

    b.def("arcTo", 5, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.size() < 5) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        cs->arcTo(static_cast<float>(ev::toDouble(a[0])),
                  static_cast<float>(ev::toDouble(a[1])),
                  static_cast<float>(ev::toDouble(a[2])),
                  static_cast<float>(ev::toDouble(a[3])),
                  static_cast<float>(ev::toDouble(a[4])));
        return ev::undefined();
    });

    b.def("bezierCurveTo", 6, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.size() < 6) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        cs->bezierCurveTo(static_cast<float>(ev::toDouble(a[0])),
                          static_cast<float>(ev::toDouble(a[1])),
                          static_cast<float>(ev::toDouble(a[2])),
                          static_cast<float>(ev::toDouble(a[3])),
                          static_cast<float>(ev::toDouble(a[4])),
                          static_cast<float>(ev::toDouble(a[5])));
        return ev::undefined();
    });

    b.def("quadraticCurveTo", 4, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.size() < 4) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        cs->quadraticCurveTo(static_cast<float>(ev::toDouble(a[0])),
                             static_cast<float>(ev::toDouble(a[1])),
                             static_cast<float>(ev::toDouble(a[2])),
                             static_cast<float>(ev::toDouble(a[3])));
        return ev::undefined();
    });

    b.def("ellipse", 8, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.size() < 7) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        float cx  = static_cast<float>(ev::toDouble(a[0]));
        float cy  = static_cast<float>(ev::toDouble(a[1]));
        float rx  = static_cast<float>(ev::toDouble(a[2]));
        float ry  = static_cast<float>(ev::toDouble(a[3]));
        float rot = static_cast<float>(ev::toDouble(a[4]));
        float sa  = static_cast<float>(ev::toDouble(a[5]));
        float ea  = static_cast<float>(ev::toDouble(a[6]));
        bool acw  = a.size() >= 8 ? ev::toBool(a[7]) : false;
        cs->ellipse(cx, cy, rx, ry, rot, sa, ea, acw);
        return ev::undefined();
    });

    b.def("polyline", 1, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.empty()) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        Value arg = a[0];
        auto tinfo = ev::typedArrayInfo(arg);
        if (tinfo.data && tinfo.byteLength >= sizeof(float) * 2) {
            int numPoints = static_cast<int>(tinfo.byteLength / (sizeof(float) * 2));
            cs->polyline(reinterpret_cast<const float*>(tinfo.data), numPoints);
            return ev::undefined();
        }
        if (ev::isObject(arg)) {
            uint32_t len = static_cast<uint32_t>(ev::toDouble(ev::getProperty(arg, "length")));
            if (len >= 2) {
                std::vector<float> pts;
                pts.reserve(len);
                for (uint32_t i = 0; i < len; ++i) {
                    pts.push_back(static_cast<float>(ev::toDouble(ev::getElement(arg, i))));
                }
                cs->polyline(pts.data(), static_cast<int>(pts.size() / 2));
            }
        }
        return ev::undefined();
    });

    b.def("isPointInPath", 2, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.size() < 2) return ev::fromBool(false);
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        if (auto* p = hostCanvasPath2DOf(a[0])) {
            float x = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            float y = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
            std::string fillRule = (a.size() > 3 && ev::isString(a[3])) ? ev::toUtf8(a[3]) : "nonzero";
            return ev::fromBool(cs->isPointInPath(p->snapshot(), x, y, fillRule));
        } else {
            float x = static_cast<float>(ev::toDouble(a[0]));
            float y = static_cast<float>(ev::toDouble(a[1]));
            std::string fillRule = (a.size() > 2 && ev::isString(a[2])) ? ev::toUtf8(a[2]) : "nonzero";
            return ev::fromBool(cs->isPointInPath(x, y, fillRule));
        }
    });
}

}  // namespace bro::bronze_host
