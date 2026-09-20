#pragma once

#include "bronze_host/host_internal.h"
#include <memory>
#include <vector>

namespace bro::dom {
class Element;
}

namespace bro::bronze_host {

struct Affine2D {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
    double e = 0.0;
    double f = 0.0;

    void reset();
    void translate(double tx, double ty);
    void scale(double sx, double sy);
    void rotate(double angleRad);
    void transform(double a2, double b2, double c2, double d2, double e2, double f2);
    void set(double na, double nb, double nc, double nd, double ne, double nf);
    bool invert(double x, double y, double& outX, double& outY) const;
};

class Canvas2DTransformTracker {
public:
    Canvas2DTransformTracker();

    void save();
    void restore();

    void translate(double tx, double ty);
    void scale(double sx, double sy);
    void rotate(double angle);
    void transform(double a, double b, double c, double d, double e, double f);
    void setTransform(double a, double b, double c, double d, double e, double f);
    void resetTransform();

    const Affine2D& current() const { return current_; }
    Value toDOMMatrixValue() const;

    using Hook = std::function<void()>;
    void addSaveHook(Hook h) { saveHooks_.push_back(std::move(h)); }
    void addRestoreHook(Hook h) { restoreHooks_.push_back(std::move(h)); }

private:
    Affine2D current_;
    std::vector<Affine2D> stack_;
    std::vector<Hook> saveHooks_;
    std::vector<Hook> restoreHooks_;
};

void installCanvas2DTransformMethods(ObjectBuilder& b, dom::Element* el, std::shared_ptr<Canvas2DTransformTracker> tracker);

}  // namespace bro::bronze_host
