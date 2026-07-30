#include "js/math_bindings.h"

#include <qjsbind/qjsbind.h>

#include <bromath/aabb.h>
#include <bromath/angle.h>
#include <bromath/color.h>
#include <bromath/curves.h>
#include <bromath/frustum.h>
#include <bromath/grid.h>
#include <bromath/hash.h>
#include <bromath/plane.h>
#include <bromath/ray.h>
#include <bromath/rng.h>
#include <bromath/scalar.h>
#include <bromath/segment.h>
#include <bromath/smoother.h>
#include <bromath/spatial_hash.h>
#include <bromath/sphere.h>
#include <bromath/vec.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::js {

// ─────────────────────────────────────────────────────────────────────────────
// Marshalling helpers
//
// Vectors cross the JS boundary as plain objects ({x,y,z} / {x,y}) or arrays
// ([x,y,z] / [x,y]) on the way in, and always as objects on the way out — the
// same shape scene/audio APIs use. Free functions are attached straight onto
// the existing bro.math object via mfn() (a thin wrapper over qjsbind's static
// function trampoline) so they live alongside the SpatialHash3D constructor.
// ─────────────────────────────────────────────────────────────────────────────

static double propNum(JSContext* ctx, JSValueConst obj, const char* key) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    double d = 0;
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

static double idxNum(JSContext* ctx, JSValueConst arr, uint32_t i) {
    JSValue v = JS_GetPropertyUint32(ctx, arr, i);
    double d = 0;
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

static bool readVec3(JSContext* ctx, JSValueConst v, bromath::Vec3& o) {
    if (JS_IsArray(v)) {
        o = {(float)idxNum(ctx, v, 0), (float)idxNum(ctx, v, 1),
             (float)idxNum(ctx, v, 2)};
        return true;
    }
    if (JS_IsObject(v)) {
        o = {(float)propNum(ctx, v, "x"), (float)propNum(ctx, v, "y"),
             (float)propNum(ctx, v, "z")};
        return true;
    }
    return false;
}

static bool readVec2(JSContext* ctx, JSValueConst v, bromath::Vec2& o) {
    if (JS_IsArray(v)) {
        o = {(float)idxNum(ctx, v, 0), (float)idxNum(ctx, v, 1)};
        return true;
    }
    if (JS_IsObject(v)) {
        o = {(float)propNum(ctx, v, "x"), (float)propNum(ctx, v, "y")};
        return true;
    }
    return false;
}

static JSValue vec3ToJS(JSContext* ctx, bromath::Vec3 v) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, v.x));
    JS_SetPropertyStr(ctx, o, "y", JS_NewFloat64(ctx, v.y));
    JS_SetPropertyStr(ctx, o, "z", JS_NewFloat64(ctx, v.z));
    return o;
}

static JSValue vec2ToJS(JSContext* ctx, bromath::Vec2 v) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, v.x));
    JS_SetPropertyStr(ctx, o, "y", JS_NewFloat64(ctx, v.y));
    return o;
}

static JSValue colorToJS(JSContext* ctx, bromath::Color c) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "r", JS_NewFloat64(ctx, c.r));
    JS_SetPropertyStr(ctx, o, "g", JS_NewFloat64(ctx, c.g));
    JS_SetPropertyStr(ctx, o, "b", JS_NewFloat64(ctx, c.b));
    JS_SetPropertyStr(ctx, o, "a", JS_NewFloat64(ctx, c.a));
    return o;
}

// Ray hit → null on miss, else { t, point:{x,y,z}, normal:{x,y,z} }.
static JSValue rayHitToJS(JSContext* ctx, const bromath::RayHit& h) {
    if (!h.hit) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "t", JS_NewFloat64(ctx, h.t));
    JS_SetPropertyStr(ctx, o, "point", vec3ToJS(ctx, h.position));
    JS_SetPropertyStr(ctx, o, "normal", vec3ToJS(ctx, h.normal));
    return o;
}

#define REQ_V3(name, val)                                                      \
    bromath::Vec3 name;                                                        \
    if (!readVec3(ctx, val, name))                                             \
        return JS_ThrowTypeError(ctx, "expected a {x,y,z} or [x,y,z] vector")

#define REQ_V2(name, val)                                                      \
    bromath::Vec2 name;                                                        \
    if (!readVec2(ctx, val, name))                                             \
        return JS_ThrowTypeError(ctx, "expected a {x,y} or [x,y] vector")

// Attach an auto-marshalling free function onto an arbitrary object.
template <typename Fn>
static void mfn(JSContext* ctx, JSValue obj, const char* name, Fn&& fn) {
    qjsbind::set_function(ctx, obj, name, std::forward<Fn>(fn));
}

// ─────────────────────────────────────────────────────────────────────────────
// SpatialHash3D — uniform-grid 3D spatial index (unchanged)
// ─────────────────────────────────────────────────────────────────────────────

struct SpatialHashWrapper {
    std::unique_ptr<bromath::SpatialHash3D> sh;
    SpatialHashWrapper(float cellSize)
        : sh(std::make_unique<bromath::SpatialHash3D>(cellSize)) {}
};
using SHW = SpatialHashWrapper;

static void installSpatialHash(JSContext* ctx) {
    qjsbind::Class<SHW>(ctx, "SpatialHash3D")
    .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> SHW* {
        double cs = 1.0;
        if (argc > 0) JS_ToFloat64(ctx, &cs, argv[0]);
        return new SHW((float)cs);
    })
    .method("reset", [](SHW* w, double cellSize) {
        w->sh->reset((float)cellSize);
    }, qjsbind::returns_this)
    .method("clear", [](SHW* w) { w->sh->clear(); }, qjsbind::returns_this)
    .method("insert", [](SHW* w, double x, double y, double z, int id) {
        w->sh->insert({(float)x, (float)y, (float)z}, (int32_t)id);
    }, qjsbind::returns_this)
    .method("insertSphere", [](SHW* w, double x, double y, double z,
                               double radius, int id) {
        w->sh->insert(bromath::Sphere{{(float)x, (float)y, (float)z},
                                      (float)radius}, (int32_t)id);
    }, qjsbind::returns_this)
    .method("remove", [](SHW* w, int id) {
        w->sh->remove((int32_t)id);
    }, qjsbind::returns_this)
    .method("radiusQuery", [](SHW* w, JSContext* ctx, double x, double y, double z,
                              double radius) -> JSValue {
        std::vector<int32_t> ids;
        w->sh->radiusQuery({(float)x, (float)y, (float)z}, (float)radius, ids);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < ids.size(); i++) {
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewInt32(ctx, ids[i]));
        }
        return arr;
    })
    .method("queryAABB", [](SHW* w, JSContext* ctx,
                            double minX, double minY, double minZ,
                            double maxX, double maxY, double maxZ) -> JSValue {
        std::vector<int32_t> ids;
        bromath::AABB3 box{
            {(float)minX, (float)minY, (float)minZ},
            {(float)maxX, (float)maxY, (float)maxZ}};
        w->sh->queryAABB(box, ids);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < ids.size(); i++) {
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewInt32(ctx, ids[i]));
        }
        return arr;
    })
    .method("nearest", [](SHW* w, double x, double y, double z, double maxRadius) -> int {
        return (int)w->sh->nearest({(float)x, (float)y, (float)z}, (float)maxRadius);
    })
    .get("size",      [](SHW* w) { return (int)w->sh->size(); })
    .get("cellSize",  [](SHW* w) -> double { return (double)w->sh->cellSize(); })
    .get("maxRadius", [](SHW* w) -> double { return (double)w->sh->maxRadius(); })
    ;
}

// ─────────────────────────────────────────────────────────────────────────────
// Rng — deterministic SplitMix64 generator carrying its own 64-bit state
// ─────────────────────────────────────────────────────────────────────────────

struct RngWrapper { uint64_t state = 0; };
using RNGW = RngWrapper;

static void installRng(JSContext* ctx) {
    qjsbind::Class<RNGW>(ctx, "Rng")
    .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> RNGW* {
        int64_t seed = 0;
        if (argc > 0) JS_ToInt64(ctx, &seed, argv[0]);
        RNGW* w = new RNGW();
        w->state = (uint64_t)seed;
        return w;
    })
    .method("reseed", [](RNGW* w, JSContext* ctx, JSValue v) {
        int64_t seed = 0;
        JS_ToInt64(ctx, &seed, v);
        w->state = (uint64_t)seed;
    }, qjsbind::returns_this)
    // Uniform float in [0,1).
    .method("float01", [](RNGW* w) -> double {
        return (double)bromath::randFloat01(w->state);
    })
    // Uniform float in [-1,1).
    .method("signed", [](RNGW* w) -> double {
        return (double)bromath::randSigned(w->state);
    })
    // Uniform float in [lo,hi).
    .method("range", [](RNGW* w, double lo, double hi) -> double {
        return (double)bromath::randRange(w->state, (float)lo, (float)hi);
    })
    // Uniform integer in [loInclusive, hiInclusive].
    .method("int", [](RNGW* w, int lo, int hi) -> int {
        return bromath::randInt(w->state, lo, hi);
    })
    // 32 fresh bits as an unsigned integer (0 .. 2^32-1).
    .method("uint32", [](RNGW* w) -> double {
        return (double)(uint32_t)(bromath::splitmix64(w->state) >> 32);
    })
    // Standard normal (mean 0, stddev 1).
    .method("normal", [](RNGW* w) -> double {
        return (double)bromath::randNormal(w->state);
    })
    // 2D gaussian {x,y}, both ~ N(0, sigma).
    .method("gaussian2D", [](RNGW* w, JSContext* ctx, double sigma) -> JSValue {
        return vec2ToJS(ctx, bromath::randGaussian2D(w->state, (float)sigma));
    })
    // Uniform point inside the unit disc (XY) → {x,y}.
    .method("inUnitDisc", [](RNGW* w, JSContext* ctx) -> JSValue {
        return vec2ToJS(ctx, bromath::randInUnitDisc(w->state));
    })
    // Uniform point inside the unit sphere → {x,y,z}.
    .method("inUnitSphere", [](RNGW* w, JSContext* ctx) -> JSValue {
        return vec3ToJS(ctx, bromath::randInUnitSphere(w->state));
    })
    // Uniform point on the unit sphere surface → {x,y,z}.
    .method("onUnitSphere", [](RNGW* w, JSContext* ctx) -> JSValue {
        return vec3ToJS(ctx, bromath::randOnUnitSphere(w->state));
    })
    ;
}

// ─────────────────────────────────────────────────────────────────────────────
// Smoother — one-pole exponential ramp
// ─────────────────────────────────────────────────────────────────────────────

struct SmootherWrapper { bromath::Smoother s; };
using SMW = SmootherWrapper;

static void installSmoother(JSContext* ctx) {
    qjsbind::Class<SMW>(ctx, "Smoother")
    .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> SMW* {
        SMW* w = new SMW();
        if (argc >= 2) {
            double timeMs = 0, sr = 0;
            JS_ToFloat64(ctx, &timeMs, argv[0]);
            JS_ToFloat64(ctx, &sr, argv[1]);
            bromath::smootherSetTime(w->s, (float)timeMs, (float)sr);
        }
        return w;
    })
    // Time to close ~95% of the gap, given the tick rate. Returns this.
    .method("setTime", [](SMW* w, double timeMs, double sampleRate) {
        bromath::smootherSetTime(w->s, (float)timeMs, (float)sampleRate);
    }, qjsbind::returns_this)
    // Snap current and target to value (no ramp). Returns this.
    .method("reset", [](SMW* w, double value) {
        bromath::smootherReset(w->s, (float)value);
    }, qjsbind::returns_this)
    // Set the value being chased. Returns this.
    .method("setTarget", [](SMW* w, double t) {
        bromath::smootherTarget(w->s, (float)t);
    }, qjsbind::returns_this)
    // Advance one tick; returns the new current value.
    .method("tick", [](SMW* w) -> double {
        return (double)bromath::smootherTick(w->s);
    })
    // Advance n ticks; returns the new current value.
    .method("tickN", [](SMW* w, int n) -> double {
        return (double)bromath::smootherTickN(w->s, n);
    })
    .get("current", [](SMW* w) -> double { return (double)w->s.current; })
    .get("target",  [](SMW* w) -> double { return (double)w->s.target; })
    .get("coeff",   [](SMW* w) -> double { return (double)w->s.coeff; })
    ;
}

// ─────────────────────────────────────────────────────────────────────────────
// Free functions: curves, color, scalar/angle, geometry queries, grid/hash
// ─────────────────────────────────────────────────────────────────────────────

static void installFreeFunctions(JSContext* ctx, JSValue m) {
    // ── Curves ──────────────────────────────────────────────────────────────
    // CSS-style cubic-bezier easing: sample y at x in [0,1].
    mfn(ctx, m, "cubicEase",
        [](double p1x, double p1y, double p2x, double p2y, double x) -> double {
            bromath::CubicEase c{(float)p1x, (float)p1y, (float)p2x, (float)p2y};
            return (double)bromath::ccubicEase(c, (float)x);
        });
    mfn(ctx, m, "bezier",
        [](JSContext* ctx, JSValue p0, JSValue p1, JSValue p2, JSValue p3,
           double t) -> JSValue {
            REQ_V3(a, p0); REQ_V3(b, p1); REQ_V3(c, p2); REQ_V3(d, p3);
            return vec3ToJS(ctx, bromath::cbezier(a, b, c, d, (float)t));
        });
    mfn(ctx, m, "bezierTangent",
        [](JSContext* ctx, JSValue p0, JSValue p1, JSValue p2, JSValue p3,
           double t) -> JSValue {
            REQ_V3(a, p0); REQ_V3(b, p1); REQ_V3(c, p2); REQ_V3(d, p3);
            return vec3ToJS(ctx, bromath::cbezierTangent(a, b, c, d, (float)t));
        });
    mfn(ctx, m, "catmullRom",
        [](JSContext* ctx, JSValue p0, JSValue p1, JSValue p2, JSValue p3,
           double t) -> JSValue {
            REQ_V3(a, p0); REQ_V3(b, p1); REQ_V3(c, p2); REQ_V3(d, p3);
            return vec3ToJS(ctx, bromath::ccatmullRom(a, b, c, d, (float)t));
        });
    mfn(ctx, m, "hermite",
        [](JSContext* ctx, JSValue p0, JSValue m0, JSValue p1, JSValue m1,
           double t) -> JSValue {
            REQ_V3(a, p0); REQ_V3(ta, m0); REQ_V3(b, p1); REQ_V3(tb, m1);
            return vec3ToJS(ctx, bromath::chermite(a, ta, b, tb, (float)t));
        });

    // ── Color (linear-RGBA primary; Color8 channels are 0..255) ─────────────
    mfn(ctx, m, "fromHex",
        [](JSContext* ctx, std::string hex) -> JSValue {
            return colorToJS(ctx, bromath::cfromHex(hex.c_str()));
        });
    mfn(ctx, m, "fromHSV",
        [](JSContext* ctx, double h, double s, double v, JSValue a) -> JSValue {
            double av = 1.0;
            if (!JS_IsUndefined(a)) JS_ToFloat64(ctx, &av, a);
            return colorToJS(ctx,
                bromath::cfromHSV((float)h, (float)s, (float)v, (float)av));
        });
    mfn(ctx, m, "fromColor8",
        [](JSContext* ctx, double r, double g, double b, JSValue a) -> JSValue {
            double av = 255.0;
            if (!JS_IsUndefined(a)) JS_ToFloat64(ctx, &av, a);
            bromath::Color8 c8{(uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)av};
            return colorToJS(ctx, bromath::cfromColor8(c8));
        });
    mfn(ctx, m, "toColor8",
        [](JSContext* ctx, double r, double g, double b, JSValue a) -> JSValue {
            double av = 1.0;
            if (!JS_IsUndefined(a)) JS_ToFloat64(ctx, &av, a);
            bromath::Color8 c8 = bromath::ctoColor8(
                bromath::Color{(float)r, (float)g, (float)b, (float)av});
            JSValue o = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, o, "r", JS_NewInt32(ctx, c8.r));
            JS_SetPropertyStr(ctx, o, "g", JS_NewInt32(ctx, c8.g));
            JS_SetPropertyStr(ctx, o, "b", JS_NewInt32(ctx, c8.b));
            JS_SetPropertyStr(ctx, o, "a", JS_NewInt32(ctx, c8.a));
            return o;
        });
    mfn(ctx, m, "linearToSrgb",
        [](double c) -> double { return (double)bromath::clinearToSrgb((float)c); });
    mfn(ctx, m, "srgbToLinear",
        [](double c) -> double { return (double)bromath::csrgbToLinear((float)c); });

    // ── Scalar / angle ──────────────────────────────────────────────────────
    mfn(ctx, m, "lerp", [](double a, double b, double t) -> double {
        return (double)bromath::lerp((float)a, (float)b, (float)t);
    });
    mfn(ctx, m, "clamp", [](double x, double lo, double hi) -> double {
        return (double)bromath::clamp((float)x, (float)lo, (float)hi);
    });
    mfn(ctx, m, "saturate", [](double x) -> double {
        return (double)bromath::saturate((float)x);
    });
    mfn(ctx, m, "invLerp", [](double a, double b, double x) -> double {
        return (double)bromath::invLerp((float)a, (float)b, (float)x);
    });
    mfn(ctx, m, "remap",
        [](double x, double inMin, double inMax, double outMin, double outMax) -> double {
            return (double)bromath::remap((float)x, (float)inMin, (float)inMax,
                                          (float)outMin, (float)outMax);
        });
    mfn(ctx, m, "smoothstep", [](double e0, double e1, double x) -> double {
        return (double)bromath::smoothstep((float)e0, (float)e1, (float)x);
    });
    mfn(ctx, m, "smootherstep", [](double e0, double e1, double x) -> double {
        return (double)bromath::smootherstep((float)e0, (float)e1, (float)x);
    });
    mfn(ctx, m, "deg2rad", [](double d) -> double {
        return (double)bromath::deg2rad((float)d);
    });
    mfn(ctx, m, "rad2deg", [](double r) -> double {
        return (double)bromath::rad2deg((float)r);
    });
    mfn(ctx, m, "wrapAngle", [](double a) -> double {
        return (double)bromath::wrapAngle((float)a);
    });
    mfn(ctx, m, "wrapAngle2Pi", [](double a) -> double {
        return (double)bromath::wrapAngle2Pi((float)a);
    });
    mfn(ctx, m, "angleDelta", [](double from, double to) -> double {
        return (double)bromath::angleDelta((float)from, (float)to);
    });
    mfn(ctx, m, "angleLerp", [](double from, double to, double t) -> double {
        return (double)bromath::angleLerp((float)from, (float)to, (float)t);
    });

    // ── Ray intersection queries (null on miss; else {t,point,normal}) ──────
    mfn(ctx, m, "rayIntersectAABB",
        [](JSContext* ctx, JSValue origin, JSValue dir, JSValue bmin,
           JSValue bmax) -> JSValue {
            REQ_V3(o, origin); REQ_V3(d, dir);
            REQ_V3(lo, bmin); REQ_V3(hi, bmax);
            bromath::Ray r{o, d};
            return rayHitToJS(ctx, bromath::rIntersectAABB(r, bromath::AABB3{lo, hi}));
        });
    mfn(ctx, m, "rayIntersectSphere",
        [](JSContext* ctx, JSValue origin, JSValue dir, JSValue center,
           double radius) -> JSValue {
            REQ_V3(o, origin); REQ_V3(d, dir); REQ_V3(c, center);
            bromath::Ray r{o, d};
            return rayHitToJS(ctx,
                bromath::rIntersectSphere(r, bromath::Sphere{c, (float)radius}));
        });
    mfn(ctx, m, "rayIntersectPlane",
        [](JSContext* ctx, JSValue origin, JSValue dir, JSValue normal,
           double d) -> JSValue {
            REQ_V3(o, origin); REQ_V3(dd, dir); REQ_V3(n, normal);
            bromath::Ray r{o, dd};
            return rayHitToJS(ctx,
                bromath::rIntersectPlane(r, bromath::Plane{n, (float)d}));
        });
    mfn(ctx, m, "rayIntersectTriangle",
        [](JSContext* ctx, JSValue origin, JSValue dir, JSValue v0, JSValue v1,
           JSValue v2, JSValue cull) -> JSValue {
            REQ_V3(o, origin); REQ_V3(d, dir);
            REQ_V3(a, v0); REQ_V3(b, v1); REQ_V3(c, v2);
            bool backface = JS_ToBool(ctx, cull) > 0;
            bromath::Ray r{o, d};
            return rayHitToJS(ctx,
                bromath::rIntersectTriangle(r, a, b, c, backface));
        });

    // ── Plane / sphere / AABB ───────────────────────────────────────────────
    mfn(ctx, m, "planeSignedDistance",
        [](JSContext* ctx, JSValue normal, double d, JSValue point) -> JSValue {
            REQ_V3(n, normal); REQ_V3(p, point);
            return JS_NewFloat64(ctx,
                (double)bromath::psignedDistance(bromath::Plane{n, (float)d}, p));
        });
    mfn(ctx, m, "planeProject",
        [](JSContext* ctx, JSValue normal, double d, JSValue point) -> JSValue {
            REQ_V3(n, normal); REQ_V3(p, point);
            return vec3ToJS(ctx,
                bromath::pproject(bromath::Plane{n, (float)d}, p));
        });
    mfn(ctx, m, "sphereContains",
        [](JSContext* ctx, JSValue center, double radius, JSValue point) -> JSValue {
            REQ_V3(c, center); REQ_V3(p, point);
            return JS_NewBool(ctx,
                bromath::scontains(bromath::Sphere{c, (float)radius}, p));
        });
    mfn(ctx, m, "sphereIntersects",
        [](JSContext* ctx, JSValue c0, double r0, JSValue c1, double r1) -> JSValue {
            REQ_V3(a, c0); REQ_V3(b, c1);
            return JS_NewBool(ctx, bromath::sintersects(
                bromath::Sphere{a, (float)r0}, bromath::Sphere{b, (float)r1}));
        });
    mfn(ctx, m, "aabbContains",
        [](JSContext* ctx, JSValue bmin, JSValue bmax, JSValue point) -> JSValue {
            REQ_V3(lo, bmin); REQ_V3(hi, bmax); REQ_V3(p, point);
            return JS_NewBool(ctx, bromath::acontains(bromath::AABB3{lo, hi}, p));
        });
    mfn(ctx, m, "aabbIntersects",
        [](JSContext* ctx, JSValue minA, JSValue maxA, JSValue minB,
           JSValue maxB) -> JSValue {
            REQ_V3(la, minA); REQ_V3(ha, maxA); REQ_V3(lb, minB); REQ_V3(hb, maxB);
            return JS_NewBool(ctx, bromath::aintersects(
                bromath::AABB3{la, ha}, bromath::AABB3{lb, hb}));
        });
    mfn(ctx, m, "aabbExpand",
        [](JSContext* ctx, JSValue bmin, JSValue bmax, JSValue point) -> JSValue {
            REQ_V3(lo, bmin); REQ_V3(hi, bmax); REQ_V3(p, point);
            bromath::AABB3 r = bromath::aexpand(bromath::AABB3{lo, hi}, p);
            JSValue o = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, o, "min", vec3ToJS(ctx, r.min));
            JS_SetPropertyStr(ctx, o, "max", vec3ToJS(ctx, r.max));
            return o;
        });
    mfn(ctx, m, "aabbMerge",
        [](JSContext* ctx, JSValue minA, JSValue maxA, JSValue minB,
           JSValue maxB) -> JSValue {
            REQ_V3(la, minA); REQ_V3(ha, maxA); REQ_V3(lb, minB); REQ_V3(hb, maxB);
            bromath::AABB3 r = bromath::amerge(
                bromath::AABB3{la, ha}, bromath::AABB3{lb, hb});
            JSValue o = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, o, "min", vec3ToJS(ctx, r.min));
            JS_SetPropertyStr(ctx, o, "max", vec3ToJS(ctx, r.max));
            return o;
        });

    // ── Frustum (planes returned/consumed as a flat 24-number array:
    //    6 planes × [nx, ny, nz, d], order left,right,bottom,top,near,far) ──
    mfn(ctx, m, "frustumFromViewProj",
        [](JSContext* ctx, JSValue vp) -> JSValue {
            if (!JS_IsArray(vp))
                return JS_ThrowTypeError(ctx, "expected a 16-element matrix array");
            bromath::Mat4 mat;
            for (uint32_t i = 0; i < 16; ++i) mat.data[i] = (float)idxNum(ctx, vp, i);
            bromath::Frustum f = bromath::ffromViewProj(mat);
            JSValue arr = JS_NewArray(ctx);
            for (int i = 0; i < 6; ++i) {
                const bromath::Plane& p = f.planes[i];
                JS_SetPropertyUint32(ctx, arr, i * 4 + 0, JS_NewFloat64(ctx, p.normal.x));
                JS_SetPropertyUint32(ctx, arr, i * 4 + 1, JS_NewFloat64(ctx, p.normal.y));
                JS_SetPropertyUint32(ctx, arr, i * 4 + 2, JS_NewFloat64(ctx, p.normal.z));
                JS_SetPropertyUint32(ctx, arr, i * 4 + 3, JS_NewFloat64(ctx, p.d));
            }
            return arr;
        });
    mfn(ctx, m, "frustumContainsPoint",
        [](JSContext* ctx, JSValue planes, JSValue point) -> JSValue {
            if (!JS_IsArray(planes))
                return JS_ThrowTypeError(ctx, "expected a 24-element planes array");
            REQ_V3(p, point);
            bromath::Frustum f;
            for (int i = 0; i < 6; ++i)
                f.planes[i] = {{(float)idxNum(ctx, planes, i * 4 + 0),
                                (float)idxNum(ctx, planes, i * 4 + 1),
                                (float)idxNum(ctx, planes, i * 4 + 2)},
                               (float)idxNum(ctx, planes, i * 4 + 3)};
            return JS_NewBool(ctx, bromath::fcontains(f, p));
        });
    mfn(ctx, m, "frustumIntersectsAABB",
        [](JSContext* ctx, JSValue planes, JSValue bmin, JSValue bmax) -> JSValue {
            if (!JS_IsArray(planes))
                return JS_ThrowTypeError(ctx, "expected a 24-element planes array");
            REQ_V3(lo, bmin); REQ_V3(hi, bmax);
            bromath::Frustum f;
            for (int i = 0; i < 6; ++i)
                f.planes[i] = {{(float)idxNum(ctx, planes, i * 4 + 0),
                                (float)idxNum(ctx, planes, i * 4 + 1),
                                (float)idxNum(ctx, planes, i * 4 + 2)},
                               (float)idxNum(ctx, planes, i * 4 + 3)};
            return JS_NewBool(ctx, bromath::fintersects(f, bromath::AABB3{lo, hi}));
        });
    mfn(ctx, m, "frustumIntersectsSphere",
        [](JSContext* ctx, JSValue planes, JSValue center, double radius) -> JSValue {
            if (!JS_IsArray(planes))
                return JS_ThrowTypeError(ctx, "expected a 24-element planes array");
            REQ_V3(c, center);
            bromath::Frustum f;
            for (int i = 0; i < 6; ++i)
                f.planes[i] = {{(float)idxNum(ctx, planes, i * 4 + 0),
                                (float)idxNum(ctx, planes, i * 4 + 1),
                                (float)idxNum(ctx, planes, i * 4 + 2)},
                               (float)idxNum(ctx, planes, i * 4 + 3)};
            return JS_NewBool(ctx,
                bromath::fintersects(f, bromath::Sphere{c, (float)radius}));
        });

    // ── Segment / capsule ───────────────────────────────────────────────────
    mfn(ctx, m, "segmentSegmentDistance",
        [](JSContext* ctx, JSValue p1, JSValue q1, JSValue p2, JSValue q2) -> JSValue {
            REQ_V3(a, p1); REQ_V3(b, q1); REQ_V3(c, p2); REQ_V3(d, q2);
            return JS_NewFloat64(ctx,
                (double)bromath::segmentSegmentDistance(a, b, c, d));
        });
    mfn(ctx, m, "capsulePenetration",
        [](JSContext* ctx, JSValue a0, JSValue a1, double ar, JSValue b0,
           JSValue b1, double br) -> JSValue {
            REQ_V3(p0, a0); REQ_V3(p1, a1); REQ_V3(q0, b0); REQ_V3(q1, b1);
            bromath::Capsule a{p0, p1, (float)ar};
            bromath::Capsule b{q0, q1, (float)br};
            return JS_NewFloat64(ctx, (double)bromath::capsulePenetration(a, b));
        });
    mfn(ctx, m, "capsulesIntersect",
        [](JSContext* ctx, JSValue a0, JSValue a1, double ar, JSValue b0,
           JSValue b1, double br) -> JSValue {
            REQ_V3(p0, a0); REQ_V3(p1, a1); REQ_V3(q0, b0); REQ_V3(q1, b1);
            bromath::Capsule a{p0, p1, (float)ar};
            bromath::Capsule b{q0, q1, (float)br};
            return JS_NewBool(ctx, bromath::capsulesIntersect(a, b));
        });

    // ── Grid (footprint = { origin:{x,y}|[x,y], cellSize, width, depth }) ────
    auto readGrid = [](JSContext* ctx, JSValueConst g) -> bromath::GridFootprint2D {
        bromath::GridFootprint2D f;
        JSValue origin = JS_GetPropertyStr(ctx, g, "origin");
        bromath::Vec2 o{0, 0};
        readVec2(ctx, origin, o);
        JS_FreeValue(ctx, origin);
        f.origin = o;
        f.cellSize = (float)propNum(ctx, g, "cellSize");
        f.width = (int)propNum(ctx, g, "width");
        f.depth = (int)propNum(ctx, g, "depth");
        return f;
    };
    // Note: readGrid is captured below by value into each lambda.
    mfn(ctx, m, "gridIndex2D",
        [readGrid](JSContext* ctx, JSValue grid, int col, int row) -> JSValue {
            return JS_NewInt32(ctx,
                bromath::gridIndex2D(readGrid(ctx, grid), col, row));
        });
    mfn(ctx, m, "gridInBounds",
        [readGrid](JSContext* ctx, JSValue grid, int col, int row) -> JSValue {
            return JS_NewBool(ctx,
                bromath::gridInBounds(readGrid(ctx, grid), col, row));
        });
    mfn(ctx, m, "gridCellOf",
        [readGrid](JSContext* ctx, JSValue grid, JSValue point) -> JSValue {
            bromath::Vec2 p{0, 0};
            if (!readVec2(ctx, point, p))
                return JS_ThrowTypeError(ctx, "expected a {x,y} or [x,y] point");
            int col = 0, row = 0;
            bromath::gridCellOf(readGrid(ctx, grid), p, col, row);
            JSValue o = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, o, "col", JS_NewInt32(ctx, col));
            JS_SetPropertyStr(ctx, o, "row", JS_NewInt32(ctx, row));
            return o;
        });
    mfn(ctx, m, "gridCellCenter",
        [readGrid](JSContext* ctx, JSValue grid, int col, int row) -> JSValue {
            return vec2ToJS(ctx,
                bromath::gridCellCenter(readGrid(ctx, grid), col, row));
        });

    // ── Hash (all return unsigned 32-bit values as plain numbers) ───────────
    mfn(ctx, m, "fnv1a32",
        [](JSContext* ctx, std::string data, JSValue seed) -> JSValue {
            uint32_t s = 2166136261u;
            if (!JS_IsUndefined(seed)) {
                int64_t sv = 0;
                JS_ToInt64(ctx, &sv, seed);
                s = (uint32_t)sv;
            }
            return JS_NewFloat64(ctx,
                (double)bromath::fnv1a32(data.data(), data.size(), s));
        });
    mfn(ctx, m, "hashU32",
        [](JSContext* ctx, double x) -> JSValue {
            return JS_NewFloat64(ctx, (double)bromath::hashU32((uint32_t)(int64_t)x));
        });
    mfn(ctx, m, "cellHash",
        [](JSContext* ctx, double x, double y, JSValue z) -> JSValue {
            if (JS_IsUndefined(z)) {
                return JS_NewFloat64(ctx,
                    (double)bromath::cellHash((int32_t)x, (int32_t)y));
            }
            double zv = 0;
            JS_ToFloat64(ctx, &zv, z);
            return JS_NewFloat64(ctx,
                (double)bromath::cellHash((int32_t)x, (int32_t)y, (int32_t)zv));
        });
    mfn(ctx, m, "positionToCell",
        [](JSContext* ctx, JSValue point, double cellSize, double bucketCount) -> JSValue {
            REQ_V3(p, point);
            return JS_NewFloat64(ctx, (double)bromath::positionToCell(
                p, (float)cellSize, (uint32_t)bucketCount));
        });
}

// ─────────────────────────────────────────────────────────────────────────────
// Assembly
// ─────────────────────────────────────────────────────────────────────────────

// Copy a globally-registered constructor onto bro.math under the same name.
static void aliasCtor(JSContext* ctx, JSValue global, JSValue mathObj,
                      const char* name) {
    JSValue ctor = JS_GetPropertyStr(ctx, global, name);
    if (!JS_IsUndefined(ctor) && !JS_IsException(ctor)) {
        JS_SetPropertyStr(ctx, mathObj, name, JS_DupValue(ctx, ctor));
    }
    JS_FreeValue(ctx, ctor);
}

void MathBindings::install(JSContext* ctx) {
    installSpatialHash(ctx);
    installRng(ctx);
    installSmoother(ctx);

    // Build namespace: bro.math.*
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue mathObj = JS_GetPropertyStr(ctx, broObj, "math");
    if (JS_IsUndefined(mathObj) || JS_IsException(mathObj)) {
        JS_FreeValue(ctx, mathObj);
        mathObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, broObj, "math", JS_DupValue(ctx, mathObj));
    }

    // qjsbind registers each class globally under its name; alias onto bro.math.
    aliasCtor(ctx, global, mathObj, "SpatialHash3D");
    aliasCtor(ctx, global, mathObj, "Rng");
    aliasCtor(ctx, global, mathObj, "Smoother");

    // Plain numeric/vector helpers live directly on bro.math.
    installFreeFunctions(ctx, mathObj);

    JS_FreeValue(ctx, mathObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void MathBindings::cleanup(JSContext*) {
    // qjsbind owns the class registration + finalizer; bro.math is reached
    // from globalThis and dropped by the engine-level globalThis sweep.
}

} // namespace bro::js
