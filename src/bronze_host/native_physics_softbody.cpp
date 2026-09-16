// native_physics_softbody.cpp — Physics soft body and cloth natives.

#include "bronze_host/native_physics_internal.h"
#include <sstream>

namespace bro::bronze_host {

namespace {

static thread_local std::vector<float> tl_softVerts;

static bool readFloatArrayOrObject(Value v, std::vector<float>& out) {
    if (readFloatVector(v, out)) return true;
    if (!ev::isObject(v)) return false;
    out.clear();
    for (uint32_t i = 0; ; ++i) {
        std::string key = std::to_string(i);
        Value elem = ev::getProperty(v, key.c_str());
        if (ev::isUndefined(elem)) break;
        out.push_back(static_cast<float>(ev::toDouble(elem)));
    }
    return !out.empty();
}

static bool readU32ArrayOrObject(Value v, std::vector<uint32_t>& out) {
    if (readU32Vector(v, out)) return true;
    if (!ev::isObject(v)) return false;
    out.clear();
    for (uint32_t i = 0; ; ++i) {
        std::string key = std::to_string(i);
        Value elem = ev::getProperty(v, key.c_str());
        if (ev::isUndefined(elem)) break;
        out.push_back(static_cast<uint32_t>(ev::toDouble(elem)));
    }
    return !out.empty();
}

}  // namespace

}  // namespace bro::bronze_host

extern "C" {

using namespace bro;
using namespace bro::bronze_host;

void bro_physics_PhysicsSoftBody_dtor(void* self) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb) return;
    if (sb->handle) {
        HostPhysicsWorld* pw = sb->world ? sb->world : getActiveWorld();
        if (auto* w = pw ? pw->getWorld() : nullptr) {
            w->destroySoftBody(sb->handle);
            pw->unregisterBody(sb->bodyTag);
        }
    }
    if (sb->world) sb->world->liveSoftBodies.erase(sb);
    delete sb;
}

void* bro_physics_PhysicsSoftBody_ctor(void) {
    return nullptr;
}

void* bro_physics_createSoftBody(const char* config) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world || !config || !*config) return nullptr;
    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return nullptr;

    Value opts = res.value;
    physics::SoftBodyOptions sopts;

    Value clothV = ev::getProperty(opts, "cloth");
    Value meshV = ev::getProperty(opts, "mesh");
    const bool hasCloth = ev::isObject(clothV);
    const bool hasMesh = ev::isObject(meshV);

    bool pinCorners = false;
    auto readPinned = [&](Value holder) {
        Value pv = ev::getProperty(holder, "pinned");
        if (!ev::isUndefined(pv) && !ev::isNull(pv)) {
            if (!ev::isObject(pv)) {
                std::string str = ev::toUtf8(pv);
                if (str == "corners") pinCorners = true;
            } else {
                readU32ArrayOrObject(pv, sopts.pinned);
            }
        }
    };

    if (hasCloth) {
        sopts.kind = physics::SoftBodyOptions::Cloth;
        sopts.gridX = static_cast<int>(getPropNumber(clothV, "gridX", 10.0));
        sopts.gridZ = static_cast<int>(getPropNumber(clothV, "gridZ", 10.0));
        if (sopts.gridX < 2 || sopts.gridZ < 2) return nullptr;

        sopts.spacing = static_cast<float>(getPropNumber(clothV, "spacing", 0.2));
        sopts.mass = static_cast<float>(getPropNumber(clothV, "mass", 1.0));
        readPinned(clothV);
        if (pinCorners) {
            int gx = sopts.gridX, gz = sopts.gridZ;
            sopts.pinned = {0, static_cast<uint32_t>(gx - 1),
                            static_cast<uint32_t>(gx * (gz - 1)),
                            static_cast<uint32_t>(gx * gz - 1)};
        }
    } else if (hasMesh) {
        sopts.kind = physics::SoftBodyOptions::Mesh;
        Value vVal = ev::getProperty(meshV, "vertices");
        if (ev::isUndefined(vVal) || ev::isNull(vVal)) {
            vVal = ev::getProperty(meshV, "positions");
        }
        std::vector<float> flatVerts;
        readFloatArrayOrObject(vVal, flatVerts);
        if (flatVerts.empty() || (flatVerts.size() % 3 != 0)) return nullptr;

        sopts.vertices.reserve(flatVerts.size() / 3);
        for (size_t i = 0; i + 2 < flatVerts.size(); i += 3) {
            sopts.vertices.emplace_back(flatVerts[i], flatVerts[i + 1], flatVerts[i + 2]);
        }
        readU32ArrayOrObject(ev::getProperty(meshV, "indices"), sopts.indices);
        if (sopts.indices.empty() || (sopts.indices.size() % 3 != 0)) return nullptr;

        sopts.pressure = static_cast<float>(getPropNumber(meshV, "pressure", 0.0));
        sopts.mass = static_cast<float>(getPropNumber(meshV, "mass", 1.0));
        readPinned(meshV);
    } else {
        return nullptr;
    }

    sopts.compliance = static_cast<float>(getPropNumber(opts, "compliance", sopts.compliance));
    sopts.shearCompliance = static_cast<float>(getPropNumber(opts, "shearCompliance", sopts.shearCompliance));
    sopts.bendCompliance = static_cast<float>(getPropNumber(opts, "bendCompliance", sopts.bendCompliance));
    sopts.numIterations = static_cast<int>(getPropNumber(opts, "numIterations", sopts.numIterations));
    sopts.friction = static_cast<float>(getPropNumber(opts, "friction", sopts.friction));
    sopts.restitution = static_cast<float>(getPropNumber(opts, "restitution", sopts.restitution));
    sopts.linearDamping = static_cast<float>(getPropNumber(opts, "linearDamping", sopts.linearDamping));
    sopts.gravityFactor = static_cast<float>(getPropNumber(opts, "gravityFactor", sopts.gravityFactor));
    sopts.vertexRadius = static_cast<float>(getPropNumber(opts, "vertexRadius", sopts.vertexRadius));
    sopts.updatePosition = getPropBool(opts, "updatePosition", sopts.updatePosition);
    sopts.doubleSided = getPropBool(opts, "doubleSided", sopts.doubleSided);
    sopts.allowSleeping = getPropBool(opts, "allowSleeping", sopts.allowSleeping);
    sopts.position = readRVec3(ev::getProperty(opts, "position"));
    sopts.rotation = readQuat(ev::getProperty(opts, "rotation"));

    Value layerVal = ev::getProperty(opts, "layer");
    if (!ev::isUndefined(layerVal) && !ev::isNull(layerVal)) {
        if (!ev::isObject(layerVal)) {
            std::string s = ev::toUtf8(layerVal);
            int idx = 0;
            if (parseDecimalIndex(s, idx)) sopts.layer = idx;
            else sopts.layer = world->layerIndex(s);
        }
    }

    uint32_t handle = world->createSoftBody(sopts);
    if (!handle) return nullptr;

    auto* sb = new HostPhysicsSoftBody();
    sb->world = pw;
    sb->handle = handle;
    sb->bodyTag = pw->registerBody(world->softBodyBody(handle));
    if (sopts.kind == physics::SoftBodyOptions::Cloth) {
        sb->gridX = std::max(2, sopts.gridX);
        sb->gridZ = std::max(2, sopts.gridZ);
    }
    pw->liveSoftBodies.insert(sb);
    return sb;
}

int32_t bro_physics_PhysicsSoftBody_vertexCount_get(void* self) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb || !sb->handle) return 0;
    HostPhysicsWorld* pw = sb->world ? sb->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    int count = w ? w->softBodyVertexCount(sb->handle) : 0;
    return count < 0 ? 0 : count;
}

int32_t bro_physics_PhysicsSoftBody_body_get(void* self) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    return sb ? sb->bodyTag : -1;
}

const char* bro_physics_PhysicsSoftBody_topology(void* self) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb || !sb->handle) return natives::strResult("null");
    HostPhysicsWorld* pw = sb->world ? sb->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return natives::strResult("null");

    std::vector<float> pos;
    std::vector<uint32_t> idx;
    if (!w->softBodyTopology(sb->handle, pos, idx)) return natives::strResult("null");

    std::ostringstream ss;
    ss << "{\"positions\":[";
    for (size_t i = 0; i < pos.size(); ++i) {
        if (i > 0) ss << ",";
        ss << pos[i];
    }
    ss << "],\"indices\":[";
    for (size_t i = 0; i < idx.size(); ++i) {
        if (i > 0) ss << ",";
        ss << idx[i];
    }
    ss << "],\"gridX\":" << sb->gridX << ",\"gridZ\":" << sb->gridZ << "}";
    return natives::strResult(ss.str());
}

void bro_physics_PhysicsSoftBody_vertices(void* self, bronze_native_buffer* out) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb || !sb->handle) {
        out->data = nullptr;
        out->length = 0;
        out->release = nullptr;
        return;
    }
    HostPhysicsWorld* pw = sb->world ? sb->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w || !w->getSoftBodyVertices(sb->handle, tl_softVerts)) {
        out->data = nullptr;
        out->length = 0;
        out->release = nullptr;
        return;
    }
    out->data = tl_softVerts.data();
    out->length = static_cast<uint32_t>(tl_softVerts.size());
    out->release = nullptr;
}

bool bro_physics_PhysicsSoftBody_pin(void* self, int32_t index, bool pinned) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb || !sb->handle) return false;
    HostPhysicsWorld* pw = sb->world ? sb->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    return w ? w->pinSoftBodyVertex(sb->handle, index, pinned) : false;
}

bool bro_physics_PhysicsSoftBody_setVertex(void* self, int32_t index, double x, double y, double z) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb || !sb->handle) return false;
    HostPhysicsWorld* pw = sb->world ? sb->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    return w ? w->setSoftBodyVertexPosition(sb->handle, index, JPH::RVec3(x, y, z)) : false;
}

bool bro_physics_PhysicsSoftBody_setVertexVelocity(void* self, int32_t index, double x, double y, double z) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb || !sb->handle) return false;
    HostPhysicsWorld* pw = sb->world ? sb->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    return w ? w->setSoftBodyVertexVelocity(sb->handle, index, JPH::Vec3((float)x, (float)y, (float)z)) : false;
}

const char* bro_physics_PhysicsSoftBody_getBounds(void* self) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb || !sb->handle) return natives::strResult("null");
    HostPhysicsWorld* pw = sb->world ? sb->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return natives::strResult("null");

    std::vector<float> verts;
    if (!w->getSoftBodyVertices(sb->handle, verts) || verts.empty()) {
        return natives::strResult("{\"min\":{\"x\":0,\"y\":0,\"z\":0},\"max\":{\"x\":0,\"y\":0,\"z\":0}}");
    }

    float minX = verts[0], maxX = verts[0];
    float minY = verts[1], maxY = verts[1];
    float minZ = verts[2], maxZ = verts[2];
    for (size_t i = 3; i + 2 < verts.size(); i += 3) {
        minX = std::min(minX, verts[i]);
        maxX = std::max(maxX, verts[i]);
        minY = std::min(minY, verts[i + 1]);
        maxY = std::max(maxY, verts[i + 1]);
        minZ = std::min(minZ, verts[i + 2]);
        maxZ = std::max(maxZ, verts[i + 2]);
    }

    std::ostringstream ss;
    ss << "{\"min\":{\"x\":" << minX << ",\"y\":" << minY << ",\"z\":" << minZ << "},"
       << "\"max\":{\"x\":" << maxX << ",\"y\":" << maxY << ",\"z\":" << maxZ << "}}";
    return natives::strResult(ss.str());
}

void bro_physics_PhysicsSoftBody_destroy(void* self) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb || !sb->handle) return;
    HostPhysicsWorld* pw = sb->world ? sb->world : getActiveWorld();
    if (auto* w = pw ? pw->getWorld() : nullptr) {
        w->destroySoftBody(sb->handle);
        pw->unregisterBody(sb->bodyTag);
    }
    // Off the world's live set, so the world's destructor no longer clears
    // this pointer for us: drop it here, or the handle's own destructor at
    // teardown erases from a world that may already be gone.
    if (sb->world) sb->world->liveSoftBodies.erase(sb);
    sb->world = nullptr;
    sb->handle = 0;
    sb->bodyTag = -1;
}

}  // extern "C"
