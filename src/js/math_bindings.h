#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// Bindings for bromath types exposed to JS. Currently:
//   - bro.math.SpatialHash3D — uniform-grid 3D spatial index over points
//     and spheres (radius / nearest / AABB queries).
class MathBindings {
public:
    static void install(JSContext* ctx);
    static void cleanup(JSContext* ctx);
};

} // namespace bro::js
