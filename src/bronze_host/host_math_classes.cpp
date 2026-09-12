#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include <bromath/spatial_hash.h>
#include <bromath/rng.h>
#include <bromath/smoother.h>

#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace bro::bronze_host {

namespace {

// ============================================================================
// SpatialHash3D
// ============================================================================
struct HostSpatialHash3D {
    bromath::SpatialHash3D sh;
    explicit HostSpatialHash3D(float cs) : sh(cs) {}
};

static HostSpatialHash3D* getSpatialHash(Value v) {
    return static_cast<HostSpatialHash3D*>(ev::handleData(v));
}

static HostClass g_spatialHashClass;

static Value spatialHashCtor(Value, std::span<const Value> a) {
    float cs = 1.0f;
    if (!a.empty() && ev::isNumber(a[0])) {
        cs = static_cast<float>(ev::toDouble(a[0]));
    }
    return g_spatialHashClass.createInstance(std::make_unique<HostSpatialHash3D>(cs));
}

static void decorateSpatialHashProto(ObjectBuilder& proto) {
    proto.accessor("cellSize", [](Value thisVal, std::span<const Value>) -> Value {
        auto* h = getSpatialHash(thisVal);
        return h ? ev::fromDouble(h->sh.cellSize()) : ev::fromDouble(1.0);
    }, nullptr);

    proto.accessor("size", [](Value thisVal, std::span<const Value>) -> Value {
        auto* h = getSpatialHash(thisVal);
        return h ? ev::fromDouble(h->sh.size()) : ev::fromDouble(0.0);
    }, nullptr);

    proto.accessor("maxRadius", [](Value thisVal, std::span<const Value>) -> Value {
        auto* h = getSpatialHash(thisVal);
        return h ? ev::fromDouble(h->sh.maxRadius()) : ev::fromDouble(0.0);
    }, nullptr);

    proto.def("reset", 1, [](Value thisVal, std::span<const Value> a) -> Value {
        auto* h = getSpatialHash(thisVal);
        if (!h) return ev::throwTypeError("SpatialHash3D.reset: invalid this");
        float cs = (!a.empty() && ev::isNumber(a[0])) ? static_cast<float>(ev::toDouble(a[0])) : 1.0f;
        h->sh.reset(cs);
        return thisVal;
    });

    proto.def("clear", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto* h = getSpatialHash(thisVal);
        if (!h) return ev::throwTypeError("SpatialHash3D.clear: invalid this");
        h->sh.clear();
        return thisVal;
    });

    proto.def("insert", 4, [](Value thisVal, std::span<const Value> a) -> Value {
        auto* h = getSpatialHash(thisVal);
        if (!h) return ev::throwTypeError("SpatialHash3D.insert: invalid this");
        if (a.size() >= 4) {
            float x = static_cast<float>(ev::toDouble(a[0]));
            float y = static_cast<float>(ev::toDouble(a[1]));
            float z = static_cast<float>(ev::toDouble(a[2]));
            int32_t id = static_cast<int32_t>(ev::toDouble(a[3]));
            h->sh.insert({x, y, z}, id);
        }
        return thisVal;
    });

    proto.def("insertSphere", 5, [](Value thisVal, std::span<const Value> a) -> Value {
        auto* h = getSpatialHash(thisVal);
        if (!h) return ev::throwTypeError("SpatialHash3D.insertSphere: invalid this");
        if (a.size() >= 5) {
            float x = static_cast<float>(ev::toDouble(a[0]));
            float y = static_cast<float>(ev::toDouble(a[1]));
            float z = static_cast<float>(ev::toDouble(a[2]));
            float r = static_cast<float>(ev::toDouble(a[3]));
            int32_t id = static_cast<int32_t>(ev::toDouble(a[4]));
            h->sh.insert(bromath::Sphere{{x, y, z}, r}, id);
        }
        return thisVal;
    });

    proto.def("remove", 1, [](Value thisVal, std::span<const Value> a) -> Value {
        auto* h = getSpatialHash(thisVal);
        if (!h) return ev::throwTypeError("SpatialHash3D.remove: invalid this");
        if (!a.empty()) {
            int32_t id = static_cast<int32_t>(ev::toDouble(a[0]));
            h->sh.remove(id);
        }
        return thisVal;
    });

    auto radiusQueryFn = [](Value thisVal, std::span<const Value> a) -> Value {
        auto* h = getSpatialHash(thisVal);
        if (!h) return ev::throwTypeError("SpatialHash3D query: invalid this");
        float x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        float y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
        float z = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        float radius = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
        std::vector<int32_t> ids;
        h->sh.radiusQuery({x, y, z}, radius, ids);
        return hostArrayOf(ids.size(), [&ids](size_t i) -> Value {
            return ev::fromDouble(ids[i]);
        });
    };
    proto.def("radiusQuery", 4, radiusQueryFn);
    proto.def("queryRadius", 4, radiusQueryFn);

    proto.def("queryAABB", 6, [](Value thisVal, std::span<const Value> a) -> Value {
        auto* h = getSpatialHash(thisVal);
        if (!h) return ev::throwTypeError("SpatialHash3D.queryAABB: invalid this");
        float minX = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        float minY = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
        float minZ = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        float maxX = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
        float maxY = a.size() > 4 ? static_cast<float>(ev::toDouble(a[4])) : 0.0f;
        float maxZ = a.size() > 5 ? static_cast<float>(ev::toDouble(a[5])) : 0.0f;
        std::vector<int32_t> ids;
        bromath::AABB3 box{{minX, minY, minZ}, {maxX, maxY, maxZ}};
        h->sh.queryAABB(box, ids);
        return hostArrayOf(ids.size(), [&ids](size_t i) -> Value {
            return ev::fromDouble(ids[i]);
        });
    });

    proto.def("nearest", 4, [](Value thisVal, std::span<const Value> a) -> Value {
        auto* h = getSpatialHash(thisVal);
        if (!h) return ev::throwTypeError("SpatialHash3D.nearest: invalid this");
        float x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        float y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
        float z = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        float maxRadius = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : std::numeric_limits<float>::infinity();
        int32_t id = h->sh.nearest({x, y, z}, maxRadius);
        return ev::fromDouble(id);
    });
}

// ============================================================================
// Rng
// ============================================================================
struct HostRng {
    uint64_t state = 0;
    explicit HostRng(uint64_t s) : state(s) {}
};

static HostRng* getRng(Value v) {
    return static_cast<HostRng*>(ev::handleData(v));
}

static HostClass g_rngClass;

static Value rngCtor(Value, std::span<const Value> a) {
    uint64_t seed = 0;
    if (!a.empty() && ev::isNumber(a[0])) {
        seed = static_cast<uint64_t>(ev::toDouble(a[0]));
    }
    return g_rngClass.createInstance(std::make_unique<HostRng>(seed));
}

static void decorateRngProto(ObjectBuilder& proto) {
    proto.def("reseed", 1, [](Value thisVal, std::span<const Value> a) -> Value {
        auto* r = getRng(thisVal);
        if (!r) return ev::throwTypeError("Rng.reseed: invalid this");
        uint64_t seed = (!a.empty() && ev::isNumber(a[0])) ? static_cast<uint64_t>(ev::toDouble(a[0])) : 0;
        r->state = seed;
        return thisVal;
    });

    proto.def("float01", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto* r = getRng(thisVal);
        return r ? ev::fromDouble(bromath::randFloat01(r->state)) : ev::fromDouble(0.0);
    });

    proto.def("signed", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto* r = getRng(thisVal);
        return r ? ev::fromDouble(bromath::randSigned(r->state)) : ev::fromDouble(0.0);
    });

    proto.def("range", 2, [](Value thisVal, std::span<const Value> a) -> Value {
        auto* r = getRng(thisVal);
        if (!r) return ev::fromDouble(0.0);
        float lo = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        float hi = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 1.0f;
        return ev::fromDouble(bromath::randRange(r->state, lo, hi));
    });

    proto.def("int", 2, [](Value thisVal, std::span<const Value> a) -> Value {
        auto* r = getRng(thisVal);
        if (!r) return ev::fromDouble(0);
        int lo = a.size() > 0 ? static_cast<int>(ev::toDouble(a[0])) : 0;
        int hi = a.size() > 1 ? static_cast<int>(ev::toDouble(a[1])) : 0;
        return ev::fromDouble(bromath::randInt(r->state, lo, hi));
    });

    proto.def("uint32", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto* r = getRng(thisVal);
        if (!r) return ev::fromDouble(0.0);
        return ev::fromDouble(static_cast<double>(static_cast<uint32_t>(bromath::splitmix64(r->state) >> 32)));
    });

    proto.def("normal", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto* r = getRng(thisVal);
        return r ? ev::fromDouble(bromath::randNormal(r->state)) : ev::fromDouble(0.0);
    });

    proto.def("gaussian2D", 1, [](Value thisVal, std::span<const Value> a) -> Value {
        auto* r = getRng(thisVal);
        float sigma = !a.empty() ? static_cast<float>(ev::toDouble(a[0])) : 1.0f;
        bromath::Vec2 v = r ? bromath::randGaussian2D(r->state, sigma) : bromath::Vec2{0, 0};
        ObjectBuilder o;
        o.set("x", ev::fromDouble(v.x));
        o.set("y", ev::fromDouble(v.y));
        return o.get();
    });

    proto.def("inUnitDisc", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto* r = getRng(thisVal);
        bromath::Vec2 v = r ? bromath::randInUnitDisc(r->state) : bromath::Vec2{0, 0};
        ObjectBuilder o;
        o.set("x", ev::fromDouble(v.x));
        o.set("y", ev::fromDouble(v.y));
        return o.get();
    });

    proto.def("inUnitSphere", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto* r = getRng(thisVal);
        bromath::Vec3 v = r ? bromath::randInUnitSphere(r->state) : bromath::Vec3{0, 0, 0};
        ObjectBuilder o;
        o.set("x", ev::fromDouble(v.x));
        o.set("y", ev::fromDouble(v.y));
        o.set("z", ev::fromDouble(v.z));
        return o.get();
    });

    proto.def("onUnitSphere", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto* r = getRng(thisVal);
        bromath::Vec3 v = r ? bromath::randOnUnitSphere(r->state) : bromath::Vec3{0, 0, 0};
        ObjectBuilder o;
        o.set("x", ev::fromDouble(v.x));
        o.set("y", ev::fromDouble(v.y));
        o.set("z", ev::fromDouble(v.z));
        return o.get();
    });
}

// ============================================================================
// Smoother
// ============================================================================
struct HostSmoother {
    bromath::Smoother s;
};

static HostSmoother* getSmoother(Value v) {
    return static_cast<HostSmoother*>(ev::handleData(v));
}

static HostClass g_smootherClass;

static Value smootherCtor(Value, std::span<const Value> a) {
    auto hs = std::make_unique<HostSmoother>();
    if (a.size() >= 2 && ev::isNumber(a[0]) && ev::isNumber(a[1])) {
        float timeMs = static_cast<float>(ev::toDouble(a[0]));
        float sr = static_cast<float>(ev::toDouble(a[1]));
        bromath::smootherSetTime(hs->s, timeMs, sr);
    }
    return g_smootherClass.createInstance(std::move(hs));
}

static void decorateSmootherProto(ObjectBuilder& proto) {
    proto.accessor("current", [](Value thisVal, std::span<const Value>) -> Value {
        auto* sm = getSmoother(thisVal);
        return sm ? ev::fromDouble(sm->s.current) : ev::fromDouble(0.0);
    }, nullptr);

    proto.accessor("target", [](Value thisVal, std::span<const Value>) -> Value {
        auto* sm = getSmoother(thisVal);
        return sm ? ev::fromDouble(sm->s.target) : ev::fromDouble(0.0);
    }, nullptr);

    proto.accessor("coeff", [](Value thisVal, std::span<const Value>) -> Value {
        auto* sm = getSmoother(thisVal);
        return sm ? ev::fromDouble(sm->s.coeff) : ev::fromDouble(0.0);
    }, nullptr);

    proto.def("setTime", 2, [](Value thisVal, std::span<const Value> a) -> Value {
        auto* sm = getSmoother(thisVal);
        if (!sm) return ev::throwTypeError("Smoother.setTime: invalid this");
        if (a.size() >= 2) {
            float timeMs = static_cast<float>(ev::toDouble(a[0]));
            float sr = static_cast<float>(ev::toDouble(a[1]));
            bromath::smootherSetTime(sm->s, timeMs, sr);
        }
        return thisVal;
    });

    proto.def("reset", 1, [](Value thisVal, std::span<const Value> a) -> Value {
        auto* sm = getSmoother(thisVal);
        if (!sm) return ev::throwTypeError("Smoother.reset: invalid this");
        float val = !a.empty() ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        bromath::smootherReset(sm->s, val);
        return thisVal;
    });

    proto.def("setTarget", 1, [](Value thisVal, std::span<const Value> a) -> Value {
        auto* sm = getSmoother(thisVal);
        if (!sm) return ev::throwTypeError("Smoother.setTarget: invalid this");
        float t = !a.empty() ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        bromath::smootherTarget(sm->s, t);
        return thisVal;
    });

    proto.def("tick", 0, [](Value thisVal, std::span<const Value>) -> Value {
        auto* sm = getSmoother(thisVal);
        return sm ? ev::fromDouble(bromath::smootherTick(sm->s)) : ev::fromDouble(0.0);
    });

    proto.def("tickN", 1, [](Value thisVal, std::span<const Value> a) -> Value {
        auto* sm = getSmoother(thisVal);
        if (!sm) return ev::fromDouble(0.0);
        int n = !a.empty() ? static_cast<int>(ev::toDouble(a[0])) : 1;
        return ev::fromDouble(bromath::smootherTickN(sm->s, n));
    });
}

} // namespace

void installMathGlobals() {
    g_spatialHashClass.install("SpatialHash3D", 1, spatialHashCtor, decorateSpatialHashProto);
    g_rngClass.install("Rng", 1, rngCtor, decorateRngProto);
    g_smootherClass.install("Smoother", 2, smootherCtor, decorateSmootherProto);
}

Value getSpatialHashConstructor() {
    return g_spatialHashClass.constructor();
}

Value getRngConstructor() {
    return g_rngClass.constructor();
}

Value getSmootherConstructor() {
    return g_smootherClass.constructor();
}

} // namespace bro::bronze_host
