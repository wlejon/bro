#pragma once

// Shared internal declarations and helpers for the bronze host physics module.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "engine/engine.h"
#include "physics/physics_world.h"
#include "util/log.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Constraints/PulleyConstraint.h>
#include <Jolt/Physics/Constraints/GearConstraint.h>
#include <Jolt/Physics/Constraints/RackAndPinionConstraint.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

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

extern PhysicsState g_phys;

physics::PhysicsWorld* getPhysicsWorld();

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

inline HostPhysicsCharacter* unwrapCharacter(Value v) {
    auto* h = static_cast<HostPhysicsCharacter*>(ev::handleData(v));
    return (h && h->tag == kHostPhysicsCharacterTag) ? h : nullptr;
}

inline HostPhysicsSoftBody* unwrapSoftBody(Value v) {
    auto* h = static_cast<HostPhysicsSoftBody*>(ev::handleData(v));
    return (h && h->tag == kHostPhysicsSoftBodyTag) ? h : nullptr;
}

extern HostClass g_characterClass;
extern HostClass g_softBodyClass;

// ---------------------------------------------------------------------------
// Helpers: Read JS inputs (Vec3, Quat, Arrays, Properties)
// ---------------------------------------------------------------------------

// A layer or axis given as a decimal string. Every caller of this used to spell
// it `all_of(isdigit)` then `std::stoi`, which has two faults and the second is
// fatal: `isdigit` on a plain `char` is undefined for any byte over 0x7F, and
// `stoi` THROWS `std::out_of_range` for a string of digits too long to fit an
// int — so `{ axis: '99999999999999999999' }` from a compiled app took the
// process down rather than being refused. Returns false for anything that is
// not a decimal integer in range, and the caller then treats the string as a
// NAME, which is what it always meant to do.
inline bool parseDecimalIndex(const std::string& s, int& out) {
    if (s.empty()) return false;
    for (const char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    try {
        size_t consumed = 0;
        const long long v = std::stoll(s, &consumed);
        if (consumed != s.size()) return false;
        if (v < 0 || v > std::numeric_limits<int>::max()) return false;
        out = static_cast<int>(v);
        return true;
    } catch (const std::exception&) {
        return false;  // out_of_range on a 19+ digit string
    }
}

inline JPH::Vec3 readVec3(Value v, JPH::Vec3 def = JPH::Vec3::sZero()) {
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

inline JPH::RVec3 readRVec3(Value v, JPH::RVec3 def = JPH::RVec3::sZero()) {
    JPH::Vec3 v3 = readVec3(v, JPH::Vec3(static_cast<float>(def.GetX()),
                                         static_cast<float>(def.GetY()),
                                         static_cast<float>(def.GetZ())));
    return JPH::RVec3(v3.GetX(), v3.GetY(), v3.GetZ());
}

inline JPH::Quat readQuat(Value v, JPH::Quat def = JPH::Quat::sIdentity()) {
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

inline bool readFloatVector(Value v, std::vector<float>& out) {
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

inline bool readU32Vector(Value v, std::vector<uint32_t>& out) {
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

inline std::string getPropString(const ev::Persistent& root, const char* name, const std::string& def = "") {
    Value v = ev::getProperty(root.get(), name);
    if (ev::isUndefined(v) || ev::isNull(v) || ev::isObject(v)) return def;
    return ev::toUtf8(v);
}

inline double getPropNumber(const ev::Persistent& root, const char* name, double def = 0.0) {
    Value v = ev::getProperty(root.get(), name);
    if (ev::isUndefined(v) || ev::isNull(v) || ev::isObject(v)) return def;
    double d = ev::toDouble(v);
    return std::isnan(d) ? def : d;
}

inline bool getPropBool(const ev::Persistent& root, const char* name, bool def = false) {
    Value v = ev::getProperty(root.get(), name);
    if (ev::isUndefined(v) || ev::isNull(v)) return def;
    return ev::toBool(v);
}

inline uint64_t getPropU64(const ev::Persistent& root, const char* name, uint64_t def = 0) {
    Value v = ev::getProperty(root.get(), name);
    if (ev::isUndefined(v) || ev::isNull(v)) return def;
    double d = ev::toDouble(v);
    return std::isnan(d) ? def : static_cast<uint64_t>(d);
}

inline bool parseCombineMode(const std::string& s, physics::CombineMode& out) {
    if (s == "average") out = physics::CombineMode::Average;
    else if (s == "min" || s == "minimum") out = physics::CombineMode::Min;
    else if (s == "max" || s == "maximum") out = physics::CombineMode::Max;
    else if (s == "multiply") out = physics::CombineMode::Multiply;
    else return false;
    return true;
}

inline bool readAreaOverride(const ev::Persistent& o, physics::AreaOverride& a, std::string& err) {
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

inline bool readBodyOptions(Value optsVal, physics::BodyOptions& out, std::string& err) {
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
            int idx = 0;
            if (parseDecimalIndex(s, idx)) {
                out.layer = idx;
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

inline void readQueryFilter(Value optsVal, physics::QueryFilter& out) {
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
                    int parsed = 0;
                    if (parseDecimalIndex(s, parsed)) idx = parsed;
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
// Helpers: Make JS Objects (Vec3, Quat, TypedArray, State)
// ---------------------------------------------------------------------------

inline Value makeFloat32Array(const float* data, size_t count) {
    Value arr = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(count));
    if (data && count > 0) {
        std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data), count * sizeof(float));
        ev::fillTypedArray(arr, bytes);
    }
    return arr;
}

inline Value makeUint32Array(const uint32_t* data, size_t count) {
    Value arr = ev::createTypedArray(ev::elements::Uint32, static_cast<uint32_t>(count));
    if (data && count > 0) {
        std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data), count * sizeof(uint32_t));
        ev::fillTypedArray(arr, bytes);
    }
    return arr;
}

inline Value makeVec3Value(float x, float y, float z) {
    ObjectBuilder b;
    b.set("x", ev::fromDouble(x));
    b.set("y", ev::fromDouble(y));
    b.set("z", ev::fromDouble(z));
    return b.get();
}

inline Value makeQuatValue(float x, float y, float z, float w) {
    ObjectBuilder b;
    b.set("x", ev::fromDouble(x));
    b.set("y", ev::fromDouble(y));
    b.set("z", ev::fromDouble(z));
    b.set("w", ev::fromDouble(w));
    return b.get();
}

inline Value makeTransformValue(physics::PhysicsWorld* world, JPH::BodyID id, bool interpolated) {
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

inline Value makeVelocityValue(const JPH::Vec3& linear, const JPH::Vec3& angular) {
    ObjectBuilder b;
    b.set("linear", makeVec3Value(linear.GetX(), linear.GetY(), linear.GetZ()));
    b.set("angular", makeVec3Value(angular.GetX(), angular.GetY(), angular.GetZ()));
    return b.get();
}

inline Value makeRayHitValue(int32_t tag, const physics::RayHit& hit, uint64_t udata) {
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

inline Value makeCharacterStateValue(const physics::CharacterState& st, int32_t groundTag) {
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
// Cross-module Function Declarations
// ---------------------------------------------------------------------------

void decorateCharacterProto(ObjectBuilder& cb);
Value physicsCreateCharacter(Value self, std::span<const Value> a);

void decorateSoftBodyProto(ObjectBuilder& sbb);
Value physicsCreateSoftBody(Value self, std::span<const Value> a);

void registerConstraintMethods(ObjectBuilder& b);
void registerQueryMethods(ObjectBuilder& b);

void drainPhysicsContactEvents();
void installPhysicsGlobals();

}  // namespace bro::bronze_host
