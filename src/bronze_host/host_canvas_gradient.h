#pragma once

#include "bronze_host/host_internal.h"
#include <include/core/SkShader.h>
#include <vector>
#include <utility>
#include <cstdint>

namespace bro::bronze_host {

struct HostCanvasGradient {
    uint32_t tag = 0x47524144u;  // 'GRAD'
    enum Kind : uint8_t { kLinear = 0, kRadial = 1 };
    Kind kind = kLinear;
    float p[6] = {};
    std::vector<std::pair<float, uint32_t>> stops;

    sk_sp<SkShader> buildShader() const;
};

HostCanvasGradient* hostCanvasGradientOf(Value v);
Value makeLinearGradientValue(float x0, float y0, float x1, float y1);
Value makeRadialGradientValue(float x0, float y0, float r0, float x1, float y1, float r1);
void installCanvasGradientClass();

}  // namespace bro::bronze_host
