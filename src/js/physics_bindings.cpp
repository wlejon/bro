// Jolt rigid-body physics bindings (the global `Physics` class + PhysicsNode
// support). Compiled only when BRO_WITH_PHYSICS is on — the header pulls Jolt,
// so the guard precedes every include. With physics off there is no `Physics`
// class (advanced apps feature-detect `typeof Physics`); nothing else installs
// it, and 3D (which embeds physics) is forced off too.
#if BRO_WITH_PHYSICS

#include "js/physics_bindings.h"
#include "js/runtime.h"
#include "physics/physics_world.h"
#include "util/log.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <qjsbind/qjsbind.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

JPH_SUPPRESS_WARNINGS

namespace bro::js {

// ---------------------------------------------------------------------------
// JsWorld — wraps a PhysicsWorld plus its tag space and JS-side bookkeeping.
// The default world is owned by the engine (s_defaultWorld points at it but
// does NOT own the PhysicsWorld); sandbox worlds own their PhysicsWorld.
// ---------------------------------------------------------------------------

struct JsCharacter;
struct JsVehicle;

struct JsWorld {
    physics::PhysicsWorld* world = nullptr;
    bool ownsWorld = false;  // true → delete `world` in destructor

    // Sandbox characters/vehicles that still reference this world. GC teardown
    // order is arbitrary, so ~JsWorld must sever their back-pointers before
    // the handle finalizers run (see characterClassFinalizer /
    // vehicleClassFinalizer).
    std::unordered_set<JsCharacter*> liveCharacters;
    std::unordered_set<JsVehicle*> liveVehicles;

    std::unordered_map<uint32_t, int32_t> bodyTags;     // BodyID idx+seq → tag
    std::unordered_map<int32_t, uint32_t> tagToBody;    // tag → BodyID idx+seq
    int32_t nextTag = 1;

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

    ~JsWorld();  // defined after JsCharacter — severs live character back-pointers
};

// ---------------------------------------------------------------------------
// Default world (engine-stepped, doesn't own PhysicsWorld).
// ---------------------------------------------------------------------------

static JsWorld* s_defaultWorld = nullptr;
static JSClassID s_worldClassId = 0;

JPH::BodyID PhysicsBindings::bodyIdForTag(int32_t tag) {
    return s_defaultWorld ? s_defaultWorld->bodyIdForTag(tag) : JPH::BodyID();
}

int32_t PhysicsBindings::tagForBodyId(JPH::BodyID id) {
    return s_defaultWorld ? s_defaultWorld->tagForBodyId(id) : -1;
}

physics::PhysicsWorld* PhysicsBindings::unwrapWorld(JSContext*, JSValueConst v) {
    if (s_worldClassId != 0 && JS_IsObject(v)) {
        if (auto* w = (JsWorld*)JS_GetOpaque(v, s_worldClassId))
            return w->world;
    }
    return s_defaultWorld ? s_defaultWorld->world : nullptr;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint64_t jsGetU64(JSContext* ctx, JSValueConst obj, const char* prop, uint64_t def = 0) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    uint64_t r = def;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        if (JS_IsBigInt(v)) {
            int64_t s; JS_ToBigInt64(ctx, &s, v); r = (uint64_t)s;
        } else {
            double d; JS_ToFloat64(ctx, &d, v); r = (uint64_t)d;
        }
    }
    JS_FreeValue(ctx, v);
    return r;
}

static JPH::Vec3 readVec3(JSContext* ctx, JSValueConst obj, JPH::Vec3 def = JPH::Vec3::sZero()) {
    if (!JS_IsObject(obj)) return def;
    return JPH::Vec3(
        (float)qjsbind::get_prop_number(ctx, obj, "x", def.GetX()),
        (float)qjsbind::get_prop_number(ctx, obj, "y", def.GetY()),
        (float)qjsbind::get_prop_number(ctx, obj, "z", def.GetZ()));
}

static JPH::Quat readQuat(JSContext* ctx, JSValueConst obj) {
    if (!JS_IsObject(obj)) return JPH::Quat::sIdentity();
    return JPH::Quat(
        (float)qjsbind::get_prop_number(ctx, obj, "x", 0),
        (float)qjsbind::get_prop_number(ctx, obj, "y", 0),
        (float)qjsbind::get_prop_number(ctx, obj, "z", 0),
        (float)qjsbind::get_prop_number(ctx, obj, "w", 1)).Normalized();
}

static bool readFloatArray(JSContext* ctx, JSValueConst v, std::vector<float>& out) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return false;
    size_t byteOffset = 0, byteLength = 0, bytesPerElement = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &byteOffset, &byteLength, &bytesPerElement);
    if (!JS_IsException(buf)) {
        size_t bufSize = 0;
        uint8_t* data = JS_GetArrayBuffer(ctx, &bufSize, buf);
        if (data && bytesPerElement == 4) {
            const float* fp = reinterpret_cast<const float*>(data + byteOffset);
            size_t n = byteLength / 4;
            out.assign(fp, fp + n);
            JS_FreeValue(ctx, buf);
            return true;
        }
        JS_FreeValue(ctx, buf);
    }
    JSValue lenVal = JS_GetPropertyStr(ctx, v, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);
    out.clear();
    out.reserve(len);
    for (uint32_t i = 0; i < len; i++) {
        JSValue el = JS_GetPropertyUint32(ctx, v, i);
        double d = 0; JS_ToFloat64(ctx, &d, el);
        out.push_back((float)d);
        JS_FreeValue(ctx, el);
    }
    return true;
}

static bool readU32Array(JSContext* ctx, JSValueConst v, std::vector<uint32_t>& out) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return false;
    size_t byteOffset = 0, byteLength = 0, bytesPerElement = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &byteOffset, &byteLength, &bytesPerElement);
    if (!JS_IsException(buf)) {
        size_t bufSize = 0;
        uint8_t* data = JS_GetArrayBuffer(ctx, &bufSize, buf);
        if (data && bytesPerElement == 4) {
            const uint32_t* up = reinterpret_cast<const uint32_t*>(data + byteOffset);
            size_t n = byteLength / 4;
            out.assign(up, up + n);
            JS_FreeValue(ctx, buf);
            return true;
        }
        if (data && bytesPerElement == 2) {
            const uint16_t* up = reinterpret_cast<const uint16_t*>(data + byteOffset);
            size_t n = byteLength / 2;
            out.clear(); out.reserve(n);
            for (size_t i = 0; i < n; i++) out.push_back(up[i]);
            JS_FreeValue(ctx, buf);
            return true;
        }
        JS_FreeValue(ctx, buf);
    }
    JSValue lenVal = JS_GetPropertyStr(ctx, v, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);
    out.clear(); out.reserve(len);
    for (uint32_t i = 0; i < len; i++) {
        JSValue el = JS_GetPropertyUint32(ctx, v, i);
        uint32_t u = 0; JS_ToUint32(ctx, &u, el);
        out.push_back(u);
        JS_FreeValue(ctx, el);
    }
    return true;
}

// Parse a BodyOptions struct from a JS opts object (against `world`).
static bool readBodyOptions(JSContext* ctx, JSValueConst opts,
                            physics::PhysicsWorld* world,
                            physics::BodyOptions& out, std::string& err) {
    std::string shape = qjsbind::get_prop_string(ctx, opts, "shape");
    if (shape.empty()) { err = "shape is required"; return false; }

    if      (shape == "box")        out.shape = physics::BodyOptions::ShapeBox;
    else if (shape == "sphere")     out.shape = physics::BodyOptions::ShapeSphere;
    else if (shape == "capsule")    out.shape = physics::BodyOptions::ShapeCapsule;
    else if (shape == "cylinder")   out.shape = physics::BodyOptions::ShapeCylinder;
    else if (shape == "convexHull") out.shape = physics::BodyOptions::ShapeConvexHull;
    else if (shape == "mesh")       out.shape = physics::BodyOptions::ShapeMesh;
    else if (shape == "compound")   out.shape = physics::BodyOptions::ShapeCompound;
    else if (shape == "chain")      out.shape = physics::BodyOptions::ShapeChain;
    else if (shape == "heightfield") out.shape = physics::BodyOptions::ShapeHeightField;
    else { err = "unknown shape: " + shape; return false; }

    JSValue posVal = JS_GetPropertyStr(ctx, opts, "position");
    out.position = readVec3(ctx, posVal);
    JS_FreeValue(ctx, posVal);

    JSValue rotVal = JS_GetPropertyStr(ctx, opts, "rotation");
    out.rotation = readQuat(ctx, rotVal);
    JS_FreeValue(ctx, rotVal);

    JSValue lpos = JS_GetPropertyStr(ctx, opts, "localPosition");
    out.localPosition = readVec3(ctx, lpos);
    JS_FreeValue(ctx, lpos);

    JSValue lrot = JS_GetPropertyStr(ctx, opts, "localRotation");
    out.localRotation = readQuat(ctx, lrot);
    JS_FreeValue(ctx, lrot);

    JSValue he = JS_GetPropertyStr(ctx, opts, "halfExtents");
    out.halfExtents = readVec3(ctx, he, JPH::Vec3(0.5f, 0.5f, 0.5f));
    JS_FreeValue(ctx, he);

    out.radius = (float)qjsbind::get_prop_number(ctx, opts, "radius", 0.5);
    out.halfHeight = (float)qjsbind::get_prop_number(ctx, opts, "halfHeight", 0.5);

    out.isStatic = qjsbind::get_prop_bool(ctx, opts, "static", false);
    out.isSensor = qjsbind::get_prop_bool(ctx, opts, "sensor", false);
    out.ccd = qjsbind::get_prop_bool(ctx, opts, "ccd", false);
    out.friction = (float)qjsbind::get_prop_number(ctx, opts, "friction", 0.5);
    out.restitution = (float)qjsbind::get_prop_number(ctx, opts, "restitution", 0.3);
    out.density = (float)qjsbind::get_prop_number(ctx, opts, "density", 1000.0);
    out.gravityFactor = (float)qjsbind::get_prop_number(ctx, opts, "gravityFactor", 1.0);
    out.linearDamping = (float)qjsbind::get_prop_number(ctx, opts, "linearDamping", 0.05);
    out.angularDamping = (float)qjsbind::get_prop_number(ctx, opts, "angularDamping", 0.05);
    out.maxLinearVelocity = (float)qjsbind::get_prop_number(ctx, opts, "maxLinearVelocity", 500.0);
    out.maxAngularVelocity = (float)qjsbind::get_prop_number(ctx, opts, "maxAngularVelocity", 0.25 * 3.14159265 * 60.0);
    out.userData = jsGetU64(ctx, opts, "userData", 0);

    std::string dofs = qjsbind::get_prop_string(ctx, opts, "dofs");
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

    JSValue layerVal = JS_GetPropertyStr(ctx, opts, "layer");
    if (JS_IsString(layerVal)) {
        const char* s = JS_ToCString(ctx, layerVal);
        if (s) { out.layer = world->layerIndex(s); JS_FreeCString(ctx, s); }
    } else if (JS_IsNumber(layerVal)) {
        int32_t i = -1; JS_ToInt32(ctx, &i, layerVal); out.layer = i;
    } else {
        out.layer = -1;
    }
    JS_FreeValue(ctx, layerVal);

    if (out.shape == physics::BodyOptions::ShapeConvexHull) {
        JSValue pts = JS_GetPropertyStr(ctx, opts, "points");
        std::vector<float> flat;
        if (readFloatArray(ctx, pts, flat) && flat.size() >= 12 && (flat.size() % 3) == 0) {
            for (size_t i = 0; i + 2 < flat.size(); i += 3)
                out.hullPoints.push_back(JPH::Vec3(flat[i], flat[i+1], flat[i+2]));
        }
        JS_FreeValue(ctx, pts);
        if (out.hullPoints.size() < 4) { err = "convexHull requires >= 4 points (flat xyz)"; return false; }
    }

    if (out.shape == physics::BodyOptions::ShapeMesh) {
        JSValue posArr = JS_GetPropertyStr(ctx, opts, "positions");
        JSValue idxArr = JS_GetPropertyStr(ctx, opts, "indices");
        std::vector<float> verts;
        std::vector<uint32_t> idx;
        readFloatArray(ctx, posArr, verts);
        readU32Array(ctx, idxArr, idx);
        JS_FreeValue(ctx, posArr);
        JS_FreeValue(ctx, idxArr);
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
        JSValue ptsArr = JS_GetPropertyStr(ctx, opts, "points");
        std::vector<float> pts;
        readFloatArray(ctx, ptsArr, pts);
        JS_FreeValue(ctx, ptsArr);
        if (pts.size() < 4 || (pts.size() % 2) != 0) {
            err = "chain requires points (flat [x0,y0,x1,y1,...]) with at least 2 points";
            return false;
        }
        for (size_t i = 0; i + 1 < pts.size(); i += 2)
            out.chainPoints.push_back(JPH::Float2(pts[i], pts[i+1]));
        out.chainDepth = (float)qjsbind::get_prop_number(ctx, opts, "depth", 20.0);
        out.chainClosed = qjsbind::get_prop_bool(ctx, opts, "closed", false);
        out.chainFlipNormal = qjsbind::get_prop_bool(ctx, opts, "flipNormal", false);
        out.isStatic = true;
    }

    if (out.shape == physics::BodyOptions::ShapeHeightField) {
        JSValue hv = JS_GetPropertyStr(ctx, opts, "heights");
        std::vector<float> heights;
        readFloatArray(ctx, hv, heights);
        JS_FreeValue(ctx, hv);
        uint32_t n = (uint32_t)qjsbind::get_prop_number(ctx, opts, "sampleCount", 0.0);
        if (n == 0 && !heights.empty())
            n = (uint32_t)std::lround(std::sqrt((double)heights.size()));
        if (n < 4 || heights.size() != (size_t)n * n) {
            err = "heightfield requires heights (n*n floats, row-major, n >= 4) with matching sampleCount";
            return false;
        }
        out.heightSamples = std::move(heights);
        out.heightSampleCount = n;
        JSValue sv = JS_GetPropertyStr(ctx, opts, "scale");
        out.heightScale = readVec3(ctx, sv, JPH::Vec3(1, 1, 1));
        JS_FreeValue(ctx, sv);
        JSValue ov = JS_GetPropertyStr(ctx, opts, "offset");
        out.heightOffset = readVec3(ctx, ov);
        JS_FreeValue(ctx, ov);
        out.isStatic = true;
    }

    if (out.shape == physics::BodyOptions::ShapeCompound) {
        JSValue partsArr = JS_GetPropertyStr(ctx, opts, "parts");
        uint32_t nparts = 0;
        if (JS_IsArray(partsArr)) {
            JSValue lenVal = JS_GetPropertyStr(ctx, partsArr, "length");
            JS_ToUint32(ctx, &nparts, lenVal);
            JS_FreeValue(ctx, lenVal);
        }
        for (uint32_t i = 0; i < nparts; i++) {
            JSValue p = JS_GetPropertyUint32(ctx, partsArr, i);
            physics::BodyOptions sub;
            std::string suberr;
            if (!readBodyOptions(ctx, p, world, sub, suberr)) {
                JS_FreeValue(ctx, p);
                JS_FreeValue(ctx, partsArr);
                err = "compound part " + std::to_string(i) + ": " + suberr;
                return false;
            }
            out.compoundParts.push_back(std::move(sub));
            JS_FreeValue(ctx, p);
        }
        JS_FreeValue(ctx, partsArr);
        if (out.compoundParts.empty()) { err = "compound requires at least one part"; return false; }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Generic operations on a JsWorld (used by both default + sandbox).
// ---------------------------------------------------------------------------

static JSValue worldCreateBody(JSContext* ctx, JsWorld* w, JSValueConst optsVal) {
    if (!w || !w->world) return JS_ThrowInternalError(ctx, "World not available");
    if (!JS_IsObject(optsVal)) return JS_ThrowTypeError(ctx, "createBody(opts) requires an object");

    physics::BodyOptions opts;
    std::string err;
    if (!readBodyOptions(ctx, optsVal, w->world, opts, err))
        return JS_ThrowTypeError(ctx, "%s", err.c_str());

    JPH::BodyID id = w->world->createBody(opts);
    if (id.IsInvalid()) return JS_ThrowInternalError(ctx, "Failed to create body");
    return JS_NewInt32(ctx, w->registerBody(id));
}

static JSValue worldDestroyBody(JSContext* ctx, JsWorld* w, int32_t tag) {
    if (!w || !w->world) return JS_UNDEFINED;
    JPH::BodyID id = w->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    w->world->destroyBody(id);
    w->unregisterBody(tag);
    return JS_UNDEFINED;
}

static JSValue worldDestroyAll(JSContext* ctx, JsWorld* w) {
    if (!w || !w->world) return JS_UNDEFINED;
    w->world->destroyAll();
    w->bodyTags.clear();
    w->tagToBody.clear();
    // Don't reset nextTag — keeping monotonic prevents accidental tag reuse
    // confusion across reset cycles.
    return JS_UNDEFINED;
}

static JSValue makeTransformObj(JSContext* ctx, physics::PhysicsWorld* world, JPH::BodyID id) {
    auto pos = world->getPosition(id);
    auto rot = world->getRotation(id);
    uint64_t udata = world->getUserData(id);

    JSValue result = JS_NewObject(ctx);
    JSValue posObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, posObj, "x", JS_NewFloat64(ctx, pos.GetX()));
    JS_SetPropertyStr(ctx, posObj, "y", JS_NewFloat64(ctx, pos.GetY()));
    JS_SetPropertyStr(ctx, posObj, "z", JS_NewFloat64(ctx, pos.GetZ()));
    JS_SetPropertyStr(ctx, result, "position", posObj);
    JSValue rotObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rotObj, "x", JS_NewFloat64(ctx, rot.GetX()));
    JS_SetPropertyStr(ctx, rotObj, "y", JS_NewFloat64(ctx, rot.GetY()));
    JS_SetPropertyStr(ctx, rotObj, "z", JS_NewFloat64(ctx, rot.GetZ()));
    JS_SetPropertyStr(ctx, rotObj, "w", JS_NewFloat64(ctx, rot.GetW()));
    JS_SetPropertyStr(ctx, result, "rotation", rotObj);
    JS_SetPropertyStr(ctx, result, "userData", JS_NewBigUint64(ctx, udata));
    return result;
}

static JSValue worldGetTransform(JSContext* ctx, JsWorld* w, int32_t tag) {
    if (!w || !w->world) return JS_UNDEFINED;
    JPH::BodyID id = w->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    return makeTransformObj(ctx, w->world, id);
}

// --- Forward decls used by the raycast bindings (defined with the shape
// queries below) ---
static void readQueryFilter(JSContext* ctx, JSValueConst opts, JsWorld* w,
                            physics::QueryFilter& out);
static void setVec3Prop(JSContext* ctx, JSValue obj, const char* name,
                        float x, float y, float z);

// raycast(ox, oy, oz, dx, dy, dz, maxDist?, opts?) — the trailing opts object
// carries the shared query filter ({ layers, ignoreBody }); it may also be
// passed directly in place of maxDist.
static void readRaycastArgs(JSContext* ctx, JsWorld* w, int argc, JSValueConst* argv,
                            double& maxDist, physics::QueryFilter& filter) {
    maxDist = 1000.0;
    if (argc >= 7) {
        if (JS_IsObject(argv[6])) readQueryFilter(ctx, argv[6], w, filter);
        else JS_ToFloat64(ctx, &maxDist, argv[6]);
    }
    if (argc >= 8 && JS_IsObject(argv[7])) readQueryFilter(ctx, argv[7], w, filter);
}

static JSValue makeRayHitObj(JSContext* ctx, JsWorld* w, const physics::RayHit& hit) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "bodyId", JS_NewInt32(ctx, w->tagForBodyId(hit.bodyID)));
    JS_SetPropertyStr(ctx, obj, "fraction", JS_NewFloat64(ctx, hit.fraction));
    JS_SetPropertyStr(ctx, obj, "userData",
        JS_NewBigUint64(ctx, hit.bodyID.IsInvalid() ? 0 : w->world->getUserData(hit.bodyID)));
    setVec3Prop(ctx, obj, "position", hit.position.GetX(), hit.position.GetY(), hit.position.GetZ());
    setVec3Prop(ctx, obj, "normal", hit.normal.GetX(), hit.normal.GetY(), hit.normal.GetZ());
    return obj;
}

static JSValue worldRaycast(JSContext* ctx, JsWorld* w, int argc, JSValueConst* argv) {
    if (!w || !w->world || argc < 6) return JS_NewArray(ctx);
    double ox, oy, oz, dx, dy, dz;
    JS_ToFloat64(ctx, &ox, argv[0]); JS_ToFloat64(ctx, &oy, argv[1]); JS_ToFloat64(ctx, &oz, argv[2]);
    JS_ToFloat64(ctx, &dx, argv[3]); JS_ToFloat64(ctx, &dy, argv[4]); JS_ToFloat64(ctx, &dz, argv[5]);
    double maxDist;
    physics::QueryFilter filter;
    readRaycastArgs(ctx, w, argc, argv, maxDist, filter);

    auto hits = w->world->raycast(
        JPH::RVec3((float)ox, (float)oy, (float)oz),
        JPH::Vec3((float)dx, (float)dy, (float)dz),
        (float)maxDist, filter);

    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (auto& hit : hits)
        JS_SetPropertyUint32(ctx, arr, i++, makeRayHitObj(ctx, w, hit));
    return arr;
}

static JSValue worldRaycastClosest(JSContext* ctx, JsWorld* w, int argc, JSValueConst* argv) {
    if (!w || !w->world || argc < 6) return JS_NULL;
    double ox, oy, oz, dx, dy, dz;
    JS_ToFloat64(ctx, &ox, argv[0]); JS_ToFloat64(ctx, &oy, argv[1]); JS_ToFloat64(ctx, &oz, argv[2]);
    JS_ToFloat64(ctx, &dx, argv[3]); JS_ToFloat64(ctx, &dy, argv[4]); JS_ToFloat64(ctx, &dz, argv[5]);
    double maxDist;
    physics::QueryFilter filter;
    readRaycastArgs(ctx, w, argc, argv, maxDist, filter);

    physics::RayHit hit;
    bool ok = w->world->raycastClosest(
        JPH::RVec3((float)ox, (float)oy, (float)oz),
        JPH::Vec3((float)dx, (float)dy, (float)dz),
        hit, (float)maxDist, filter);
    if (!ok) return JS_NULL;
    return makeRayHitObj(ctx, w, hit);
}

// Layer/body filter shared by the narrow-phase query bindings. `layers` is an
// array of layer names or indices selecting which object layers the query can
// see (independent of the collision matrix); `ignoreBody` excludes one tag.
static void readQueryFilter(JSContext* ctx, JSValueConst opts, JsWorld* w,
                            physics::QueryFilter& out) {
    JSValue lv = JS_GetPropertyStr(ctx, opts, "layers");
    if (JS_IsArray(lv)) {
        uint32_t mask = 0;
        JSValue lenV = JS_GetPropertyStr(ctx, lv, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lenV); JS_FreeValue(ctx, lenV);
        for (uint32_t i = 0; i < n; i++) {
            JSValue el = JS_GetPropertyUint32(ctx, lv, i);
            int32_t idx = -1;
            if (JS_IsString(el)) {
                const char* s = JS_ToCString(ctx, el);
                if (s) { idx = w->world->layerIndex(s); JS_FreeCString(ctx, s); }
            } else if (JS_IsNumber(el)) {
                JS_ToInt32(ctx, &idx, el);
            }
            if (idx >= 0 && idx < 32) mask |= 1u << idx;
            JS_FreeValue(ctx, el);
        }
        out.layerMask = mask;
    }
    JS_FreeValue(ctx, lv);
    int32_t ignore = (int32_t)qjsbind::get_prop_number(ctx, opts, "ignoreBody", -1.0);
    if (ignore >= 0) out.ignoreBody = w->bodyIdForTag(ignore);
}

// Query shapes are described like createBody opts (shape kind + dimensions +
// position/rotation) but must be convex.
static bool readQueryShape(JSContext* ctx, JSValueConst opts, JsWorld* w,
                           physics::BodyOptions& shape, std::string& err) {
    if (!readBodyOptions(ctx, opts, w->world, shape, err)) return false;
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

static void setVec3Prop(JSContext* ctx, JSValue obj, const char* name,
                        float x, float y, float z) {
    JSValue v = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, v, "x", JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, v, "y", JS_NewFloat64(ctx, y));
    JS_SetPropertyStr(ctx, v, "z", JS_NewFloat64(ctx, z));
    JS_SetPropertyStr(ctx, obj, name, v);
}

static JSValue makeCastHitObj(JSContext* ctx, JsWorld* w, const physics::ShapeCastHit& hit) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "bodyId", JS_NewInt32(ctx, w->tagForBodyId(hit.bodyID)));
    JS_SetPropertyStr(ctx, obj, "fraction", JS_NewFloat64(ctx, hit.fraction));
    JS_SetPropertyStr(ctx, obj, "userData",
        JS_NewBigUint64(ctx, hit.bodyID.IsInvalid() ? 0 : w->world->getUserData(hit.bodyID)));
    setVec3Prop(ctx, obj, "position", hit.position.GetX(), hit.position.GetY(), hit.position.GetZ());
    setVec3Prop(ctx, obj, "normal", hit.normal.GetX(), hit.normal.GetY(), hit.normal.GetZ());
    return obj;
}

static JSValue worldCastShape(JSContext* ctx, JsWorld* w, JSValueConst optsVal,
                              bool closestOnly) {
    if (!w || !w->world) return closestOnly ? JS_NULL : JS_NewArray(ctx);
    if (!JS_IsObject(optsVal)) return JS_ThrowTypeError(ctx, "castShape(opts) requires an object");

    physics::BodyOptions shape;
    std::string err;
    if (!readQueryShape(ctx, optsVal, w, shape, err))
        return JS_ThrowTypeError(ctx, "%s", err.c_str());
    physics::QueryFilter filter;
    readQueryFilter(ctx, optsVal, w, filter);

    JSValue dv = JS_GetPropertyStr(ctx, optsVal, "direction");
    JPH::Vec3 dir = readVec3(ctx, dv);
    JS_FreeValue(ctx, dv);
    if (dir == JPH::Vec3::sZero())
        return JS_ThrowTypeError(ctx, "castShape requires a non-zero direction");
    double maxDist = qjsbind::get_prop_number(ctx, optsVal, "maxDistance", 1000.0);

    if (closestOnly) {
        physics::ShapeCastHit hit;
        if (!w->world->castShapeClosest(shape, dir, (float)maxDist, hit, filter))
            return JS_NULL;
        return makeCastHitObj(ctx, w, hit);
    }
    auto hits = w->world->castShape(shape, dir, (float)maxDist, filter);
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (auto& hit : hits)
        JS_SetPropertyUint32(ctx, arr, i++, makeCastHitObj(ctx, w, hit));
    return arr;
}

static JSValue worldOverlapShape(JSContext* ctx, JsWorld* w, JSValueConst optsVal) {
    if (!w || !w->world) return JS_NewArray(ctx);
    if (!JS_IsObject(optsVal)) return JS_ThrowTypeError(ctx, "overlapShape(opts) requires an object");

    physics::BodyOptions shape;
    std::string err;
    if (!readQueryShape(ctx, optsVal, w, shape, err))
        return JS_ThrowTypeError(ctx, "%s", err.c_str());
    physics::QueryFilter filter;
    readQueryFilter(ctx, optsVal, w, filter);

    auto hits = w->world->overlapShape(shape, filter);
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (auto& hit : hits) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "bodyId", JS_NewInt32(ctx, w->tagForBodyId(hit.bodyID)));
        JS_SetPropertyStr(ctx, obj, "depth", JS_NewFloat64(ctx, hit.depth));
        JS_SetPropertyStr(ctx, obj, "userData",
            JS_NewBigUint64(ctx, hit.bodyID.IsInvalid() ? 0 : w->world->getUserData(hit.bodyID)));
        setVec3Prop(ctx, obj, "position", hit.position.GetX(), hit.position.GetY(), hit.position.GetZ());
        setVec3Prop(ctx, obj, "normal", hit.normal.GetX(), hit.normal.GetY(), hit.normal.GetZ());
        JS_SetPropertyUint32(ctx, arr, i++, obj);
    }
    return arr;
}

static JSValue worldOverlapPoint(JSContext* ctx, JsWorld* w, int argc, JSValueConst* argv) {
    if (!w || !w->world || argc < 3) return JS_NewArray(ctx);
    double x, y, z;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]); JS_ToFloat64(ctx, &z, argv[2]);
    physics::QueryFilter filter;
    if (argc >= 4 && JS_IsObject(argv[3])) readQueryFilter(ctx, argv[3], w, filter);

    auto bodies = w->world->overlapPoint(JPH::RVec3((float)x, (float)y, (float)z), filter);
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (auto& id : bodies) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "bodyId", JS_NewInt32(ctx, w->tagForBodyId(id)));
        JS_SetPropertyStr(ctx, obj, "userData", JS_NewBigUint64(ctx, w->world->getUserData(id)));
        JS_SetPropertyUint32(ctx, arr, i++, obj);
    }
    return arr;
}

static JSValue worldGetBrokenConstraints(JSContext* ctx, JsWorld* w) {
    if (!w || !w->world) return JS_NewArray(ctx);
    auto broken = w->world->drainBrokenConstraints();
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (uint32_t h : broken)
        JS_SetPropertyUint32(ctx, arr, i++, JS_NewUint32(ctx, h));
    return arr;
}

static JSValue worldGetContacts(JSContext* ctx, JsWorld* w) {
    if (!w || !w->world) return JS_NewArray(ctx);
    auto events = w->world->drainContactEvents();
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (auto& e : events) {
        JSValue obj = JS_NewObject(ctx);
        const char* typeStr = (e.type == physics::ContactEvent::Added) ? "added" : "removed";
        JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, typeStr));
        int32_t t1 = w->tagForBodyId(e.body1);
        int32_t t2 = w->tagForBodyId(e.body2);
        JS_SetPropertyStr(ctx, obj, "body1", JS_NewInt32(ctx, t1));
        JS_SetPropertyStr(ctx, obj, "body2", JS_NewInt32(ctx, t2));
        JS_SetPropertyStr(ctx, obj, "sensor", JS_NewBool(ctx, e.isSensor));
        JS_SetPropertyUint32(ctx, arr, i++, obj);
    }
    return arr;
}

// SixDOF axis names in Jolt EAxis order (tx,ty,tz,rx,ry,rz).
static const char* kSixDofAxisNames[6] = {
    "translationX", "translationY", "translationZ",
    "rotationX", "rotationY", "rotationZ",
};

static int motorAxisIndex(const std::string& name) {
    for (int i = 0; i < 6; i++)
        if (name == kSixDofAxisNames[i]) return i;
    if (name == "tx") return 0;
    if (name == "ty") return 1;
    if (name == "tz") return 2;
    if (name == "rx") return 3;
    if (name == "ry") return 4;
    if (name == "rz") return 5;
    return -1;
}

// Parse a motor options object: { type:'velocity'|'position'|'off', target,
// maxForce?, maxTorque?, frequency?, damping?, axis? } (axis only meaningful
// for sixdof/wheel handles).
static bool readMotorOptions(JSContext* ctx, JSValueConst o,
                             physics::MotorOptions& m, std::string& err) {
    if (!JS_IsObject(o)) { err = "motor options must be an object"; return false; }
    std::string type = qjsbind::get_prop_string(ctx, o, "type");
    if (type == "velocity")      m.state = physics::MotorOptions::Velocity;
    else if (type == "position") m.state = physics::MotorOptions::Position;
    else if (type == "off" || type.empty()) m.state = physics::MotorOptions::Off;
    else { err = "motor type must be 'velocity' | 'position' | 'off'"; return false; }

    m.target    = (float)qjsbind::get_prop_number(ctx, o, "target", 0.0);
    m.maxForce  = (float)qjsbind::get_prop_number(ctx, o, "maxForce", -1.0);
    m.maxTorque = (float)qjsbind::get_prop_number(ctx, o, "maxTorque", -1.0);
    m.frequency = (float)qjsbind::get_prop_number(ctx, o, "frequency", -1.0);
    m.damping   = (float)qjsbind::get_prop_number(ctx, o, "damping", -1.0);

    JSValue av = JS_GetPropertyStr(ctx, o, "axis");
    if (JS_IsString(av)) {
        const char* s = JS_ToCString(ctx, av);
        int idx = s ? motorAxisIndex(s) : -1;
        if (s) JS_FreeCString(ctx, s);
        if (idx < 0) {
            JS_FreeValue(ctx, av);
            err = "motor axis must be translationX..Z / rotationX..Z";
            return false;
        }
        m.axis = idx;
    } else if (JS_IsNumber(av)) {
        int32_t idx = -1; JS_ToInt32(ctx, &idx, av);
        m.axis = idx;
    }
    JS_FreeValue(ctx, av);
    return true;
}

// Parse a sixdof per-axis config value: 'locked' | 'free' |
// { min, max, frequency?, damping?, friction? }. An object without min/max
// is a free axis (useful for friction-only axes).
static bool readSixDofAxis(JSContext* ctx, JSValueConst v,
                           physics::SixDofAxis& a, std::string& err) {
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        std::string mode = s ? s : "";
        if (s) JS_FreeCString(ctx, s);
        if (mode == "locked")    a.mode = physics::SixDofAxis::Locked;
        else if (mode == "free") a.mode = physics::SixDofAxis::Free;
        else { err = "axis mode must be 'locked' | 'free' | {min,max,...}"; return false; }
        return true;
    }
    if (JS_IsObject(v)) {
        JSValue minV = JS_GetPropertyStr(ctx, v, "min");
        JSValue maxV = JS_GetPropertyStr(ctx, v, "max");
        if (JS_IsNumber(minV) && JS_IsNumber(maxV)) {
            double lo = 0, hi = 0;
            JS_ToFloat64(ctx, &lo, minV); JS_ToFloat64(ctx, &hi, maxV);
            a.mode = physics::SixDofAxis::Limited;
            a.min = (float)lo; a.max = (float)hi;
        } else {
            a.mode = physics::SixDofAxis::Free;
        }
        JS_FreeValue(ctx, minV); JS_FreeValue(ctx, maxV);
        a.springFrequency = (float)qjsbind::get_prop_number(ctx, v, "frequency", 0.0);
        a.springDamping   = (float)qjsbind::get_prop_number(ctx, v, "damping", 1.0);
        a.maxFriction     = (float)qjsbind::get_prop_number(ctx, v, "friction", 0.0);
        return true;
    }
    err = "axis config must be 'locked' | 'free' | {min,max,...}";
    return false;
}

static JSValue worldCreateConstraint(JSContext* ctx, JsWorld* w, JSValueConst o) {
    if (!w || !w->world) return JS_ThrowInternalError(ctx, "World not available");
    if (!JS_IsObject(o)) return JS_ThrowTypeError(ctx, "createConstraint(opts) requires an object");

    physics::ConstraintOptions cs;

    std::string type = qjsbind::get_prop_string(ctx, o, "type");
    if (type == "distance") cs.type = physics::ConstraintOptions::Distance;
    else if (type == "point") cs.type = physics::ConstraintOptions::Point;
    else if (type == "hinge") cs.type = physics::ConstraintOptions::Hinge;
    else if (type == "fixed") cs.type = physics::ConstraintOptions::Fixed;
    else if (type == "slider") cs.type = physics::ConstraintOptions::Slider;
    else if (type == "wheel")  cs.type = physics::ConstraintOptions::Wheel;
    else if (type == "cone")   cs.type = physics::ConstraintOptions::Cone;
    else if (type == "swingTwist" || type == "swing-twist") cs.type = physics::ConstraintOptions::SwingTwist;
    else if (type == "pulley") cs.type = physics::ConstraintOptions::Pulley;
    else if (type == "gear")   cs.type = physics::ConstraintOptions::Gear;
    else if (type == "rackAndPinion" || type == "rack-and-pinion") cs.type = physics::ConstraintOptions::RackAndPinion;
    else if (type == "sixdof" || type == "sixDof" || type == "6dof") cs.type = physics::ConstraintOptions::SixDOF;
    else return JS_ThrowTypeError(ctx, "constraint type required (distance|point|hinge|fixed|slider|wheel|cone|swingTwist|pulley|gear|rackAndPinion|sixdof)");

    JSValue b1v = JS_GetPropertyStr(ctx, o, "body1");
    JSValue b2v = JS_GetPropertyStr(ctx, o, "body2");
    int32_t t1 = -1, t2 = -1;
    if (JS_IsNumber(b1v)) JS_ToInt32(ctx, &t1, b1v);
    if (JS_IsNumber(b2v)) JS_ToInt32(ctx, &t2, b2v);
    JS_FreeValue(ctx, b1v);
    JS_FreeValue(ctx, b2v);

    cs.body1 = w->bodyIdForTag(t1);
    cs.body2 = w->bodyIdForTag(t2);
    if (cs.body1.IsInvalid())
        return JS_ThrowTypeError(ctx, "constraint body1 tag is invalid");

    JSValue p1 = JS_GetPropertyStr(ctx, o, "point1");
    cs.point1 = readVec3(ctx, p1); JS_FreeValue(ctx, p1);
    JSValue p2 = JS_GetPropertyStr(ctx, o, "point2");
    cs.point2 = readVec3(ctx, p2); JS_FreeValue(ctx, p2);

    JSValue ax = JS_GetPropertyStr(ctx, o, "axis");
    cs.axis = readVec3(ctx, ax, JPH::Vec3(0, 1, 0)); JS_FreeValue(ctx, ax);

    cs.minDistance = (float)qjsbind::get_prop_number(ctx, o, "minDistance", -1.0);
    cs.maxDistance = (float)qjsbind::get_prop_number(ctx, o, "maxDistance", -1.0);

    JSValue lmin = JS_GetPropertyStr(ctx, o, "limitMin");
    JSValue lmax = JS_GetPropertyStr(ctx, o, "limitMax");
    if (!JS_IsUndefined(lmin) && !JS_IsUndefined(lmax)) {
        double a, b;
        JS_ToFloat64(ctx, &a, lmin); JS_ToFloat64(ctx, &b, lmax);
        cs.limitMin = (float)a; cs.limitMax = (float)b;
        cs.hasLimits = true;
    }
    JS_FreeValue(ctx, lmin); JS_FreeValue(ctx, lmax);

    cs.breakingImpulse = (float)qjsbind::get_prop_number(ctx, o, "breakingImpulse", 0.0);
    cs.collideConnected = qjsbind::get_prop_bool(ctx, o, "collideConnected", false);

    if (cs.type == physics::ConstraintOptions::Wheel) {
        JSValue sa = JS_GetPropertyStr(ctx, o, "suspensionAxis");
        cs.wheelSuspensionAxis = readVec3(ctx, sa, JPH::Vec3(0, 1, 0));
        JS_FreeValue(ctx, sa);
        JSValue ha = JS_GetPropertyStr(ctx, o, "hingeAxis");
        cs.wheelHingeAxis = readVec3(ctx, ha, JPH::Vec3(0, 0, 1));
        JS_FreeValue(ctx, ha);
        cs.wheelHertz = (float)qjsbind::get_prop_number(ctx, o, "hertz", 2.0);
        cs.wheelDampingRatio = (float)qjsbind::get_prop_number(ctx, o, "dampingRatio", 0.7);
        JSValue lo = JS_GetPropertyStr(ctx, o, "lowerTranslation");
        JSValue hi = JS_GetPropertyStr(ctx, o, "upperTranslation");
        if (!JS_IsUndefined(lo) && !JS_IsUndefined(hi)) {
            double a, b;
            JS_ToFloat64(ctx, &a, lo); JS_ToFloat64(ctx, &b, hi);
            cs.wheelLowerTranslation = (float)a;
            cs.wheelUpperTranslation = (float)b;
            cs.wheelHasTranslationLimits = true;
        }
        JS_FreeValue(ctx, lo); JS_FreeValue(ctx, hi);
        cs.wheelEnableMotor = qjsbind::get_prop_bool(ctx, o, "enableMotor", false);
        cs.wheelMotorSpeed = (float)qjsbind::get_prop_number(ctx, o, "motorSpeed", 0.0);
        cs.wheelMaxMotorTorque = (float)qjsbind::get_prop_number(ctx, o, "maxMotorTorque", 0.0);
    }

    if (cs.type == physics::ConstraintOptions::Cone) {
        cs.coneHalfAngle = (float)qjsbind::get_prop_number(ctx, o, "halfConeAngle", 0.0);
    }

    if (cs.type == physics::ConstraintOptions::SwingTwist) {
        JSValue pa = JS_GetPropertyStr(ctx, o, "planeAxis");
        cs.planeAxis = readVec3(ctx, pa, JPH::Vec3(0, 1, 0)); JS_FreeValue(ctx, pa);
        cs.normalHalfConeAngle = (float)qjsbind::get_prop_number(ctx, o, "normalHalfConeAngle", 0.0);
        cs.planeHalfConeAngle = (float)qjsbind::get_prop_number(ctx, o, "planeHalfConeAngle", 0.0);
        cs.twistMinAngle = (float)qjsbind::get_prop_number(ctx, o, "twistMinAngle", 0.0);
        cs.twistMaxAngle = (float)qjsbind::get_prop_number(ctx, o, "twistMaxAngle", 0.0);
        cs.maxFrictionTorque = (float)qjsbind::get_prop_number(ctx, o, "maxFrictionTorque", 0.0);
    }

    if (cs.type == physics::ConstraintOptions::Pulley) {
        JSValue bp1 = JS_GetPropertyStr(ctx, o, "bodyPoint1");
        cs.bodyPoint1 = readVec3(ctx, bp1); JS_FreeValue(ctx, bp1);
        JSValue fp1 = JS_GetPropertyStr(ctx, o, "fixedPoint1");
        cs.fixedPoint1 = readVec3(ctx, fp1); JS_FreeValue(ctx, fp1);
        JSValue bp2 = JS_GetPropertyStr(ctx, o, "bodyPoint2");
        cs.bodyPoint2 = readVec3(ctx, bp2); JS_FreeValue(ctx, bp2);
        JSValue fp2 = JS_GetPropertyStr(ctx, o, "fixedPoint2");
        cs.fixedPoint2 = readVec3(ctx, fp2); JS_FreeValue(ctx, fp2);
        cs.ratio = (float)qjsbind::get_prop_number(ctx, o, "ratio", 1.0);
        cs.minLength = (float)qjsbind::get_prop_number(ctx, o, "minLength", 0.0);
        cs.maxLength = (float)qjsbind::get_prop_number(ctx, o, "maxLength", -1.0);
    }

    if (cs.type == physics::ConstraintOptions::Gear ||
        cs.type == physics::ConstraintOptions::RackAndPinion) {
        JSValue ha1 = JS_GetPropertyStr(ctx, o, "hingeAxis1");
        cs.hingeAxis1 = readVec3(ctx, ha1, JPH::Vec3(1, 0, 0)); JS_FreeValue(ctx, ha1);
        JSValue ha2 = JS_GetPropertyStr(ctx, o, cs.type == physics::ConstraintOptions::Gear ? "hingeAxis2" : "sliderAxis");
        cs.hingeAxis2 = readVec3(ctx, ha2, JPH::Vec3(1, 0, 0)); JS_FreeValue(ctx, ha2);
        cs.ratio = (float)qjsbind::get_prop_number(ctx, o, "ratio", 1.0);
        cs.dependentConstraint1 = (uint32_t)qjsbind::get_prop_number(ctx, o, "constraint1", 0.0);
        cs.dependentConstraint2 = (uint32_t)qjsbind::get_prop_number(ctx, o, "constraint2", 0.0);
        if (!cs.dependentConstraint1 || !cs.dependentConstraint2) {
            const char* m = cs.type == physics::ConstraintOptions::Gear
                ? "gear requires constraint1/constraint2 (two hinge constraint handles)"
                : "rackAndPinion requires constraint1 (pinion hinge) and constraint2 (rack slider) handles";
            return JS_ThrowTypeError(ctx, "%s", m);
        }
    }

    if (cs.type == physics::ConstraintOptions::SixDOF) {
        JSValue axv = JS_GetPropertyStr(ctx, o, "axisX");
        cs.sixDofAxisX = readVec3(ctx, axv, JPH::Vec3(1, 0, 0)); JS_FreeValue(ctx, axv);
        JSValue ayv = JS_GetPropertyStr(ctx, o, "axisY");
        cs.sixDofAxisY = readVec3(ctx, ayv, JPH::Vec3(0, 1, 0)); JS_FreeValue(ctx, ayv);
        std::string swing = qjsbind::get_prop_string(ctx, o, "swingType");
        cs.sixDofSwingPyramid = (swing == "pyramid");

        JSValue axes = JS_GetPropertyStr(ctx, o, "axes");
        if (JS_IsObject(axes)) {
            for (int i = 0; i < 6; i++) {
                JSValue v = JS_GetPropertyStr(ctx, axes, kSixDofAxisNames[i]);
                if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                    std::string err;
                    if (!readSixDofAxis(ctx, v, cs.sixDofAxes[i], err)) {
                        JS_FreeValue(ctx, v);
                        JS_FreeValue(ctx, axes);
                        return JS_ThrowTypeError(ctx, "sixdof axes.%s: %s",
                                                 kSixDofAxisNames[i], err.c_str());
                    }
                }
                JS_FreeValue(ctx, v);
            }
        }
        JS_FreeValue(ctx, axes);

        // Per-axis motors at create: motors: { rotationZ: {...}, ... }
        JSValue motors = JS_GetPropertyStr(ctx, o, "motors");
        if (JS_IsObject(motors)) {
            for (int i = 0; i < 6; i++) {
                JSValue v = JS_GetPropertyStr(ctx, motors, kSixDofAxisNames[i]);
                if (JS_IsObject(v)) {
                    physics::MotorOptions m;
                    std::string err;
                    if (!readMotorOptions(ctx, v, m, err)) {
                        JS_FreeValue(ctx, v);
                        JS_FreeValue(ctx, motors);
                        return JS_ThrowTypeError(ctx, "sixdof motors.%s: %s",
                                                 kSixDofAxisNames[i], err.c_str());
                    }
                    m.axis = i;  // keyed axis wins over any inline "axis" field
                    cs.motors.push_back(m);
                }
                JS_FreeValue(ctx, v);
            }
        }
        JS_FreeValue(ctx, motors);
    }

    // Single motor at create for hinge / slider: motor: { type, target, ... }
    if (cs.type == physics::ConstraintOptions::Hinge ||
        cs.type == physics::ConstraintOptions::Slider) {
        JSValue mv = JS_GetPropertyStr(ctx, o, "motor");
        if (JS_IsObject(mv)) {
            physics::MotorOptions m;
            std::string err;
            if (!readMotorOptions(ctx, mv, m, err)) {
                JS_FreeValue(ctx, mv);
                return JS_ThrowTypeError(ctx, "motor: %s", err.c_str());
            }
            cs.motors.push_back(m);
        }
        JS_FreeValue(ctx, mv);
    }

    uint32_t handle = w->world->createConstraint(cs);
    if (!handle) return JS_ThrowInternalError(ctx, "Failed to create constraint");
    return JS_NewUint32(ctx, handle);
}

// setConstraintMotor(handle, opts) — runtime motor control shared by the
// default world and sandbox handles.
static JSValue worldSetConstraintMotor(JSContext* ctx, JsWorld* w,
                                       JSValueConst hVal, JSValueConst optsVal) {
    if (!w || !w->world) return JS_ThrowInternalError(ctx, "World not available");
    uint32_t h = 0; JS_ToUint32(ctx, &h, hVal);
    physics::MotorOptions m;
    std::string err;
    if (!readMotorOptions(ctx, optsVal, m, err))
        return JS_ThrowTypeError(ctx, "setConstraintMotor: %s", err.c_str());
    return JS_NewBool(ctx, w->world->setConstraintMotor(h, m));
}

// ---------------------------------------------------------------------------
// Character controller (Physics.createCharacter / handle.createCharacter).
// The JS object wraps a PhysicsWorld character handle. For sandbox worlds it
// holds a strong ref to the world handle object so the JsWorld outlives every
// character created from it; for the default world it routes through
// s_defaultWorld (nulled in cleanup), so no lifetime pin is needed.
// ---------------------------------------------------------------------------

static JSClassID s_characterClassId = 0;

struct JsCharacter {
    JsWorld* world = nullptr;          // sandbox only; default world uses s_defaultWorld
    uint32_t handle = 0;               // 0 after destroy()
    JSValue worldRef = JS_UNDEFINED;   // strong ref to a sandbox world handle
};

// Same shape for vehicles (class machinery lives after the character section).
struct JsVehicle {
    JsWorld* world = nullptr;          // sandbox only; default world uses s_defaultWorld
    uint32_t handle = 0;               // 0 after destroy()
    int32_t bodyTag = -1;              // chassis body tag (for .chassisBody)
    JSValue worldRef = JS_UNDEFINED;   // strong ref to a sandbox world handle
};

// GC teardown may finalize the world handle before its characters/vehicles;
// sever the back-pointers first so their finalizers never touch a deleted
// JsWorld (PhysicsWorld::shutdown destroys the underlying objects itself).
JsWorld::~JsWorld() {
    for (JsCharacter* c : liveCharacters) {
        c->world = nullptr;
        c->handle = 0;
    }
    for (JsVehicle* v : liveVehicles) {
        v->world = nullptr;
        v->handle = 0;
    }
    if (ownsWorld && world) {
        world->shutdown();
        delete world;
    }
    world = nullptr;
}

static JsWorld* characterWorld(JsCharacter* c) {
    return JS_IsUndefined(c->worldRef) ? s_defaultWorld : c->world;
}

static void characterClassFinalizer(JSRuntime* rt, JSValue val) {
    JsCharacter* c = (JsCharacter*)JS_GetOpaque(val, s_characterClassId);
    if (!c) return;
    JsWorld* w = characterWorld(c);
    if (w && w->world && c->handle) w->world->destroyCharacter(c->handle);
    if (c->world) c->world->liveCharacters.erase(c);
    JS_FreeValueRT(rt, c->worldRef);
    delete c;
}

// worldRef lives in opaque data, invisible to the GC — without this mark hook
// the teardown cycle collector counts it as an external root and leaks the
// world handle (QuickJS Debug asserts on gc_obj_list).
static void characterClassGcMark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func) {
    JsCharacter* c = (JsCharacter*)JS_GetOpaque(val, s_characterClassId);
    if (c) JS_MarkValue(rt, c->worldRef, mark_func);
}

static JSClassDef s_characterClassDef = {
    "PhysicsCharacter",
    characterClassFinalizer,  // finalizer
    characterClassGcMark,     // gc_mark
    nullptr,                  // call
    nullptr,                  // exotic
};

static JsCharacter* characterFromThis(JSContext* ctx, JSValueConst thisVal) {
    return (JsCharacter*)JS_GetOpaque2(ctx, thisVal, s_characterClassId);
}

static JSValue jsc_setVelocity(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsCharacter* c = characterFromThis(ctx, thisVal);
    if (!c) return JS_EXCEPTION;
    JsWorld* w = characterWorld(c);
    if (!w || !w->world || !c->handle || argc < 3) return JS_UNDEFINED;
    double x, y, z;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]); JS_ToFloat64(ctx, &z, argv[2]);
    w->world->setCharacterVelocity(c->handle, JPH::Vec3((float)x, (float)y, (float)z));
    return JS_UNDEFINED;
}

static JSValue jsc_setPosition(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsCharacter* c = characterFromThis(ctx, thisVal);
    if (!c) return JS_EXCEPTION;
    JsWorld* w = characterWorld(c);
    if (!w || !w->world || !c->handle || argc < 3) return JS_UNDEFINED;
    double x, y, z;
    JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]); JS_ToFloat64(ctx, &z, argv[2]);
    w->world->setCharacterPosition(c->handle, JPH::RVec3((float)x, (float)y, (float)z));
    return JS_UNDEFINED;
}

static JSValue jsc_getState(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    JsCharacter* c = characterFromThis(ctx, thisVal);
    if (!c) return JS_EXCEPTION;
    JsWorld* w = characterWorld(c);
    if (!w || !w->world || !c->handle) return JS_NULL;
    physics::CharacterState st;
    if (!w->world->getCharacterState(c->handle, st)) return JS_NULL;

    JSValue obj = JS_NewObject(ctx);
    setVec3Prop(ctx, obj, "position", st.position.GetX(), st.position.GetY(), st.position.GetZ());
    setVec3Prop(ctx, obj, "velocity", st.velocity.GetX(), st.velocity.GetY(), st.velocity.GetZ());
    const char* ground = "inAir";
    switch (st.ground) {
        case physics::CharacterGround::OnGround:      ground = "onGround"; break;
        case physics::CharacterGround::OnSteepGround: ground = "onSteepGround"; break;
        case physics::CharacterGround::NotSupported:  ground = "notSupported"; break;
        case physics::CharacterGround::InAir:         ground = "inAir"; break;
    }
    JS_SetPropertyStr(ctx, obj, "groundState", JS_NewString(ctx, ground));
    JS_SetPropertyStr(ctx, obj, "isGrounded",
        JS_NewBool(ctx, st.ground == physics::CharacterGround::OnGround));
    setVec3Prop(ctx, obj, "groundNormal",
        st.groundNormal.GetX(), st.groundNormal.GetY(), st.groundNormal.GetZ());
    setVec3Prop(ctx, obj, "groundVelocity",
        st.groundVelocity.GetX(), st.groundVelocity.GetY(), st.groundVelocity.GetZ());
    JS_SetPropertyStr(ctx, obj, "groundBodyId",
        JS_NewInt32(ctx, st.groundBody.IsInvalid() ? -1 : w->tagForBodyId(st.groundBody)));
    return obj;
}

static JSValue jsc_getPosition(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    JsCharacter* c = characterFromThis(ctx, thisVal);
    if (!c) return JS_EXCEPTION;
    JsWorld* w = characterWorld(c);
    if (!w || !w->world || !c->handle) return JS_NULL;
    physics::CharacterState st;
    if (!w->world->getCharacterState(c->handle, st)) return JS_NULL;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, st.position.GetX()));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, st.position.GetY()));
    JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, st.position.GetZ()));
    return obj;
}

static JSValue jsc_getVelocity(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    JsCharacter* c = characterFromThis(ctx, thisVal);
    if (!c) return JS_EXCEPTION;
    JsWorld* w = characterWorld(c);
    if (!w || !w->world || !c->handle) return JS_NULL;
    physics::CharacterState st;
    if (!w->world->getCharacterState(c->handle, st)) return JS_NULL;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, st.velocity.GetX()));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, st.velocity.GetY()));
    JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, st.velocity.GetZ()));
    return obj;
}

static JSValue jsc_destroy(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    JsCharacter* c = characterFromThis(ctx, thisVal);
    if (!c) return JS_EXCEPTION;
    JsWorld* w = characterWorld(c);
    if (w && w->world && c->handle) w->world->destroyCharacter(c->handle);
    c->handle = 0;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry s_characterProtoFuncs[] = {
    JS_CFUNC_DEF("setVelocity", 3, jsc_setVelocity),
    JS_CFUNC_DEF("getVelocity", 0, jsc_getVelocity),
    JS_CFUNC_DEF("setPosition", 3, jsc_setPosition),
    JS_CFUNC_DEF("getPosition", 0, jsc_getPosition),
    JS_CFUNC_DEF("getState", 0, jsc_getState),
    JS_CFUNC_DEF("destroy", 0, jsc_destroy),
};

// `worldVal` is the sandbox world handle object, or JS_UNDEFINED for the
// default world.
static JSValue worldCreateCharacter(JSContext* ctx, JsWorld* w, JSValueConst worldVal,
                                    JSValueConst optsVal) {
    if (!w || !w->world) return JS_ThrowInternalError(ctx, "World not available");
    if (!JS_IsObject(optsVal)) return JS_ThrowTypeError(ctx, "createCharacter(opts) requires an object");

    physics::CharacterOptions opts;
    JSValue posVal = JS_GetPropertyStr(ctx, optsVal, "position");
    opts.position = readVec3(ctx, posVal);
    JS_FreeValue(ctx, posVal);
    JSValue upVal = JS_GetPropertyStr(ctx, optsVal, "up");
    opts.up = readVec3(ctx, upVal, JPH::Vec3(0, 1, 0));
    JS_FreeValue(ctx, upVal);
    opts.radius = (float)qjsbind::get_prop_number(ctx, optsVal, "radius", 0.3);
    opts.halfHeight = (float)qjsbind::get_prop_number(ctx, optsVal, "halfHeight", 0.6);
    opts.mass = (float)qjsbind::get_prop_number(ctx, optsVal, "mass", 70.0);
    opts.maxSlopeAngle = (float)qjsbind::get_prop_number(ctx, optsVal, "maxSlopeAngle", 50.0);
    opts.maxStrength = (float)qjsbind::get_prop_number(ctx, optsVal, "maxStrength", 100.0);
    opts.padding = (float)qjsbind::get_prop_number(ctx, optsVal, "padding", 0.02);
    opts.stepUp = (float)qjsbind::get_prop_number(ctx, optsVal, "stepUp", 0.4);
    opts.stickToFloor = (float)qjsbind::get_prop_number(ctx, optsVal, "stickToFloor", 0.5);

    JSValue layerVal = JS_GetPropertyStr(ctx, optsVal, "layer");
    if (JS_IsString(layerVal)) {
        const char* s = JS_ToCString(ctx, layerVal);
        if (s) { opts.layer = w->world->layerIndex(s); JS_FreeCString(ctx, s); }
    } else if (JS_IsNumber(layerVal)) {
        int32_t i = -1; JS_ToInt32(ctx, &i, layerVal); opts.layer = i;
    }
    JS_FreeValue(ctx, layerVal);

    uint32_t handle = w->world->createCharacter(opts);
    if (!handle) return JS_ThrowInternalError(ctx, "Failed to create character");

    JSValue obj = JS_NewObjectClass(ctx, s_characterClassId);
    if (JS_IsException(obj)) {
        w->world->destroyCharacter(handle);
        return obj;
    }
    auto* jc = new JsCharacter();
    jc->handle = handle;
    if (!JS_IsUndefined(worldVal)) {
        jc->world = w;
        jc->worldRef = JS_DupValue(ctx, worldVal);
        w->liveCharacters.insert(jc);
    }
    JS_SetOpaque(obj, jc);
    return obj;
}

// ---------------------------------------------------------------------------
// Wheeled vehicles (Physics.createVehicle / handle.createVehicle). Same
// lifetime pattern as PhysicsCharacter above: sandbox handles pin their world
// handle object via worldRef (marked in vehicleClassGcMark, severed by
// ~JsWorld for arbitrary finalizer order); default-world handles route
// through s_defaultWorld.
// ---------------------------------------------------------------------------

static JSClassID s_vehicleClassId = 0;

static JsWorld* vehicleWorld(JsVehicle* v) {
    return JS_IsUndefined(v->worldRef) ? s_defaultWorld : v->world;
}

static void vehicleClassFinalizer(JSRuntime* rt, JSValue val) {
    JsVehicle* v = (JsVehicle*)JS_GetOpaque(val, s_vehicleClassId);
    if (!v) return;
    JsWorld* w = vehicleWorld(v);
    if (w && w->world && v->handle) w->world->destroyVehicle(v->handle);
    if (v->world) v->world->liveVehicles.erase(v);
    JS_FreeValueRT(rt, v->worldRef);
    delete v;
}

// worldRef lives in opaque data, invisible to the GC — without this mark hook
// the teardown cycle collector counts it as an external root and leaks the
// world handle (QuickJS Debug asserts on gc_obj_list).
static void vehicleClassGcMark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func) {
    JsVehicle* v = (JsVehicle*)JS_GetOpaque(val, s_vehicleClassId);
    if (v) JS_MarkValue(rt, v->worldRef, mark_func);
}

static JSClassDef s_vehicleClassDef = {
    "PhysicsVehicle",
    vehicleClassFinalizer,  // finalizer
    vehicleClassGcMark,     // gc_mark
    nullptr,                // call
    nullptr,                // exotic
};

static JsVehicle* vehicleFromThis(JSContext* ctx, JSValueConst thisVal) {
    return (JsVehicle*)JS_GetOpaque2(ctx, thisVal, s_vehicleClassId);
}

// Parse one wheel entry of the `wheels` array.
static void readVehicleWheel(JSContext* ctx, JSValueConst o,
                             physics::VehicleWheelOptions& w) {
    JSValue pos = JS_GetPropertyStr(ctx, o, "position");
    w.position = readVec3(ctx, pos);
    JS_FreeValue(ctx, pos);
    JSValue sd = JS_GetPropertyStr(ctx, o, "suspensionDirection");
    w.suspensionDirection = readVec3(ctx, sd, JPH::Vec3(0, -1, 0));
    JS_FreeValue(ctx, sd);
    w.radius = (float)qjsbind::get_prop_number(ctx, o, "radius", 0.3);
    w.width = (float)qjsbind::get_prop_number(ctx, o, "width", 0.1);
    w.suspensionMinLength = (float)qjsbind::get_prop_number(ctx, o, "suspensionMinLength", 0.3);
    w.suspensionMaxLength = (float)qjsbind::get_prop_number(ctx, o, "suspensionMaxLength", 0.5);
    w.suspensionFrequency = (float)qjsbind::get_prop_number(ctx, o, "suspensionFrequency", 1.5);
    w.suspensionDamping = (float)qjsbind::get_prop_number(ctx, o, "suspensionDamping", 0.5);
    w.steerable = qjsbind::get_prop_bool(ctx, o, "steerable", false);
    w.maxSteerAngle = (float)qjsbind::get_prop_number(ctx, o, "maxSteerAngle", 70.0);
    w.driven = qjsbind::get_prop_bool(ctx, o, "driven", false);
    w.maxBrakeTorque = (float)qjsbind::get_prop_number(ctx, o, "maxBrakeTorque", 1500.0);
    w.maxHandBrakeTorque = (float)qjsbind::get_prop_number(ctx, o, "maxHandBrakeTorque", 0.0);
}

// `worldVal` is the sandbox world handle object, or JS_UNDEFINED for the
// default world.
static JSValue worldCreateVehicle(JSContext* ctx, JsWorld* w, JSValueConst worldVal,
                                  JSValueConst optsVal) {
    if (!w || !w->world) return JS_ThrowInternalError(ctx, "World not available");
    if (!JS_IsObject(optsVal)) return JS_ThrowTypeError(ctx, "createVehicle(opts) requires an object");

    physics::VehicleOptions opts;

    // Chassis: an existing body tag (`body`) or inline creation opts
    // (`chassis`, same schema as createBody, forced dynamic).
    int32_t chassisTag = -1;
    bool createdChassis = false;
    JSValue bodyVal = JS_GetPropertyStr(ctx, optsVal, "body");
    if (JS_IsNumber(bodyVal)) {
        JS_ToInt32(ctx, &chassisTag, bodyVal);
    }
    JS_FreeValue(ctx, bodyVal);
    if (chassisTag < 0) {
        JSValue chassisVal = JS_GetPropertyStr(ctx, optsVal, "chassis");
        if (JS_IsObject(chassisVal)) {
            physics::BodyOptions bodyOpts;
            std::string err;
            if (!readBodyOptions(ctx, chassisVal, w->world, bodyOpts, err)) {
                JS_FreeValue(ctx, chassisVal);
                return JS_ThrowTypeError(ctx, "chassis: %s", err.c_str());
            }
            JS_FreeValue(ctx, chassisVal);
            bodyOpts.isStatic = false;
            JPH::BodyID id = w->world->createBody(bodyOpts);
            if (id.IsInvalid())
                return JS_ThrowInternalError(ctx, "Failed to create chassis body");
            chassisTag = w->registerBody(id);
            createdChassis = true;
        } else {
            JS_FreeValue(ctx, chassisVal);
            return JS_ThrowTypeError(ctx, "createVehicle requires body (tag) or chassis (createBody opts)");
        }
    }
    opts.body = w->bodyIdForTag(chassisTag);
    if (opts.body.IsInvalid())
        return JS_ThrowTypeError(ctx, "createVehicle: chassis body tag is invalid");

    JSValue upVal = JS_GetPropertyStr(ctx, optsVal, "up");
    opts.up = readVec3(ctx, upVal, JPH::Vec3(0, 1, 0));
    JS_FreeValue(ctx, upVal);
    JSValue fwdVal = JS_GetPropertyStr(ctx, optsVal, "forward");
    opts.forward = readVec3(ctx, fwdVal, JPH::Vec3(0, 0, 1));
    JS_FreeValue(ctx, fwdVal);
    opts.maxPitchRollAngle =
        (float)qjsbind::get_prop_number(ctx, optsVal, "maxPitchRollAngle", 180.0);

    // Cleanup for the error paths below: only tear down what we created.
    auto fail = [&](JSValue exception) {
        if (createdChassis) {
            w->world->destroyBody(w->bodyIdForTag(chassisTag));
            w->unregisterBody(chassisTag);
        }
        return exception;
    };

    JSValue wheelsVal = JS_GetPropertyStr(ctx, optsVal, "wheels");
    uint32_t nWheels = 0;
    if (JS_IsArray(wheelsVal)) {
        JSValue lenV = JS_GetPropertyStr(ctx, wheelsVal, "length");
        JS_ToUint32(ctx, &nWheels, lenV);
        JS_FreeValue(ctx, lenV);
    }
    for (uint32_t i = 0; i < nWheels; i++) {
        JSValue wv = JS_GetPropertyUint32(ctx, wheelsVal, i);
        physics::VehicleWheelOptions wheel;
        if (JS_IsObject(wv)) readVehicleWheel(ctx, wv, wheel);
        JS_FreeValue(ctx, wv);
        opts.wheels.push_back(wheel);
    }
    JS_FreeValue(ctx, wheelsVal);
    if (opts.wheels.empty())
        return fail(JS_ThrowTypeError(ctx, "createVehicle requires a non-empty wheels array"));

    JSValue engVal = JS_GetPropertyStr(ctx, optsVal, "engine");
    if (JS_IsObject(engVal)) {
        opts.engine.maxTorque = (float)qjsbind::get_prop_number(ctx, engVal, "maxTorque", 500.0);
        opts.engine.minRPM = (float)qjsbind::get_prop_number(ctx, engVal, "minRPM", 1000.0);
        opts.engine.maxRPM = (float)qjsbind::get_prop_number(ctx, engVal, "maxRPM", 6000.0);
    }
    JS_FreeValue(ctx, engVal);

    JSValue trVal = JS_GetPropertyStr(ctx, optsVal, "transmission");
    if (JS_IsObject(trVal)) {
        std::string mode = qjsbind::get_prop_string(ctx, trVal, "mode");
        opts.transmission.manual = (mode == "manual");
        JSValue gr = JS_GetPropertyStr(ctx, trVal, "gearRatios");
        readFloatArray(ctx, gr, opts.transmission.gearRatios);
        JS_FreeValue(ctx, gr);
        JSValue rgr = JS_GetPropertyStr(ctx, trVal, "reverseGearRatios");
        readFloatArray(ctx, rgr, opts.transmission.reverseGearRatios);
        JS_FreeValue(ctx, rgr);
        opts.transmission.switchTime = (float)qjsbind::get_prop_number(ctx, trVal, "switchTime", 0.5);
        opts.transmission.clutchStrength = (float)qjsbind::get_prop_number(ctx, trVal, "clutchStrength", 10.0);
        opts.transmission.shiftUpRPM = (float)qjsbind::get_prop_number(ctx, trVal, "shiftUpRPM", 4000.0);
        opts.transmission.shiftDownRPM = (float)qjsbind::get_prop_number(ctx, trVal, "shiftDownRPM", 2000.0);
    }
    JS_FreeValue(ctx, trVal);

    JSValue diffsVal = JS_GetPropertyStr(ctx, optsVal, "differentials");
    if (JS_IsArray(diffsVal)) {
        JSValue lenV = JS_GetPropertyStr(ctx, diffsVal, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lenV); JS_FreeValue(ctx, lenV);
        for (uint32_t i = 0; i < n; i++) {
            JSValue dv = JS_GetPropertyUint32(ctx, diffsVal, i);
            physics::VehicleDifferentialOptions d;
            if (JS_IsObject(dv)) {
                d.leftWheel = (int)qjsbind::get_prop_number(ctx, dv, "leftWheel", -1.0);
                d.rightWheel = (int)qjsbind::get_prop_number(ctx, dv, "rightWheel", -1.0);
                d.ratio = (float)qjsbind::get_prop_number(ctx, dv, "ratio", 3.42);
                d.leftRightSplit = (float)qjsbind::get_prop_number(ctx, dv, "leftRightSplit", 0.5);
                d.limitedSlipRatio = (float)qjsbind::get_prop_number(ctx, dv, "limitedSlipRatio", 1.4);
                d.engineTorqueRatio = (float)qjsbind::get_prop_number(ctx, dv, "engineTorqueRatio", 1.0);
            }
            JS_FreeValue(ctx, dv);
            opts.differentials.push_back(d);
        }
    }
    JS_FreeValue(ctx, diffsVal);
    opts.differentialLimitedSlipRatio =
        (float)qjsbind::get_prop_number(ctx, optsVal, "differentialLimitedSlipRatio", 1.4);

    JSValue barsVal = JS_GetPropertyStr(ctx, optsVal, "antiRollBars");
    if (JS_IsArray(barsVal)) {
        JSValue lenV = JS_GetPropertyStr(ctx, barsVal, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lenV); JS_FreeValue(ctx, lenV);
        for (uint32_t i = 0; i < n; i++) {
            JSValue bv = JS_GetPropertyUint32(ctx, barsVal, i);
            physics::VehicleAntiRollBarOptions bar;
            if (JS_IsObject(bv)) {
                bar.leftWheel = (int)qjsbind::get_prop_number(ctx, bv, "leftWheel", 0.0);
                bar.rightWheel = (int)qjsbind::get_prop_number(ctx, bv, "rightWheel", 1.0);
                bar.stiffness = (float)qjsbind::get_prop_number(ctx, bv, "stiffness", 1000.0);
            }
            JS_FreeValue(ctx, bv);
            opts.antiRollBars.push_back(bar);
        }
    }
    JS_FreeValue(ctx, barsVal);

    std::string tester = qjsbind::get_prop_string(ctx, optsVal, "collisionTester");
    if (tester == "ray")         opts.tester = physics::VehicleOptions::TesterRay;
    else if (tester == "sphere") opts.tester = physics::VehicleOptions::TesterCastSphere;
    else if (tester == "cylinder" || tester.empty())
        opts.tester = physics::VehicleOptions::TesterCastCylinder;
    else
        return fail(JS_ThrowTypeError(ctx, "collisionTester must be 'ray' | 'sphere' | 'cylinder'"));

    JSValue tlVal = JS_GetPropertyStr(ctx, optsVal, "testerLayer");
    if (JS_IsString(tlVal)) {
        const char* s = JS_ToCString(ctx, tlVal);
        if (s) { opts.testerLayer = w->world->layerIndex(s); JS_FreeCString(ctx, s); }
    } else if (JS_IsNumber(tlVal)) {
        int32_t i = -1; JS_ToInt32(ctx, &i, tlVal); opts.testerLayer = i;
    }
    JS_FreeValue(ctx, tlVal);

    uint32_t handle = w->world->createVehicle(opts);
    if (!handle)
        return fail(JS_ThrowInternalError(ctx, "Failed to create vehicle"));

    JSValue obj = JS_NewObjectClass(ctx, s_vehicleClassId);
    if (JS_IsException(obj)) {
        w->world->destroyVehicle(handle);
        return fail(obj);
    }
    auto* jv = new JsVehicle();
    jv->handle = handle;
    jv->bodyTag = chassisTag;
    if (!JS_IsUndefined(worldVal)) {
        jv->world = w;
        jv->worldRef = JS_DupValue(ctx, worldVal);
        w->liveVehicles.insert(jv);
    }
    JS_SetOpaque(obj, jv);
    return obj;
}

static JSValue jsv_setInput(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsVehicle* v = vehicleFromThis(ctx, thisVal);
    if (!v) return JS_EXCEPTION;
    JsWorld* w = vehicleWorld(v);
    if (!w || !w->world || !v->handle || argc < 1 || !JS_IsObject(argv[0]))
        return JS_UNDEFINED;
    double fwd = qjsbind::get_prop_number(ctx, argv[0], "forward", 0.0);
    double right = qjsbind::get_prop_number(ctx, argv[0], "right", 0.0);
    double brake = qjsbind::get_prop_number(ctx, argv[0], "brake", 0.0);
    double handBrake = qjsbind::get_prop_number(ctx, argv[0], "handBrake", 0.0);
    w->world->setVehicleInput(v->handle, (float)fwd, (float)right,
                              (float)brake, (float)handBrake);
    return JS_UNDEFINED;
}

static JSValue jsv_setGear(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsVehicle* v = vehicleFromThis(ctx, thisVal);
    if (!v) return JS_EXCEPTION;
    JsWorld* w = vehicleWorld(v);
    if (!w || !w->world || !v->handle || argc < 1) return JS_UNDEFINED;
    int32_t gear = 0; JS_ToInt32(ctx, &gear, argv[0]);
    double clutch = 1.0;
    if (argc >= 2) JS_ToFloat64(ctx, &clutch, argv[1]);
    w->world->setVehicleGear(v->handle, gear, (float)clutch);
    return JS_UNDEFINED;
}

static JSValue jsv_wheelState(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsVehicle* v = vehicleFromThis(ctx, thisVal);
    if (!v) return JS_EXCEPTION;
    JsWorld* w = vehicleWorld(v);
    if (!w || !w->world || !v->handle || argc < 1) return JS_NULL;
    int32_t idx = -1; JS_ToInt32(ctx, &idx, argv[0]);
    physics::VehicleWheelState st;
    if (!w->world->getVehicleWheelState(v->handle, idx, st)) return JS_NULL;

    JSValue obj = JS_NewObject(ctx);
    setVec3Prop(ctx, obj, "position", st.position.GetX(), st.position.GetY(), st.position.GetZ());
    JSValue rot = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rot, "x", JS_NewFloat64(ctx, st.rotation.GetX()));
    JS_SetPropertyStr(ctx, rot, "y", JS_NewFloat64(ctx, st.rotation.GetY()));
    JS_SetPropertyStr(ctx, rot, "z", JS_NewFloat64(ctx, st.rotation.GetZ()));
    JS_SetPropertyStr(ctx, rot, "w", JS_NewFloat64(ctx, st.rotation.GetW()));
    JS_SetPropertyStr(ctx, obj, "rotation", rot);
    JS_SetPropertyStr(ctx, obj, "suspensionLength", JS_NewFloat64(ctx, st.suspensionLength));
    JS_SetPropertyStr(ctx, obj, "steerAngle", JS_NewFloat64(ctx, st.steerAngle));
    JS_SetPropertyStr(ctx, obj, "rotationAngle", JS_NewFloat64(ctx, st.rotationAngle));
    JS_SetPropertyStr(ctx, obj, "angularVelocity", JS_NewFloat64(ctx, st.angularVelocity));
    JS_SetPropertyStr(ctx, obj, "contact", JS_NewBool(ctx, st.contact));
    JS_SetPropertyStr(ctx, obj, "contactBody",
        JS_NewInt32(ctx, st.contactBody.IsInvalid() ? -1 : w->tagForBodyId(st.contactBody)));
    setVec3Prop(ctx, obj, "contactNormal",
        st.contactNormal.GetX(), st.contactNormal.GetY(), st.contactNormal.GetZ());
    return obj;
}

static JSValue jsv_getState(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    JsVehicle* v = vehicleFromThis(ctx, thisVal);
    if (!v) return JS_EXCEPTION;
    JsWorld* w = vehicleWorld(v);
    if (!w || !w->world || !v->handle) return JS_NULL;
    physics::VehicleState st;
    if (!w->world->getVehicleState(v->handle, st)) return JS_NULL;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "speed", JS_NewFloat64(ctx, st.speed));
    JS_SetPropertyStr(ctx, obj, "rpm", JS_NewFloat64(ctx, st.rpm));
    JS_SetPropertyStr(ctx, obj, "gear", JS_NewInt32(ctx, st.gear));
    JS_SetPropertyStr(ctx, obj, "isSwitchingGear", JS_NewBool(ctx, st.isSwitchingGear));
    return obj;
}

static JSValue jsv_getWheelCount(JSContext* ctx, JSValueConst thisVal) {
    JsVehicle* v = vehicleFromThis(ctx, thisVal);
    if (!v) return JS_EXCEPTION;
    JsWorld* w = vehicleWorld(v);
    if (!w || !w->world || !v->handle) return JS_NewInt32(ctx, 0);
    int n = w->world->vehicleWheelCount(v->handle);
    return JS_NewInt32(ctx, n < 0 ? 0 : n);
}

static JSValue jsv_getChassisBody(JSContext* ctx, JSValueConst thisVal) {
    JsVehicle* v = vehicleFromThis(ctx, thisVal);
    if (!v) return JS_EXCEPTION;
    return JS_NewInt32(ctx, v->bodyTag);
}

static JSValue jsv_getSpeed(JSContext* ctx, JSValueConst thisVal) {
    JsVehicle* v = vehicleFromThis(ctx, thisVal);
    if (!v) return JS_EXCEPTION;
    JsWorld* w = vehicleWorld(v);
    physics::VehicleState st;
    if (!w || !w->world || !v->handle || !w->world->getVehicleState(v->handle, st))
        return JS_NewFloat64(ctx, 0);
    return JS_NewFloat64(ctx, st.speed);
}

static JSValue jsv_getRpm(JSContext* ctx, JSValueConst thisVal) {
    JsVehicle* v = vehicleFromThis(ctx, thisVal);
    if (!v) return JS_EXCEPTION;
    JsWorld* w = vehicleWorld(v);
    physics::VehicleState st;
    if (!w || !w->world || !v->handle || !w->world->getVehicleState(v->handle, st))
        return JS_NewFloat64(ctx, 0);
    return JS_NewFloat64(ctx, st.rpm);
}

static JSValue jsv_getGear(JSContext* ctx, JSValueConst thisVal) {
    JsVehicle* v = vehicleFromThis(ctx, thisVal);
    if (!v) return JS_EXCEPTION;
    JsWorld* w = vehicleWorld(v);
    physics::VehicleState st;
    if (!w || !w->world || !v->handle || !w->world->getVehicleState(v->handle, st))
        return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, st.gear);
}

static JSValue jsv_destroy(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    JsVehicle* v = vehicleFromThis(ctx, thisVal);
    if (!v) return JS_EXCEPTION;
    JsWorld* w = vehicleWorld(v);
    if (w && w->world && v->handle) w->world->destroyVehicle(v->handle);
    v->handle = 0;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry s_vehicleProtoFuncs[] = {
    JS_CFUNC_DEF("setInput", 1, jsv_setInput),
    JS_CFUNC_DEF("setGear", 2, jsv_setGear),
    JS_CFUNC_DEF("wheelState", 1, jsv_wheelState),
    JS_CFUNC_DEF("getState", 0, jsv_getState),
    JS_CFUNC_DEF("destroy", 0, jsv_destroy),
    JS_CGETSET_DEF("wheelCount", jsv_getWheelCount, nullptr),
    JS_CGETSET_DEF("chassisBody", jsv_getChassisBody, nullptr),
    JS_CGETSET_DEF("speed", jsv_getSpeed, nullptr),
    JS_CGETSET_DEF("rpm", jsv_getRpm, nullptr),
    JS_CGETSET_DEF("gear", jsv_getGear, nullptr),
};

// ---------------------------------------------------------------------------
// Default-world Physics.* functions (route to s_defaultWorld)
// ---------------------------------------------------------------------------

#define DEFW_GUARD() \
    if (!s_defaultWorld || !s_defaultWorld->world) return JS_ThrowInternalError(ctx, "Physics not available")

static JSValue js_physics_createWorld(JSContext* ctx, JSValueConst, int /*argc*/, JSValueConst* /*argv*/) {
    if (!s_defaultWorld) return JS_ThrowInternalError(ctx, "Physics not available");
    return JS_TRUE;
}

static JSValue js_physics_setGravity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    double x = 0, y = -9.81, z = 0;
    if (argc >= 1) JS_ToFloat64(ctx, &x, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &y, argv[1]);
    if (argc >= 3) JS_ToFloat64(ctx, &z, argv[2]);
    s_defaultWorld->world->setGravity((float)x, (float)y, (float)z);
    return JS_UNDEFINED;
}

static JSValue js_physics_getGravity(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    DEFW_GUARD();
    auto g = s_defaultWorld->world->gravity();
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, g.GetX()));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, g.GetY()));
    JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, g.GetZ()));
    return obj;
}

static JSValue js_physics_setLayers(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "Physics.setLayers({names, matrix}) requires an object");
    JSValue namesVal = JS_GetPropertyStr(ctx, argv[0], "names");
    JSValue matrixVal = JS_GetPropertyStr(ctx, argv[0], "matrix");

    std::vector<std::string> names;
    if (JS_IsArray(namesVal)) {
        JSValue lenV = JS_GetPropertyStr(ctx, namesVal, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lenV); JS_FreeValue(ctx, lenV);
        for (uint32_t i = 0; i < n; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, namesVal, i);
            const char* s = JS_ToCString(ctx, v);
            if (s) { names.emplace_back(s); JS_FreeCString(ctx, s); }
            JS_FreeValue(ctx, v);
        }
    }

    std::vector<bool> matrix;
    if (JS_IsArray(matrixVal)) {
        JSValue lenV = JS_GetPropertyStr(ctx, matrixVal, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lenV); JS_FreeValue(ctx, lenV);
        for (uint32_t i = 0; i < n; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, matrixVal, i);
            matrix.push_back(JS_ToBool(ctx, v) != 0);
            JS_FreeValue(ctx, v);
        }
    }
    JS_FreeValue(ctx, namesVal);
    JS_FreeValue(ctx, matrixVal);

    bool ok = s_defaultWorld->world->configureLayers(names, matrix);
    return JS_NewBool(ctx, ok);
}

static JSValue js_physics_createBody(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1) return JS_ThrowTypeError(ctx, "Physics.createBody(opts) requires an object");
    return worldCreateBody(ctx, s_defaultWorld, argv[0]);
}

static JSValue js_physics_destroyBody(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    return worldDestroyBody(ctx, s_defaultWorld, tag);
}

static JSValue js_physics_destroyAll(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    DEFW_GUARD();
    return worldDestroyAll(ctx, s_defaultWorld);
}

static JSValue js_physics_getTransform(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    return worldGetTransform(ctx, s_defaultWorld, tag);
}

static JSValue js_physics_getVelocity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = s_defaultWorld->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    auto lv = s_defaultWorld->world->getLinearVelocity(id);
    auto av = s_defaultWorld->world->getAngularVelocity(id);
    JSValue result = JS_NewObject(ctx);
    JSValue lin = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, lin, "x", JS_NewFloat64(ctx, lv.GetX()));
    JS_SetPropertyStr(ctx, lin, "y", JS_NewFloat64(ctx, lv.GetY()));
    JS_SetPropertyStr(ctx, lin, "z", JS_NewFloat64(ctx, lv.GetZ()));
    JS_SetPropertyStr(ctx, result, "linear", lin);
    JSValue ang = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ang, "x", JS_NewFloat64(ctx, av.GetX()));
    JS_SetPropertyStr(ctx, ang, "y", JS_NewFloat64(ctx, av.GetY()));
    JS_SetPropertyStr(ctx, ang, "z", JS_NewFloat64(ctx, av.GetZ()));
    JS_SetPropertyStr(ctx, result, "angular", ang);
    return result;
}

#define VEC3_BODY_FN_DEFAULT(name, call) \
static JSValue js_physics_##name(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) { \
    DEFW_GUARD(); \
    if (argc < 4) return JS_UNDEFINED; \
    int32_t tag; double x, y, z; \
    JS_ToInt32(ctx, &tag, argv[0]); \
    JS_ToFloat64(ctx, &x, argv[1]); JS_ToFloat64(ctx, &y, argv[2]); JS_ToFloat64(ctx, &z, argv[3]); \
    JPH::BodyID id = s_defaultWorld->bodyIdForTag(tag); \
    if (id.IsInvalid()) return JS_UNDEFINED; \
    s_defaultWorld->world->call(id, JPH::Vec3((float)x, (float)y, (float)z)); \
    return JS_UNDEFINED; \
}

static JSValue js_physics_setPosition(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 4) return JS_UNDEFINED;
    int32_t tag; double x, y, z;
    JS_ToInt32(ctx, &tag, argv[0]);
    JS_ToFloat64(ctx, &x, argv[1]); JS_ToFloat64(ctx, &y, argv[2]); JS_ToFloat64(ctx, &z, argv[3]);
    JPH::BodyID id = s_defaultWorld->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    s_defaultWorld->world->setPosition(id, JPH::RVec3((float)x, (float)y, (float)z));
    return JS_UNDEFINED;
}

static JSValue js_physics_setRotation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 5) return JS_UNDEFINED;
    int32_t tag; double rx, ry, rz, rw;
    JS_ToInt32(ctx, &tag, argv[0]);
    JS_ToFloat64(ctx, &rx, argv[1]); JS_ToFloat64(ctx, &ry, argv[2]);
    JS_ToFloat64(ctx, &rz, argv[3]); JS_ToFloat64(ctx, &rw, argv[4]);
    JPH::BodyID id = s_defaultWorld->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    s_defaultWorld->world->setRotation(id, JPH::Quat((float)rx, (float)ry, (float)rz, (float)rw).Normalized());
    return JS_UNDEFINED;
}

VEC3_BODY_FN_DEFAULT(setLinearVelocity, setLinearVelocity)
VEC3_BODY_FN_DEFAULT(setAngularVelocity, setAngularVelocity)
VEC3_BODY_FN_DEFAULT(addForce, addForce)
VEC3_BODY_FN_DEFAULT(addImpulse, addImpulse)
VEC3_BODY_FN_DEFAULT(addTorque, addTorque)

static JSValue js_physics_setUserData(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 2) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = s_defaultWorld->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    uint64_t data = 0;
    if (JS_IsBigInt(argv[1])) {
        int64_t s; JS_ToBigInt64(ctx, &s, argv[1]); data = (uint64_t)s;
    } else {
        double d = 0; JS_ToFloat64(ctx, &d, argv[1]); data = (uint64_t)d;
    }
    s_defaultWorld->world->setUserData(id, data);
    return JS_UNDEFINED;
}

static JSValue js_physics_getUserData(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = s_defaultWorld->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    return JS_NewBigUint64(ctx, s_defaultWorld->world->getUserData(id));
}

static JSValue js_physics_setLayer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 2) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = s_defaultWorld->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    int layer = -1;
    if (JS_IsString(argv[1])) {
        const char* s = JS_ToCString(ctx, argv[1]);
        if (s) { layer = s_defaultWorld->world->layerIndex(s); JS_FreeCString(ctx, s); }
    } else if (JS_IsNumber(argv[1])) {
        JS_ToInt32(ctx, &layer, argv[1]);
    }
    if (layer < 0) return JS_FALSE;
    s_defaultWorld->world->setLayer(id, layer);
    return JS_TRUE;
}

static JSValue js_physics_setKinematic(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = s_defaultWorld->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    s_defaultWorld->world->setKinematic(id);
    return JS_UNDEFINED;
}

static JSValue js_physics_moveKinematic(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    // (tag, x, y, z, dt) or (tag, x, y, z, qx, qy, qz, qw, dt)
    if (argc < 5) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = s_defaultWorld->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    double x, y, z;
    JS_ToFloat64(ctx, &x, argv[1]); JS_ToFloat64(ctx, &y, argv[2]); JS_ToFloat64(ctx, &z, argv[3]);
    JPH::Quat rot = s_defaultWorld->world->getRotation(id);
    double dt = 0;
    if (argc >= 9) {
        double qx, qy, qz, qw;
        JS_ToFloat64(ctx, &qx, argv[4]); JS_ToFloat64(ctx, &qy, argv[5]);
        JS_ToFloat64(ctx, &qz, argv[6]); JS_ToFloat64(ctx, &qw, argv[7]);
        rot = JPH::Quat((float)qx, (float)qy, (float)qz, (float)qw).Normalized();
        JS_ToFloat64(ctx, &dt, argv[8]);
    } else {
        JS_ToFloat64(ctx, &dt, argv[4]);
    }
    s_defaultWorld->world->moveKinematic(id, JPH::RVec3((float)x, (float)y, (float)z), rot, (float)dt);
    return JS_UNDEFINED;
}

static JSValue js_physics_raycast(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_defaultWorld) return JS_NewArray(ctx);
    return worldRaycast(ctx, s_defaultWorld, argc, argv);
}

static JSValue js_physics_raycastClosest(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_defaultWorld) return JS_NULL;
    return worldRaycastClosest(ctx, s_defaultWorld, argc, argv);
}

static JSValue js_physics_castShape(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1) return JS_ThrowTypeError(ctx, "Physics.castShape(opts) requires an object");
    return worldCastShape(ctx, s_defaultWorld, argv[0], /*closestOnly=*/false);
}

static JSValue js_physics_castShapeClosest(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1) return JS_ThrowTypeError(ctx, "Physics.castShapeClosest(opts) requires an object");
    return worldCastShape(ctx, s_defaultWorld, argv[0], /*closestOnly=*/true);
}

static JSValue js_physics_overlapShape(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1) return JS_ThrowTypeError(ctx, "Physics.overlapShape(opts) requires an object");
    return worldOverlapShape(ctx, s_defaultWorld, argv[0]);
}

static JSValue js_physics_overlapPoint(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_defaultWorld) return JS_NewArray(ctx);
    return worldOverlapPoint(ctx, s_defaultWorld, argc, argv);
}

static JSValue js_physics_getContacts(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!s_defaultWorld) return JS_NewArray(ctx);
    return worldGetContacts(ctx, s_defaultWorld);
}

static JSValue js_physics_setTimeStep(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1) return JS_UNDEFINED;
    double dt; JS_ToFloat64(ctx, &dt, argv[0]);
    s_defaultWorld->world->setTimeStep((float)dt);
    return JS_UNDEFINED;
}

static JSValue js_physics_isActive(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_defaultWorld || argc < 1) return JS_FALSE;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = s_defaultWorld->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_FALSE;
    return JS_NewBool(ctx, s_defaultWorld->world->isActive(id));
}

static JSValue js_physics_activate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = s_defaultWorld->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    s_defaultWorld->world->activate(id);
    return JS_UNDEFINED;
}

static JSValue js_physics_getAllTransforms(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!s_defaultWorld || !s_defaultWorld->world) {
        return JS_NewArrayBufferCopy(ctx, nullptr, 0);
    }
    size_t count = s_defaultWorld->bodyTags.size();
    size_t stride = 8;
    std::vector<float> buf(count * stride);
    size_t i = 0;
    auto& bi = s_defaultWorld->world->system().GetBodyInterfaceNoLock();
    for (auto& [key, tag] : s_defaultWorld->bodyTags) {
        JPH::BodyID id(key);
        auto pos = bi.GetPosition(id);
        auto rot = bi.GetRotation(id);
        float* p = buf.data() + i * stride;
        p[0] = static_cast<float>(tag);
        p[1] = pos.GetX(); p[2] = pos.GetY(); p[3] = pos.GetZ();
        p[4] = rot.GetX(); p[5] = rot.GetY(); p[6] = rot.GetZ(); p[7] = rot.GetW();
        i++;
    }
    size_t byteLen = buf.size() * sizeof(float);
    JSValue ab = JS_NewArrayBufferCopy(ctx, reinterpret_cast<uint8_t*>(buf.data()), byteLen);
    if (JS_IsException(ab)) return JS_EXCEPTION;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue f32ctor = JS_GetPropertyStr(ctx, global, "Float32Array");
    JSValue result = JS_CallConstructor(ctx, f32ctor, 1, &ab);
    JS_FreeValue(ctx, f32ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, ab);
    return result;
}

static JSValue js_physics_createCharacter(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1) return JS_ThrowTypeError(ctx, "Physics.createCharacter(opts) requires an object");
    return worldCreateCharacter(ctx, s_defaultWorld, JS_UNDEFINED, argv[0]);
}

static JSValue js_physics_createVehicle(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1) return JS_ThrowTypeError(ctx, "Physics.createVehicle(opts) requires an object");
    return worldCreateVehicle(ctx, s_defaultWorld, JS_UNDEFINED, argv[0]);
}

static JSValue js_physics_createConstraint(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1) return JS_ThrowTypeError(ctx, "Physics.createConstraint(opts) requires an object");
    return worldCreateConstraint(ctx, s_defaultWorld, argv[0]);
}

static JSValue js_physics_destroyConstraint(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1) return JS_UNDEFINED;
    uint32_t h; JS_ToUint32(ctx, &h, argv[0]);
    s_defaultWorld->world->destroyConstraint(h);
    return JS_UNDEFINED;
}

static JSValue js_physics_setConstraintEnabled(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 2) return JS_UNDEFINED;
    uint32_t h; JS_ToUint32(ctx, &h, argv[0]);
    bool en = JS_ToBool(ctx, argv[1]);
    s_defaultWorld->world->setConstraintEnabled(h, en);
    return JS_UNDEFINED;
}

static JSValue js_physics_setWheelMotor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 4) return JS_ThrowTypeError(ctx, "setWheelMotor(handle, enabled, speed, maxTorque)");
    uint32_t h; JS_ToUint32(ctx, &h, argv[0]);
    bool en = JS_ToBool(ctx, argv[1]);
    double speed = 0, torque = 0;
    JS_ToFloat64(ctx, &speed, argv[2]);
    JS_ToFloat64(ctx, &torque, argv[3]);
    s_defaultWorld->world->setWheelMotor(h, en, (float)speed, (float)torque);
    return JS_UNDEFINED;
}

static JSValue js_physics_setConstraintMotor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 2) return JS_ThrowTypeError(ctx, "setConstraintMotor(handle, opts)");
    return worldSetConstraintMotor(ctx, s_defaultWorld, argv[0], argv[1]);
}

static JSValue js_physics_setConstraintBreakingImpulse(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 2) return JS_ThrowTypeError(ctx, "setConstraintBreakingImpulse(handle, threshold)");
    uint32_t h; JS_ToUint32(ctx, &h, argv[0]);
    double t = 0; JS_ToFloat64(ctx, &t, argv[1]);
    s_defaultWorld->world->setConstraintBreakingImpulse(h, (float)t);
    return JS_UNDEFINED;
}

static JSValue js_physics_getConstraintBreakingImpulse(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    DEFW_GUARD();
    if (argc < 1) return JS_UNDEFINED;
    uint32_t h; JS_ToUint32(ctx, &h, argv[0]);
    return JS_NewFloat64(ctx, s_defaultWorld->world->getConstraintBreakingImpulse(h));
}

static JSValue js_physics_getBrokenConstraints(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!s_defaultWorld) return JS_NewArray(ctx);
    return worldGetBrokenConstraints(ctx, s_defaultWorld);
}

// ---------------------------------------------------------------------------
// Sandbox world JS class (Physics.createWorldHandle → returns object).
// ---------------------------------------------------------------------------

static void worldClassFinalizer(JSRuntime* rt, JSValue val) {
    JsWorld* w = (JsWorld*)JS_GetOpaque(val, s_worldClassId);
    if (w) delete w;
}

static JSClassDef s_worldClassDef = {
    "PhysicsWorldHandle",
    worldClassFinalizer,  // finalizer
    nullptr,              // gc_mark
    nullptr,              // call
    nullptr,              // exotic
};

static JsWorld* worldFromThis(JSContext* ctx, JSValueConst thisVal) {
    return (JsWorld*)JS_GetOpaque2(ctx, thisVal, s_worldClassId);
}

static JSValue jsw_step(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || !w->world) return JS_UNDEFINED;
    double dt = w->world->timeStep();
    if (argc >= 1) JS_ToFloat64(ctx, &dt, argv[0]);
    w->world->setTimeStep((float)dt);
    w->world->stepInline();
    return JS_UNDEFINED;
}

static JSValue jsw_setGravity(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || !w->world) return JS_UNDEFINED;
    double x = 0, y = -9.81, z = 0;
    if (argc >= 1) JS_ToFloat64(ctx, &x, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &y, argv[1]);
    if (argc >= 3) JS_ToFloat64(ctx, &z, argv[2]);
    w->world->setGravity((float)x, (float)y, (float)z);
    return JS_UNDEFINED;
}

static JSValue jsw_createBody(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "createBody(opts) requires an object");
    return worldCreateBody(ctx, w, argv[0]);
}

static JSValue jsw_destroyBody(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    return worldDestroyBody(ctx, w, tag);
}

static JSValue jsw_destroyAll(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w) return JS_UNDEFINED;
    return worldDestroyAll(ctx, w);
}

static JSValue jsw_destroy(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w) return JS_UNDEFINED;
    if (w->ownsWorld && w->world) {
        w->world->shutdown();
        delete w->world;
        w->world = nullptr;
    }
    w->bodyTags.clear();
    w->tagToBody.clear();
    return JS_UNDEFINED;
}

static JSValue jsw_getTransform(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    return worldGetTransform(ctx, w, tag);
}

// Bulk transform readout for a sandbox world — mirrors Physics.getAllTransforms
// for the default world. Returns a Float32Array packed [tag, px,py,pz,
// qx,qy,qz,qw, ...] at stride 8. One allocation, no per-body JS objects —
// preferred when syncing many bodies per frame (e.g. thousands of instanced
// rigid bodies into a single InstancedMeshNode).
static JSValue jsw_getAllTransforms(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || !w->world) return JS_NewArrayBufferCopy(ctx, nullptr, 0);
    size_t count = w->bodyTags.size();
    size_t stride = 8;
    std::vector<float> buf(count * stride);
    size_t i = 0;
    auto& bi = w->world->system().GetBodyInterfaceNoLock();
    for (auto& [key, tag] : w->bodyTags) {
        JPH::BodyID id(key);
        auto pos = bi.GetPosition(id);
        auto rot = bi.GetRotation(id);
        float* p = buf.data() + i * stride;
        p[0] = static_cast<float>(tag);
        p[1] = pos.GetX(); p[2] = pos.GetY(); p[3] = pos.GetZ();
        p[4] = rot.GetX(); p[5] = rot.GetY(); p[6] = rot.GetZ(); p[7] = rot.GetW();
        i++;
    }
    size_t byteLen = buf.size() * sizeof(float);
    JSValue ab = JS_NewArrayBufferCopy(ctx, reinterpret_cast<uint8_t*>(buf.data()), byteLen);
    if (JS_IsException(ab)) return JS_EXCEPTION;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue f32ctor = JS_GetPropertyStr(ctx, global, "Float32Array");
    JSValue result = JS_CallConstructor(ctx, f32ctor, 1, &ab);
    JS_FreeValue(ctx, f32ctor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, ab);
    return result;
}

static JSValue jsw_getVelocity(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || !w->world || argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = w->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    auto lv = w->world->getLinearVelocity(id);
    auto av = w->world->getAngularVelocity(id);
    JSValue result = JS_NewObject(ctx);
    JSValue lin = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, lin, "x", JS_NewFloat64(ctx, lv.GetX()));
    JS_SetPropertyStr(ctx, lin, "y", JS_NewFloat64(ctx, lv.GetY()));
    JS_SetPropertyStr(ctx, lin, "z", JS_NewFloat64(ctx, lv.GetZ()));
    JS_SetPropertyStr(ctx, result, "linear", lin);
    JSValue ang = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ang, "x", JS_NewFloat64(ctx, av.GetX()));
    JS_SetPropertyStr(ctx, ang, "y", JS_NewFloat64(ctx, av.GetY()));
    JS_SetPropertyStr(ctx, ang, "z", JS_NewFloat64(ctx, av.GetZ()));
    JS_SetPropertyStr(ctx, result, "angular", ang);
    return result;
}

static JSValue jsw_setPosition(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || !w->world || argc < 4) return JS_UNDEFINED;
    int32_t tag; double x, y, z;
    JS_ToInt32(ctx, &tag, argv[0]);
    JS_ToFloat64(ctx, &x, argv[1]); JS_ToFloat64(ctx, &y, argv[2]); JS_ToFloat64(ctx, &z, argv[3]);
    JPH::BodyID id = w->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    w->world->setPosition(id, JPH::RVec3((float)x, (float)y, (float)z));
    return JS_UNDEFINED;
}

#define VEC3_BODY_FN_HANDLE(name, call) \
static JSValue jsw_##name(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) { \
    JsWorld* w = worldFromThis(ctx, thisVal); \
    if (!w || !w->world || argc < 4) return JS_UNDEFINED; \
    int32_t tag; double x, y, z; \
    JS_ToInt32(ctx, &tag, argv[0]); \
    JS_ToFloat64(ctx, &x, argv[1]); JS_ToFloat64(ctx, &y, argv[2]); JS_ToFloat64(ctx, &z, argv[3]); \
    JPH::BodyID id = w->bodyIdForTag(tag); \
    if (id.IsInvalid()) return JS_UNDEFINED; \
    w->world->call(id, JPH::Vec3((float)x, (float)y, (float)z)); \
    return JS_UNDEFINED; \
}

VEC3_BODY_FN_HANDLE(setLinearVelocity, setLinearVelocity)
VEC3_BODY_FN_HANDLE(setAngularVelocity, setAngularVelocity)
VEC3_BODY_FN_HANDLE(addForce, addForce)
VEC3_BODY_FN_HANDLE(addImpulse, addImpulse)
VEC3_BODY_FN_HANDLE(addTorque, addTorque)

static JSValue jsw_raycast(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w) return JS_NewArray(ctx);
    return worldRaycast(ctx, w, argc, argv);
}

static JSValue jsw_raycastClosest(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w) return JS_NULL;
    return worldRaycastClosest(ctx, w, argc, argv);
}

static JSValue jsw_castShape(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || argc < 1) return JS_NewArray(ctx);
    return worldCastShape(ctx, w, argv[0], /*closestOnly=*/false);
}

static JSValue jsw_castShapeClosest(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || argc < 1) return JS_NULL;
    return worldCastShape(ctx, w, argv[0], /*closestOnly=*/true);
}

static JSValue jsw_overlapShape(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || argc < 1) return JS_NewArray(ctx);
    return worldOverlapShape(ctx, w, argv[0]);
}

static JSValue jsw_overlapPoint(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w) return JS_NewArray(ctx);
    return worldOverlapPoint(ctx, w, argc, argv);
}

static JSValue jsw_getContacts(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w) return JS_NewArray(ctx);
    return worldGetContacts(ctx, w);
}

static JSValue jsw_getBrokenConstraints(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w) return JS_NewArray(ctx);
    return worldGetBrokenConstraints(ctx, w);
}

static JSValue jsw_setConstraintBreakingImpulse(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || !w->world || argc < 2) return JS_UNDEFINED;
    uint32_t h; JS_ToUint32(ctx, &h, argv[0]);
    double t = 0; JS_ToFloat64(ctx, &t, argv[1]);
    w->world->setConstraintBreakingImpulse(h, (float)t);
    return JS_UNDEFINED;
}

static JSValue jsw_getConstraintBreakingImpulse(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || !w->world || argc < 1) return JS_UNDEFINED;
    uint32_t h; JS_ToUint32(ctx, &h, argv[0]);
    return JS_NewFloat64(ctx, w->world->getConstraintBreakingImpulse(h));
}

static JSValue jsw_createConstraint(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "createConstraint(opts) requires an object");
    return worldCreateConstraint(ctx, w, argv[0]);
}

static JSValue jsw_createCharacter(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "createCharacter(opts) requires an object");
    return worldCreateCharacter(ctx, w, thisVal, argv[0]);
}

static JSValue jsw_createVehicle(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "createVehicle(opts) requires an object");
    return worldCreateVehicle(ctx, w, thisVal, argv[0]);
}

static JSValue jsw_setConstraintMotor(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || !w->world || argc < 2) return JS_FALSE;
    return worldSetConstraintMotor(ctx, w, argv[0], argv[1]);
}

static JSValue jsw_destroyConstraint(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || !w->world || argc < 1) return JS_UNDEFINED;
    uint32_t h; JS_ToUint32(ctx, &h, argv[0]);
    w->world->destroyConstraint(h);
    return JS_UNDEFINED;
}

static JSValue jsw_setLayers(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || !w->world || argc < 1 || !JS_IsObject(argv[0])) return JS_FALSE;
    JSValue namesVal = JS_GetPropertyStr(ctx, argv[0], "names");
    JSValue matrixVal = JS_GetPropertyStr(ctx, argv[0], "matrix");
    std::vector<std::string> names;
    if (JS_IsArray(namesVal)) {
        JSValue lenV = JS_GetPropertyStr(ctx, namesVal, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lenV); JS_FreeValue(ctx, lenV);
        for (uint32_t i = 0; i < n; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, namesVal, i);
            const char* s = JS_ToCString(ctx, v);
            if (s) { names.emplace_back(s); JS_FreeCString(ctx, s); }
            JS_FreeValue(ctx, v);
        }
    }
    std::vector<bool> matrix;
    if (JS_IsArray(matrixVal)) {
        JSValue lenV = JS_GetPropertyStr(ctx, matrixVal, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lenV); JS_FreeValue(ctx, lenV);
        for (uint32_t i = 0; i < n; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, matrixVal, i);
            matrix.push_back(JS_ToBool(ctx, v) != 0);
            JS_FreeValue(ctx, v);
        }
    }
    JS_FreeValue(ctx, namesVal);
    JS_FreeValue(ctx, matrixVal);
    return JS_NewBool(ctx, w->world->configureLayers(names, matrix));
}

static JSValue jsw_setKinematic(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || !w->world || argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = w->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    w->world->setKinematic(id);
    return JS_UNDEFINED;
}

static JSValue jsw_moveKinematic(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || !w->world || argc < 5) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = w->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    double x, y, z;
    JS_ToFloat64(ctx, &x, argv[1]); JS_ToFloat64(ctx, &y, argv[2]); JS_ToFloat64(ctx, &z, argv[3]);
    JPH::Quat rot = w->world->getRotation(id);
    double dt = 0;
    if (argc >= 9) {
        double qx, qy, qz, qw;
        JS_ToFloat64(ctx, &qx, argv[4]); JS_ToFloat64(ctx, &qy, argv[5]);
        JS_ToFloat64(ctx, &qz, argv[6]); JS_ToFloat64(ctx, &qw, argv[7]);
        rot = JPH::Quat((float)qx, (float)qy, (float)qz, (float)qw).Normalized();
        JS_ToFloat64(ctx, &dt, argv[8]);
    } else {
        JS_ToFloat64(ctx, &dt, argv[4]);
    }
    w->world->moveKinematic(id, JPH::RVec3((float)x, (float)y, (float)z), rot, (float)dt);
    return JS_UNDEFINED;
}

static JSValue jsw_setLayer(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || !w->world || argc < 2) return JS_FALSE;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = w->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_FALSE;
    int layer = -1;
    if (JS_IsString(argv[1])) {
        const char* s = JS_ToCString(ctx, argv[1]);
        if (s) { layer = w->world->layerIndex(s); JS_FreeCString(ctx, s); }
    } else if (JS_IsNumber(argv[1])) {
        JS_ToInt32(ctx, &layer, argv[1]);
    }
    if (layer < 0) return JS_FALSE;
    w->world->setLayer(id, layer);
    return JS_TRUE;
}

static JSValue jsw_setUserData(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || !w->world || argc < 2) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = w->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    uint64_t data = 0;
    if (JS_IsBigInt(argv[1])) {
        int64_t s; JS_ToBigInt64(ctx, &s, argv[1]); data = (uint64_t)s;
    } else {
        double d = 0; JS_ToFloat64(ctx, &d, argv[1]); data = (uint64_t)d;
    }
    w->world->setUserData(id, data);
    return JS_UNDEFINED;
}

static JSValue jsw_getUserData(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || !w->world || argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = w->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    return JS_NewBigUint64(ctx, w->world->getUserData(id));
}

static JSValue jsw_activate(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    JsWorld* w = worldFromThis(ctx, thisVal);
    if (!w || !w->world || argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = w->bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    w->world->activate(id);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry s_worldProtoFuncs[] = {
    JS_CFUNC_DEF("step", 1, jsw_step),
    JS_CFUNC_DEF("setGravity", 3, jsw_setGravity),
    JS_CFUNC_DEF("setLayers", 1, jsw_setLayers),
    JS_CFUNC_DEF("createBody", 1, jsw_createBody),
    JS_CFUNC_DEF("destroyBody", 1, jsw_destroyBody),
    JS_CFUNC_DEF("destroyAll", 0, jsw_destroyAll),
    JS_CFUNC_DEF("destroy", 0, jsw_destroy),
    JS_CFUNC_DEF("getTransform", 1, jsw_getTransform),
    JS_CFUNC_DEF("getAllTransforms", 0, jsw_getAllTransforms),
    JS_CFUNC_DEF("getVelocity", 1, jsw_getVelocity),
    JS_CFUNC_DEF("setPosition", 4, jsw_setPosition),
    JS_CFUNC_DEF("setLinearVelocity", 4, jsw_setLinearVelocity),
    JS_CFUNC_DEF("setAngularVelocity", 4, jsw_setAngularVelocity),
    JS_CFUNC_DEF("addForce", 4, jsw_addForce),
    JS_CFUNC_DEF("addImpulse", 4, jsw_addImpulse),
    JS_CFUNC_DEF("addTorque", 4, jsw_addTorque),
    JS_CFUNC_DEF("setLayer", 2, jsw_setLayer),
    JS_CFUNC_DEF("setKinematic", 1, jsw_setKinematic),
    JS_CFUNC_DEF("moveKinematic", 5, jsw_moveKinematic),
    JS_CFUNC_DEF("setUserData", 2, jsw_setUserData),
    JS_CFUNC_DEF("getUserData", 1, jsw_getUserData),
    JS_CFUNC_DEF("activate", 1, jsw_activate),
    JS_CFUNC_DEF("raycast", 8, jsw_raycast),
    JS_CFUNC_DEF("raycastClosest", 8, jsw_raycastClosest),
    JS_CFUNC_DEF("castShape", 1, jsw_castShape),
    JS_CFUNC_DEF("castShapeClosest", 1, jsw_castShapeClosest),
    JS_CFUNC_DEF("overlapShape", 1, jsw_overlapShape),
    JS_CFUNC_DEF("overlapPoint", 4, jsw_overlapPoint),
    JS_CFUNC_DEF("getContacts", 0, jsw_getContacts),
    JS_CFUNC_DEF("createCharacter", 1, jsw_createCharacter),
    JS_CFUNC_DEF("createVehicle", 1, jsw_createVehicle),
    JS_CFUNC_DEF("createConstraint", 1, jsw_createConstraint),
    JS_CFUNC_DEF("setConstraintMotor", 2, jsw_setConstraintMotor),
    JS_CFUNC_DEF("destroyConstraint", 1, jsw_destroyConstraint),
    JS_CFUNC_DEF("setConstraintBreakingImpulse", 2, jsw_setConstraintBreakingImpulse),
    JS_CFUNC_DEF("getConstraintBreakingImpulse", 1, jsw_getConstraintBreakingImpulse),
    JS_CFUNC_DEF("getBrokenConstraints", 0, jsw_getBrokenConstraints),
};

static JSValue js_physics_createWorldHandle(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int maxBodies = 1024;
    JPH::Vec3 gravity(0, -9.81f, 0);
    if (argc >= 1 && JS_IsObject(argv[0])) {
        double mb = qjsbind::get_prop_number(ctx, argv[0], "maxBodies", 1024);
        maxBodies = (int)mb;
        JSValue gv = JS_GetPropertyStr(ctx, argv[0], "gravity");
        if (JS_IsObject(gv)) gravity = readVec3(ctx, gv, gravity);
        JS_FreeValue(ctx, gv);
    }

    auto* pw = new physics::PhysicsWorld();
    if (!pw->init(maxBodies)) {
        delete pw;
        return JS_ThrowInternalError(ctx, "Failed to init sandbox PhysicsWorld");
    }
    pw->setGravity(gravity.GetX(), gravity.GetY(), gravity.GetZ());
    // Sandbox worlds are NOT thread-stepped; caller drives via .step(dt).

    auto* jw = new JsWorld();
    jw->world = pw;
    jw->ownsWorld = true;

    JSValue obj = JS_NewObjectClass(ctx, s_worldClassId);
    if (JS_IsException(obj)) {
        delete jw;
        return obj;
    }
    JS_SetOpaque(obj, jw);
    return obj;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void PhysicsBindings::install(JSContext* ctx, physics::PhysicsWorld* world) {
    // Default world: don't take ownership, engine owns the PhysicsWorld.
    s_defaultWorld = new JsWorld();
    s_defaultWorld->world = world;
    s_defaultWorld->ownsWorld = false;

    // Register world handle class.
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (s_worldClassId == 0) {
        JS_NewClassID(rt, &s_worldClassId);
    }
    if (!JS_IsRegisteredClass(rt, s_worldClassId)) {
        JS_NewClass(rt, s_worldClassId, &s_worldClassDef);
    }
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, s_worldProtoFuncs,
                               sizeof(s_worldProtoFuncs)/sizeof(s_worldProtoFuncs[0]));
    JS_SetClassProto(ctx, s_worldClassId, proto);

    // Register character handle class.
    if (s_characterClassId == 0) {
        JS_NewClassID(rt, &s_characterClassId);
    }
    if (!JS_IsRegisteredClass(rt, s_characterClassId)) {
        JS_NewClass(rt, s_characterClassId, &s_characterClassDef);
    }
    JSValue charProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, charProto, s_characterProtoFuncs,
                               sizeof(s_characterProtoFuncs)/sizeof(s_characterProtoFuncs[0]));
    JS_SetClassProto(ctx, s_characterClassId, charProto);

    // Register vehicle handle class.
    if (s_vehicleClassId == 0) {
        JS_NewClassID(rt, &s_vehicleClassId);
    }
    if (!JS_IsRegisteredClass(rt, s_vehicleClassId)) {
        JS_NewClass(rt, s_vehicleClassId, &s_vehicleClassDef);
    }
    JSValue vehProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, vehProto, s_vehicleProtoFuncs,
                               sizeof(s_vehicleProtoFuncs)/sizeof(s_vehicleProtoFuncs[0]));
    JS_SetClassProto(ctx, s_vehicleClassId, vehProto);

    qjsbind::Namespace(ctx, "Physics")
        .function("createWorld", js_physics_createWorld, 1)
        .function("createWorldHandle", js_physics_createWorldHandle, 1)
        .function("setGravity", js_physics_setGravity, 3)
        .function("getGravity", js_physics_getGravity, 0)
        .function("setLayers", js_physics_setLayers, 1)
        .function("createBody", js_physics_createBody, 1)
        .function("destroyBody", js_physics_destroyBody, 1)
        .function("destroyAll", js_physics_destroyAll, 0)
        .function("getTransform", js_physics_getTransform, 1)
        .function("getVelocity", js_physics_getVelocity, 1)
        .function("setPosition", js_physics_setPosition, 4)
        .function("setRotation", js_physics_setRotation, 5)
        .function("setLinearVelocity", js_physics_setLinearVelocity, 4)
        .function("setAngularVelocity", js_physics_setAngularVelocity, 4)
        .function("addForce", js_physics_addForce, 4)
        .function("addImpulse", js_physics_addImpulse, 4)
        .function("addTorque", js_physics_addTorque, 4)
        .function("setUserData", js_physics_setUserData, 2)
        .function("getUserData", js_physics_getUserData, 1)
        .function("setLayer", js_physics_setLayer, 2)
        .function("setKinematic", js_physics_setKinematic, 1)
        .function("moveKinematic", js_physics_moveKinematic, 5)
        .function("raycast", js_physics_raycast, 8)
        .function("raycastClosest", js_physics_raycastClosest, 8)
        .function("castShape", js_physics_castShape, 1)
        .function("castShapeClosest", js_physics_castShapeClosest, 1)
        .function("overlapShape", js_physics_overlapShape, 1)
        .function("overlapPoint", js_physics_overlapPoint, 4)
        .function("getContacts", js_physics_getContacts, 0)
        .function("setTimeStep", js_physics_setTimeStep, 1)
        .function("isActive", js_physics_isActive, 1)
        .function("activate", js_physics_activate, 1)
        .function("getAllTransforms", js_physics_getAllTransforms, 0)
        .function("createCharacter", js_physics_createCharacter, 1)
        .function("createVehicle", js_physics_createVehicle, 1)
        .function("createConstraint", js_physics_createConstraint, 1)
        .function("destroyConstraint", js_physics_destroyConstraint, 1)
        .function("setConstraintEnabled", js_physics_setConstraintEnabled, 2)
        .function("setWheelMotor", js_physics_setWheelMotor, 4)
        .function("setConstraintMotor", js_physics_setConstraintMotor, 2)
        .function("setConstraintBreakingImpulse", js_physics_setConstraintBreakingImpulse, 2)
        .function("getConstraintBreakingImpulse", js_physics_getConstraintBreakingImpulse, 1)
        .function("getBrokenConstraints", js_physics_getBrokenConstraints, 0);
}

void PhysicsBindings::cleanup(JSContext* ctx) {
    if (ctx) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSAtom atom = JS_NewAtom(ctx, "Physics");
        JS_DeleteProperty(ctx, global, atom, 0);
        JS_FreeAtom(ctx, atom);
        JS_FreeValue(ctx, global);
    }
    if (s_defaultWorld) {
        // Default world doesn't own its PhysicsWorld; just drop the JsWorld shell.
        delete s_defaultWorld;
        s_defaultWorld = nullptr;
    }
}

} // namespace bro::js

#endif  // BRO_WITH_PHYSICS
