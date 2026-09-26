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
            const Rooted cfg(res.value);
            readQueryFilter(cfg, filter, pw);
            Value lm = ev::getProperty(cfg, "layerMask");
            if (ev::isNumber(lm)) filter.layerMask = jsToUint32(ev::toDouble(lm));
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
            const Rooted cfg(res.value);
            readQueryFilter(cfg, filter, pw);
            Value lm = ev::getProperty(cfg, "layerMask");
            if (ev::isNumber(lm)) filter.layerMask = jsToUint32(ev::toDouble(lm));
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
    const Rooted cfg(res.value);

    physics::BodyOptions shape;
    std::string err;
    if (!readQueryShape(cfg, shape, err, pw)) {
        return natives::strResult("{\"error\":\"query shape must be convex (box|sphere|capsule|cylinder|convexHull)\"}");
    }

    physics::QueryFilter filter;
    readQueryFilter(cfg, filter, pw);

    JPH::Vec3 dir = readVec3(ev::getProperty(cfg, "direction"));
    if (dir == JPH::Vec3::sZero()) {
        return natives::strResult("{\"error\":\"castShape requires a non-zero direction\"}");
    }
    double maxDist = getPropNumber(cfg, "maxDistance", 1000.0);

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
    const Rooted cfg(res.value);

    physics::BodyOptions shape;
    std::string err;
    if (!readQueryShape(cfg, shape, err, pw)) {
        return natives::strResult("{\"error\":\"query shape must be convex (box|sphere|capsule|cylinder|convexHull)\"}");
    }

    physics::QueryFilter filter;
    readQueryFilter(cfg, filter, pw);

    JPH::Vec3 dir = readVec3(ev::getProperty(cfg, "direction"));
    if (dir == JPH::Vec3::sZero()) {
        return natives::strResult("{\"error\":\"castShape requires a non-zero direction\"}");
    }
    double maxDist = getPropNumber(cfg, "maxDistance", 1000.0);

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
            const Rooted cfg(res.value);
            physics::BodyOptions shape;
            std::string err;
            if (readQueryShape(cfg, shape, err, pw)) {
                physics::QueryFilter filter;
                readQueryFilter(cfg, filter, pw);
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
    const Rooted cfg(res.value);

    physics::BodyOptions shape;
    std::string err;
    if (!readQueryShape(cfg, shape, err, pw)) {
        return natives::strResult("{\"error\":\"query shape must be convex (box|sphere|capsule|cylinder|convexHull)\"}");
    }

    physics::QueryFilter filter;
    readQueryFilter(cfg, filter, pw);

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
            const Rooted cfg(res.value);
            readQueryFilter(cfg, filter, pw);
            Value lm = ev::getProperty(cfg, "layerMask");
            if (ev::isNumber(lm)) filter.layerMask = jsToUint32(ev::toDouble(lm));
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

static thread_local std::vector<double> tl_penetrationsBuf;

void bro_physics_penetrations(const char* config, bronze_native_buffer* out) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    tl_penetrationsBuf.clear();

    if (!world) {
        out->data = nullptr;
        out->length = 0;
        out->release = nullptr;
        return;
    }

    physics::PhysicsWorld::PenetrationOptions opts;

    if (config && *config) {
        auto res = ev::parseJson(config);
        if (!res.thrown && ev::isObject(res.value)) {
            const Rooted cfg(res.value);

            opts.minDepth = static_cast<float>(getPropNumber(cfg, "minDepth", -FLT_MAX));
            opts.maxSeparation = static_cast<float>(getPropNumber(cfg, "maxSeparation", 0.0));

            Value lm = ev::getProperty(cfg, "layerMask");
            if (ev::isNumber(lm)) {
                opts.layerMask = jsToUint32(ev::toDouble(lm));
            }

            Value bodiesVal = ev::getProperty(cfg, "bodies");
            if (ev::isObject(bodiesVal)) {
                const Rooted bv(bodiesVal);
                Value lenV = ev::getProperty(bv, "length");
                if (ev::isNumber(lenV)) {
                    uint32_t len = satCast<uint32_t>(ev::toDouble(lenV));
                    opts.bodies.reserve(len);
                    for (uint32_t i = 0; i < len; ++i) {
                        Value elem = ev::getElement(bv, i);
                        if (ev::isNumber(elem)) {
                            int32_t tag = static_cast<int32_t>(ev::toDouble(elem));
                            JPH::BodyID bid = pw->bodyIdForTag(tag);
                            if (!bid.IsInvalid()) opts.bodies.push_back(bid);
                        }
                    }
                }
            }

            Value ignoreVal = ev::getProperty(cfg, "ignorePairs");
            if (ev::isObject(ignoreVal)) {
                const Rooted iv(ignoreVal);
                Value lenV = ev::getProperty(iv, "length");
                if (ev::isNumber(lenV)) {
                    uint32_t len = satCast<uint32_t>(ev::toDouble(lenV));
                    for (uint32_t i = 0; i < len; ++i) {
                        Value pairElem = ev::getElement(iv, i);
                        if (ev::isObject(pairElem)) {
                            const Rooted pv(pairElem);
                            Value plenV = ev::getProperty(pv, "length");
                            uint32_t plen = ev::isNumber(plenV) ? satCast<uint32_t>(ev::toDouble(plenV)) : 0;
                            if (plen >= 2) {
                                Value a = ev::getElement(pv, 0);
                                Value b = ev::getElement(pv, 1);
                                if (ev::isNumber(a) && ev::isNumber(b)) {
                                    JPH::BodyID bidA = pw->bodyIdForTag(static_cast<int32_t>(ev::toDouble(a)));
                                    JPH::BodyID bidB = pw->bodyIdForTag(static_cast<int32_t>(ev::toDouble(b)));
                                    if (!bidA.IsInvalid() && !bidB.IsInvalid()) {
                                        opts.ignorePairs.push_back({ bidA, bidB });
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    auto hits = world->penetrations(opts);

    constexpr size_t stride = 8;
    tl_penetrationsBuf.reserve(hits.size() * stride);

    for (const auto& h : hits) {
        int32_t tagA = pw->tagForBodyId(h.body1);
        int32_t tagB = pw->tagForBodyId(h.body2);
        uint32_t subA = h.subShape1;
        uint32_t subB = h.subShape2;
        if (tagA > tagB) {
            std::swap(tagA, tagB);
            std::swap(subA, subB);
        }
        tl_penetrationsBuf.push_back(static_cast<double>(tagA));
        tl_penetrationsBuf.push_back(static_cast<double>(subA));
        tl_penetrationsBuf.push_back(static_cast<double>(tagB));
        tl_penetrationsBuf.push_back(static_cast<double>(subB));
        tl_penetrationsBuf.push_back(static_cast<double>(h.depth));
        tl_penetrationsBuf.push_back(static_cast<double>(h.position.GetX()));
        tl_penetrationsBuf.push_back(static_cast<double>(h.position.GetY()));
        tl_penetrationsBuf.push_back(static_cast<double>(h.position.GetZ()));
    }

    out->data = tl_penetrationsBuf.data();
    out->length = static_cast<uint32_t>(tl_penetrationsBuf.size());
    out->release = nullptr;
}

void bro_physics_setTransforms(const double* data, uint32_t data_len, int32_t stride) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world || !data || data_len == 0) return;
    if (stride != 8 && stride != 17) return;
    if (data_len % static_cast<uint32_t>(stride) != 0) return;

    if (stride == 8) {
        std::vector<physics::PhysicsWorld::BodyTransformUpdate> updates;
        updates.reserve(data_len / 8);
        for (uint32_t i = 0; i < data_len; i += 8) {
            int32_t tag = static_cast<int32_t>(data[i]);
            JPH::BodyID bid = pw->bodyIdForTag(tag);
            if (bid.IsInvalid()) continue;
            JPH::RVec3 pos(data[i+1], data[i+2], data[i+3]);
            JPH::Quat rot((float)data[i+4], (float)data[i+5], (float)data[i+6], (float)data[i+7]);
            updates.push_back({ bid, pos, rot });
        }
        world->setTransforms(updates);
    } else {
        std::vector<physics::PhysicsWorld::BodyTransformUpdate> updates;
        updates.reserve(data_len / 17);
        for (uint32_t i = 0; i < data_len; i += 17) {
            int32_t tag = static_cast<int32_t>(data[i]);
            JPH::BodyID bid = pw->bodyIdForTag(tag);
            if (bid.IsInvalid()) continue;
            const double* m = &data[i + 1];
            JPH::RVec3 pos(m[12], m[13], m[14]);
            JPH::Vec3 c0((float)m[0], (float)m[1], (float)m[2]);
            JPH::Vec3 c1((float)m[4], (float)m[5], (float)m[6]);
            JPH::Vec3 c2((float)m[8], (float)m[9], (float)m[10]);
            float l0 = c0.Length();
            float l1 = c1.Length();
            float l2 = c2.Length();
            if (l0 > 1e-6f) c0 /= l0;
            if (l1 > 1e-6f) c1 /= l1;
            if (l2 > 1e-6f) c2 /= l2;
            // A mirroring matrix keeps a proper rotation with the flip in scale.
            if (c0.Cross(c1).Dot(c2) < 0.0f) { c0 = -c0; l0 = -l0; }
            JPH::Mat44 mat(
                JPH::Vec4(c0, 0.0f),
                JPH::Vec4(c1, 0.0f),
                JPH::Vec4(c2, 0.0f),
                JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f)
            );
            JPH::Quat rot = mat.GetQuaternion().Normalized();
            updates.push_back({ bid, pos, rot, JPH::Vec3(l0, l1, l2) });
        }
        world->setTransforms(updates);
    }
}

void bro_physics_setTransform(int32_t tag, double px, double py, double pz, double qx, double qy, double qz, double qw) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world) return;
    JPH::BodyID bid = pw->bodyIdForTag(tag);
    if (bid.IsInvalid()) return;
    world->setTransform(bid, JPH::RVec3(px, py, pz), JPH::Quat((float)qx, (float)qy, (float)qz, (float)qw));
}

}  // extern "C"
