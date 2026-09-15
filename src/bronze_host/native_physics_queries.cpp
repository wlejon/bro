// native_physics_queries.cpp — Physics query natives (raycasts, overlaps, shapecasts).

#include "bronze_host/native_physics_internal.h"
#include <sstream>

namespace bro::bronze_host {

namespace {

static thread_local std::vector<int32_t> tl_overlapScratch;

void formatRayHit(std::ostringstream& ss, int32_t tag, uint64_t udata, const physics::RayHit& hit) {
    ss << "{\"body\":" << tag
       << ",\"bodyId\":" << tag
       << ",\"fraction\":" << hit.fraction
       << ",\"userData\":" << udata
       << ",\"position\":{\"x\":" << hit.position.GetX() << ",\"y\":" << hit.position.GetY() << ",\"z\":" << hit.position.GetZ() << "}"
       << ",\"normal\":{\"x\":" << hit.normal.GetX() << ",\"y\":" << hit.normal.GetY() << ",\"z\":" << hit.normal.GetZ() << "}}";
}

void formatShapeCastHit(std::ostringstream& ss, int32_t tag, uint64_t udata, const physics::ShapeCastHit& hit) {
    ss << "{\"body\":" << tag
       << ",\"bodyId\":" << tag
       << ",\"fraction\":" << hit.fraction
       << ",\"userData\":" << udata
       << ",\"position\":{\"x\":" << hit.position.GetX() << ",\"y\":" << hit.position.GetY() << ",\"z\":" << hit.position.GetZ() << "}"
       << ",\"normal\":{\"x\":" << hit.normal.GetX() << ",\"y\":" << hit.normal.GetY() << ",\"z\":" << hit.normal.GetZ() << "}}";
}

void formatShapeOverlapHit(std::ostringstream& ss, int32_t tag, uint64_t udata, const physics::OverlapHit& hit) {
    ss << "{\"body\":" << tag
       << ",\"bodyId\":" << tag
       << ",\"depth\":" << hit.depth
       << ",\"userData\":" << udata
       << ",\"position\":{\"x\":" << hit.position.GetX() << ",\"y\":" << hit.position.GetY() << ",\"z\":" << hit.position.GetZ() << "}"
       << ",\"normal\":{\"x\":" << hit.normal.GetX() << ",\"y\":" << hit.normal.GetY() << ",\"z\":" << hit.normal.GetZ() << "}}";
}

}  // namespace

}  // namespace bro::bronze_host

extern "C" {

using namespace bro;
using namespace bro::bronze_host;

const char* bro_physics_raycastClosestRaw(double ox, double oy, double oz,
                                          double dx, double dy, double dz,
                                          double maxDist, int32_t mask) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world) return natives::strResult("null");

    JPH::RVec3 origin(ox, oy, oz);
    JPH::Vec3 dir((float)dx, (float)dy, (float)dz);
    physics::QueryFilter filter;
    if (mask != 0) filter.layerMask = static_cast<uint32_t>(mask);

    physics::RayHit hit;
    bool ok = world->raycastClosest(origin, dir, hit, (float)maxDist, filter);
    if (!ok) return natives::strResult("null");

    int32_t tag = pw->tagForBodyId(hit.bodyID);
    uint64_t udata = hit.bodyID.IsInvalid() ? 0 : world->getUserData(hit.bodyID);

    std::ostringstream ss;
    formatRayHit(ss, tag, udata, hit);
    return natives::strResult(ss.str());
}

const char* bro_physics_raycastRaw(double ox, double oy, double oz,
                                   double dx, double dy, double dz,
                                   double maxDist, int32_t mask) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world) return natives::strResult("[]");

    JPH::RVec3 origin(ox, oy, oz);
    JPH::Vec3 dir((float)dx, (float)dy, (float)dz);
    physics::QueryFilter filter;
    if (mask != 0) filter.layerMask = static_cast<uint32_t>(mask);

    std::vector<physics::RayHit> hits = world->raycast(origin, dir, (float)maxDist, filter);

    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < hits.size(); ++i) {
        if (i > 0) ss << ",";
        int32_t tag = pw->tagForBodyId(hits[i].bodyID);
        uint64_t udata = hits[i].bodyID.IsInvalid() ? 0 : world->getUserData(hits[i].bodyID);
        formatRayHit(ss, tag, udata, hits[i]);
    }
    ss << "]";
    return natives::strResult(ss.str());
}

const char* bro_physics_raycastClosestJsonRaw(double ox, double oy, double oz,
                                              double dx, double dy, double dz,
                                              double maxDist, const char* filterConfig) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world) return natives::strResult("null");

    JPH::RVec3 origin(ox, oy, oz);
    JPH::Vec3 dir((float)dx, (float)dy, (float)dz);
    physics::QueryFilter filter;
    if (filterConfig && *filterConfig) {
        auto res = ev::parseJson(filterConfig);
        if (!res.thrown && ev::isObject(res.value)) {
            readQueryFilter(res.value, filter, pw);
            Value lm = ev::getProperty(res.value, "layerMask");
            if (ev::isNumber(lm)) filter.layerMask = static_cast<uint32_t>(ev::toDouble(lm));
        }
    }

    physics::RayHit hit;
    bool ok = world->raycastClosest(origin, dir, hit, (float)maxDist, filter);
    if (!ok) return natives::strResult("null");

    int32_t tag = pw->tagForBodyId(hit.bodyID);
    uint64_t udata = hit.bodyID.IsInvalid() ? 0 : world->getUserData(hit.bodyID);

    std::ostringstream ss;
    formatRayHit(ss, tag, udata, hit);
    return natives::strResult(ss.str());
}

const char* bro_physics_raycastJsonRaw(double ox, double oy, double oz,
                                       double dx, double dy, double dz,
                                       double maxDist, const char* filterConfig) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world) return natives::strResult("[]");

    JPH::RVec3 origin(ox, oy, oz);
    JPH::Vec3 dir((float)dx, (float)dy, (float)dz);
    physics::QueryFilter filter;
    if (filterConfig && *filterConfig) {
        auto res = ev::parseJson(filterConfig);
        if (!res.thrown && ev::isObject(res.value)) {
            readQueryFilter(res.value, filter, pw);
            Value lm = ev::getProperty(res.value, "layerMask");
            if (ev::isNumber(lm)) filter.layerMask = static_cast<uint32_t>(ev::toDouble(lm));
        }
    }

    std::vector<physics::RayHit> hits = world->raycast(origin, dir, (float)maxDist, filter);

    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < hits.size(); ++i) {
        if (i > 0) ss << ",";
        int32_t tag = pw->tagForBodyId(hits[i].bodyID);
        uint64_t udata = hits[i].bodyID.IsInvalid() ? 0 : world->getUserData(hits[i].bodyID);
        formatRayHit(ss, tag, udata, hits[i]);
    }
    ss << "]";
    return natives::strResult(ss.str());
}

const char* bro_physics_castShapeRaw(const char* config) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world || !config || !*config) return natives::strResult("[]");

    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return natives::strResult("[]");

    physics::BodyOptions shape;
    std::string err;
    if (!readQueryShape(res.value, shape, err, pw)) {
        return natives::strResult("{\"error\":\"query shape must be convex (box|sphere|capsule|cylinder|convexHull)\"}");
    }

    physics::QueryFilter filter;
    readQueryFilter(res.value, filter, pw);

    Value dv = ev::getProperty(res.value, "direction");
    JPH::Vec3 dir = readVec3(dv);
    if (dir == JPH::Vec3::sZero()) {
        return natives::strResult("{\"error\":\"castShape requires a non-zero direction\"}");
    }
    double maxDist = getPropNumber(res.value, "maxDistance", 1000.0);

    auto hits = world->castShape(shape, dir, static_cast<float>(maxDist), filter);
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < hits.size(); ++i) {
        if (i > 0) ss << ",";
        int32_t tag = pw->tagForBodyId(hits[i].bodyID);
        uint64_t udata = hits[i].bodyID.IsInvalid() ? 0 : world->getUserData(hits[i].bodyID);
        formatShapeCastHit(ss, tag, udata, hits[i]);
    }
    ss << "]";
    return natives::strResult(ss.str());
}

const char* bro_physics_castShapeClosestRaw(const char* config) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world || !config || !*config) return natives::strResult("null");

    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return natives::strResult("null");

    physics::BodyOptions shape;
    std::string err;
    if (!readQueryShape(res.value, shape, err, pw)) {
        return natives::strResult("{\"error\":\"query shape must be convex (box|sphere|capsule|cylinder|convexHull)\"}");
    }

    physics::QueryFilter filter;
    readQueryFilter(res.value, filter, pw);

    Value dv = ev::getProperty(res.value, "direction");
    JPH::Vec3 dir = readVec3(dv);
    if (dir == JPH::Vec3::sZero()) {
        return natives::strResult("{\"error\":\"castShape requires a non-zero direction\"}");
    }
    double maxDist = getPropNumber(res.value, "maxDistance", 1000.0);

    physics::ShapeCastHit hit;
    if (!world->castShapeClosest(shape, dir, static_cast<float>(maxDist), hit, filter)) {
        return natives::strResult("null");
    }

    int32_t tag = pw->tagForBodyId(hit.bodyID);
    uint64_t udata = hit.bodyID.IsInvalid() ? 0 : world->getUserData(hit.bodyID);

    std::ostringstream ss;
    formatShapeCastHit(ss, tag, udata, hit);
    return natives::strResult(ss.str());
}

void bro_physics_overlapShapeRaw(const char* config, bronze_native_buffer* out) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    tl_overlapScratch.clear();

    if (world && config && *config) {
        auto res = ev::parseJson(config);
        if (!res.thrown && ev::isObject(res.value)) {
            physics::BodyOptions shape;
            std::string err;
            if (readQueryShape(res.value, shape, err, pw)) {
                physics::QueryFilter filter;
                readQueryFilter(res.value, filter, pw);
                auto hits = world->overlapShape(shape, filter);
                for (const auto& h : hits) {
                    int32_t tag = pw->tagForBodyId(h.bodyID);
                    if (tag > 0) tl_overlapScratch.push_back(tag);
                }
            }
        }
    }

    out->data = tl_overlapScratch.data();
    out->length = static_cast<uint32_t>(tl_overlapScratch.size());
    out->release = nullptr;
}

const char* bro_physics_overlapShapeJsonRaw(const char* config) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world || !config || !*config) return natives::strResult("[]");

    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return natives::strResult("[]");

    physics::BodyOptions shape;
    std::string err;
    if (!readQueryShape(res.value, shape, err, pw)) {
        return natives::strResult("{\"error\":\"query shape must be convex (box|sphere|capsule|cylinder|convexHull)\"}");
    }

    physics::QueryFilter filter;
    readQueryFilter(res.value, filter, pw);

    auto hits = world->overlapShape(shape, filter);
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < hits.size(); ++i) {
        if (i > 0) ss << ",";
        int32_t tag = pw->tagForBodyId(hits[i].bodyID);
        uint64_t udata = hits[i].bodyID.IsInvalid() ? 0 : world->getUserData(hits[i].bodyID);
        formatShapeOverlapHit(ss, tag, udata, hits[i]);
    }
    ss << "]";
    return natives::strResult(ss.str());
}

void bro_physics_overlapSphereRaw(double x, double y, double z, double radius, bronze_native_buffer* out) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    tl_overlapScratch.clear();
    if (world) {
        physics::BodyOptions shape;
        shape.shape = physics::BodyOptions::ShapeSphere;
        shape.position = JPH::RVec3(x, y, z);
        shape.radius = (float)radius;
        physics::QueryFilter filter;
        auto hits = world->overlapShape(shape, filter);
        for (const auto& h : hits) {
            int32_t tag = pw->tagForBodyId(h.bodyID);
            if (tag > 0) tl_overlapScratch.push_back(tag);
        }
    }
    out->data = tl_overlapScratch.data();
    out->length = static_cast<uint32_t>(tl_overlapScratch.size());
    out->release = nullptr;
}

void bro_physics_overlapBoxRaw(double cx, double cy, double cz, double hx, double hy, double hz, bronze_native_buffer* out) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    tl_overlapScratch.clear();
    if (world) {
        physics::BodyOptions shape;
        shape.shape = physics::BodyOptions::ShapeBox;
        shape.position = JPH::RVec3(cx, cy, cz);
        shape.halfExtents = JPH::Vec3((float)hx, (float)hy, (float)hz);
        physics::QueryFilter filter;
        auto hits = world->overlapShape(shape, filter);
        for (const auto& h : hits) {
            int32_t tag = pw->tagForBodyId(h.bodyID);
            if (tag > 0) tl_overlapScratch.push_back(tag);
        }
    }
    out->data = tl_overlapScratch.data();
    out->length = static_cast<uint32_t>(tl_overlapScratch.size());
    out->release = nullptr;
}

void bro_physics_overlapPointRaw(double x, double y, double z, int32_t mask, bronze_native_buffer* out) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    tl_overlapScratch.clear();
    if (world) {
        JPH::RVec3 pt(x, y, z);
        physics::QueryFilter filter;
        if (mask != 0) filter.layerMask = static_cast<uint32_t>(mask);
        auto bodies = world->overlapPoint(pt, filter);
        for (auto bid : bodies) {
            int32_t tag = pw->tagForBodyId(bid);
            if (tag > 0) tl_overlapScratch.push_back(tag);
        }
    }
    out->data = tl_overlapScratch.data();
    out->length = static_cast<uint32_t>(tl_overlapScratch.size());
    out->release = nullptr;
}

const char* bro_physics_overlapPointJsonRaw(double x, double y, double z, const char* filterConfig) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world) return natives::strResult("[]");

    JPH::RVec3 pt(x, y, z);
    physics::QueryFilter filter;
    if (filterConfig && *filterConfig) {
        auto res = ev::parseJson(filterConfig);
        if (!res.thrown && ev::isObject(res.value)) {
            readQueryFilter(res.value, filter, pw);
            Value lm = ev::getProperty(res.value, "layerMask");
            if (ev::isNumber(lm)) filter.layerMask = static_cast<uint32_t>(ev::toDouble(lm));
        }
    }

    auto bodies = world->overlapPoint(pt, filter);
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < bodies.size(); ++i) {
        if (i > 0) ss << ",";
        int32_t tag = pw->tagForBodyId(bodies[i]);
        uint64_t udata = bodies[i].IsInvalid() ? 0 : world->getUserData(bodies[i]);
        ss << "{\"bodyId\":" << tag << ",\"userData\":" << udata << "}";
    }
    ss << "]";
    return natives::strResult(ss.str());
}

const char* bro_physics_getContacts(void) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world) return natives::strResult("{\"overflow\":false,\"events\":[]}");

    bool overflowed = false;
    auto events = world->drainContactEvents(&overflowed);
    pw->lastContactEvents = events;
    pw->lastContactOverflow = overflowed;

    std::ostringstream ss;
    ss << "{\"overflow\":" << (overflowed ? "true" : "false") << ",\"events\":[";
    for (size_t i = 0; i < events.size(); ++i) {
        if (i > 0) ss << ",";
        const auto& e = events[i];
        int32_t t1 = pw->tagForBodyId(e.body1);
        int32_t t2 = pw->tagForBodyId(e.body2);
        const char* typeStr = (e.type == physics::ContactEvent::Added) ? "added" : "removed";
        ss << "{\"type\":\"" << typeStr << "\""
           << ",\"body1\":" << t1
           << ",\"body2\":" << t2
           << ",\"sensor\":" << (e.isSensor ? "true" : "false");
        if (e.type == physics::ContactEvent::Added) {
            ss << ",\"normal\":{\"x\":" << e.normal.x << ",\"y\":" << e.normal.y << ",\"z\":" << e.normal.z << "}"
               << ",\"penetration\":" << e.penetration
               << ",\"impulse\":" << e.impulse
               << ",\"points\":[";
            for (int k = 0; k < e.numPoints; ++k) {
                if (k > 0) ss << ",";
                ss << "{\"x\":" << e.points[k].x << ",\"y\":" << e.points[k].y << ",\"z\":" << e.points[k].z << "}";
            }
            ss << "]";
        }
        ss << "}";
    }
    ss << "]}";
    return natives::strResult(ss.str());
}

}  // extern "C"
