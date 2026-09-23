#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include <bromath/aabb.h>
#include <bromath/angle.h>
#include <bromath/color.h>
#include <bromath/curves.h>
#include <bromath/frustum.h>
#include <bromath/grid.h>
#include <bromath/hash.h>
#include <bromath/plane.h>
#include <bromath/ray.h>
#include <bromath/scalar.h>
#include <bromath/segment.h>
#include <bromath/sphere.h>
#include <bromath/vec.h>

#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

Value getSpatialHashConstructor();
Value getRngConstructor();
Value getSmootherConstructor();

namespace {

static double propNum(Value obj, const char* key) {
    Value v = ev::getProperty(obj, key);
    return ev::toDouble(v);
}

static double idxNum(Value arr, size_t i) {
    Value v = ev::getElement(arr, static_cast<uint32_t>(i));
    return ev::toDouble(v);
}

// The readers root their argument: each component read allocates.
static bool readVec3(Value vIn, bromath::Vec3& o) {
    if (!ev::isObject(vIn)) return false;
    const Rooted v(vIn);
    if (hostIsArray(v)) {
        o = { static_cast<float>(idxNum(v, 0)), static_cast<float>(idxNum(v, 1)), static_cast<float>(idxNum(v, 2)) };
        return true;
    }
    o = { static_cast<float>(propNum(v, "x")), static_cast<float>(propNum(v, "y")), static_cast<float>(propNum(v, "z")) };
    return true;
}

static bool readVec2(Value vIn, bromath::Vec2& o) {
    if (!ev::isObject(vIn)) return false;
    const Rooted v(vIn);
    if (hostIsArray(v)) {
        o = { static_cast<float>(idxNum(v, 0)), static_cast<float>(idxNum(v, 1)) };
        return true;
    }
    o = { static_cast<float>(propNum(v, "x")), static_cast<float>(propNum(v, "y")) };
    return true;
}

static Value vec3ToJS(bromath::Vec3 v) {
    ObjectBuilder o;
    o.set("x", ev::fromDouble(v.x));
    o.set("y", ev::fromDouble(v.y));
    o.set("z", ev::fromDouble(v.z));
    return o.get();
}

static Value vec2ToJS(bromath::Vec2 v) {
    ObjectBuilder o;
    o.set("x", ev::fromDouble(v.x));
    o.set("y", ev::fromDouble(v.y));
    return o.get();
}

static Value colorToJS(bromath::Color c) {
    ObjectBuilder o;
    o.set("r", ev::fromDouble(c.r));
    o.set("g", ev::fromDouble(c.g));
    o.set("b", ev::fromDouble(c.b));
    o.set("a", ev::fromDouble(c.a));
    return o.get();
}

static Value rayHitToJS(const bromath::RayHit& h) {
    if (!h.hit) return ev::null();
    ObjectBuilder o;
    o.set("t", ev::fromDouble(h.t));
    o.set("point", vec3ToJS(h.position));
    o.set("normal", vec3ToJS(h.normal));
    return o.get();
}

static bromath::GridFootprint2D readGrid(Value gIn) {
    const Rooted g(gIn);
    bromath::GridFootprint2D f;
    Value originVal = ev::getProperty(g, "origin");
    bromath::Vec2 o{0, 0};
    readVec2(originVal, o);
    f.origin = o;
    f.cellSize = static_cast<float>(propNum(g, "cellSize"));
    f.width = satCast<int>(propNum(g, "width"));
    f.depth = satCast<int>(propNum(g, "depth"));
    return f;
}

} // namespace

Value makeBroMathValue() {
    ObjectBuilder m;

    // Aliased constructors
    m.set("SpatialHash3D", getSpatialHashConstructor());
    m.set("Rng", getRngConstructor());
    m.set("Smoother", getSmootherConstructor());

    // Curves
    m.def("cubicEase", 5, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 5) return ev::fromDouble(0.0);
        bromath::CubicEase c{
            static_cast<float>(ev::toDouble(a[0])),
            static_cast<float>(ev::toDouble(a[1])),
            static_cast<float>(ev::toDouble(a[2])),
            static_cast<float>(ev::toDouble(a[3]))
        };
        return ev::fromDouble(bromath::ccubicEase(c, static_cast<float>(ev::toDouble(a[4]))));
    });

    m.def("bezier", 5, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 5) return ev::null();
        bromath::Vec3 p0{0,0,0}, p1{0,0,0}, p2{0,0,0}, p3{0,0,0};
        if (!readVec3(a[0], p0) || !readVec3(a[1], p1) || !readVec3(a[2], p2) || !readVec3(a[3], p3)) {
            return ev::throwTypeError("expected vector");
        }
        float t = static_cast<float>(ev::toDouble(a[4]));
        return vec3ToJS(bromath::cbezier(p0, p1, p2, p3, t));
    });

    m.def("bezierTangent", 5, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 5) return ev::null();
        bromath::Vec3 p0{0,0,0}, p1{0,0,0}, p2{0,0,0}, p3{0,0,0};
        if (!readVec3(a[0], p0) || !readVec3(a[1], p1) || !readVec3(a[2], p2) || !readVec3(a[3], p3)) {
            return ev::throwTypeError("expected vector");
        }
        float t = static_cast<float>(ev::toDouble(a[4]));
        return vec3ToJS(bromath::cbezierTangent(p0, p1, p2, p3, t));
    });

    m.def("catmullRom", 5, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 5) return ev::null();
        bromath::Vec3 p0{0,0,0}, p1{0,0,0}, p2{0,0,0}, p3{0,0,0};
        if (!readVec3(a[0], p0) || !readVec3(a[1], p1) || !readVec3(a[2], p2) || !readVec3(a[3], p3)) {
            return ev::throwTypeError("expected vector");
        }
        float t = static_cast<float>(ev::toDouble(a[4]));
        return vec3ToJS(bromath::ccatmullRom(p0, p1, p2, p3, t));
    });

    m.def("hermite", 5, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 5) return ev::null();
        bromath::Vec3 p0{0,0,0}, m0{0,0,0}, p1{0,0,0}, m1{0,0,0};
        if (!readVec3(a[0], p0) || !readVec3(a[1], m0) || !readVec3(a[2], p1) || !readVec3(a[3], m1)) {
            return ev::throwTypeError("expected vector");
        }
        float t = static_cast<float>(ev::toDouble(a[4]));
        return vec3ToJS(bromath::chermite(p0, m0, p1, m1, t));
    });

    // Color
    m.def("fromHex", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return colorToJS(bromath::Color{0, 0, 0, 1});
        std::string hex = ev::toUtf8(a[0]);
        return colorToJS(bromath::cfromHex(hex.c_str()));
    });

    m.def("fromHSV", 4, [](Value, std::span<const Value> a) -> Value {
        float h = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        float s = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
        float v = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        float alpha = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 1.0f;
        return colorToJS(bromath::cfromHSV(h, s, v, alpha));
    });

    m.def("fromColor8", 4, [](Value, std::span<const Value> a) -> Value {
        uint8_t r = a.size() > 0 ? satCast<uint8_t>(ev::toDouble(a[0])) : 0;
        uint8_t g = a.size() > 1 ? satCast<uint8_t>(ev::toDouble(a[1])) : 0;
        uint8_t b = a.size() > 2 ? satCast<uint8_t>(ev::toDouble(a[2])) : 0;
        uint8_t alpha = a.size() > 3 ? satCast<uint8_t>(ev::toDouble(a[3])) : 255;
        bromath::Color8 c8{r, g, b, alpha};
        return colorToJS(bromath::cfromColor8(c8));
    });

    m.def("toColor8", 4, [](Value, std::span<const Value> a) -> Value {
        float r = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        float g = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
        float b = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        float alpha = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 1.0f;
        bromath::Color8 c8 = bromath::ctoColor8(bromath::Color{r, g, b, alpha});
        ObjectBuilder o;
        o.set("r", ev::fromDouble(c8.r));
        o.set("g", ev::fromDouble(c8.g));
        o.set("b", ev::fromDouble(c8.b));
        o.set("a", ev::fromDouble(c8.a));
        return o.get();
    });

    m.def("linearToSrgb", 1, [](Value, std::span<const Value> a) -> Value {
        float c = !a.empty() ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        return ev::fromDouble(bromath::clinearToSrgb(c));
    });

    m.def("srgbToLinear", 1, [](Value, std::span<const Value> a) -> Value {
        float c = !a.empty() ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        return ev::fromDouble(bromath::csrgbToLinear(c));
    });

    // Scalar / angle
    m.def("lerp", 3, [](Value, std::span<const Value> a) -> Value {
        float p0 = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        float p1 = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
        float t  = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        return ev::fromDouble(bromath::lerp(p0, p1, t));
    });

    m.def("clamp", 3, [](Value, std::span<const Value> a) -> Value {
        float x  = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        float lo = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
        float hi = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 1.0f;
        return ev::fromDouble(bromath::clamp(x, lo, hi));
    });

    m.def("saturate", 1, [](Value, std::span<const Value> a) -> Value {
        float x = !a.empty() ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        return ev::fromDouble(bromath::saturate(x));
    });

    m.def("invLerp", 3, [](Value, std::span<const Value> a) -> Value {
        float p0 = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        float p1 = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
        float x  = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        return ev::fromDouble(bromath::invLerp(p0, p1, x));
    });

    m.def("remap", 5, [](Value, std::span<const Value> a) -> Value {
        float x      = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        float inMin  = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
        float inMax  = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 1.0f;
        float outMin = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
        float outMax = a.size() > 4 ? static_cast<float>(ev::toDouble(a[4])) : 1.0f;
        return ev::fromDouble(bromath::remap(x, inMin, inMax, outMin, outMax));
    });

    m.def("smoothstep", 3, [](Value, std::span<const Value> a) -> Value {
        float e0 = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        float e1 = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 1.0f;
        float x  = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        return ev::fromDouble(bromath::smoothstep(e0, e1, x));
    });

    m.def("smootherstep", 3, [](Value, std::span<const Value> a) -> Value {
        float e0 = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        float e1 = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 1.0f;
        float x  = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        return ev::fromDouble(bromath::smootherstep(e0, e1, x));
    });

    auto deg2radFn = [](Value, std::span<const Value> a) -> Value {
        float d = !a.empty() ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        return ev::fromDouble(bromath::deg2rad(d));
    };
    m.def("deg2rad", 1, deg2radFn);
    m.def("degToRad", 1, deg2radFn);

    auto rad2degFn = [](Value, std::span<const Value> a) -> Value {
        float r = !a.empty() ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        return ev::fromDouble(bromath::rad2deg(r));
    };
    m.def("rad2deg", 1, rad2degFn);
    m.def("radToDeg", 1, rad2degFn);

    m.def("wrapAngle", 1, [](Value, std::span<const Value> a) -> Value {
        float ang = !a.empty() ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        return ev::fromDouble(bromath::wrapAngle(ang));
    });

    m.def("wrapAngle2Pi", 1, [](Value, std::span<const Value> a) -> Value {
        float ang = !a.empty() ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        return ev::fromDouble(bromath::wrapAngle2Pi(ang));
    });

    auto angleDeltaFn = [](Value, std::span<const Value> a) -> Value {
        float from = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        float to   = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
        return ev::fromDouble(bromath::angleDelta(from, to));
    };
    m.def("angleDelta", 2, angleDeltaFn);
    m.def("angleDiff", 2, angleDeltaFn);

    m.def("angleLerp", 3, [](Value, std::span<const Value> a) -> Value {
        float from = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        float to   = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
        float t    = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        return ev::fromDouble(bromath::angleLerp(from, to, t));
    });

    // Ray intersection queries
    m.def("rayIntersectAABB", 4, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 4) return ev::null();
        bromath::Vec3 origin{0,0,0}, dir{0,0,0}, bmin{0,0,0}, bmax{0,0,0};
        if (!readVec3(a[0], origin) || !readVec3(a[1], dir) || !readVec3(a[2], bmin) || !readVec3(a[3], bmax)) {
            return ev::throwTypeError("expected vector");
        }
        bromath::Ray r{origin, dir};
        return rayHitToJS(bromath::rIntersectAABB(r, bromath::AABB3{bmin, bmax}));
    });

    m.def("rayIntersectSphere", 4, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 4) return ev::null();
        bromath::Vec3 origin{0,0,0}, dir{0,0,0}, center{0,0,0};
        if (!readVec3(a[0], origin) || !readVec3(a[1], dir) || !readVec3(a[2], center)) {
            return ev::throwTypeError("expected vector");
        }
        float radius = static_cast<float>(ev::toDouble(a[3]));
        bromath::Ray r{origin, dir};
        return rayHitToJS(bromath::rIntersectSphere(r, bromath::Sphere{center, radius}));
    });

    m.def("rayIntersectPlane", 4, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 4) return ev::null();
        bromath::Vec3 origin{0,0,0}, dir{0,0,0}, normal{0,0,0};
        if (!readVec3(a[0], origin) || !readVec3(a[1], dir) || !readVec3(a[2], normal)) {
            return ev::throwTypeError("expected vector");
        }
        float d = static_cast<float>(ev::toDouble(a[3]));
        bromath::Ray r{origin, dir};
        return rayHitToJS(bromath::rIntersectPlane(r, bromath::Plane{normal, d}));
    });

    m.def("rayIntersectTriangle", 7, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 6) return ev::null();
        bromath::Vec3 origin{0,0,0}, dir{0,0,0}, v0{0,0,0}, v1{0,0,0}, v2{0,0,0};
        if (!readVec3(a[0], origin) || !readVec3(a[1], dir) || !readVec3(a[2], v0) || !readVec3(a[3], v1) || !readVec3(a[4], v2)) {
            return ev::throwTypeError("expected vector");
        }
        bool cull = a.size() > 5 ? ev::toBool(a[5]) : false;
        bromath::Ray r{origin, dir};
        return rayHitToJS(bromath::rIntersectTriangle(r, v0, v1, v2, cull));
    });

    // Plane / sphere / AABB
    m.def("planeSignedDistance", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3) return ev::fromDouble(0.0);
        bromath::Vec3 normal{0,0,0}, pt{0,0,0};
        if (!readVec3(a[0], normal) || !readVec3(a[2], pt)) return ev::throwTypeError("expected vector");
        float d = static_cast<float>(ev::toDouble(a[1]));
        return ev::fromDouble(bromath::psignedDistance(bromath::Plane{normal, d}, pt));
    });

    m.def("planeProject", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3) return ev::null();
        bromath::Vec3 normal{0,0,0}, pt{0,0,0};
        if (!readVec3(a[0], normal) || !readVec3(a[2], pt)) return ev::throwTypeError("expected vector");
        float d = static_cast<float>(ev::toDouble(a[1]));
        return vec3ToJS(bromath::pproject(bromath::Plane{normal, d}, pt));
    });

    m.def("sphereContains", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3) return ev::fromBool(false);
        bromath::Vec3 center{0,0,0}, pt{0,0,0};
        if (!readVec3(a[0], center) || !readVec3(a[2], pt)) return ev::throwTypeError("expected vector");
        float r = static_cast<float>(ev::toDouble(a[1]));
        return ev::fromBool(bromath::scontains(bromath::Sphere{center, r}, pt));
    });

    m.def("sphereIntersects", 4, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 4) return ev::fromBool(false);
        bromath::Vec3 c0{0,0,0}, c1{0,0,0};
        if (!readVec3(a[0], c0) || !readVec3(a[2], c1)) return ev::throwTypeError("expected vector");
        float r0 = static_cast<float>(ev::toDouble(a[1]));
        float r1 = static_cast<float>(ev::toDouble(a[3]));
        return ev::fromBool(bromath::sintersects(bromath::Sphere{c0, r0}, bromath::Sphere{c1, r1}));
    });

    m.def("aabbContains", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3) return ev::fromBool(false);
        bromath::Vec3 bmin{0,0,0}, bmax{0,0,0}, pt{0,0,0};
        if (!readVec3(a[0], bmin) || !readVec3(a[1], bmax) || !readVec3(a[2], pt)) return ev::throwTypeError("expected vector");
        return ev::fromBool(bromath::acontains(bromath::AABB3{bmin, bmax}, pt));
    });

    m.def("aabbIntersects", 4, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 4) return ev::fromBool(false);
        bromath::Vec3 minA{0,0,0}, maxA{0,0,0}, minB{0,0,0}, maxB{0,0,0};
        if (!readVec3(a[0], minA) || !readVec3(a[1], maxA) || !readVec3(a[2], minB) || !readVec3(a[3], maxB)) {
            return ev::throwTypeError("expected vector");
        }
        return ev::fromBool(bromath::aintersects(bromath::AABB3{minA, maxA}, bromath::AABB3{minB, maxB}));
    });

    m.def("aabbExpand", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3) return ev::null();
        bromath::Vec3 bmin{0,0,0}, bmax{0,0,0}, pt{0,0,0};
        if (!readVec3(a[0], bmin) || !readVec3(a[1], bmax) || !readVec3(a[2], pt)) return ev::throwTypeError("expected vector");
        bromath::AABB3 r = bromath::aexpand(bromath::AABB3{bmin, bmax}, pt);
        ObjectBuilder o;
        o.set("min", vec3ToJS(r.min));
        o.set("max", vec3ToJS(r.max));
        return o.get();
    });

    m.def("aabbMerge", 4, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 4) return ev::null();
        bromath::Vec3 minA{0,0,0}, maxA{0,0,0}, minB{0,0,0}, maxB{0,0,0};
        if (!readVec3(a[0], minA) || !readVec3(a[1], maxA) || !readVec3(a[2], minB) || !readVec3(a[3], maxB)) {
            return ev::throwTypeError("expected vector");
        }
        bromath::AABB3 r = bromath::amerge(bromath::AABB3{minA, maxA}, bromath::AABB3{minB, maxB});
        ObjectBuilder o;
        o.set("min", vec3ToJS(r.min));
        o.set("max", vec3ToJS(r.max));
        return o.get();
    });

    // Frustum
    m.def("frustumFromViewProj", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !hostIsArray(a[0])) return ev::throwTypeError("expected 16-element matrix array");
        bromath::Mat4 mat;
        for (uint32_t i = 0; i < 16; ++i) mat.data[i] = static_cast<float>(idxNum(a[0], i));
        bromath::Frustum f = bromath::ffromViewProj(mat);
        return hostArrayOf(24, [&f](size_t idx) -> Value {
            size_t planeIdx = idx / 4;
            size_t comp = idx % 4;
            const auto& p = f.planes[planeIdx];
            if (comp == 0) return ev::fromDouble(p.normal.x);
            if (comp == 1) return ev::fromDouble(p.normal.y);
            if (comp == 2) return ev::fromDouble(p.normal.z);
            return ev::fromDouble(p.d);
        });
    });

    m.def("frustumContainsPoint", 2, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2 || !hostIsArray(a[0])) return ev::throwTypeError("expected planes array");
        bromath::Vec3 pt{0,0,0};
        if (!readVec3(a[1], pt)) return ev::throwTypeError("expected vector");
        bromath::Frustum f;
        for (int i = 0; i < 6; ++i) {
            f.planes[i] = {
                { static_cast<float>(idxNum(a[0], i * 4 + 0)),
                  static_cast<float>(idxNum(a[0], i * 4 + 1)),
                  static_cast<float>(idxNum(a[0], i * 4 + 2)) },
                static_cast<float>(idxNum(a[0], i * 4 + 3))
            };
        }
        return ev::fromBool(bromath::fcontains(f, pt));
    });

    m.def("frustumIntersectsAABB", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3 || !hostIsArray(a[0])) return ev::throwTypeError("expected planes array");
        bromath::Vec3 lo{0,0,0}, hi{0,0,0};
        if (!readVec3(a[1], lo) || !readVec3(a[2], hi)) return ev::throwTypeError("expected vector");
        bromath::Frustum f;
        for (int i = 0; i < 6; ++i) {
            f.planes[i] = {
                { static_cast<float>(idxNum(a[0], i * 4 + 0)),
                  static_cast<float>(idxNum(a[0], i * 4 + 1)),
                  static_cast<float>(idxNum(a[0], i * 4 + 2)) },
                static_cast<float>(idxNum(a[0], i * 4 + 3))
            };
        }
        return ev::fromBool(bromath::fintersects(f, bromath::AABB3{lo, hi}));
    });

    m.def("frustumIntersectsSphere", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3 || !hostIsArray(a[0])) return ev::throwTypeError("expected planes array");
        bromath::Vec3 c{0,0,0};
        if (!readVec3(a[1], c)) return ev::throwTypeError("expected vector");
        float r = static_cast<float>(ev::toDouble(a[2]));
        bromath::Frustum f;
        for (int i = 0; i < 6; ++i) {
            f.planes[i] = {
                { static_cast<float>(idxNum(a[0], i * 4 + 0)),
                  static_cast<float>(idxNum(a[0], i * 4 + 1)),
                  static_cast<float>(idxNum(a[0], i * 4 + 2)) },
                static_cast<float>(idxNum(a[0], i * 4 + 3))
            };
        }
        return ev::fromBool(bromath::fintersects(f, bromath::Sphere{c, r}));
    });

    // Segment / capsule
    m.def("segmentSegmentDistance", 4, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 4) return ev::fromDouble(0.0);
        bromath::Vec3 p1{0,0,0}, q1{0,0,0}, p2{0,0,0}, q2{0,0,0};
        if (!readVec3(a[0], p1) || !readVec3(a[1], q1) || !readVec3(a[2], p2) || !readVec3(a[3], q2)) {
            return ev::throwTypeError("expected vector");
        }
        return ev::fromDouble(bromath::segmentSegmentDistance(p1, q1, p2, q2));
    });

    m.def("capsulePenetration", 6, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 6) return ev::fromDouble(0.0);
        bromath::Vec3 p0{0,0,0}, p1{0,0,0}, q0{0,0,0}, q1{0,0,0};
        if (!readVec3(a[0], p0) || !readVec3(a[1], p1) || !readVec3(a[3], q0) || !readVec3(a[4], q1)) {
            return ev::throwTypeError("expected vector");
        }
        float ar = static_cast<float>(ev::toDouble(a[2]));
        float br = static_cast<float>(ev::toDouble(a[5]));
        bromath::Capsule capA{p0, p1, ar};
        bromath::Capsule capB{q0, q1, br};
        return ev::fromDouble(bromath::capsulePenetration(capA, capB));
    });

    m.def("capsulesIntersect", 6, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 6) return ev::fromBool(false);
        bromath::Vec3 p0{0,0,0}, p1{0,0,0}, q0{0,0,0}, q1{0,0,0};
        if (!readVec3(a[0], p0) || !readVec3(a[1], p1) || !readVec3(a[3], q0) || !readVec3(a[4], q1)) {
            return ev::throwTypeError("expected vector");
        }
        float ar = static_cast<float>(ev::toDouble(a[2]));
        float br = static_cast<float>(ev::toDouble(a[5]));
        bromath::Capsule capA{p0, p1, ar};
        bromath::Capsule capB{q0, q1, br};
        return ev::fromBool(bromath::capsulesIntersect(capA, capB));
    });

    // Grid
    m.def("gridIndex2D", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3 || !ev::isObject(a[0])) return ev::fromDouble(-1);
        int col = satCast<int>(ev::toDouble(a[1]));
        int row = satCast<int>(ev::toDouble(a[2]));
        // row * width + col in 64 bits: bromath's int arithmetic overflows
        // (undefined behaviour) for a far cell of a wide grid, and the index
        // is a JS number, which holds the exact 64-bit answer.
        const bromath::GridFootprint2D grid = readGrid(a[0]);
        return ev::fromDouble(static_cast<double>(
            static_cast<int64_t>(row) * grid.width + col));
    });

    m.def("gridInBounds", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3 || !ev::isObject(a[0])) return ev::fromBool(false);
        int col = satCast<int>(ev::toDouble(a[1]));
        int row = satCast<int>(ev::toDouble(a[2]));
        return ev::fromBool(bromath::gridInBounds(readGrid(a[0]), col, row));
    });

    m.def("gridCellOf", 2, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2 || !ev::isObject(a[0])) return ev::null();
        bromath::Vec2 pt{0, 0};
        if (!readVec2(a[1], pt)) return ev::throwTypeError("expected point");
        int col = 0, row = 0;
        bromath::gridCellOf(readGrid(a[0]), pt, col, row);
        ObjectBuilder o;
        o.set("col", ev::fromDouble(col));
        o.set("row", ev::fromDouble(row));
        return o.get();
    });

    m.def("gridCellCenter", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3 || !ev::isObject(a[0])) return ev::null();
        int col = satCast<int>(ev::toDouble(a[1]));
        int row = satCast<int>(ev::toDouble(a[2]));
        return vec2ToJS(bromath::gridCellCenter(readGrid(a[0]), col, row));
    });

    // Hash
    m.def("fnv1a32", 2, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::fromDouble(0.0);
        std::string str = ev::toUtf8(a[0]);
        uint32_t seed = a.size() > 1 ? satCast<uint32_t>(ev::toDouble(a[1])) : 2166136261u;
        return ev::fromDouble(static_cast<double>(bromath::fnv1a32(str.data(), str.size(), seed)));
    });

    m.def("hashU32", 1, [](Value, std::span<const Value> a) -> Value {
        uint32_t x = !a.empty() ? satCast<uint32_t>(ev::toDouble(a[0])) : 0;
        return ev::fromDouble(static_cast<double>(bromath::hashU32(x)));
    });

    m.def("cellHash", 3, [](Value, std::span<const Value> a) -> Value {
        int32_t x = a.size() > 0 ? satCast<int32_t>(ev::toDouble(a[0])) : 0;
        int32_t y = a.size() > 1 ? satCast<int32_t>(ev::toDouble(a[1])) : 0;
        if (a.size() > 2 && !ev::isUndefined(a[2])) {
            int32_t z = satCast<int32_t>(ev::toDouble(a[2]));
            return ev::fromDouble(static_cast<double>(bromath::cellHash(x, y, z)));
        }
        return ev::fromDouble(static_cast<double>(bromath::cellHash(x, y)));
    });

    m.def("positionToCell", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3) return ev::fromDouble(0.0);
        bromath::Vec3 pt{0,0,0};
        if (!readVec3(a[0], pt)) return ev::throwTypeError("expected vector");
        float cs = static_cast<float>(ev::toDouble(a[1]));
        uint32_t buckets = satCast<uint32_t>(ev::toDouble(a[2]));
        return ev::fromDouble(static_cast<double>(bromath::positionToCell(pt, cs, buckets)));
    });

    m.set("SpatialHash3D", getSpatialHashConstructor());
    m.set("Rng", getRngConstructor());
    m.set("Smoother", getSmootherConstructor());

    return m.get();
}

} // namespace bro::bronze_host
