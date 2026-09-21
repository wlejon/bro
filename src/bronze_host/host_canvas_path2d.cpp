// MSVC's <cmath> only defines M_PI under _USE_MATH_DEFINES, and it has to be
// set before the first include that pulls <math.h> in.
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include "bronze_host/host_canvas_path2d.h"
#include "bronze_host/host_canvas2d_paths.h"
#include "bronze_host/gl_internal.h"
#include <include/core/SkMatrix.h>
#include <include/core/SkRect.h>
#include <include/utils/SkParsePath.h>
#include <cmath>

namespace bro::bronze_host {

namespace {

HostClass g_path2DClass;

void path2dDtor(void* p) {
    delete static_cast<HostCanvasPath2D*>(p);
}

} // namespace

void HostCanvasPath2D::arc(float cx, float cy, float radius, float startAngle, float endAngle, bool acw) {
    float startDeg = startAngle * 180.0f / static_cast<float>(M_PI);
    float endDeg = endAngle * 180.0f / static_cast<float>(M_PI);

    float sweep = endDeg - startDeg;
    if (acw && sweep > 0) sweep -= 360.0f;
    else if (!acw && sweep < 0) sweep += 360.0f;

    SkRect oval = SkRect::MakeXYWH(cx - radius, cy - radius, radius * 2, radius * 2);

    float sx = cx + radius * std::cos(startAngle);
    float sy = cy + radius * std::sin(startAngle);

    SkPath current = builder.snapshot();
    if (current.isEmpty()) {
        builder.moveTo(sx, sy);
    } else {
        builder.lineTo(sx, sy);
    }

    if (std::abs(sweep) >= 360.0f) {
        builder.addOval(oval, acw ? SkPathDirection::kCCW : SkPathDirection::kCW);
    } else {
        builder.arcTo(oval, startDeg, sweep, false);
    }
}

void HostCanvasPath2D::arcTo(float x1, float y1, float x2, float y2, float radius) {
    builder.arcTo(SkPoint::Make(x1, y1), SkPoint::Make(x2, y2), radius);
}

void HostCanvasPath2D::ellipse(float cx, float cy, float rx, float ry, float rotation,
                              float startAngle, float endAngle, bool acw) {
    float startDeg = startAngle * 180.0f / static_cast<float>(M_PI);
    float endDeg = endAngle * 180.0f / static_cast<float>(M_PI);
    float sweep = endDeg - startDeg;
    if (acw && sweep > 0) sweep -= 360.0f;
    else if (!acw && sweep < 0) sweep += 360.0f;

    SkPathBuilder tmp;
    SkRect oval = SkRect::MakeXYWH(-rx, -ry, rx * 2, ry * 2);
    float sx = rx * std::cos(startAngle);
    float sy = ry * std::sin(startAngle);
    tmp.moveTo(sx, sy);
    if (std::abs(sweep) >= 360.0f) {
        tmp.addOval(oval, acw ? SkPathDirection::kCCW : SkPathDirection::kCW);
    } else {
        tmp.arcTo(oval, startDeg, sweep, false);
    }

    SkPath tmpPath = tmp.detach();
    SkMatrix mat;
    mat.setRotate(rotation * 180.0f / static_cast<float>(M_PI));
    mat.postTranslate(cx, cy);
    tmpPath = tmpPath.makeTransform(mat);

    builder.addPath(tmpPath);
}

void HostCanvasPath2D::rect(float x, float y, float w, float h) {
    builder.addRect(SkRect::MakeXYWH(x, y, w, h));
}

void HostCanvasPath2D::roundRect(float x, float y, float w, float h, const SkVector radii[4]) {
    SkRRect rrect;
    SkRect r = SkRect::MakeXYWH(x, y, w, h).makeSorted();
    rrect.setRectRadii(r, radii);
    builder.addRRect(rrect);
}

void HostCanvasPath2D::addPath(const SkPath& p) {
    builder.addPath(p);
}

HostCanvasPath2D* hostCanvasPath2DOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    void* p = ev::handleData(v);
    if (!p) return nullptr;
    auto* host = static_cast<HostCanvasPath2D*>(p);
    return (host->tag == kHostPath2DTag) ? host : nullptr;
}

Value makePath2DValue() {
    auto* p = new HostCanvasPath2D();
    return g_path2DClass.make(p, path2dDtor);
}

Value makePath2DValue(const SkPath& path) {
    auto* p = new HostCanvasPath2D();
    p->builder.addPath(path);
    return g_path2DClass.make(p, path2dDtor);
}

void installPath2DClass() {
    static bool s_installed = false;
    if (s_installed) return;
    s_installed = true;

    g_path2DClass.install("Path2D", 0,
        [](Value, std::span<const Value> a) -> Value {
            auto* p = new HostCanvasPath2D();
            if (!a.empty()) {
                if (HostCanvasPath2D* other = hostCanvasPath2DOf(a[0])) {
                    p->builder.addPath(other->snapshot());
                } else if (ev::isString(a[0])) {
                    std::string d = ev::toUtf8(a[0]);
                    SkPath svgPath;
                    if (SkParsePath::FromSVGString(d.c_str(), &svgPath)) {
                        p->builder.addPath(svgPath);
                    }
                }
            }
            return g_path2DClass.make(p, path2dDtor);
        },
        [](ObjectBuilder& proto) {
            proto.def("closePath", 0, [](Value self, std::span<const Value>) -> Value {
                if (auto* p = hostCanvasPath2DOf(self)) p->closePath();
                return ev::undefined();
            });

            proto.def("moveTo", 2, [](Value self, std::span<const Value> a) -> Value {
                if (auto* p = hostCanvasPath2DOf(self)) {
                    float x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
                    float y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
                    p->moveTo(x, y);
                }
                return ev::undefined();
            });

            proto.def("lineTo", 2, [](Value self, std::span<const Value> a) -> Value {
                if (auto* p = hostCanvasPath2DOf(self)) {
                    float x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
                    float y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
                    p->lineTo(x, y);
                }
                return ev::undefined();
            });

            proto.def("quadraticCurveTo", 4, [](Value self, std::span<const Value> a) -> Value {
                if (auto* p = hostCanvasPath2DOf(self)) {
                    float cpx = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
                    float cpy = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
                    float x   = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
                    float y   = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
                    p->quadraticCurveTo(cpx, cpy, x, y);
                }
                return ev::undefined();
            });

            proto.def("bezierCurveTo", 6, [](Value self, std::span<const Value> a) -> Value {
                if (auto* p = hostCanvasPath2DOf(self)) {
                    float cp1x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
                    float cp1y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
                    float cp2x = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
                    float cp2y = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
                    float x    = a.size() > 4 ? static_cast<float>(ev::toDouble(a[4])) : 0.0f;
                    float y    = a.size() > 5 ? static_cast<float>(ev::toDouble(a[5])) : 0.0f;
                    p->bezierCurveTo(cp1x, cp1y, cp2x, cp2y, x, y);
                }
                return ev::undefined();
            });

            proto.def("arc", 5, [](Value self, std::span<const Value> a) -> Value {
                if (auto* p = hostCanvasPath2DOf(self)) {
                    float x       = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
                    float y       = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
                    float radius  = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
                    float startA  = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
                    float endA    = a.size() > 4 ? static_cast<float>(ev::toDouble(a[4])) : 0.0f;
                    bool acw      = a.size() > 5 ? ev::toBool(a[5]) : false;
                    p->arc(x, y, radius, startA, endA, acw);
                }
                return ev::undefined();
            });

            proto.def("arcTo", 5, [](Value self, std::span<const Value> a) -> Value {
                if (auto* p = hostCanvasPath2DOf(self)) {
                    float x1     = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
                    float y1     = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
                    float x2     = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
                    float y2     = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
                    float radius = a.size() > 4 ? static_cast<float>(ev::toDouble(a[4])) : 0.0f;
                    p->arcTo(x1, y1, x2, y2, radius);
                }
                return ev::undefined();
            });

            proto.def("ellipse", 7, [](Value self, std::span<const Value> a) -> Value {
                if (auto* p = hostCanvasPath2DOf(self)) {
                    float x        = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
                    float y        = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
                    float rx       = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
                    float ry       = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
                    float rot      = a.size() > 4 ? static_cast<float>(ev::toDouble(a[4])) : 0.0f;
                    float startA   = a.size() > 5 ? static_cast<float>(ev::toDouble(a[5])) : 0.0f;
                    float endA     = a.size() > 6 ? static_cast<float>(ev::toDouble(a[6])) : 0.0f;
                    bool acw       = a.size() > 7 ? ev::toBool(a[7]) : false;
                    p->ellipse(x, y, rx, ry, rot, startA, endA, acw);
                }
                return ev::undefined();
            });

            proto.def("rect", 4, [](Value self, std::span<const Value> a) -> Value {
                if (auto* p = hostCanvasPath2DOf(self)) {
                    float x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
                    float y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
                    float w = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
                    float h = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
                    p->rect(x, y, w, h);
                }
                return ev::undefined();
            });

            proto.def("roundRect", 4, [](Value self, std::span<const Value> a) -> Value {
                if (auto* p = hostCanvasPath2DOf(self)) {
                    if (a.size() < 4) return ev::undefined();
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
                    p->roundRect(x, y, w, h, radii);
                }
                return ev::undefined();
            });

            proto.def("addPath", 1, [](Value self, std::span<const Value> a) -> Value {
                if (auto* p = hostCanvasPath2DOf(self)) {
                    if (!a.empty()) {
                        if (auto* other = hostCanvasPath2DOf(a[0])) {
                            SkPath src = other->snapshot();
                            if (a.size() > 1 && ev::isObject(a[1])) {
                                Value m = a[1];
                                float ma = static_cast<float>(ev::toDouble(ev::getProperty(m, "a")));
                                float mb = static_cast<float>(ev::toDouble(ev::getProperty(m, "b")));
                                float mc = static_cast<float>(ev::toDouble(ev::getProperty(m, "c")));
                                float md = static_cast<float>(ev::toDouble(ev::getProperty(m, "d")));
                                float me = static_cast<float>(ev::toDouble(ev::getProperty(m, "e")));
                                float mf = static_cast<float>(ev::toDouble(ev::getProperty(m, "f")));
                                SkMatrix skMat;
                                skMat.setAll(ma, mc, me, mb, md, mf, 0, 0, 1);
                                src = src.makeTransform(skMat);
                            }
                            p->addPath(src);
                        }
                    }
                }
                return ev::undefined();
            });
        });
}

} // namespace bro::bronze_host
