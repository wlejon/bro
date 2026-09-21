#pragma once

#include "bronze_host/host_internal.h"
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPath.h>
#include <include/core/SkRRect.h>

namespace bro::bronze_host {

constexpr uint32_t kHostPath2DTag = 0x50415448u; // 'PATH'

struct HostCanvasPath2D {
    uint32_t tag = kHostPath2DTag;
    SkPathBuilder builder;

    SkPath snapshot() const {
        return builder.snapshot();
    }

    void closePath() { builder.close(); }
    void moveTo(float x, float y) { builder.moveTo(x, y); }
    void lineTo(float x, float y) { builder.lineTo(x, y); }
    void quadraticCurveTo(float cpx, float cpy, float x, float y) {
        builder.quadTo(cpx, cpy, x, y);
    }
    void bezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) {
        builder.cubicTo(cp1x, cp1y, cp2x, cp2y, x, y);
    }
    void arc(float cx, float cy, float radius, float startAngle, float endAngle, bool acw);
    void arcTo(float x1, float y1, float x2, float y2, float radius);
    void ellipse(float cx, float cy, float rx, float ry, float rotation,
                 float startAngle, float endAngle, bool acw);
    void rect(float x, float y, float w, float h);
    void roundRect(float x, float y, float w, float h, const SkVector radii[4]);
    void addPath(const SkPath& p);
};

HostCanvasPath2D* hostCanvasPath2DOf(Value v);
Value makePath2DValue();
Value makePath2DValue(const SkPath& path);
void installPath2DClass();

} // namespace bro::bronze_host
