#include "bronze_host/host_canvas_gradient.h"
#include "bronze_host/gl_internal.h"
#include "canvas/canvas2d.h"
#include "util/string_utils.h"

#include <include/core/SkColor.h>
#include <include/core/SkPoint.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradient.h>

#include <cmath>

namespace bro::bronze_host {

namespace {

HostClass g_canvasGradientClass;

void hostCanvasGradientDtor(void* p) {
    delete static_cast<HostCanvasGradient*>(p);
}

void decorateCanvasGradientProto(ObjectBuilder& b) {
    b.def("addColorStop", 2, [](Value self, std::span<const Value> a) {
        auto* g = hostCanvasGradientOf(self);
        if (!g || a.size() < 2) return ev::undefined();
        double offset = ev::toDouble(a[0]);
        if (!std::isfinite(offset)) return ev::undefined();
        offset = offset < 0.0 ? 0.0 : (offset > 1.0 ? 1.0 : offset);
        std::string color = ev::toUtf8(a[1]);
        uint8_t r = 0, gc = 0, b_col = 0, a_col = 255;
        // A gradient belongs to no element, so its currentcolor is opaque
        // black (HTML, addColorStop).
        const bool current = util::toLower(util::trim(color)) == "currentcolor";
        if (!current && !canvas::parseCSSColor(color, r, gc, b_col, a_col)) return ev::undefined();
        uint32_t argb = (static_cast<uint32_t>(a_col) << 24) |
                        (static_cast<uint32_t>(r)     << 16) |
                        (static_cast<uint32_t>(gc)    << 8)  |
                         static_cast<uint32_t>(b_col);
        g->stops.emplace_back(static_cast<float>(offset), argb);
        return ev::undefined();
    });
}

}  // namespace

sk_sp<SkShader> HostCanvasGradient::buildShader() const {
    if (stops.empty()) return nullptr;

    std::vector<SkColor4f> colors;
    std::vector<float> positions;
    colors.reserve(stops.size() + 1);
    positions.reserve(stops.size() + 1);
    for (const auto& [off, argb] : stops) {
        colors.push_back(SkColor4f::FromColor(static_cast<SkColor>(argb)));
        positions.push_back(off);
    }
    if (colors.size() == 1) {
        colors.push_back(colors[0]);
        positions.push_back(positions[0]);
    }

    SkGradient::Colors c(
        SkSpan<const SkColor4f>(colors.data(),    colors.size()),
        SkSpan<const float>    (positions.data(), positions.size()),
        SkTileMode::kClamp);
    SkGradient grad(c, SkGradient::Interpolation{});

    if (kind == kLinear) {
        SkPoint pts[2] = { {p[0], p[1]}, {p[2], p[3]} };
        return SkShaders::LinearGradient(pts, grad);
    }
    return SkShaders::TwoPointConicalGradient(
        { p[0], p[1] }, p[2],
        { p[3], p[4] }, p[5],
        grad);
}

void installCanvasGradientClass() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    g_canvasGradientClass.install("CanvasGradient", 0, nullptr, decorateCanvasGradientProto);
}

HostCanvasGradient* hostCanvasGradientOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* g = static_cast<HostCanvasGradient*>(ev::handleData(v));
    if (!g || g->tag != 0x47524144u) return nullptr;
    return g;
}

Value makeLinearGradientValue(float x0, float y0, float x1, float y1) {
    installCanvasGradientClass();
    auto* g = new HostCanvasGradient();
    g->kind = HostCanvasGradient::kLinear;
    g->p[0] = x0; g->p[1] = y0;
    g->p[2] = x1; g->p[3] = y1;
    return g_canvasGradientClass.make(g, hostCanvasGradientDtor);
}

Value makeRadialGradientValue(float x0, float y0, float r0, float x1, float y1, float r1) {
    installCanvasGradientClass();
    auto* g = new HostCanvasGradient();
    g->kind = HostCanvasGradient::kRadial;
    g->p[0] = x0; g->p[1] = y0; g->p[2] = r0;
    g->p[3] = x1; g->p[4] = y1; g->p[5] = r1;
    return g_canvasGradientClass.make(g, hostCanvasGradientDtor);
}

}  // namespace bro::bronze_host
