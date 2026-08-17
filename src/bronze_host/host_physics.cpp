// Jolt rigid-body physics bindings for the bronze host layer.
//
// Exposes bro's multi-threaded Jolt physics engine (`PhysicsWorld*` from
// `hostEngine()->physicsWorld()`) through the global `Physics` namespace,
// `PhysicsCharacter`, and `PhysicsSoftBody` handles.
//
// Follows bronze GC rules strictly:
// - Payload structs are plain host memory, freed by handle finalizers.
// - Finalizers never touch the embed API / never own Persistents.
// - Persistents and child objects live on JS properties.
// - Heap pointers from typedArrayInfo/arrayBufferInfo are consumed before any allocation.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "engine/engine.h"
#include "physics/physics_world.h"
#include "util/log.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Body/BodyID.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// Tag state & PhysicsWorld lookup
// ---------------------------------------------------------------------------

struct PhysicsState {
    std::unordered_map<uint32_t, int32_t> bodyTags;   // BodyID idx+seq -> tag
    std::unordered_map<int32_t, uint32_t> tagToBody;  // tag -> BodyID idx+seq
    int32_t nextTag = 1;

    ev::Persistent physicsObj;
    std::vector<physics::ContactEvent> lastContactEvents;
    bool lastContactOverflow = false;

    int32_t registerBody(JPH::BodyID id) {
        if (id.IsInvalid()) return -1;
        int32_t tag = nextTag++;
        bodyTags[id.GetIndexAndSequenceNumber()] = tag;
        tagToBody[tag] = id.GetIndexAndSequenceNumber();
        return tag;
    }

    void unregisterBody(int32_t tag) {
        auto it = tagToBody.find(tag);
        if (it == tagToBody.end()) return;
        bodyTags.erase(it->second);
        tagToBody.erase(it);
    }

    void unregisterBodyId(JPH::BodyID id) {
        auto it = bodyTags.find(id.GetIndexAndSequenceNumber());
        if (it == bodyTags.end()) return;
        tagToBody.erase(it->second);
        bodyTags.erase(it);
    }

    JPH::BodyID bodyIdForTag(int32_t tag) const {
        auto it = tagToBody.find(tag);
        if (it == tagToBody.end()) return JPH::BodyID();
        return JPH::BodyID(it->second);
    }

    int32_t tagForBodyId(JPH::BodyID id) const {
        auto it = bodyTags.find(id.GetIndexAndSequenceNumber());
        return it != bodyTags.end() ? it->second : -1;
    }

    void clear() {
        bodyTags.clear();
        tagToBody.clear();
        lastContactEvents.clear();
    }
};

static PhysicsState g_phys;

physics::PhysicsWorld* getPhysicsWorld() {
    auto* e = hostEngine();
    return e ? e->physicsWorld() : nullptr;
}

// ---------------------------------------------------------------------------
// Helpers: Read JS inputs (Vec3, Quat, Arrays, Properties)
// ---------------------------------------------------------------------------

JPH::Vec3 readVec3(Value v, JPH::Vec3 def = JPH::Vec3::sZero()) {
    if (!ev::isObject(v)) return def;
    ev::Persistent root(v);
    Value xV = ev::getProperty(root.get(), "x");
    Value yV = ev::getProperty(root.get(), "y");
    Value zV = ev::getProperty(root.get(), "z");
    if (!ev::isUndefined(xV) || !ev::isUndefined(yV) || !ev::isUndefined(zV)) {
        double x = (!ev::isUndefined(xV) && !ev::isObject(xV)) ? ev::toDouble(xV) : def.GetX();
        double y = (!ev::isUndefined(yV) && !ev::isObject(yV)) ? ev::toDouble(yV) : def.GetY();
        double z = (!ev::isUndefined(zV) && !ev::isObject(zV)) ? ev::toDouble(zV) : def.GetZ();
        return JPH::Vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    }
    Value e0 = ev::getElement(root.get(), 0);
    Value e1 = ev::getElement(root.get(), 1);
    Value e2 = ev::getElement(root.get(), 2);
    if (!ev::isUndefined(e0) && !ev::isUndefined(e1) && !ev::isUndefined(e2)) {
        double x = !ev::isObject(e0) ? ev::toDouble(e0) : def.GetX();
        double y = !ev::isObject(e1) ? ev::toDouble(e1) : def.GetY();
        double z = !ev::isObject(e2) ? ev::toDouble(e2) : def.GetZ();
        return JPH::Vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    }
    return def;
}

JPH::RVec3 readRVec3(Value v, JPH::RVec3 def = JPH::RVec3::sZero()) {
    JPH::Vec3 v3 = readVec3(v, JPH::Vec3(static_cast<float>(def.GetX()),
                                         static_cast<float>(def.GetY()),
                                         static_cast<float>(def.GetZ())));
    return JPH::RVec3(v3.GetX(), v3.GetY(), v3.GetZ());
}

JPH::Quat readQuat(Value v, JPH::Quat def = JPH::Quat::sIdentity()) {
    if (!ev::isObject(v)) return def;
    ev::Persistent root(v);
    Value xV = ev::getProperty(root.get(), "x");
    Value yV = ev::getProperty(root.get(), "y");
    Value zV = ev::getProperty(root.get(), "z");
    Value wV = ev::getProperty(root.get(), "w");
    if (!ev::isUndefined(xV) || !ev::isUndefined(yV) || !ev::isUndefined(zV) || !ev::isUndefined(wV)) {
        double x = (!ev::isUndefined(xV) && !ev::isObject(xV)) ? ev::toDouble(xV) : 0.0;
        double y = (!ev::isUndefined(yV) && !ev::isObject(yV)) ? ev::toDouble(yV) : 0.0;
        double z = (!ev::isUndefined(zV) && !ev::isObject(zV)) ? ev::toDouble(zV) : 0.0;
        double w = (!ev::isUndefined(wV) && !ev::isObject(wV)) ? ev::toDouble(wV) : 1.0;
        return JPH::Quat(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w)).Normalized();
    }
    Value e0 = ev::getElement(root.get(), 0);
    Value e1 = ev::getElement(root.get(), 1);
    Value e2 = ev::getElement(root.get(), 2);
    Value e3 = ev::getElement(root.get(), 3);
    if (!ev::isUndefined(e0) && !ev::isUndefined(e1) && !ev::isUndefined(e2) && !ev::isUndefined(e3)) {
        double x = !ev::isObject(e0) ? ev::toDouble(e0) : 0.0;
        double y = !ev::isObject(e1) ? ev::toDouble(e1) : 0.0;
        double z = !ev::isObject(e2) ? ev::toDouble(e2) : 0.0;
        double w = !ev::isObject(e3) ? ev::toDouble(e3) : 1.0;
        return JPH::Quat(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w)).Normalized();
    }
    return def;
}

bool readFloatVector(Value v, std::vector<float>& out) {
    if (ev::isUndefined(v) || ev::isNull(v)) return false;
    if (auto info = ev::typedArrayInfo(v)) {
        if (info.data && info.bytesPerElement == sizeof(float)) {
            const float* fp = reinterpret_cast<const float*>(info.data);
            out.assign(fp, fp + info.elementCount);
            return true;
        }
    }
    if (!ev::isObject(v)) return false;
    ev::Persistent root(v);
    Value lenV = ev::getProperty(root.get(), "length");
    if (ev::isUndefined(lenV) || ev::isObject(lenV)) return false;
    uint32_t len = static_cast<uint32_t>(ev::toDouble(lenV));
    out.clear();
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        Value e = ev::getElement(root.get(), i);
        double d = (!ev::isUndefined(e) && !ev::isObject(e)) ? ev::toDouble(e) : 0.0;
        out.push_back(static_cast<float>(d));
    }
    return true;
}

bool readU32Vector(Value v, std::vector<uint32_t>& out) {
    if (ev::isUndefined(v) || ev::isNull(v)) return false;
    if (auto info = ev::typedArrayInfo(v)) {
        if (info.data && info.bytesPerElement == sizeof(uint32_t)) {
            const uint32_t* up = reinterpret_cast<const uint32_t*>(info.data);
            out.assign(up, up + info.elementCount);
            return true;
        }
        if (info.data && info.bytesPerElement == sizeof(uint16_t)) {
            const uint16_t* up = reinterpret_cast<const uint16_t*>(info.data);
            out.clear();
            out.reserve(info.elementCount);
            for (uint32_t i = 0; i < info.elementCount; ++i) out.push_back(up[i]);
            return true;
        }
    }
    if (!ev::isObject(v)) return false;
    ev::Persistent root(v);
    Value lenV = ev::getProperty(root.get(), "length");
    if (ev::isUndefined(lenV) || ev::isObject(lenV)) return false;
    uint32_t len = static_cast<uint32_t>(ev::toDouble(lenV));
    out.clear();
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        Value e = ev::getElement(root.get(), i);
        double d = (!ev::isUndefined(e) && !ev::isObject(e)) ? ev::toDouble(e) : 0.0;
        out.push_back(static_cast<uint32_t>(d));
    }
    return true;
}

std::string getPropString(const ev::Persistent& root, const char* name, const std::string& def = "") {
    Value v = ev::getProperty(root.get(), name);
    if (ev::isUndefined(v) || ev::isNull(v) || ev::isObject(v)) return def;
    return ev::toUtf8(v);
}

double getPropNumber(const ev::Persistent& root, const char* name, double def = 0.0) {
    Value v = ev::getProperty(root.get(), name);
    if (ev::isUndefined(v) || ev::isNull(v) || ev::isObject(v)) return def;
    double d = ev::toDouble(v);
    return std::isnan(d) ? def : d;
}

bool getPropBool(const ev::Persistent& root, const char* name, bool def = false) {
    Value v = ev::getProperty(root.get(), name);
    if (ev::isUndefined(v) || ev::isNull(v)) return def;
    return ev::toBool(v);
}

uint64_t getPropU64(const ev::Persistent& root, const char* name, uint64_t def = 0) {
    Value v = ev::getProperty(root.get(), name);
    if (ev::isUndefined(v) || ev::isNull(v) || ev::isObject(v)) return def;
    double d = ev::toDouble(v);
    return std::isnan(d) ? def : static_cast<uint64_t>(d);
}

// ---------------------------------------------------------------------------
// Helpers: Make JS Objects (Vec3, Quat, TypedArray, State)
// ---------------------------------------------------------------------------

Value makeFloat32Array(const float* data, size_t count) {
    Value arr = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(count));
    if (data && count > 0) {
        std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data), count * sizeof(float));
        ev::fillTypedArray(arr, bytes);
    }
    return arr;
}

Value makeUint32Array(const uint32_t* data, size_t count) {
    Value arr = ev::createTypedArray(ev::elements::Uint32, static_cast<uint32_t>(count));
    if (data && count > 0) {
        std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data), count * sizeof(uint32_t));
        ev::fillTypedArray(arr, bytes);
    }
    return arr;
}

Value makeVec3Value(float x, float y, float z) {
    ObjectBuilder b;
    b.set("x", ev::fromDouble(x));
    b.set("y", ev::fromDouble(y));
    b.set("z", ev::fromDouble(z));
    return b.get();
}

Value makeQuatValue(float x, float y, float z, float w) {
    ObjectBuilder b;
    b.set("x", ev::fromDouble(x));
    b.set("y", ev::fromDouble(y));
    b.set("z", ev::fromDouble(z));
    b.set("w", ev::fromDouble(w));
    return b.get();
}

Value makeTransformValue(physics::PhysicsWorld* world, JPH::BodyID id, bool interpolated) {
    JPH::RVec3 pos;
    JPH::Quat rot;
    if (interpolated) {
        world->getRenderTransform(id, pos, rot);
    } else {
        pos = world->getPosition(id);
        rot = world->getRotation(id);
    }
    uint64_t udata = world->getUserData(id);

    ObjectBuilder b;
    b.set("position", makeVec3Value(static_cast<float>(pos.GetX()),
                                   static_cast<float>(pos.GetY()),
                                   static_cast<float>(pos.GetZ())));
    b.set("rotation", makeQuatValue(rot.GetX(), rot.GetY(), rot.GetZ(), rot.GetW()));
    b.set("userData", ev::fromDouble(static_cast<double>(udata)));
    return b.get();
}

Value makeVelocityValue(const JPH::Vec3& linear, const JPH::Vec3& angular) {
    ObjectBuilder b;
    b.set("linear", makeVec3Value(linear.GetX(), linear.GetY(), linear.GetZ()));
    b.set("angular", makeVec3Value(angular.GetX(), angular.GetY(), angular.GetZ()));
    return b.get();
}

Value makeRayHitValue(int32_t tag, const physics::RayHit& hit, uint64_t udata) {
    ObjectBuilder b;
    b.set("body", ev::fromDouble(tag));
    b.set("bodyId", ev::fromDouble(tag));
    b.set("fraction", ev::fromDouble(hit.fraction));
    b.set("position", makeVec3Value(static_cast<float>(hit.position.GetX()),
                                   static_cast<float>(hit.position.GetY()),
                                   static_cast<float>(hit.position.GetZ())));
    b.set("normal", makeVec3Value(hit.normal.GetX(), hit.normal.GetY(), hit.normal.GetZ()));
    b.set("userData", ev::fromDouble(static_cast<double>(udata)));
    return b.get();
}

Value makeCharacterStateValue(const physics::CharacterState& st, int32_t groundTag) {
    ObjectBuilder b;
    b.set("position", makeVec3Value(static_cast<float>(st.position.GetX()),
                                   static_cast<float>(st.position.GetY()),
                                   static_cast<float>(st.position.GetZ())));
    b.set("velocity", makeVec3Value(st.velocity.GetX(), st.velocity.GetY(), st.velocity.GetZ()));
    const char* groundStr = "inAir";
    switch (st.ground) {
        case physics::CharacterGround::OnGround: groundStr = "onGround"; break;
        case physics::CharacterGround::OnSteepGround: groundStr = "onSteepGround"; break;
        case physics::CharacterGround::NotSupported: groundStr = "notSupported"; break;
        case physics::CharacterGround::InAir: groundStr = "inAir"; break;
    }
    b.set("groundState", ev::fromUtf8(groundStr));
    b.set("isGrounded", ev::fromBool(st.ground == physics::CharacterGround::OnGround));
    b.set("groundNormal", makeVec3Value(st.groundNormal.GetX(), st.groundNormal.GetY(), st.groundNormal.GetZ()));
    b.set("groundVelocity", makeVec3Value(st.groundVelocity.GetX(), st.groundVelocity.GetY(), st.groundVelocity.GetZ()));
    b.set("groundBodyId", ev::fromDouble(groundTag));
    return b.get();
}

// ---------------------------------------------------------------------------
// Options Parsing
// ---------------------------------------------------------------------------

bool parseCombineMode(const std::string& s, physics::CombineMode& out) {
    if (s == "average") out = physics::CombineMode::Average;
    else if (s == "min" || s == "minimum") out = physics::CombineMode::Min;
    else if (s == "max" || s == "maximum") out = physics::CombineMode::Max;
    else if (s == "multiply") out = physics::CombineMode::Multiply;
    else return false;
    return true;
}

bool readAreaOverride(const ev::Persistent& o, physics::AreaOverride& a, std::string& err) {
    std::string mode = getPropString(o, "gravityMode");
    Value gv = ev::getProperty(o.get(), "gravity");
    const bool hasVec = ev::isObject(gv);
    if (hasVec) a.gravity = readVec3(gv);

    a.pointGravity = getPropBool(o, "gravityPoint", a.pointGravity);
    a.strength = static_cast<float>(getPropNumber(o, "gravityStrength", a.strength));
    a.falloffDistance = static_cast<float>(getPropNumber(o, "falloffDistance", a.falloffDistance));

    Value sv = ev::getProperty(o.get(), "gravityScale");
    const bool hasScale = !ev::isUndefined(sv) && !ev::isObject(sv);
    if (hasScale) a.gravityScale = static_cast<float>(ev::toDouble(sv));

    a.linearDamping = static_cast<float>(getPropNumber(o, "linearDamping", a.linearDamping));
    a.angularDamping = static_cast<float>(getPropNumber(o, "angularDamping", a.angularDamping));
    a.priority = static_cast<int>(getPropNumber(o, "priority", a.priority));

    if (mode == "scale" || (mode.empty() && hasScale && !hasVec && !a.pointGravity)) {
        if (!hasScale) { err = "area gravityMode 'scale' requires gravityScale"; return false; }
        a.gravityMode = physics::AreaOverride::GravityScale;
    } else if (mode == "combine") {
        a.gravityMode = physics::AreaOverride::GravityCombine;
    } else if (mode == "replace" || (mode.empty() && (hasVec || a.pointGravity))) {
        a.gravityMode = physics::AreaOverride::GravityReplace;
    } else if (mode.empty()) {
        a.gravityMode = physics::AreaOverride::GravityNone;
    } else {
        err = "area gravityMode must be 'replace' | 'combine' | 'scale'";
        return false;
    }
    return true;
}

bool readBodyOptions(Value optsVal, physics::BodyOptions& out, std::string& err) {
    if (!ev::isObject(optsVal)) { err = "opts must be an object"; return false; }
    ev::Persistent opts(optsVal);

    std::string shape = getPropString(opts, "shape");
    if (shape.empty()) { err = "shape is required"; return false; }

    if      (shape == "box")         out.shape = physics::BodyOptions::ShapeBox;
    else if (shape == "sphere")      out.shape = physics::BodyOptions::ShapeSphere;
    else if (shape == "capsule")     out.shape = physics::BodyOptions::ShapeCapsule;
    else if (shape == "cylinder")    out.shape = physics::BodyOptions::ShapeCylinder;
    else if (shape == "convexHull")  out.shape = physics::BodyOptions::ShapeConvexHull;
    else if (shape == "mesh")        out.shape = physics::BodyOptions::ShapeMesh;
    else if (shape == "compound")    out.shape = physics::BodyOptions::ShapeCompound;
    else if (shape == "chain")       out.shape = physics::BodyOptions::ShapeChain;
    else if (shape == "heightfield") out.shape = physics::BodyOptions::ShapeHeightField;
    else { err = "unknown shape: " + shape; return false; }

    out.position = readRVec3(ev::getProperty(opts.get(), "position"));
    out.rotation = readQuat(ev::getProperty(opts.get(), "rotation"));
    out.localPosition = readVec3(ev::getProperty(opts.get(), "localPosition"));
    out.localRotation = readQuat(ev::getProperty(opts.get(), "localRotation"));

    out.halfExtents = readVec3(ev::getProperty(opts.get(), "halfExtents"), JPH::Vec3(0.5f, 0.5f, 0.5f));
    out.radius = static_cast<float>(getPropNumber(opts, "radius", out.radius));
    out.halfHeight = static_cast<float>(getPropNumber(opts, "halfHeight", out.halfHeight));

    out.isStatic = getPropBool(opts, "static", out.isStatic) || getPropBool(opts, "isStatic", false);
    out.isSensor = getPropBool(opts, "sensor", out.isSensor) || getPropBool(opts, "isSensor", false);
    out.ccd = getPropBool(opts, "ccd", out.ccd);

    Value areaVal = ev::getProperty(opts.get(), "area");
    if (ev::isObject(areaVal)) {
        if (!out.isSensor) { err = "area field overrides require sensor: true"; return false; }
        ev::Persistent areaPersist(areaVal);
        if (!readAreaOverride(areaPersist, out.area, err)) return false;
        out.hasArea = true;
    }

    out.friction = static_cast<float>(getPropNumber(opts, "friction", out.friction));
    out.restitution = static_cast<float>(getPropNumber(opts, "restitution", out.restitution));

    std::string fc = getPropString(opts, "frictionCombine");
    if (!fc.empty() && !parseCombineMode(fc, out.frictionCombine)) {
        err = "frictionCombine must be 'average' | 'min' | 'max' | 'multiply'";
        return false;
    }
    std::string rc = getPropString(opts, "restitutionCombine");
    if (!rc.empty() && !parseCombineMode(rc, out.restitutionCombine)) {
        err = "restitutionCombine must be 'average' | 'min' | 'max' | 'multiply'";
        return false;
    }

    out.density = static_cast<float>(getPropNumber(opts, "density", out.density));
    out.mass = static_cast<float>(getPropNumber(opts, "mass", out.mass));
    out.gravityFactor = static_cast<float>(getPropNumber(opts, "gravityFactor", out.gravityFactor));
    out.linearDamping = static_cast<float>(getPropNumber(opts, "linearDamping", out.linearDamping));
    out.angularDamping = static_cast<float>(getPropNumber(opts, "angularDamping", out.angularDamping));
    out.maxLinearVelocity = static_cast<float>(getPropNumber(opts, "maxLinearVelocity", out.maxLinearVelocity));
    out.maxAngularVelocity = static_cast<float>(getPropNumber(opts, "maxAngularVelocity", out.maxAngularVelocity));
    out.userData = getPropU64(opts, "userData", 0);

    std::string dofs = getPropString(opts, "dofs");
    if (dofs == "2d" || dofs == "plane2d" || dofs == "Plane2D") {
        out.dofs = JPH::EAllowedDOFs::Plane2D;
    } else if (dofs == "all" || dofs.empty()) {
        out.dofs = JPH::EAllowedDOFs::All;
    } else {
        out.dofs = JPH::EAllowedDOFs::None;
        size_t i = 0;
        while (i < dofs.size()) {
            size_t j = dofs.find(',', i);
            std::string tok = dofs.substr(i, j == std::string::npos ? std::string::npos : j - i);
            if      (tok == "tx") out.dofs = out.dofs | JPH::EAllowedDOFs::TranslationX;
            else if (tok == "ty") out.dofs = out.dofs | JPH::EAllowedDOFs::TranslationY;
            else if (tok == "tz") out.dofs = out.dofs | JPH::EAllowedDOFs::TranslationZ;
            else if (tok == "rx") out.dofs = out.dofs | JPH::EAllowedDOFs::RotationX;
            else if (tok == "ry") out.dofs = out.dofs | JPH::EAllowedDOFs::RotationY;
            else if (tok == "rz") out.dofs = out.dofs | JPH::EAllowedDOFs::RotationZ;
            if (j == std::string::npos) break;
            i = j + 1;
        }
    }

    Value layerVal = ev::getProperty(opts.get(), "layer");
    auto* world = getPhysicsWorld();
    if (!ev::isUndefined(layerVal) && !ev::isNull(layerVal)) {
        if (!ev::isObject(layerVal)) {
            std::string s = ev::toUtf8(layerVal);
            bool isNumber = !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit);
            if (isNumber) {
                out.layer = std::stoi(s);
            } else if (world) {
                out.layer = world->layerIndex(s);
            }
        }
    }

    if (out.shape == physics::BodyOptions::ShapeConvexHull) {
        Value ptsVal = ev::getProperty(opts.get(), "points");
        std::vector<float> flat;
        if (readFloatVector(ptsVal, flat) && flat.size() >= 12 && (flat.size() % 3) == 0) {
            for (size_t i = 0; i + 2 < flat.size(); i += 3)
                out.hullPoints.push_back(JPH::Vec3(flat[i], flat[i+1], flat[i+2]));
        }
        if (out.hullPoints.size() < 4) { err = "convexHull requires >= 4 points (flat xyz)"; return false; }
    }

    if (out.shape == physics::BodyOptions::ShapeMesh) {
        Value posVal = ev::getProperty(opts.get(), "positions");
        Value idxVal = ev::getProperty(opts.get(), "indices");
        std::vector<float> verts;
        std::vector<uint32_t> idx;
        readFloatVector(posVal, verts);
        readU32Vector(idxVal, idx);
        if (verts.size() < 9 || (verts.size() % 3) != 0 || idx.size() < 3 || (idx.size() % 3) != 0) {
            err = "mesh requires positions (flat xyz) and indices (triangle list)";
            return false;
        }
        for (size_t i = 0; i + 2 < verts.size(); i += 3)
            out.meshVertices.push_back(JPH::Vec3(verts[i], verts[i+1], verts[i+2]));
        out.meshIndices = std::move(idx);
        out.isStatic = true;
    }

    if (out.shape == physics::BodyOptions::ShapeChain) {
        Value ptsVal = ev::getProperty(opts.get(), "points");
        std::vector<float> pts;
        readFloatVector(ptsVal, pts);
        if (pts.size() < 4 || (pts.size() % 2) != 0) {
            err = "chain requires points (flat [x0,y0,x1,y1,...]) with at least 2 points";
            return false;
        }
        for (size_t i = 0; i + 1 < pts.size(); i += 2)
            out.chainPoints.push_back(JPH::Float2(pts[i], pts[i+1]));
        out.chainDepth = static_cast<float>(getPropNumber(opts, "depth", out.chainDepth));
        out.chainClosed = getPropBool(opts, "closed", out.chainClosed);
        out.chainFlipNormal = getPropBool(opts, "flipNormal", out.chainFlipNormal);
        out.isStatic = true;
    }

    if (out.shape == physics::BodyOptions::ShapeHeightField) {
        Value hv = ev::getProperty(opts.get(), "heights");
        std::vector<float> heights;
        readFloatVector(hv, heights);
        uint32_t n = static_cast<uint32_t>(getPropNumber(opts, "sampleCount", 0.0));
        if (n == 0 && !heights.empty())
            n = static_cast<uint32_t>(std::lround(std::sqrt(static_cast<double>(heights.size()))));
        if (n < 4 || heights.size() != static_cast<size_t>(n) * n) {
            err = "heightfield requires heights (n*n floats, row-major, n >= 4) with matching sampleCount";
            return false;
        }
        out.heightSamples = std::move(heights);
        out.heightSampleCount = n;
        out.heightScale = readVec3(ev::getProperty(opts.get(), "scale"), JPH::Vec3(1, 1, 1));
        out.heightOffset = readVec3(ev::getProperty(opts.get(), "offset"));
        out.isStatic = true;
    }

    if (out.shape == physics::BodyOptions::ShapeCompound) {
        Value partsVal = ev::getProperty(opts.get(), "parts");
        if (ev::isObject(partsVal)) {
            ev::Persistent partsPersist(partsVal);
            Value lenV = ev::getProperty(partsPersist.get(), "length");
            uint32_t nparts = (!ev::isUndefined(lenV) && !ev::isObject(lenV)) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
            for (uint32_t i = 0; i < nparts; ++i) {
                Value p = ev::getElement(partsPersist.get(), i);
                physics::BodyOptions sub;
                std::string suberr;
                if (!readBodyOptions(p, sub, suberr)) {
                    err = "compound part " + std::to_string(i) + ": " + suberr;
                    return false;
                }
                out.compoundParts.push_back(std::move(sub));
            }
        }
        if (out.compoundParts.empty()) { err = "compound requires at least one part"; return false; }
    }

    return true;
}

void readQueryFilter(Value optsVal, physics::QueryFilter& out) {
    if (!ev::isObject(optsVal)) return;
    ev::Persistent opts(optsVal);
    auto* world = getPhysicsWorld();

    Value lv = ev::getProperty(opts.get(), "layers");
    if (ev::isObject(lv)) {
        ev::Persistent lvPersist(lv);
        Value lenV = ev::getProperty(lvPersist.get(), "length");
        if (!ev::isUndefined(lenV) && !ev::isObject(lenV)) {
            uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
            uint32_t mask = 0;
            for (uint32_t i = 0; i < n; ++i) {
                Value el = ev::getElement(lvPersist.get(), i);
                int32_t idx = -1;
                if (!ev::isObject(el) && !ev::isUndefined(el)) {
                    std::string s = ev::toUtf8(el);
                    bool isNumber = !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit);
                    if (isNumber) idx = std::stoi(s);
                    else if (world) idx = world->layerIndex(s);
                }
                if (idx >= 0 && idx < 32) mask |= (1u << idx);
            }
            out.layerMask = mask;
        }
    }

    Value maskVal = ev::getProperty(opts.get(), "layerMask");
    if (!ev::isUndefined(maskVal) && !ev::isObject(maskVal)) {
        out.layerMask = static_cast<uint32_t>(ev::toDouble(maskVal));
    }

    int32_t ignore = static_cast<int32_t>(getPropNumber(opts, "ignoreBody", -1.0));
    if (ignore >= 0) out.ignoreBody = g_phys.bodyIdForTag(ignore);

    Value iv = ev::getProperty(opts.get(), "ignoreBodies");
    if (ev::isObject(iv)) {
        ev::Persistent ivPersist(iv);
        Value lenV = ev::getProperty(ivPersist.get(), "length");
        if (!ev::isUndefined(lenV) && !ev::isObject(lenV)) {
            uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
            out.ignoreBodies.reserve(n);
            for (uint32_t i = 0; i < n; ++i) {
                Value el = ev::getElement(ivPersist.get(), i);
                int32_t tag = (!ev::isUndefined(el) && !ev::isObject(el)) ? static_cast<int32_t>(ev::toDouble(el)) : -1;
                if (tag >= 0) {
                    JPH::BodyID id = g_phys.bodyIdForTag(tag);
                    if (!id.IsInvalid()) out.ignoreBodies.push_back(id);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Character Controller & Soft Body Structures
// ---------------------------------------------------------------------------

struct HostPhysicsCharacter {
    uint32_t tag = kHostPhysicsCharacterTag;
    uint32_t handle = 0;
    int32_t innerTag = -1;
};

struct HostPhysicsSoftBody {
    uint32_t tag = kHostPhysicsSoftBodyTag;
    uint32_t handle = 0;
    int32_t bodyTag = -1;
    int gridX = 0;
    int gridZ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// Contact Event Delivery (hostFrame hook)
// ---------------------------------------------------------------------------

void drainPhysicsContactEvents() {
    auto* world = getPhysicsWorld();
    if (!world) return;

    bool overflowed = false;
    auto events = world->drainContactEvents(&overflowed);
    g_phys.lastContactOverflow = overflowed;
    g_phys.lastContactEvents = events;

    if (events.empty() || ev::isUndefined(g_phys.physicsObj.get())) return;

    std::vector<ev::Persistent> handlers;
    {
        Value on = ev::getProperty(g_phys.physicsObj.get(), "oncontact");
        if (ev::isFunction(on)) handlers.emplace_back(on);
    }
    {
        Value on = ev::getProperty(g_phys.physicsObj.get(), "onContact");
        if (ev::isFunction(on)) handlers.emplace_back(on);
    }
    for (ev::Persistent& entry : hostListSnapshot(g_phys.physicsObj, "__bronzeHostListeners_contact")) {
        if (ev::isFunction(entry.get())) handlers.push_back(std::move(entry));
    }
    if (handlers.empty()) return;

    for (const auto& e : events) {
        int32_t t1 = g_phys.tagForBodyId(e.body1);
        int32_t t2 = g_phys.tagForBodyId(e.body2);

        for (auto& handler : handlers) {
            ev::Persistent evt(ev::createObject());
            const char* typeStr = (e.type == physics::ContactEvent::Added) ? "added" : "removed";
            evt.set(ev::setProperty(evt.get(), "type", ev::fromUtf8(typeStr)));
            evt.set(ev::setProperty(evt.get(), "body1", ev::fromDouble(t1)));
            evt.set(ev::setProperty(evt.get(), "body2", ev::fromDouble(t2)));
            evt.set(ev::setProperty(evt.get(), "sensor", ev::fromBool(e.isSensor)));
            if (e.type == physics::ContactEvent::Added) {
                evt.set(ev::setProperty(evt.get(), "normal", makeVec3Value(e.normal.x, e.normal.y, e.normal.z)));
                evt.set(ev::setProperty(evt.get(), "penetration", ev::fromDouble(e.penetration)));
                evt.set(ev::setProperty(evt.get(), "impulse", ev::fromDouble(e.impulse)));
                Value pts = hostArrayOf(e.numPoints, [&](size_t k) {
                    return makeVec3Value(e.points[k].x, e.points[k].y, e.points[k].z);
                });
                evt.set(ev::setProperty(evt.get(), "points", pts));
            }

            Value arg = evt.get();
            ev::CallResult r = ev::call(handler.get(), g_phys.physicsObj.get(), std::span<const Value>(&arg, 1));
            if (r.thrown) {
                reportBronzeError("Physics.onContact", r.value);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Installation & Surface Definition
// ---------------------------------------------------------------------------

static Value makePhysicsObject() {
    ObjectBuilder b;

    // Body Management
    b.def("createBody", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world) return ev::throwError("PhysicsWorld not available");
        if (a.empty() || !ev::isObject(a[0])) return ev::throwTypeError("createBody(options) requires an object");

        physics::BodyOptions opts;
        std::string err;
        if (!readBodyOptions(a[0], opts, err)) return ev::throwTypeError("createBody: " + err);

        JPH::BodyID id = world->createBody(opts);
        if (id.IsInvalid()) return ev::throwError("Failed to create body");
        return ev::fromDouble(g_phys.registerBody(id));
    });

    b.def("destroyBody", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        world->destroyBody(id, [](JPH::BodyID bid) { g_phys.unregisterBodyId(bid); });
        if (!world->bodyExists(id)) g_phys.unregisterBody(tag);
        return ev::undefined();
    });

    b.def("destroyAll", 0, [](Value, std::span<const Value>) -> Value {
        auto* world = getPhysicsWorld();
        if (world) {
            world->destroyAll();
            g_phys.clear();
        }
        return ev::undefined();
    });

    b.def("getTransform", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::null();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::null();

        bool interp = a.size() >= 2 && ev::isObject(a[1]) && getPropBool(ev::Persistent(a[1]), "interpolated", false);
        return makeTransformValue(world, id, interp);
    });

    b.def("setPosition", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::RVec3 pos;
        if (a.size() >= 4) {
            pos = JPH::RVec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            pos = readRVec3(a[1]);
        }
        world->setPosition(id, pos);
        return ev::undefined();
    });

    b.def("setRotation", 5, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Quat rot;
        if (a.size() >= 5) {
            rot = JPH::Quat(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)),
                            static_cast<float>(numAt(a, 3)), static_cast<float>(numAt(a, 4))).Normalized();
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            rot = readQuat(a[1]);
        }
        world->setRotation(id, rot);
        return ev::undefined();
    });

    b.def("getVelocity", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::null();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::null();

        auto lv = world->getLinearVelocity(id);
        auto av = world->getAngularVelocity(id);
        return makeVelocityValue(lv, av);
    });

    b.def("setLinearVelocity", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Vec3 v;
        if (a.size() >= 4) {
            v = JPH::Vec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            v = readVec3(a[1]);
        }
        world->setLinearVelocity(id, v);
        return ev::undefined();
    });

    b.def("setAngularVelocity", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Vec3 v;
        if (a.size() >= 4) {
            v = JPH::Vec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            v = readVec3(a[1]);
        }
        world->setAngularVelocity(id, v);
        return ev::undefined();
    });

    b.def("addForce", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Vec3 f;
        if (a.size() >= 4) {
            f = JPH::Vec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            f = readVec3(a[1]);
        }
        world->addForce(id, f);
        return ev::undefined();
    });

    b.def("addImpulse", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Vec3 imp;
        if (a.size() >= 4) {
            imp = JPH::Vec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            imp = readVec3(a[1]);
        }
        world->addImpulse(id, imp);
        return ev::undefined();
    });

    b.def("addTorque", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        JPH::Vec3 t;
        if (a.size() >= 4) {
            t = JPH::Vec3(static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        } else if (a.size() >= 2 && ev::isObject(a[1])) {
            t = readVec3(a[1]);
        }
        world->addTorque(id, t);
        return ev::undefined();
    });

    b.def("setKinematic", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setKinematic(id);
        return ev::undefined();
    });

    b.def("setMotionType", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setMotionType(id, boolAt(a, 1));
        return ev::undefined();
    });

    b.def("moveKinematic", 5, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 5) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();

        double x = numAt(a, 1), y = numAt(a, 2), z = numAt(a, 3);
        JPH::Quat rot = world->getRotation(id);
        double dt = 0.0;
        if (a.size() >= 9) {
            rot = JPH::Quat(static_cast<float>(numAt(a, 4)), static_cast<float>(numAt(a, 5)),
                            static_cast<float>(numAt(a, 6)), static_cast<float>(numAt(a, 7))).Normalized();
            dt = numAt(a, 8);
        } else {
            dt = numAt(a, 4);
        }
        world->moveKinematic(id, JPH::RVec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)), rot, static_cast<float>(dt));
        return ev::undefined();
    });

    b.def("setUserData", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setUserData(id, static_cast<uint64_t>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getUserData", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(static_cast<double>(world->getUserData(id)));
    });

    b.def("setLayer", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::fromBool(false);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromBool(false);

        int layer = -1;
        if (!ev::isObject(a[1])) {
            std::string s = ev::toUtf8(a[1]);
            bool isNumber = !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit);
            if (isNumber) layer = std::stoi(s);
            else layer = world->layerIndex(s);
        }
        if (layer < 0) return ev::fromBool(false);
        world->setLayer(id, layer);
        return ev::fromBool(true);
    });

    b.def("setInterpolation", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (world && !a.empty()) world->setInterpolation(boolAt(a, 0));
        return ev::undefined();
    });

    b.def("getInterpolation", 0, [](Value, std::span<const Value>) -> Value {
        auto* world = getPhysicsWorld();
        return ev::fromBool(world ? world->interpolation() : false);
    });

    b.def("setMass", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setMass(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getMass", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(world->getMass(id));
    });

    b.def("setLinearDamping", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setLinearDamping(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getLinearDamping", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(world->getLinearDamping(id));
    });

    b.def("setAngularDamping", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setAngularDamping(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getAngularDamping", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(world->getAngularDamping(id));
    });

    b.def("setGravityFactor", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setGravityFactor(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getGravityFactor", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromDouble(1.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(1.0);
        return ev::fromDouble(world->getGravityFactor(id));
    });

    b.def("setFriction", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setFriction(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getFriction", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(world->getFriction(id));
    });

    b.def("setRestitution", 2, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::undefined();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::undefined();
        world->setRestitution(id, static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getRestitution", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromDouble(0.0);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::fromDouble(0.0);
        return ev::fromDouble(world->getRestitution(id));
    });

    b.def("getBodyProperties", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::null();
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        if (id.IsInvalid()) return ev::null();

        ObjectBuilder o;
        o.set("mass", ev::fromDouble(world->getMass(id)));
        o.set("friction", ev::fromDouble(world->getFriction(id)));
        o.set("restitution", ev::fromDouble(world->getRestitution(id)));
        o.set("linearDamping", ev::fromDouble(world->getLinearDamping(id)));
        o.set("angularDamping", ev::fromDouble(world->getAngularDamping(id)));
        o.set("gravityFactor", ev::fromDouble(world->getGravityFactor(id)));
        return o.get();
    });

    b.def("setGravity", 3, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 3) return ev::undefined();
        world->setGravity(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("getGravity", 0, [](Value, std::span<const Value>) -> Value {
        auto* world = getPhysicsWorld();
        if (!world) return makeVec3Value(0, -9.81f, 0);
        auto g = world->gravity();
        return makeVec3Value(g.GetX(), g.GetY(), g.GetZ());
    });

    b.def("setLayers", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty() || !ev::isObject(a[0])) return ev::fromBool(false);
        ev::Persistent root(a[0]);
        Value namesVal = ev::getProperty(root.get(), "names");
        Value matVal = ev::getProperty(root.get(), "matrix");

        std::vector<std::string> names;
        if (ev::isObject(namesVal)) {
            ev::Persistent np(namesVal);
            Value lenV = ev::getProperty(np.get(), "length");
            if (!ev::isUndefined(lenV) && !ev::isObject(lenV)) {
                uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
                for (uint32_t i = 0; i < n; ++i) {
                    Value el = ev::getElement(np.get(), i);
                    names.push_back(!ev::isObject(el) ? ev::toUtf8(el) : "");
                }
            }
        }

        std::vector<bool> matrix;
        if (ev::isObject(matVal)) {
            ev::Persistent mp(matVal);
            Value lenV = ev::getProperty(mp.get(), "length");
            if (!ev::isUndefined(lenV) && !ev::isObject(lenV)) {
                uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
                for (uint32_t i = 0; i < n; ++i) {
                    Value el = ev::getElement(mp.get(), i);
                    matrix.push_back(ev::toBool(el));
                }
            }
        }
        return ev::fromBool(world->configureLayers(names, matrix));
    });

    b.def("isActive", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return ev::fromBool(false);
        int32_t tag = i32At(a, 0);
        JPH::BodyID id = g_phys.bodyIdForTag(tag);
        return ev::fromBool(!id.IsInvalid() && world->isActive(id));
    });

    b.def("activate", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (world && !a.empty()) {
            int32_t tag = i32At(a, 0);
            JPH::BodyID id = g_phys.bodyIdForTag(tag);
            if (!id.IsInvalid()) world->activate(id);
        }
        return ev::undefined();
    });

    b.def("setTimeStep", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (world && !a.empty()) world->setTimeStep(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("getAllTransforms", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world) return makeFloat32Array(nullptr, 0);

        bool interp = !a.empty() && ev::isObject(a[0]) && getPropBool(ev::Persistent(a[0]), "interpolated", false);
        size_t count = g_phys.bodyTags.size();
        size_t stride = 8;
        std::vector<float> buf(count * stride);
        size_t i = 0;
        auto& bi = world->getBodyInterface();
        for (auto& [key, tag] : g_phys.bodyTags) {
            JPH::BodyID id(key);
            JPH::RVec3 pos;
            JPH::Quat rot;
            if (interp) {
                world->getRenderTransform(id, pos, rot);
            } else {
                pos = bi.GetPosition(id);
                rot = bi.GetRotation(id);
            }
            float* p = buf.data() + i * stride;
            p[0] = static_cast<float>(tag);
            p[1] = static_cast<float>(pos.GetX());
            p[2] = static_cast<float>(pos.GetY());
            p[3] = static_cast<float>(pos.GetZ());
            p[4] = rot.GetX(); p[5] = rot.GetY(); p[6] = rot.GetZ(); p[7] = rot.GetW();
            i++;
        }
        return makeFloat32Array(buf.data(), buf.size());
    });

    // Queries
    b.def("raycast", 8, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return hostArrayOf(0, [](size_t) { return ev::undefined(); });

        JPH::RVec3 origin;
        JPH::Vec3 dir;
        float maxDist = 1000.0f;
        physics::QueryFilter filter;

        if (ev::isObject(a[0])) {
            origin = readRVec3(a[0]);
            JPH::RVec3 target = readRVec3(a[1]);
            JPH::Vec3 diff = JPH::Vec3(target - origin);
            float len = diff.Length();
            maxDist = len > 0.0f ? len : 1000.0f;
            dir = len > 1e-6f ? diff / len : JPH::Vec3(0, 0, 0);
            if (a.size() >= 3) {
                if (ev::isObject(a[2])) readQueryFilter(a[2], filter);
                else filter.layerMask = static_cast<uint32_t>(numAt(a, 2));
            }
        } else if (a.size() >= 6) {
            origin = JPH::RVec3(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
            dir = JPH::Vec3(static_cast<float>(numAt(a, 3)), static_cast<float>(numAt(a, 4)), static_cast<float>(numAt(a, 5)));
            if (a.size() >= 7) {
                if (ev::isObject(a[6])) readQueryFilter(a[6], filter);
                else maxDist = static_cast<float>(numAt(a, 6));
            }
            if (a.size() >= 8 && ev::isObject(a[7])) readQueryFilter(a[7], filter);
        }

        auto hits = world->raycast(origin, dir, maxDist, filter);
        return hostArrayOf(hits.size(), [&](size_t i) {
            const auto& hit = hits[i];
            int32_t tag = g_phys.tagForBodyId(hit.bodyID);
            uint64_t udata = hit.bodyID.IsInvalid() ? 0 : world->getUserData(hit.bodyID);
            return makeRayHitValue(tag, hit, udata);
        });
    });

    b.def("raycastClosest", 8, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::null();

        JPH::RVec3 origin;
        JPH::Vec3 dir;
        float maxDist = 1000.0f;
        physics::QueryFilter filter;

        if (ev::isObject(a[0])) {
            origin = readRVec3(a[0]);
            JPH::RVec3 target = readRVec3(a[1]);
            JPH::Vec3 diff = JPH::Vec3(target - origin);
            float len = diff.Length();
            maxDist = len > 0.0f ? len : 1000.0f;
            dir = len > 1e-6f ? diff / len : JPH::Vec3(0, 0, 0);
            if (a.size() >= 3) {
                if (ev::isObject(a[2])) readQueryFilter(a[2], filter);
                else filter.layerMask = static_cast<uint32_t>(numAt(a, 2));
            }
        } else if (a.size() >= 6) {
            origin = JPH::RVec3(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
            dir = JPH::Vec3(static_cast<float>(numAt(a, 3)), static_cast<float>(numAt(a, 4)), static_cast<float>(numAt(a, 5)));
            if (a.size() >= 7) {
                if (ev::isObject(a[6])) readQueryFilter(a[6], filter);
                else maxDist = static_cast<float>(numAt(a, 6));
            }
            if (a.size() >= 8 && ev::isObject(a[7])) readQueryFilter(a[7], filter);
        }

        physics::RayHit hit;
        bool ok = world->raycastClosest(origin, dir, hit, maxDist, filter);
        if (!ok) return ev::null();
        int32_t tag = g_phys.tagForBodyId(hit.bodyID);
        uint64_t udata = hit.bodyID.IsInvalid() ? 0 : world->getUserData(hit.bodyID);
        return makeRayHitValue(tag, hit, udata);
    });

    b.def("overlapSphere", 3, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return hostArrayOf(0, [](size_t) { return ev::undefined(); });

        physics::BodyOptions shape;
        shape.shape = physics::BodyOptions::ShapeSphere;
        shape.position = readRVec3(a[0]);
        shape.radius = a.size() >= 2 ? static_cast<float>(numAt(a, 1)) : 0.5f;

        physics::QueryFilter filter;
        if (a.size() >= 3) {
            if (ev::isObject(a[2])) readQueryFilter(a[2], filter);
            else filter.layerMask = static_cast<uint32_t>(numAt(a, 2));
        }

        auto hits = world->overlapShape(shape, filter);
        return hostArrayOf(hits.size(), [&](size_t i) {
            int32_t tag = g_phys.tagForBodyId(hits[i].bodyID);
            return ev::fromDouble(tag);
        });
    });

    b.def("overlapBox", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return hostArrayOf(0, [](size_t) { return ev::undefined(); });

        physics::BodyOptions shape;
        shape.shape = physics::BodyOptions::ShapeBox;
        shape.position = readRVec3(a[0]);
        shape.halfExtents = a.size() >= 2 ? readVec3(a[1], JPH::Vec3(0.5f, 0.5f, 0.5f)) : JPH::Vec3(0.5f, 0.5f, 0.5f);
        if (a.size() >= 3 && ev::isObject(a[2])) shape.rotation = readQuat(a[2]);

        physics::QueryFilter filter;
        if (a.size() >= 4) {
            if (ev::isObject(a[3])) readQueryFilter(a[3], filter);
            else filter.layerMask = static_cast<uint32_t>(numAt(a, 3));
        }

        auto hits = world->overlapShape(shape, filter);
        return hostArrayOf(hits.size(), [&](size_t i) {
            int32_t tag = g_phys.tagForBodyId(hits[i].bodyID);
            return ev::fromDouble(tag);
        });
    });

    b.def("overlapPoint", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 3) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        JPH::RVec3 pt(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
        physics::QueryFilter filter;
        if (a.size() >= 4 && ev::isObject(a[3])) readQueryFilter(a[3], filter);

        auto bodies = world->overlapPoint(pt, filter);
        return hostArrayOf(bodies.size(), [&](size_t i) {
            int32_t tag = g_phys.tagForBodyId(bodies[i]);
            return ev::fromDouble(tag);
        });
    });

    // Character Controller Constructor
    b.def("createCharacter", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world) return ev::throwError("PhysicsWorld not available");
        if (a.empty() || !ev::isObject(a[0])) return ev::throwTypeError("createCharacter(options) requires an object");

        ev::Persistent opts(a[0]);
        physics::CharacterOptions copts;
        copts.position = readRVec3(ev::getProperty(opts.get(), "position"));
        copts.up = readVec3(ev::getProperty(opts.get(), "up"), JPH::Vec3(0, 1, 0));
        copts.radius = static_cast<float>(getPropNumber(opts, "radius", copts.radius));
        copts.halfHeight = static_cast<float>(getPropNumber(opts, "halfHeight", copts.halfHeight));
        copts.mass = static_cast<float>(getPropNumber(opts, "mass", copts.mass));
        copts.maxSlopeAngle = static_cast<float>(getPropNumber(opts, "maxSlopeAngle", copts.maxSlopeAngle));
        copts.maxStrength = static_cast<float>(getPropNumber(opts, "maxStrength", copts.maxStrength));
        copts.padding = static_cast<float>(getPropNumber(opts, "padding", copts.padding));
        copts.stepUp = static_cast<float>(getPropNumber(opts, "stepUp", copts.stepUp));
        copts.stickToFloor = static_cast<float>(getPropNumber(opts, "stickToFloor", copts.stickToFloor));
        copts.innerBody = getPropBool(opts, "innerBody", copts.innerBody);

        Value layerVal = ev::getProperty(opts.get(), "layer");
        if (!ev::isUndefined(layerVal) && !ev::isNull(layerVal)) {
            if (!ev::isObject(layerVal)) {
                std::string s = ev::toUtf8(layerVal);
                bool isNumber = !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit);
                if (isNumber) copts.layer = std::stoi(s);
                else copts.layer = world->layerIndex(s);
            }
        }

        uint32_t handle = world->createCharacter(copts);
        if (!handle) return ev::throwError("Failed to create character");

        auto* pc = new HostPhysicsCharacter();
        pc->handle = handle;
        if (copts.innerBody) {
            JPH::BodyID innerId = world->characterInnerBody(handle);
            pc->innerTag = g_phys.registerBody(innerId);
        }

        ObjectBuilder cb(ev::makeHandle(pc, [](void* p) {
            delete static_cast<HostPhysicsCharacter*>(p);
        }));

        cb.def("setVelocity", 3, [pc](Value, std::span<const Value> args) {
            auto* w = getPhysicsWorld();
            if (w && pc->handle) {
                JPH::Vec3 v;
                if (args.size() >= 3) {
                    v = JPH::Vec3(static_cast<float>(numAt(args, 0)), static_cast<float>(numAt(args, 1)), static_cast<float>(numAt(args, 2)));
                } else if (!args.empty() && ev::isObject(args[0])) {
                    v = readVec3(args[0]);
                }
                w->setCharacterVelocity(pc->handle, v);
            }
            return ev::undefined();
        });

        cb.def("getVelocity", 0, [pc](Value, std::span<const Value>) {
            auto* w = getPhysicsWorld();
            if (!w || !pc->handle) return ev::null();
            physics::CharacterState st;
            if (!w->getCharacterState(pc->handle, st)) return ev::null();
            return makeVec3Value(st.velocity.GetX(), st.velocity.GetY(), st.velocity.GetZ());
        });

        cb.def("setPosition", 3, [pc](Value, std::span<const Value> args) {
            auto* w = getPhysicsWorld();
            if (w && pc->handle) {
                JPH::RVec3 p;
                if (args.size() >= 3) {
                    p = JPH::RVec3(static_cast<float>(numAt(args, 0)), static_cast<float>(numAt(args, 1)), static_cast<float>(numAt(args, 2)));
                } else if (!args.empty() && ev::isObject(args[0])) {
                    p = readRVec3(args[0]);
                }
                w->setCharacterPosition(pc->handle, p);
            }
            return ev::undefined();
        });

        cb.def("getPosition", 0, [pc](Value, std::span<const Value>) {
            auto* w = getPhysicsWorld();
            if (!w || !pc->handle) return ev::null();
            physics::CharacterState st;
            if (!w->getCharacterState(pc->handle, st)) return ev::null();
            return makeVec3Value(static_cast<float>(st.position.GetX()),
                                 static_cast<float>(st.position.GetY()),
                                 static_cast<float>(st.position.GetZ()));
        });

        cb.def("getState", 0, [pc](Value, std::span<const Value>) {
            auto* w = getPhysicsWorld();
            if (!w || !pc->handle) return ev::null();
            physics::CharacterState st;
            if (!w->getCharacterState(pc->handle, st)) return ev::null();
            int32_t groundTag = st.groundBody.IsInvalid() ? -1 : g_phys.tagForBodyId(st.groundBody);
            return makeCharacterStateValue(st, groundTag);
        });

        cb.def("setShape", 1, [pc](Value, std::span<const Value> args) {
            auto* w = getPhysicsWorld();
            if (!w || !pc->handle || args.empty() || !ev::isObject(args[0])) return ev::fromBool(false);
            physics::BodyOptions shape;
            std::string err;
            if (!readBodyOptions(args[0], shape, err)) return ev::throwTypeError("setShape: " + err);
            return ev::fromBool(w->setCharacterShape(pc->handle, shape));
        });

        cb.accessor("innerBody", [pc](Value, std::span<const Value>) {
            return ev::fromDouble(pc->innerTag);
        }, nullptr);

        cb.def("destroy", 0, [pc](Value, std::span<const Value>) {
            auto* w = getPhysicsWorld();
            if (w && pc->handle) {
                if (pc->innerTag >= 0) g_phys.unregisterBody(pc->innerTag);
                w->destroyCharacter(pc->handle);
            }
            pc->handle = 0;
            pc->innerTag = -1;
            return ev::undefined();
        });

        return cb.get();
    });

    // Soft Body Constructor
    b.def("createSoftBody", 1, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world) return ev::throwError("PhysicsWorld not available");
        if (a.empty() || !ev::isObject(a[0])) return ev::throwTypeError("createSoftBody(options) requires an object");

        ev::Persistent opts(a[0]);
        physics::SoftBodyOptions sopts;

        Value clothV = ev::getProperty(opts.get(), "cloth");
        Value meshV = ev::getProperty(opts.get(), "mesh");
        const bool hasCloth = ev::isObject(clothV);
        const bool hasMesh = ev::isObject(meshV);

        bool pinCorners = false;
        auto readPinned = [&](Value holder) {
            ev::Persistent hp(holder);
            Value pv = ev::getProperty(hp.get(), "pinned");
            if (!ev::isUndefined(pv) && !ev::isNull(pv)) {
                if (!ev::isObject(pv)) {
                    std::string str = ev::toUtf8(pv);
                    if (str == "corners") pinCorners = true;
                } else {
                    readU32Vector(pv, sopts.pinned);
                }
            }
        };

        if (hasCloth && !hasMesh) {
            ev::Persistent cp(clothV);
            sopts.kind = physics::SoftBodyOptions::Cloth;
            sopts.gridX = static_cast<int>(getPropNumber(cp, "gridX", sopts.gridX));
            sopts.gridZ = static_cast<int>(getPropNumber(cp, "gridZ", sopts.gridZ));
            sopts.spacing = static_cast<float>(getPropNumber(cp, "spacing", sopts.spacing));
            sopts.mass = static_cast<float>(getPropNumber(cp, "mass", sopts.mass));
            if (sopts.gridX < 2 || sopts.gridZ < 2) return ev::throwTypeError("cloth.gridX/gridZ must be >= 2");
            readPinned(clothV);
            if (pinCorners) {
                uint32_t gx = static_cast<uint32_t>(sopts.gridX), gz = static_cast<uint32_t>(sopts.gridZ);
                sopts.pinned.insert(sopts.pinned.end(), { 0u, gx - 1, (gz - 1) * gx, gz * gx - 1 });
            }
        } else if (hasMesh && !hasCloth) {
            ev::Persistent mp(meshV);
            sopts.kind = physics::SoftBodyOptions::Mesh;
            std::vector<float> verts;
            readFloatVector(ev::getProperty(mp.get(), "vertices"), verts);
            readU32Vector(ev::getProperty(mp.get(), "indices"), sopts.indices);
            sopts.vertices.reserve(verts.size() / 3);
            for (size_t i = 0; i + 2 < verts.size(); i += 3)
                sopts.vertices.emplace_back(verts[i], verts[i+1], verts[i+2]);
            sopts.mass = static_cast<float>(getPropNumber(mp, "mass", sopts.mass));
            sopts.pressure = static_cast<float>(getPropNumber(mp, "pressure", sopts.pressure));
            readPinned(meshV);
            if (sopts.vertices.size() < 3 || sopts.indices.size() < 3)
                return ev::throwTypeError("mesh requires vertices (xyz triples) and indices (triangle list)");
        } else {
            return ev::throwTypeError("createSoftBody requires exactly one of cloth: {...} or mesh: {...}");
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
        sopts.position = readRVec3(ev::getProperty(opts.get(), "position"));
        sopts.rotation = readQuat(ev::getProperty(opts.get(), "rotation"));

        Value layerVal = ev::getProperty(opts.get(), "layer");
        if (!ev::isUndefined(layerVal) && !ev::isNull(layerVal)) {
            if (!ev::isObject(layerVal)) {
                std::string s = ev::toUtf8(layerVal);
                bool isNumber = !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit);
                if (isNumber) sopts.layer = std::stoi(s);
                else sopts.layer = world->layerIndex(s);
            }
        }

        uint32_t handle = world->createSoftBody(sopts);
        if (!handle) return ev::throwError("Failed to create soft body");

        auto* sb = new HostPhysicsSoftBody();
        sb->handle = handle;
        sb->bodyTag = g_phys.registerBody(world->softBodyBody(handle));
        if (sopts.kind == physics::SoftBodyOptions::Cloth) {
            sb->gridX = std::max(2, sopts.gridX);
            sb->gridZ = std::max(2, sopts.gridZ);
        }

        ObjectBuilder sbb(ev::makeHandle(sb, [](void* p) {
            delete static_cast<HostPhysicsSoftBody*>(p);
        }));

        sbb.accessor("body", [sb](Value, std::span<const Value>) {
            return ev::fromDouble(sb->bodyTag);
        }, nullptr);

        sbb.accessor("vertexCount", [sb](Value, std::span<const Value>) {
            auto* w = getPhysicsWorld();
            int count = (w && sb->handle) ? w->softBodyVertexCount(sb->handle) : 0;
            return ev::fromDouble(count < 0 ? 0 : count);
        }, nullptr);

        sbb.def("vertices", 0, [sb](Value, std::span<const Value>) {
            auto* w = getPhysicsWorld();
            if (!w || !sb->handle) return ev::null();
            std::vector<float> flat;
            if (!w->getSoftBodyVertices(sb->handle, flat)) return ev::null();
            return makeFloat32Array(flat.data(), flat.size());
        });

        sbb.def("topology", 0, [sb](Value, std::span<const Value>) {
            auto* w = getPhysicsWorld();
            if (!w || !sb->handle) return ev::null();
            std::vector<float> pos;
            std::vector<uint32_t> idx;
            if (!w->softBodyTopology(sb->handle, pos, idx)) return ev::null();
            ObjectBuilder res;
            res.set("positions", makeFloat32Array(pos.data(), pos.size()));
            res.set("indices", makeUint32Array(idx.data(), idx.size()));
            res.set("gridX", ev::fromDouble(sb->gridX));
            res.set("gridZ", ev::fromDouble(sb->gridZ));
            return res.get();
        });

        sbb.def("setVertex", 4, [sb](Value, std::span<const Value> args) {
            auto* w = getPhysicsWorld();
            if (!w || !sb->handle || args.size() < 4) return ev::fromBool(false);
            int idx = i32At(args, 0);
            double x = numAt(args, 1), y = numAt(args, 2), z = numAt(args, 3);
            return ev::fromBool(w->setSoftBodyVertexPosition(sb->handle, idx, JPH::RVec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z))));
        });

        sbb.def("setVertexVelocity", 4, [sb](Value, std::span<const Value> args) {
            auto* w = getPhysicsWorld();
            if (!w || !sb->handle || args.size() < 4) return ev::fromBool(false);
            int idx = i32At(args, 0);
            double x = numAt(args, 1), y = numAt(args, 2), z = numAt(args, 3);
            return ev::fromBool(w->setSoftBodyVertexVelocity(sb->handle, idx, JPH::Vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z))));
        });

        sbb.def("pin", 2, [sb](Value, std::span<const Value> args) {
            auto* w = getPhysicsWorld();
            if (!w || !sb->handle || args.empty()) return ev::fromBool(false);
            int idx = i32At(args, 0);
            bool pinned = args.size() < 2 ? true : boolAt(args, 1);
            return ev::fromBool(w->pinSoftBodyVertex(sb->handle, idx, pinned));
        });

        sbb.def("destroy", 0, [sb](Value, std::span<const Value>) {
            auto* w = getPhysicsWorld();
            if (w && sb->handle) {
                w->destroySoftBody(sb->handle);
                g_phys.unregisterBody(sb->bodyTag);
            }
            sb->handle = 0;
            sb->bodyTag = -1;
            return ev::undefined();
        });

        return sbb.get();
    });

    // Contact Events
    b.def("onContact", 1, [](Value, std::span<const Value> a) {
        if (!a.empty() && ev::isFunction(a[0])) {
            addHostListener(g_phys.physicsObj, "contact", a[0]);
        }
        return ev::undefined();
    });

    b.def("addEventListener", 2, [](Value, std::span<const Value> a) {
        if (a.size() >= 2 && ev::isFunction(a[1])) {
            std::string type = !ev::isObject(a[0]) ? ev::toUtf8(a[0]) : "";
            if (!type.empty()) {
                addHostListener(g_phys.physicsObj, type, a[1]);
            }
        }
        return ev::undefined();
    });

    b.def("removeEventListener", 2, [](Value, std::span<const Value> a) {
        if (a.size() >= 2 && ev::isFunction(a[1])) {
            std::string type = !ev::isObject(a[0]) ? ev::toUtf8(a[0]) : "";
            if (!type.empty()) {
                removeHostListener(g_phys.physicsObj, type, a[1]);
            }
        }
        return ev::undefined();
    });

    b.def("getContacts", 0, [](Value, std::span<const Value>) {
        auto* world = getPhysicsWorld();
        if (!world) return hostArrayOf(0, [](size_t) { return ev::undefined(); });

        bool overflowed = false;
        auto events = world->drainContactEvents(&overflowed);
        if (!events.empty()) {
            g_phys.lastContactEvents = events;
            g_phys.lastContactOverflow = overflowed;
        }

        const auto& evts = g_phys.lastContactEvents;
        return hostArrayOf(evts.size(), [&](size_t i) {
            const auto& e = evts[i];
            ObjectBuilder obj;
            const char* typeStr = (e.type == physics::ContactEvent::Added) ? "added" : "removed";
            obj.set("type", ev::fromUtf8(typeStr));
            int32_t t1 = g_phys.tagForBodyId(e.body1);
            int32_t t2 = g_phys.tagForBodyId(e.body2);
            obj.set("body1", ev::fromDouble(t1));
            obj.set("body2", ev::fromDouble(t2));
            obj.set("sensor", ev::fromBool(e.isSensor));
            if (e.type == physics::ContactEvent::Added) {
                obj.set("normal", makeVec3Value(e.normal.x, e.normal.y, e.normal.z));
                obj.set("penetration", ev::fromDouble(e.penetration));
                obj.set("impulse", ev::fromDouble(e.impulse));
                Value pts = hostArrayOf(e.numPoints, [&](size_t k) {
                    return makeVec3Value(e.points[k].x, e.points[k].y, e.points[k].z);
                });
                obj.set("points", pts);
            }
            return obj.get();
        });
    });

    return b.get();
}

void installPhysicsGlobals() {
    Value physVal = makePhysicsObject();
    g_phys.physicsObj.set(physVal);
    ev::registerGlobal("Physics", physVal);
    ev::registerGlobal("PhysicsCharacter", makeBrandConstructor("PhysicsCharacter"));
    ev::registerGlobal("PhysicsSoftBody", makeBrandConstructor("PhysicsSoftBody"));
}

}  // namespace bro::bronze_host
