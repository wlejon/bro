// =============================================================================
// bro_math_c_abi.cpp — C++ forwarding implementations for bro.math
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_math_c_abi.h"
#include <bromath/scalar.h>
#include <bromath/angle.h>
#include <bromath/hash.h>
#include <bromath/spatial_hash.h>
#include <bromath/rng.h>
#include <bromath/smoother.h>
#include <cstring>

extern "C" {


double bro_math_lerp(double a, double b, double t) {
    return static_cast<double>(bromath::lerp(static_cast<float>(a), static_cast<float>(b), static_cast<float>(t)));
}

double bro_math_clamp(double x, double lo, double hi) {
    return static_cast<double>(bromath::clamp(static_cast<float>(x), static_cast<float>(lo), static_cast<float>(hi)));
}

double bro_math_saturate(double x) {
    return static_cast<double>(bromath::saturate(static_cast<float>(x)));
}

double bro_math_invLerp(double a, double b, double x) {
    return static_cast<double>(bromath::invLerp(static_cast<float>(a), static_cast<float>(b), static_cast<float>(x)));
}

double bro_math_remap(double x, double inMin, double inMax, double outMin, double outMax) {
    return static_cast<double>(bromath::remap(static_cast<float>(x), static_cast<float>(inMin), static_cast<float>(inMax),
                                              static_cast<float>(outMin), static_cast<float>(outMax)));
}

double bro_math_smoothstep(double e0, double e1, double x) {
    return static_cast<double>(bromath::smoothstep(static_cast<float>(e0), static_cast<float>(e1), static_cast<float>(x)));
}

double bro_math_smootherstep(double e0, double e1, double x) {
    return static_cast<double>(bromath::smootherstep(static_cast<float>(e0), static_cast<float>(e1), static_cast<float>(x)));
}

double bro_math_degToRad(double deg) {
    return static_cast<double>(bromath::deg2rad(static_cast<float>(deg)));
}

double bro_math_radToDeg(double rad) {
    return static_cast<double>(bromath::rad2deg(static_cast<float>(rad)));
}

double bro_math_wrapAngle(double a) {
    return static_cast<double>(bromath::wrapAngle(static_cast<float>(a)));
}

double bro_math_angleDiff(double a, double b) {
    return static_cast<double>(bromath::angleDelta(static_cast<float>(a), static_cast<float>(b)));
}

double bro_math_fnv1a32(const char* data, double seed) {
    if (!data) return 0.0;
    uint32_t s = (seed == 0.0) ? 2166136261u : static_cast<uint32_t>(seed);
    return static_cast<double>(bromath::fnv1a32(data, std::strlen(data), s));
}

double bro_math_hashU32(double x) {
    return static_cast<double>(bromath::hashU32(static_cast<uint32_t>(x)));
}

double bro_math_cellHash(double x, double y, double z) {
    return static_cast<double>(bromath::cellHash(static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(z)));
}

// SpatialHash3D
void* bro_SpatialHash3D_create(double cellSize, double bucketCount) {
    (void)bucketCount;
    return new bromath::SpatialHash3D(static_cast<float>(cellSize > 0.0 ? cellSize : 1.0));
}

void bro_SpatialHash3D_destroy(void* self) {
    delete static_cast<bromath::SpatialHash3D*>(self);
}

void* bro_SpatialHash3D_insert(void* self, double id, double x, double y, double z) {
    if (self) {
        static_cast<bromath::SpatialHash3D*>(self)->insert(
            bromath::Vec3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)},
            static_cast<int32_t>(id));
    }
    return self;
}

bool bro_SpatialHash3D_remove(void* self, double id, double x, double y, double z) {
    (void)x; (void)y; (void)z;
    if (!self) return false;
    static_cast<bromath::SpatialHash3D*>(self)->remove(static_cast<int32_t>(id));
    return true;
}

void* bro_SpatialHash3D_clear(void* self) {
    if (self) static_cast<bromath::SpatialHash3D*>(self)->clear();
    return self;
}

double bro_SpatialHash3D_get_size(void* self) {
    if (!self) return 0.0;
    return static_cast<double>(static_cast<bromath::SpatialHash3D*>(self)->size());
}

double bro_SpatialHash3D_nearest(void* self, double x, double y, double z, double maxDist) {
    if (!self) return -1.0;
    return static_cast<double>(static_cast<bromath::SpatialHash3D*>(self)->nearest(
        bromath::Vec3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)},
        static_cast<float>(maxDist)));
}

// Rng
struct RngState {
    uint64_t state = 0;
};

void* bro_Rng_create(double seed) {
    auto* r = new RngState();
    r->state = static_cast<uint64_t>(seed);
    return r;
}

void bro_Rng_destroy(void* self) {
    delete static_cast<RngState*>(self);
}

void* bro_Rng_reseed(void* self, double seed) {
    if (self) static_cast<RngState*>(self)->state = static_cast<uint64_t>(seed);
    return self;
}

double bro_Rng_float01(void* self) {
    if (!self) return 0.0;
    return static_cast<double>(bromath::randFloat01(static_cast<RngState*>(self)->state));
}

double bro_Rng_signed(void* self) {
    if (!self) return 0.0;
    return static_cast<double>(bromath::randSigned(static_cast<RngState*>(self)->state));
}

double bro_Rng_range(void* self, double lo, double hi) {
    if (!self) return 0.0;
    return static_cast<double>(bromath::randRange(static_cast<RngState*>(self)->state, static_cast<float>(lo), static_cast<float>(hi)));
}

int32_t bro_Rng_int(void* self, int32_t lo, int32_t hi) {
    if (!self) return 0;
    return bromath::randInt(static_cast<RngState*>(self)->state, lo, hi);
}

double bro_Rng_uint32(void* self) {
    if (!self) return 0.0;
    return static_cast<double>(static_cast<uint32_t>(bromath::splitmix64(static_cast<RngState*>(self)->state) >> 32));
}

double bro_Rng_normal(void* self) {
    if (!self) return 0.0;
    return static_cast<double>(bromath::randNormal(static_cast<RngState*>(self)->state));
}

// Smoother
void* bro_Smoother_create(double timeMs, double sampleRate) {
    auto* s = new bromath::Smoother();
    if (timeMs > 0.0 && sampleRate > 0.0) {
        bromath::smootherSetTime(*s, static_cast<float>(timeMs), static_cast<float>(sampleRate));
    }
    return s;
}

void bro_Smoother_destroy(void* self) {
    delete static_cast<bromath::Smoother*>(self);
}

void* bro_Smoother_setTime(void* self, double timeMs, double sampleRate) {
    if (self) bromath::smootherSetTime(*static_cast<bromath::Smoother*>(self), static_cast<float>(timeMs), static_cast<float>(sampleRate));
    return self;
}

void* bro_Smoother_reset(void* self, double value) {
    if (self) bromath::smootherReset(*static_cast<bromath::Smoother*>(self), static_cast<float>(value));
    return self;
}

void* bro_Smoother_setTarget(void* self, double t) {
    if (self) bromath::smootherTarget(*static_cast<bromath::Smoother*>(self), static_cast<float>(t));
    return self;
}

double bro_Smoother_tick(void* self) {
    if (!self) return 0.0;
    return static_cast<double>(bromath::smootherTick(*static_cast<bromath::Smoother*>(self)));
}

double bro_Smoother_tickN(void* self, int32_t n) {
    if (!self) return 0.0;
    return static_cast<double>(bromath::smootherTickN(*static_cast<bromath::Smoother*>(self), n));
}

double bro_Smoother_get_current(void* self) {
    if (!self) return 0.0;
    return static_cast<double>(static_cast<bromath::Smoother*>(self)->current);
}

double bro_Smoother_get_target(void* self) {
    if (!self) return 0.0;
    return static_cast<double>(static_cast<bromath::Smoother*>(self)->target);
}

double bro_Smoother_get_coeff(void* self) {
    if (!self) return 0.0;
    return static_cast<double>(static_cast<bromath::Smoother*>(self)->coeff);
}

} // extern "C"
