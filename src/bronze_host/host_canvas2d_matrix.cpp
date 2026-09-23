#include "bronze_host/host_canvas2d_matrix.h"
#include "bronze_host/host_canvas_path2d.h"
#include "bronze_host/gl_internal.h"
#include "canvas/canvas_scene.h"
#include "dom/element.h"
#include <cmath>

namespace bro::bronze_host {

void Affine2D::reset() {
    a = 1.0; b = 0.0;
    c = 0.0; d = 1.0;
    e = 0.0; f = 0.0;
}

void Affine2D::translate(double tx, double ty) {
    e += a * tx + c * ty;
    f += b * tx + d * ty;
}

void Affine2D::scale(double sx, double sy) {
    a *= sx;
    b *= sx;
    c *= sy;
    d *= sy;
}

void Affine2D::rotate(double angleRad) {
    double cosA = std::cos(angleRad);
    double sinA = std::sin(angleRad);
    double na = a * cosA + c * sinA;
    double nb = b * cosA + d * sinA;
    double nc = -a * sinA + c * cosA;
    double nd = -b * sinA + d * cosA;
    a = na; b = nb;
    c = nc; d = nd;
}

void Affine2D::transform(double a2, double b2, double c2, double d2, double e2, double f2) {
    double na = a * a2 + c * b2;
    double nb = b * a2 + d * b2;
    double nc = a * c2 + c * d2;
    double nd = b * c2 + d * d2;
    double ne = a * e2 + c * f2 + e;
    double nf = b * e2 + d * f2 + f;
    a = na; b = nb;
    c = nc; d = nd;
    e = ne; f = nf;
}

void Affine2D::set(double na, double nb, double nc, double nd, double ne, double nf) {
    a = na; b = nb;
    c = nc; d = nd;
    e = ne; f = nf;
}

bool Affine2D::invert(double x, double y, double& outX, double& outY) const {
    double det = a * d - b * c;
    if (std::abs(det) < 1e-12) return false;
    double invDet = 1.0 / det;
    double dx = x - e;
    double dy = y - f;
    outX = (d * dx - c * dy) * invDet;
    outY = (-b * dx + a * dy) * invDet;
    return true;
}

Canvas2DTransformTracker::Canvas2DTransformTracker() {
    current_.reset();
}

void Canvas2DTransformTracker::save() {
    stack_.push_back(current_);
    for (auto& h : saveHooks_) h();
}

void Canvas2DTransformTracker::restore() {
    if (!stack_.empty()) {
        current_ = stack_.back();
        stack_.pop_back();
    }
    for (auto& h : restoreHooks_) h();
}

void Canvas2DTransformTracker::translate(double tx, double ty) {
    current_.translate(tx, ty);
}

void Canvas2DTransformTracker::scale(double sx, double sy) {
    current_.scale(sx, sy);
}

void Canvas2DTransformTracker::rotate(double angle) {
    current_.rotate(angle);
}

void Canvas2DTransformTracker::transform(double a, double b, double c, double d, double e, double f) {
    current_.transform(a, b, c, d, e, f);
}

void Canvas2DTransformTracker::setTransform(double a, double b, double c, double d, double e, double f) {
    current_.set(a, b, c, d, e, f);
}

void Canvas2DTransformTracker::resetTransform() {
    current_.reset();
}

void Canvas2DTransformTracker::reset() {
    current_.reset();
    stack_.clear();
}

Value Canvas2DTransformTracker::toDOMMatrixValue() const {
    ObjectBuilder m;
    m.set("a", ev::fromDouble(current_.a));
    m.set("b", ev::fromDouble(current_.b));
    m.set("c", ev::fromDouble(current_.c));
    m.set("d", ev::fromDouble(current_.d));
    m.set("e", ev::fromDouble(current_.e));
    m.set("f", ev::fromDouble(current_.f));
    m.set("m11", ev::fromDouble(current_.a));
    m.set("m12", ev::fromDouble(current_.b));
    m.set("m13", ev::fromDouble(0.0));
    m.set("m14", ev::fromDouble(0.0));
    m.set("m21", ev::fromDouble(current_.c));
    m.set("m22", ev::fromDouble(current_.d));
    m.set("m23", ev::fromDouble(0.0));
    m.set("m24", ev::fromDouble(0.0));
    m.set("m31", ev::fromDouble(0.0));
    m.set("m32", ev::fromDouble(0.0));
    m.set("m33", ev::fromDouble(1.0));
    m.set("m34", ev::fromDouble(0.0));
    m.set("m41", ev::fromDouble(current_.e));
    m.set("m42", ev::fromDouble(current_.f));
    m.set("m43", ev::fromDouble(0.0));
    m.set("m44", ev::fromDouble(1.0));
    m.set("is2D", ev::fromBool(true));
    bool isIdent = (current_.a == 1.0 && current_.b == 0.0 &&
                    current_.c == 0.0 && current_.d == 1.0 &&
                    current_.e == 0.0 && current_.f == 0.0);
    m.set("isIdentity", ev::fromBool(isIdent));
    return m.get();
}

void installCanvas2DTransformMethods(ObjectBuilder& b, dom::Element* el, std::shared_ptr<Canvas2DTransformTracker> tracker) {
    b.def("isPointInPath", 2, [el, tracker](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.empty()) return ev::fromBool(false);
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        HostCanvasPath2D* path = hostCanvasPath2DOf(a[0]);
        size_t argIdx = path ? 1 : 0;
        if (a.size() < argIdx + 2) return ev::fromBool(false);
        double x = ev::toDouble(a[argIdx]);
        double y = ev::toDouble(a[argIdx + 1]);
        std::string fillRule = "nonzero";
        if (a.size() > argIdx + 2 && ev::isString(a[argIdx + 2])) {
            fillRule = ev::toUtf8(a[argIdx + 2]);
        }
        double localX = x, localY = y;
        if (!tracker->current().invert(x, y, localX, localY)) {
            return ev::fromBool(false);
        }
        bool in = false;
        if (path) {
            in = cs->isPointInPath(path->snapshot(), static_cast<float>(localX),
                                   static_cast<float>(localY), fillRule);
        } else {
            in = cs->isPointInPath(static_cast<float>(localX),
                                   static_cast<float>(localY), fillRule);
        }
        return ev::fromBool(in);
    });

    b.def("save", 0, [el, tracker](Value, std::span<const Value>) -> Value {
        tracker->save();
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->save();
        }
        return ev::undefined();
    });

    b.def("restore", 0, [el, tracker](Value, std::span<const Value>) -> Value {
        tracker->restore();
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->restore();
        }
        return ev::undefined();
    });

    b.def("translate", 2, [el, tracker](Value, std::span<const Value> a) -> Value {
        if (a.size() >= 2) {
            double tx = ev::toDouble(a[0]);
            double ty = ev::toDouble(a[1]);
            tracker->translate(tx, ty);
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                cs->translate(static_cast<float>(tx), static_cast<float>(ty));
            }
        }
        return ev::undefined();
    });

    b.def("scale", 2, [el, tracker](Value, std::span<const Value> a) -> Value {
        if (a.size() >= 2) {
            double sx = ev::toDouble(a[0]);
            double sy = ev::toDouble(a[1]);
            tracker->scale(sx, sy);
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                cs->scale(static_cast<float>(sx), static_cast<float>(sy));
            }
        }
        return ev::undefined();
    });

    b.def("rotate", 1, [el, tracker](Value, std::span<const Value> a) -> Value {
        if (a.size() >= 1) {
            double angle = ev::toDouble(a[0]);
            tracker->rotate(angle);
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                cs->rotate(static_cast<float>(angle));
            }
        }
        return ev::undefined();
    });

    b.def("setTransform", 6, [el, tracker](Value, std::span<const Value> a) -> Value {
        if (a.empty()) {
            tracker->resetTransform();
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                cs->resetTransform();
            }
            return ev::undefined();
        }
        if (a.size() == 1 && ev::isObject(a[0])) {
            // Each getProperty may allocate (or run a getter), so every read
            // goes through the rooted argument slot a[0] and is reduced to a
            // number before the next one.
            auto member = [&a](const char* key, bool& present) {
                Value v = ev::getProperty(a[0], key);
                present = ev::isNumber(v);
                return present ? ev::toDouble(v) : 0.0;
            };
            bool hasA = false, hasD = false, unused = false;
            const double ma = member("a", hasA);
            const double mb = member("b", unused);
            const double mc = member("c", unused);
            const double md = member("d", hasD);
            const double me = member("e", unused);
            const double mf = member("f", unused);
            if (hasA && hasD) {
                tracker->setTransform(ma, mb, mc, md, me, mf);
                if (el && el->canvasScene()) {
                    auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                    cs->setTransform(static_cast<float>(ma), static_cast<float>(mb),
                                     static_cast<float>(mc), static_cast<float>(md),
                                     static_cast<float>(me), static_cast<float>(mf));
                }
                return ev::undefined();
            }
        }
        if (a.size() >= 6) {
            double ma = ev::toDouble(a[0]);
            double mb = ev::toDouble(a[1]);
            double mc = ev::toDouble(a[2]);
            double md = ev::toDouble(a[3]);
            double me = ev::toDouble(a[4]);
            double mf = ev::toDouble(a[5]);
            tracker->setTransform(ma, mb, mc, md, me, mf);
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                cs->setTransform(static_cast<float>(ma), static_cast<float>(mb),
                                 static_cast<float>(mc), static_cast<float>(md),
                                 static_cast<float>(me), static_cast<float>(mf));
            }
        } else {
            tracker->resetTransform();
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                cs->resetTransform();
            }
        }
        return ev::undefined();
    });

    b.def("resetTransform", 0, [el, tracker](Value, std::span<const Value>) -> Value {
        tracker->resetTransform();
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->resetTransform();
        }
        return ev::undefined();
    });

    b.def("transform", 6, [el, tracker](Value, std::span<const Value> a) -> Value {
        if (a.size() >= 6) {
            double ma = ev::toDouble(a[0]);
            double mb = ev::toDouble(a[1]);
            double mc = ev::toDouble(a[2]);
            double md = ev::toDouble(a[3]);
            double me = ev::toDouble(a[4]);
            double mf = ev::toDouble(a[5]);
            tracker->transform(ma, mb, mc, md, me, mf);
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                cs->transform(static_cast<float>(ma), static_cast<float>(mb),
                              static_cast<float>(mc), static_cast<float>(md),
                              static_cast<float>(me), static_cast<float>(mf));
            }
        }
        return ev::undefined();
    });

    b.def("getTransform", 0, [tracker](Value, std::span<const Value>) -> Value {
        return tracker->toDOMMatrixValue();
    });
}

}  // namespace bro::bronze_host
