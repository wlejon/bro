// native_physics_queries.cpp — Physics query natives (raycasts, overlaps, shapecasts).

#include "bronze_host/native_physics_internal.h"

namespace bro::bronze_host {

namespace {

static thread_local std::vector<int32_t> tl_overlapScratch;

}  // namespace

}  // namespace bro::bronze_host

extern "C" {

using namespace bro;
using namespace bro::bronze_host;

const char* bro_physics_raycastClosestRaw(double ox, double oy, double oz,
                                          double dx, double dy, double dz,
                                          double maxDist, int32_t mask) {
    auto* pw = &g_defaultWorld;
    auto* world = pw->getWorld();
    if (!world) return natives::strResult("null");

    JPH::RVec3 origin(ox, oy, oz);
    JPH::Vec3 dir((float)dx, (float)dy, (float)dz);
    physics::QueryFilter filter;
    if (mask != 0) filter.layerMask = static_cast<uint32_t>(mask);

    physics::RayHit hit;
    bool ok = world->raycastClosest(origin, dir, hit, (float)maxDist, filter);
    if (!ok) return natives::strResult("null");

    int32_t tag = pw->tagForBodyId(hit.bodyID);
    std::string s = "{\"body\":" + std::to_string(tag) +
                    ",\"fraction\":" + std::to_string(hit.fraction) +
                    ",\"position\":{\"x\":" + std::to_string(hit.position.GetX()) +
                    ",\"y\":" + std::to_string(hit.position.GetY()) +
                    ",\"z\":" + std::to_string(hit.position.GetZ()) + "}" +
                    ",\"normal\":{\"x\":" + std::to_string(hit.normal.GetX()) +
                    ",\"y\":" + std::to_string(hit.normal.GetY()) +
                    ",\"z\":" + std::to_string(hit.normal.GetZ()) + "}}";
    return natives::strResult(s);
}

const char* bro_physics_raycastRaw(double ox, double oy, double oz,
                                   double dx, double dy, double dz,
                                   double maxDist, int32_t mask) {
    auto* pw = &g_defaultWorld;
    auto* world = pw->getWorld();
    if (!world) return natives::strResult("[]");

    JPH::RVec3 origin(ox, oy, oz);
    JPH::Vec3 dir((float)dx, (float)dy, (float)dz);
    physics::QueryFilter filter;
    if (mask != 0) filter.layerMask = static_cast<uint32_t>(mask);

    std::vector<physics::RayHit> hits = world->raycast(origin, dir, (float)maxDist, filter);

    std::string s = "[";
    for (size_t i = 0; i < hits.size(); ++i) {
        if (i > 0) s += ",";
        int32_t tag = pw->tagForBodyId(hits[i].bodyID);
        s += "{\"body\":" + std::to_string(tag) +
             ",\"fraction\":" + std::to_string(hits[i].fraction) +
             ",\"position\":{\"x\":" + std::to_string(hits[i].position.GetX()) +
             ",\"y\":" + std::to_string(hits[i].position.GetY()) +
             ",\"z\":" + std::to_string(hits[i].position.GetZ()) + "}" +
             ",\"normal\":{\"x\":" + std::to_string(hits[i].normal.GetX()) +
             ",\"y\":" + std::to_string(hits[i].normal.GetY()) +
             ",\"z\":" + std::to_string(hits[i].normal.GetZ()) + "}}";
    }
    s += "]";
    return natives::strResult(s);
}

const char* bro_physics_castShapeRaw(const char* config) {
    return natives::strResult("[]");
}

const char* bro_physics_castShapeClosestRaw(const char* config) {
    return natives::strResult("null");
}

void bro_physics_overlapShapeRaw(const char* config, bronze_native_buffer* out) {
    out->data = nullptr;
    out->length = 0;
    out->release = nullptr;
}

void bro_physics_overlapSphereRaw(double x, double y, double z, double radius, bronze_native_buffer* out) {
    auto* pw = &g_defaultWorld;
    auto* world = pw->getWorld();
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
    auto* pw = &g_defaultWorld;
    auto* world = pw->getWorld();
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
    auto* pw = &g_defaultWorld;
    auto* world = pw->getWorld();
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

const char* bro_physics_getContacts(void) {
    return natives::strResult("[]");
}

}  // extern "C"
