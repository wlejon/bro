#pragma once

// Shared internal declarations and helpers for the bronze host physics module.

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "natives/physics/native_physics_decl.h"
#include "engine/engine.h"
#include "physics/physics_world.h"
#include "util/log.h"
#include "embed/embed.h"

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
#include <unordered_set>
#include <vector>

namespace bro::bronze_host {

inline constexpr uint32_t kHostPhysicsWorldTag     = 0x50574C44u;  // 'PWLD'
inline constexpr uint32_t kHostPhysicsCharacterTag = 0x50434852u;  // 'PCHR'
inline constexpr uint32_t kHostPhysicsSoftBodyTag  = 0x50534F46u;  // 'PSOF'
inline constexpr uint32_t kHostPhysicsVehicleTag   = 0x50564548u;  // 'PVEH'
inline constexpr uint32_t kHostPhysicsRagdollTag   = 0x50524147u;  // 'PRAG'

struct HostPhysicsCharacter;
struct HostPhysicsSoftBody;
struct HostPhysicsVehicle;
struct HostPhysicsRagdoll;
struct HostPhysicsWorld;

struct HostPhysicsWorld {
    uint32_t tag = kHostPhysicsWorldTag;
    physics::PhysicsWorld* world = nullptr;
    bool ownsWorld = false;

    std::unordered_set<HostPhysicsCharacter*> liveCharacters;
    std::unordered_set<HostPhysicsVehicle*> liveVehicles;
    std::unordered_set<HostPhysicsRagdoll*> liveRagdolls;
    std::unordered_set<HostPhysicsSoftBody*> liveSoftBodies;

    std::unordered_map<uint32_t, int32_t> bodyTags;   // BodyID idx+seq -> tag
    std::unordered_map<int32_t, uint32_t> tagToBody;  // tag -> BodyID idx+seq
    int32_t nextTag = 1;

    std::vector<physics::ContactEvent> lastContactEvents;
    bool lastContactOverflow = false;

    int32_t registerBody(JPH::BodyID id) {
        if (id.IsInvalid()) return -1;
        int32_t t = nextTag++;
        bodyTags[id.GetIndexAndSequenceNumber()] = t;
        tagToBody[t] = id.GetIndexAndSequenceNumber();
        return t;
    }

    void unregisterBody(int32_t t) {
        auto it = tagToBody.find(t);
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

    JPH::BodyID bodyIdForTag(int32_t t) const {
        auto it = tagToBody.find(t);
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

    physics::PhysicsWorld* getWorld() const {
        if (world) return world;
        auto* eng = hostEngine();
        return eng ? eng->physicsWorld() : nullptr;
    }

    ~HostPhysicsWorld();
};

extern HostPhysicsWorld g_defaultWorld;
HostPhysicsWorld* getActiveWorld();
void pushActiveWorld(HostPhysicsWorld* w);
void popActiveWorld();

struct HostPhysicsCharacter {
    uint32_t tag = kHostPhysicsCharacterTag;
    HostPhysicsWorld* world = nullptr;
    uint32_t handle = 0;
    int32_t innerTag = -1;
};

struct HostPhysicsSoftBody {
    uint32_t tag = kHostPhysicsSoftBodyTag;
    HostPhysicsWorld* world = nullptr;
    uint32_t handle = 0;
    int32_t bodyTag = -1;
    int gridX = 0;
    int gridZ = 0;
};

struct HostPhysicsVehicle {
    uint32_t tag = kHostPhysicsVehicleTag;
    HostPhysicsWorld* world = nullptr;
    uint32_t handle = 0;
    int32_t bodyTag = -1;
    bool ownsChassis = false;
    physics::VehicleOptions::Controller type = physics::VehicleOptions::ControllerWheeled;
};

struct HostPhysicsRagdoll {
    uint32_t tag = kHostPhysicsRagdollTag;
    HostPhysicsWorld* world = nullptr;
    uint32_t handle = 0;
    std::vector<int32_t> partTags;
    std::vector<int32_t> parents;
    std::vector<std::string> names;
};

inline HostPhysicsWorld::~HostPhysicsWorld() {
    for (auto* c : liveCharacters) {
        if (c) { c->world = nullptr; c->handle = 0; }
    }
    liveCharacters.clear();
    for (auto* v : liveVehicles) {
        if (v) { v->world = nullptr; v->handle = 0; }
    }
    liveVehicles.clear();
    for (auto* r : liveRagdolls) {
        if (r) { r->world = nullptr; r->handle = 0; }
    }
    liveRagdolls.clear();
    for (auto* s : liveSoftBodies) {
        if (s) { s->world = nullptr; s->handle = 0; }
    }
    liveSoftBodies.clear();
    if (ownsWorld && world) {
        world->shutdown();
        delete world;
        world = nullptr;
    }
}

// Value parsing helpers
inline double getPropNumber(Value obj, const char* name, double def) {
    if (!ev::isObject(obj)) return def;
    Value v = ev::getProperty(obj, name);
    return (!ev::isUndefined(v) && !ev::isNull(v) && !ev::isObject(v)) ? ev::toDouble(v) : def;
}

inline bool getPropBool(Value obj, const char* name, bool def) {
    if (!ev::isObject(obj)) return def;
    Value v = ev::getProperty(obj, name);
    return (!ev::isUndefined(v) && !ev::isNull(v)) ? ev::toBool(v) : def;
}

inline std::string getPropString(Value obj, const char* name) {
    if (!ev::isObject(obj)) return "";
    Value v = ev::getProperty(obj, name);
    return (!ev::isUndefined(v) && !ev::isNull(v) && !ev::isObject(v)) ? ev::toUtf8(v) : "";
}

inline uint64_t getPropU64(Value obj, const char* name, uint64_t def) {
    if (!ev::isObject(obj)) return def;
    Value v = ev::getProperty(obj, name);
    return (!ev::isUndefined(v) && !ev::isNull(v) && !ev::isObject(v)) ? static_cast<uint64_t>(ev::toDouble(v)) : def;
}


inline JPH::Vec3 readVec3(Value v, JPH::Vec3 def = JPH::Vec3::sZero()) {
    if (!ev::isObject(v)) return def;
    Value xV = ev::getProperty(v, "x");
    Value yV = ev::getProperty(v, "y");
    Value zV = ev::getProperty(v, "z");
    if (!ev::isUndefined(xV) || !ev::isUndefined(yV) || !ev::isUndefined(zV)) {
        double x = (!ev::isUndefined(xV) && !ev::isObject(xV)) ? ev::toDouble(xV) : def.GetX();
        double y = (!ev::isUndefined(yV) && !ev::isObject(yV)) ? ev::toDouble(yV) : def.GetY();
        double z = (!ev::isUndefined(zV) && !ev::isObject(zV)) ? ev::toDouble(zV) : def.GetZ();
        return JPH::Vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    }
    Value e0 = ev::getElement(v, 0);
    Value e1 = ev::getElement(v, 1);
    Value e2 = ev::getElement(v, 2);
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
    Value xV = ev::getProperty(v, "x");
    Value yV = ev::getProperty(v, "y");
    Value zV = ev::getProperty(v, "z");
    Value wV = ev::getProperty(v, "w");
    if (!ev::isUndefined(wV)) {
        return JPH::Quat(static_cast<float>(ev::toDouble(xV)),
                         static_cast<float>(ev::toDouble(yV)),
                         static_cast<float>(ev::toDouble(zV)),
                         static_cast<float>(ev::toDouble(wV)));
    }
    Value e3 = ev::getElement(v, 3);
    if (!ev::isUndefined(e3)) {
        return JPH::Quat(static_cast<float>(ev::toDouble(ev::getElement(v, 0))),
                         static_cast<float>(ev::toDouble(ev::getElement(v, 1))),
                         static_cast<float>(ev::toDouble(ev::getElement(v, 2))),
                         static_cast<float>(ev::toDouble(e3)));
    }
    return def;
}

inline bool parseCombineMode(const std::string& s, physics::CombineMode& out) {
    if      (s == "average")  out = physics::CombineMode::Average;
    else if (s == "min")      out = physics::CombineMode::Min;
    else if (s == "max")      out = physics::CombineMode::Max;
    else if (s == "multiply") out = physics::CombineMode::Multiply;
    else return false;
    return true;
}

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
        return false;
    }
}

inline bool readAreaOverride(Value v, physics::AreaOverride& a, std::string& err) {
    if (!ev::isObject(v)) { err = "area override must be an object"; return false; }
    std::string mode = getPropString(v, "gravityMode");
    if (mode == "replace") a.gravityMode = physics::AreaOverride::GravityReplace;
    else if (mode == "combine") a.gravityMode = physics::AreaOverride::GravityCombine;
    else if (mode == "scale") a.gravityMode = physics::AreaOverride::GravityScale;
    else if (mode == "none") a.gravityMode = physics::AreaOverride::GravityNone;

    Value gv = ev::getProperty(v, "gravity");
    if (!ev::isUndefined(gv) && !ev::isNull(gv)) {
        a.gravity = readVec3(gv);
        if (mode.empty()) a.gravityMode = physics::AreaOverride::GravityReplace;
    }
    Value gp = ev::getProperty(v, "gravityPoint");
    if (!ev::isUndefined(gp)) a.pointGravity = ev::toBool(gp);
    Value gs = ev::getProperty(v, "gravityStrength");
    if (!ev::isUndefined(gs)) a.strength = static_cast<float>(ev::toDouble(gs));
    Value fo = ev::getProperty(v, "falloffDistance");
    if (!ev::isUndefined(fo)) a.falloffDistance = static_cast<float>(ev::toDouble(fo));
    Value gsc = ev::getProperty(v, "gravityScale");
    if (!ev::isUndefined(gsc)) {
        a.gravityScale = static_cast<float>(ev::toDouble(gsc));
        if (mode.empty()) a.gravityMode = physics::AreaOverride::GravityScale;
    }
    if (mode.empty() && a.gravityMode == physics::AreaOverride::GravityNone) {
        if (!ev::isUndefined(gp) || !ev::isUndefined(gs)) {
            a.gravityMode = physics::AreaOverride::GravityReplace;
        }
    }

    Value ld = ev::getProperty(v, "linearDamping");
    if (!ev::isUndefined(ld) && !ev::isNull(ld)) a.linearDamping = static_cast<float>(ev::toDouble(ld));
    Value ad = ev::getProperty(v, "angularDamping");
    if (!ev::isUndefined(ad) && !ev::isNull(ad)) a.angularDamping = static_cast<float>(ev::toDouble(ad));
    a.priority = static_cast<int>(getPropNumber(v, "priority", 0));
    return true;
}

inline bool readBodyOptions(Value v, physics::BodyOptions& out, std::string& err, physics::PhysicsWorld* world = nullptr) {
    if (!ev::isObject(v)) { err = "expected object"; return false; }
    std::string shape = getPropString(v, "shape");
    if (shape.empty() || shape == "box") out.shape = physics::BodyOptions::ShapeBox;
    else if (shape == "sphere")      out.shape = physics::BodyOptions::ShapeSphere;
    else if (shape == "capsule")     out.shape = physics::BodyOptions::ShapeCapsule;
    else if (shape == "cylinder")    out.shape = physics::BodyOptions::ShapeCylinder;
    else if (shape == "convexHull")  out.shape = physics::BodyOptions::ShapeConvexHull;
    else if (shape == "mesh") {
        if (getPropBool(v, "decompose", false)) {
            out.shape = physics::BodyOptions::ShapeDecomposedMesh;
        } else {
            out.shape = physics::BodyOptions::ShapeMesh;
        }
    }
    else if (shape == "decomposedMesh") out.shape = physics::BodyOptions::ShapeDecomposedMesh;
    else if (shape == "compound")    out.shape = physics::BodyOptions::ShapeCompound;
    else if (shape == "chain")       out.shape = physics::BodyOptions::ShapeChain;
    else if (shape == "heightfield") out.shape = physics::BodyOptions::ShapeHeightField;
    else { err = "unknown shape: " + shape; return false; }

    out.position = readRVec3(ev::getProperty(v, "position"));
    out.rotation = readQuat(ev::getProperty(v, "rotation"));
    out.localPosition = readVec3(ev::getProperty(v, "localPosition"));
    out.localRotation = readQuat(ev::getProperty(v, "localRotation"));

    out.halfExtents = readVec3(ev::getProperty(v, "halfExtents"), JPH::Vec3(0.5f, 0.5f, 0.5f));
    out.radius = static_cast<float>(getPropNumber(v, "radius", out.radius));
    out.halfHeight = static_cast<float>(getPropNumber(v, "halfHeight", out.halfHeight));

    out.isStatic = getPropBool(v, "static", out.isStatic) || getPropBool(v, "isStatic", false);
    out.isSensor = getPropBool(v, "sensor", out.isSensor) || getPropBool(v, "isSensor", false);
    out.ccd = getPropBool(v, "ccd", out.ccd);

    Value areaVal = ev::getProperty(v, "area");
    if (ev::isObject(areaVal)) {
        if (!out.isSensor) { err = "area field overrides require sensor: true"; return false; }
        if (!readAreaOverride(areaVal, out.area, err)) return false;
        out.hasArea = true;
    }

    out.friction = static_cast<float>(getPropNumber(v, "friction", out.friction));
    out.restitution = static_cast<float>(getPropNumber(v, "restitution", out.restitution));

    std::string fc = getPropString(v, "frictionCombine");
    if (!fc.empty() && !parseCombineMode(fc, out.frictionCombine)) {
        err = "frictionCombine must be 'average' | 'min' | 'max' | 'multiply'";
        return false;
    }
    std::string rc = getPropString(v, "restitutionCombine");
    if (!rc.empty() && !parseCombineMode(rc, out.restitutionCombine)) {
        err = "restitutionCombine must be 'average' | 'min' | 'max' | 'multiply'";
        return false;
    }

    out.density = static_cast<float>(getPropNumber(v, "density", out.density));
    out.mass = static_cast<float>(getPropNumber(v, "mass", out.mass));
    out.gravityFactor = static_cast<float>(getPropNumber(v, "gravityFactor", out.gravityFactor));
    out.linearDamping = static_cast<float>(getPropNumber(v, "linearDamping", out.linearDamping));
    out.angularDamping = static_cast<float>(getPropNumber(v, "angularDamping", out.angularDamping));
    out.maxLinearVelocity = static_cast<float>(getPropNumber(v, "maxLinearVelocity", out.maxLinearVelocity));
    out.maxAngularVelocity = static_cast<float>(getPropNumber(v, "maxAngularVelocity", out.maxAngularVelocity));
    out.userData = getPropU64(v, "userData", 0);

    if (out.shape == physics::BodyOptions::ShapeConvexHull) {
        Value ptsVal = ev::getProperty(v, "points");
        std::vector<float> flat;
        if (readFloatVector(ptsVal, flat) && flat.size() >= 12 && (flat.size() % 3) == 0) {
            for (size_t i = 0; i + 2 < flat.size(); i += 3)
                out.hullPoints.push_back(JPH::Vec3(flat[i], flat[i+1], flat[i+2]));
        }
        if (out.hullPoints.size() < 4) { err = "convexHull requires >= 4 points (flat xyz)"; return false; }
    }

    if (out.shape == physics::BodyOptions::ShapeCompound) {
        Value partsVal = ev::getProperty(v, "parts");
        if (ev::isObject(partsVal)) {
            Value lenV = ev::getProperty(partsVal, "length");
            if (ev::isNumber(lenV)) {
                uint32_t len = static_cast<uint32_t>(ev::toDouble(lenV));
                for (uint32_t i = 0; i < len; ++i) {
                    Value pVal = ev::getElement(partsVal, i);
                    physics::BodyOptions partOpts;
                    std::string partErr;
                    if (readBodyOptions(pVal, partOpts, partErr, world)) {
                        out.compoundParts.push_back(std::move(partOpts));
                    }
                }
            }
        }
    }
    Value layerVal = ev::getProperty(v, "layer");
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

    if (out.shape == physics::BodyOptions::ShapeHeightField) {
        out.isStatic = true;
        Value hVal = ev::getProperty(v, "heights");
        if (ev::isUndefined(hVal) || ev::isNull(hVal)) {
            hVal = ev::getProperty(v, "heightSamples");
        }
        readFloatVector(hVal, out.heightSamples);
        if (out.heightSamples.empty() && ev::isObject(hVal)) {
            for (uint32_t i = 0; ; ++i) {
                std::string key = std::to_string(i);
                Value elem = ev::getProperty(hVal, key.c_str());
                if (ev::isUndefined(elem)) break;
                out.heightSamples.push_back(static_cast<float>(ev::toDouble(elem)));
            }
        }
        double sc = getPropNumber(v, "sampleCount", 0.0);
        if (sc <= 0.0) sc = getPropNumber(v, "heightSampleCount", 0.0);
        out.heightSampleCount = static_cast<uint32_t>(sc);

        Value scaleVal = ev::getProperty(v, "scale");
        if (ev::isObject(scaleVal)) {
            out.heightScale = readVec3(scaleVal, JPH::Vec3::sReplicate(1.0f));
        } else {
            Value hs = ev::getProperty(v, "heightScale");
            if (ev::isObject(hs)) out.heightScale = readVec3(hs, JPH::Vec3::sReplicate(1.0f));
        }

        Value offVal = ev::getProperty(v, "offset");
        if (ev::isObject(offVal)) {
            out.heightOffset = readVec3(offVal, JPH::Vec3::sZero());
        } else {
            Value ho = ev::getProperty(v, "heightOffset");
            if (ev::isObject(ho)) out.heightOffset = readVec3(ho, JPH::Vec3::sZero());
        }
    }

    if (out.shape == physics::BodyOptions::ShapeMesh) {
        out.isStatic = true;
        Value posVal = ev::getProperty(v, "positions");
        if (ev::isUndefined(posVal) || ev::isNull(posVal)) {
            posVal = ev::getProperty(v, "vertices");
        }
        std::vector<float> flat;
        if (readFloatVector(posVal, flat) && flat.size() >= 9 && (flat.size() % 3) == 0) {
            for (size_t i = 0; i + 2 < flat.size(); i += 3) {
                out.meshVertices.emplace_back(flat[i], flat[i+1], flat[i+2]);
            }
        }
        readU32Vector(ev::getProperty(v, "indices"), out.meshIndices);
    }

    if (out.shape == physics::BodyOptions::ShapeDecomposedMesh) {
        out.maxHulls = static_cast<int>(getPropNumber(v, "maxHulls", out.maxHulls));
        out.maxVerticesPerHull = static_cast<int>(getPropNumber(v, "maxVerticesPerHull", out.maxVerticesPerHull));
        out.decompResolution = static_cast<float>(getPropNumber(v, "resolution", out.decompResolution));
        out.decompResolution = static_cast<float>(getPropNumber(v, "decompResolution", out.decompResolution));
        out.minVolumePerHull = static_cast<float>(getPropNumber(v, "minVolumePerHull", out.minVolumePerHull));

        Value posVal = ev::getProperty(v, "positions");
        if (ev::isUndefined(posVal) || ev::isNull(posVal)) {
            posVal = ev::getProperty(v, "vertices");
        }
        std::vector<float> flat;
        if (readFloatVector(posVal, flat) && flat.size() >= 9 && (flat.size() % 3) == 0) {
            for (size_t i = 0; i + 2 < flat.size(); i += 3) {
                out.meshVertices.emplace_back(flat[i], flat[i+1], flat[i+2]);
            }
        }
        readU32Vector(ev::getProperty(v, "indices"), out.meshIndices);
    }

    return true;
}

static const char* const kSixDofAxisNames[6] = {
    "translationX", "translationY", "translationZ",
    "rotationX", "rotationY", "rotationZ",
};

inline int motorAxisIndex(const std::string& name) {
    for (int i = 0; i < 6; i++) {
        if (name == kSixDofAxisNames[i]) return i;
    }
    if (name == "tx") return 0;
    if (name == "ty") return 1;
    if (name == "tz") return 2;
    if (name == "rx") return 3;
    if (name == "ry") return 4;
    if (name == "rz") return 5;
    return -1;
}

inline bool readMotorOptions(Value oVal, physics::MotorOptions& m, std::string& err) {
    if (!ev::isObject(oVal)) { err = "motor options must be an object"; return false; }
    std::string type = getPropString(oVal, "type");
    if (type == "velocity")      m.state = physics::MotorOptions::Velocity;
    else if (type == "position") m.state = physics::MotorOptions::Position;
    else if (type == "off" || type.empty()) m.state = physics::MotorOptions::Off;
    else { err = "motor type must be 'velocity' | 'position' | 'off'"; return false; }

    m.target    = static_cast<float>(getPropNumber(oVal, "target", m.target));
    m.maxForce  = static_cast<float>(getPropNumber(oVal, "maxForce", m.maxForce));
    m.maxTorque = static_cast<float>(getPropNumber(oVal, "maxTorque", m.maxTorque));
    m.frequency = static_cast<float>(getPropNumber(oVal, "frequency", m.frequency));
    m.damping   = static_cast<float>(getPropNumber(oVal, "damping", m.damping));

    Value av = ev::getProperty(oVal, "axis");
    if (!ev::isUndefined(av) && !ev::isNull(av) && !ev::isObject(av)) {
        std::string s = ev::toUtf8(av);
        int idx = motorAxisIndex(s);
        int numeric = 0;
        if (idx >= 0) {
            m.axis = idx;
        } else if (parseDecimalIndex(s, numeric)) {
            m.axis = numeric;
        } else {
            err = "motor axis must be translationX..Z / rotationX..Z";
            return false;
        }
    }
    return true;
}

inline bool readSixDofAxis(Value v, physics::SixDofAxis& a, std::string& err) {
    if (!ev::isObject(v)) {
        std::string mode = ev::toUtf8(v);
        if (mode == "locked")    a.mode = physics::SixDofAxis::Locked;
        else if (mode == "free") a.mode = physics::SixDofAxis::Free;
        else { err = "axis mode must be 'locked' | 'free' | {min,max,...}"; return false; }
        return true;
    }
    Value minV = ev::getProperty(v, "min");
    Value maxV = ev::getProperty(v, "max");
    if (!ev::isUndefined(minV) && !ev::isObject(minV) && !ev::isUndefined(maxV) && !ev::isObject(maxV)) {
        a.mode = physics::SixDofAxis::Limited;
        a.min = static_cast<float>(ev::toDouble(minV));
        a.max = static_cast<float>(ev::toDouble(maxV));
    } else {
        a.mode = physics::SixDofAxis::Free;
    }
    a.springFrequency = static_cast<float>(getPropNumber(v, "frequency", a.springFrequency));
    a.springDamping   = static_cast<float>(getPropNumber(v, "damping", a.springDamping));
    a.maxFriction     = static_cast<float>(getPropNumber(v, "friction", a.maxFriction));
    return true;
}

inline void readQueryFilter(Value v, physics::QueryFilter& filter, HostPhysicsWorld* pw) {
    if (!ev::isObject(v)) return;
    Value ign = ev::getProperty(v, "ignoreBody");
    if (!ev::isUndefined(ign) && !ev::isNull(ign) && !ev::isObject(ign)) {
        int32_t t = static_cast<int32_t>(ev::toDouble(ign));
        filter.ignoreBody = pw->bodyIdForTag(t);
    }
    Value igns = ev::getProperty(v, "ignoreBodies");
    if (ev::isObject(igns)) {
        Value lenV = ev::getProperty(igns, "length");
        uint32_t len = ev::isNumber(lenV) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
        for (uint32_t i = 0; i < len; ++i) {
            Value it = ev::getElement(igns, i);
            if (ev::isNumber(it)) {
                int32_t t = static_cast<int32_t>(ev::toDouble(it));
                JPH::BodyID bid = pw->bodyIdForTag(t);
                if (!bid.IsInvalid()) filter.ignoreBodies.push_back(bid);
            }
        }
    }
    Value layers = ev::getProperty(v, "layers");
    if (ev::isObject(layers)) {
        auto* world = pw->getWorld();
        if (world) {
            filter.layerMask = 0;
            Value lenV = ev::getProperty(layers, "length");
            uint32_t len = ev::isNumber(lenV) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
            for (uint32_t i = 0; i < len; ++i) {
                Value it = ev::getElement(layers, i);
                if (!ev::isUndefined(it) && !ev::isObject(it)) {
                    std::string s = ev::toUtf8(it);
                    int idx = world->layerIndex(s);
                    if (idx >= 0 && idx < 32) filter.layerMask |= (1u << idx);
                }
            }
        }
    }
}

inline bool readQueryShape(Value optsVal, physics::BodyOptions& shape, std::string& err, HostPhysicsWorld* pw) {
    if (!readBodyOptions(optsVal, shape, err, pw ? pw->getWorld() : nullptr)) return false;
    switch (shape.shape) {
        case physics::BodyOptions::ShapeBox:
        case physics::BodyOptions::ShapeSphere:
        case physics::BodyOptions::ShapeCapsule:
        case physics::BodyOptions::ShapeCylinder:
        case physics::BodyOptions::ShapeConvexHull:
            return true;
        default:
            err = "query shape must be convex (box|sphere|capsule|cylinder|convexHull)";
            return false;
    }
}

}  // namespace bro::bronze_host
