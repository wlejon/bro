#include "js/physics_bindings.h"
#include "js/runtime.h"
#include "physics/physics_world.h"
#include "util/log.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <qjsbind/qjsbind.h>

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

JPH_SUPPRESS_WARNINGS

namespace bro::js {

// ---------------------------------------------------------------------------
// JsWorld — wraps a PhysicsWorld plus its tag space and JS-side bookkeeping.
// The default world is owned by the engine (s_defaultWorld points at it but
// does NOT own the PhysicsWorld); sandbox worlds own their PhysicsWorld.
// ---------------------------------------------------------------------------

struct JsWorld {
    physics::PhysicsWorld* world = nullptr;
    bool ownsWorld = false;  // true → delete `world` in destructor

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

    ~JsWorld() {
        if (ownsWorld && world) {
            world->shutdown();
            delete world;
        }
        world = nullptr;
    }
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

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static double jsGetNum(JSContext* ctx, JSValueConst obj, const char* prop, double def = 0.0) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    double r = def;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) JS_ToFloat64(ctx, &r, v);
    JS_FreeValue(ctx, v);
    return r;
}

static bool jsGetBool(JSContext* ctx, JSValueConst obj, const char* prop, bool def = false) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    bool r = def;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) r = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return r;
}

static std::string jsGetString(JSContext* ctx, JSValueConst obj, const char* prop) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    std::string out;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { out = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    return out;
}

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
        (float)jsGetNum(ctx, obj, "x", def.GetX()),
        (float)jsGetNum(ctx, obj, "y", def.GetY()),
        (float)jsGetNum(ctx, obj, "z", def.GetZ()));
}

static JPH::Quat readQuat(JSContext* ctx, JSValueConst obj) {
    if (!JS_IsObject(obj)) return JPH::Quat::sIdentity();
    return JPH::Quat(
        (float)jsGetNum(ctx, obj, "x", 0),
        (float)jsGetNum(ctx, obj, "y", 0),
        (float)jsGetNum(ctx, obj, "z", 0),
        (float)jsGetNum(ctx, obj, "w", 1)).Normalized();
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
    std::string shape = jsGetString(ctx, opts, "shape");
    if (shape.empty()) { err = "shape is required"; return false; }

    if      (shape == "box")        out.shape = physics::BodyOptions::ShapeBox;
    else if (shape == "sphere")     out.shape = physics::BodyOptions::ShapeSphere;
    else if (shape == "capsule")    out.shape = physics::BodyOptions::ShapeCapsule;
    else if (shape == "cylinder")   out.shape = physics::BodyOptions::ShapeCylinder;
    else if (shape == "convexHull") out.shape = physics::BodyOptions::ShapeConvexHull;
    else if (shape == "mesh")       out.shape = physics::BodyOptions::ShapeMesh;
    else if (shape == "compound")   out.shape = physics::BodyOptions::ShapeCompound;
    else if (shape == "chain")      out.shape = physics::BodyOptions::ShapeChain;
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

    out.radius = (float)jsGetNum(ctx, opts, "radius", 0.5);
    out.halfHeight = (float)jsGetNum(ctx, opts, "halfHeight", 0.5);

    out.isStatic = jsGetBool(ctx, opts, "static", false);
    out.isSensor = jsGetBool(ctx, opts, "sensor", false);
    out.ccd = jsGetBool(ctx, opts, "ccd", false);
    out.friction = (float)jsGetNum(ctx, opts, "friction", 0.5);
    out.restitution = (float)jsGetNum(ctx, opts, "restitution", 0.3);
    out.density = (float)jsGetNum(ctx, opts, "density", 1000.0);
    out.gravityFactor = (float)jsGetNum(ctx, opts, "gravityFactor", 1.0);
    out.linearDamping = (float)jsGetNum(ctx, opts, "linearDamping", 0.05);
    out.angularDamping = (float)jsGetNum(ctx, opts, "angularDamping", 0.05);
    out.maxLinearVelocity = (float)jsGetNum(ctx, opts, "maxLinearVelocity", 500.0);
    out.maxAngularVelocity = (float)jsGetNum(ctx, opts, "maxAngularVelocity", 0.25 * 3.14159265 * 60.0);
    out.userData = jsGetU64(ctx, opts, "userData", 0);

    std::string dofs = jsGetString(ctx, opts, "dofs");
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
        out.chainDepth = (float)jsGetNum(ctx, opts, "depth", 20.0);
        out.chainClosed = jsGetBool(ctx, opts, "closed", false);
        out.chainFlipNormal = jsGetBool(ctx, opts, "flipNormal", false);
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

static JSValue worldRaycast(JSContext* ctx, JsWorld* w, int argc, JSValueConst* argv) {
    if (!w || !w->world || argc < 6) return JS_NewArray(ctx);
    double ox, oy, oz, dx, dy, dz;
    JS_ToFloat64(ctx, &ox, argv[0]); JS_ToFloat64(ctx, &oy, argv[1]); JS_ToFloat64(ctx, &oz, argv[2]);
    JS_ToFloat64(ctx, &dx, argv[3]); JS_ToFloat64(ctx, &dy, argv[4]); JS_ToFloat64(ctx, &dz, argv[5]);
    double maxDist = 1000.0;
    if (argc >= 7) JS_ToFloat64(ctx, &maxDist, argv[6]);

    auto hits = w->world->raycast(
        JPH::RVec3((float)ox, (float)oy, (float)oz),
        JPH::Vec3((float)dx, (float)dy, (float)dz),
        (float)maxDist);

    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (auto& hit : hits) {
        JSValue obj = JS_NewObject(ctx);
        int32_t tag = w->tagForBodyId(hit.bodyID);
        JS_SetPropertyStr(ctx, obj, "bodyId", JS_NewInt32(ctx, tag));
        JS_SetPropertyStr(ctx, obj, "fraction", JS_NewFloat64(ctx, hit.fraction));
        JS_SetPropertyStr(ctx, obj, "userData",
            JS_NewBigUint64(ctx, hit.bodyID.IsInvalid() ? 0 : w->world->getUserData(hit.bodyID)));
        JSValue posObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, posObj, "x", JS_NewFloat64(ctx, hit.position.GetX()));
        JS_SetPropertyStr(ctx, posObj, "y", JS_NewFloat64(ctx, hit.position.GetY()));
        JS_SetPropertyStr(ctx, posObj, "z", JS_NewFloat64(ctx, hit.position.GetZ()));
        JS_SetPropertyStr(ctx, obj, "position", posObj);
        JS_SetPropertyUint32(ctx, arr, i++, obj);
    }
    return arr;
}

static JSValue worldRaycastClosest(JSContext* ctx, JsWorld* w, int argc, JSValueConst* argv) {
    if (!w || !w->world || argc < 6) return JS_NULL;
    double ox, oy, oz, dx, dy, dz;
    JS_ToFloat64(ctx, &ox, argv[0]); JS_ToFloat64(ctx, &oy, argv[1]); JS_ToFloat64(ctx, &oz, argv[2]);
    JS_ToFloat64(ctx, &dx, argv[3]); JS_ToFloat64(ctx, &dy, argv[4]); JS_ToFloat64(ctx, &dz, argv[5]);
    double maxDist = 1000.0;
    if (argc >= 7) JS_ToFloat64(ctx, &maxDist, argv[6]);

    physics::RayHit hit;
    bool ok = w->world->raycastClosest(
        JPH::RVec3((float)ox, (float)oy, (float)oz),
        JPH::Vec3((float)dx, (float)dy, (float)dz),
        hit, (float)maxDist);
    if (!ok) return JS_NULL;

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "bodyId", JS_NewInt32(ctx, w->tagForBodyId(hit.bodyID)));
    JS_SetPropertyStr(ctx, obj, "fraction", JS_NewFloat64(ctx, hit.fraction));
    JS_SetPropertyStr(ctx, obj, "userData",
        JS_NewBigUint64(ctx, hit.bodyID.IsInvalid() ? 0 : w->world->getUserData(hit.bodyID)));
    JSValue posObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, posObj, "x", JS_NewFloat64(ctx, hit.position.GetX()));
    JS_SetPropertyStr(ctx, posObj, "y", JS_NewFloat64(ctx, hit.position.GetY()));
    JS_SetPropertyStr(ctx, posObj, "z", JS_NewFloat64(ctx, hit.position.GetZ()));
    JS_SetPropertyStr(ctx, obj, "position", posObj);
    return obj;
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

static JSValue worldCreateConstraint(JSContext* ctx, JsWorld* w, JSValueConst o) {
    if (!w || !w->world) return JS_ThrowInternalError(ctx, "World not available");
    if (!JS_IsObject(o)) return JS_ThrowTypeError(ctx, "createConstraint(opts) requires an object");

    physics::ConstraintOptions cs;

    std::string type = jsGetString(ctx, o, "type");
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
    else return JS_ThrowTypeError(ctx, "constraint type required (distance|point|hinge|fixed|slider|wheel|cone|swingTwist|pulley|gear|rackAndPinion)");

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

    cs.minDistance = (float)jsGetNum(ctx, o, "minDistance", -1.0);
    cs.maxDistance = (float)jsGetNum(ctx, o, "maxDistance", -1.0);

    JSValue lmin = JS_GetPropertyStr(ctx, o, "limitMin");
    JSValue lmax = JS_GetPropertyStr(ctx, o, "limitMax");
    if (!JS_IsUndefined(lmin) && !JS_IsUndefined(lmax)) {
        double a, b;
        JS_ToFloat64(ctx, &a, lmin); JS_ToFloat64(ctx, &b, lmax);
        cs.limitMin = (float)a; cs.limitMax = (float)b;
        cs.hasLimits = true;
    }
    JS_FreeValue(ctx, lmin); JS_FreeValue(ctx, lmax);

    cs.breakingImpulse = (float)jsGetNum(ctx, o, "breakingImpulse", 0.0);
    cs.collideConnected = jsGetBool(ctx, o, "collideConnected", false);

    if (cs.type == physics::ConstraintOptions::Wheel) {
        JSValue sa = JS_GetPropertyStr(ctx, o, "suspensionAxis");
        cs.wheelSuspensionAxis = readVec3(ctx, sa, JPH::Vec3(0, 1, 0));
        JS_FreeValue(ctx, sa);
        JSValue ha = JS_GetPropertyStr(ctx, o, "hingeAxis");
        cs.wheelHingeAxis = readVec3(ctx, ha, JPH::Vec3(0, 0, 1));
        JS_FreeValue(ctx, ha);
        cs.wheelHertz = (float)jsGetNum(ctx, o, "hertz", 2.0);
        cs.wheelDampingRatio = (float)jsGetNum(ctx, o, "dampingRatio", 0.7);
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
        cs.wheelEnableMotor = jsGetBool(ctx, o, "enableMotor", false);
        cs.wheelMotorSpeed = (float)jsGetNum(ctx, o, "motorSpeed", 0.0);
        cs.wheelMaxMotorTorque = (float)jsGetNum(ctx, o, "maxMotorTorque", 0.0);
    }

    if (cs.type == physics::ConstraintOptions::Cone) {
        cs.coneHalfAngle = (float)jsGetNum(ctx, o, "halfConeAngle", 0.0);
    }

    if (cs.type == physics::ConstraintOptions::SwingTwist) {
        JSValue pa = JS_GetPropertyStr(ctx, o, "planeAxis");
        cs.planeAxis = readVec3(ctx, pa, JPH::Vec3(0, 1, 0)); JS_FreeValue(ctx, pa);
        cs.normalHalfConeAngle = (float)jsGetNum(ctx, o, "normalHalfConeAngle", 0.0);
        cs.planeHalfConeAngle = (float)jsGetNum(ctx, o, "planeHalfConeAngle", 0.0);
        cs.twistMinAngle = (float)jsGetNum(ctx, o, "twistMinAngle", 0.0);
        cs.twistMaxAngle = (float)jsGetNum(ctx, o, "twistMaxAngle", 0.0);
        cs.maxFrictionTorque = (float)jsGetNum(ctx, o, "maxFrictionTorque", 0.0);
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
        cs.ratio = (float)jsGetNum(ctx, o, "ratio", 1.0);
        cs.minLength = (float)jsGetNum(ctx, o, "minLength", 0.0);
        cs.maxLength = (float)jsGetNum(ctx, o, "maxLength", -1.0);
    }

    if (cs.type == physics::ConstraintOptions::Gear ||
        cs.type == physics::ConstraintOptions::RackAndPinion) {
        JSValue ha1 = JS_GetPropertyStr(ctx, o, "hingeAxis1");
        cs.hingeAxis1 = readVec3(ctx, ha1, JPH::Vec3(1, 0, 0)); JS_FreeValue(ctx, ha1);
        JSValue ha2 = JS_GetPropertyStr(ctx, o, cs.type == physics::ConstraintOptions::Gear ? "hingeAxis2" : "sliderAxis");
        cs.hingeAxis2 = readVec3(ctx, ha2, JPH::Vec3(1, 0, 0)); JS_FreeValue(ctx, ha2);
        cs.ratio = (float)jsGetNum(ctx, o, "ratio", 1.0);
        cs.dependentConstraint1 = (uint32_t)jsGetNum(ctx, o, "constraint1", 0.0);
        cs.dependentConstraint2 = (uint32_t)jsGetNum(ctx, o, "constraint2", 0.0);
        if (!cs.dependentConstraint1 || !cs.dependentConstraint2) {
            const char* m = cs.type == physics::ConstraintOptions::Gear
                ? "gear requires constraint1/constraint2 (two hinge constraint handles)"
                : "rackAndPinion requires constraint1 (pinion hinge) and constraint2 (rack slider) handles";
            return JS_ThrowTypeError(ctx, "%s", m);
        }
    }

    uint32_t handle = w->world->createConstraint(cs);
    if (!handle) return JS_ThrowInternalError(ctx, "Failed to create constraint");
    return JS_NewUint32(ctx, handle);
}

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
    JS_CFUNC_DEF("raycast", 7, jsw_raycast),
    JS_CFUNC_DEF("raycastClosest", 7, jsw_raycastClosest),
    JS_CFUNC_DEF("getContacts", 0, jsw_getContacts),
    JS_CFUNC_DEF("createConstraint", 1, jsw_createConstraint),
    JS_CFUNC_DEF("destroyConstraint", 1, jsw_destroyConstraint),
    JS_CFUNC_DEF("setConstraintBreakingImpulse", 2, jsw_setConstraintBreakingImpulse),
    JS_CFUNC_DEF("getConstraintBreakingImpulse", 1, jsw_getConstraintBreakingImpulse),
    JS_CFUNC_DEF("getBrokenConstraints", 0, jsw_getBrokenConstraints),
};

static JSValue js_physics_createWorldHandle(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int maxBodies = 1024;
    JPH::Vec3 gravity(0, -9.81f, 0);
    if (argc >= 1 && JS_IsObject(argv[0])) {
        double mb = jsGetNum(ctx, argv[0], "maxBodies", 1024);
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
        .function("raycast", js_physics_raycast, 7)
        .function("raycastClosest", js_physics_raycastClosest, 7)
        .function("getContacts", js_physics_getContacts, 0)
        .function("setTimeStep", js_physics_setTimeStep, 1)
        .function("isActive", js_physics_isActive, 1)
        .function("activate", js_physics_activate, 1)
        .function("getAllTransforms", js_physics_getAllTransforms, 0)
        .function("createConstraint", js_physics_createConstraint, 1)
        .function("destroyConstraint", js_physics_destroyConstraint, 1)
        .function("setConstraintEnabled", js_physics_setConstraintEnabled, 2)
        .function("setWheelMotor", js_physics_setWheelMotor, 4)
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
