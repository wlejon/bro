#include "js/physics_bindings.h"
#include "js/runtime.h"
#include "physics/physics_world.h"
#include "util/log.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <qjsbind/qjsbind.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

JPH_SUPPRESS_WARNINGS

namespace bro::js {

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static physics::PhysicsWorld* s_world = nullptr;

// Map from BodyID index+sequence to a user-assigned integer tag (for JS identification)
static std::unordered_map<uint32_t, int32_t> s_bodyTags;
static std::unordered_map<int32_t, uint32_t> s_tagToBody;
static int32_t s_nextTag = 1;

JPH::BodyID PhysicsBindings::bodyIdForTag(int32_t tag) {
    auto it = s_tagToBody.find(tag);
    if (it == s_tagToBody.end()) return JPH::BodyID();
    return JPH::BodyID(it->second);
}

int32_t PhysicsBindings::tagForBodyId(JPH::BodyID id) {
    auto it = s_bodyTags.find(id.GetIndexAndSequenceNumber());
    return it != s_bodyTags.end() ? it->second : -1;
}

static int32_t registerBody(JPH::BodyID id) {
    if (id.IsInvalid()) return -1;
    int32_t tag = s_nextTag++;
    s_bodyTags[id.GetIndexAndSequenceNumber()] = tag;
    s_tagToBody[tag] = id.GetIndexAndSequenceNumber();
    return tag;
}

static void unregisterBody(int32_t tag) {
    auto it = s_tagToBody.find(tag);
    if (it == s_tagToBody.end()) return;
    s_bodyTags.erase(it->second);
    s_tagToBody.erase(it);
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

// Read Float32Array / Array of numbers into a flat float vector.
static bool readFloatArray(JSContext* ctx, JSValueConst v, std::vector<float>& out) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return false;
    // Try TypedArray buffer first
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
    // Fallback: regular array
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

// Parse a BodyOptions struct from a JS opts object.
static bool readBodyOptions(JSContext* ctx, JSValueConst opts, physics::BodyOptions& out, std::string& err) {
    std::string shape = jsGetString(ctx, opts, "shape");
    if (shape.empty()) { err = "shape is required"; return false; }

    if      (shape == "box")        out.shape = physics::BodyOptions::ShapeBox;
    else if (shape == "sphere")     out.shape = physics::BodyOptions::ShapeSphere;
    else if (shape == "capsule")    out.shape = physics::BodyOptions::ShapeCapsule;
    else if (shape == "cylinder")   out.shape = physics::BodyOptions::ShapeCylinder;
    else if (shape == "convexHull") out.shape = physics::BodyOptions::ShapeConvexHull;
    else if (shape == "mesh")       out.shape = physics::BodyOptions::ShapeMesh;
    else if (shape == "compound")   out.shape = physics::BodyOptions::ShapeCompound;
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
    out.userData = jsGetU64(ctx, opts, "userData", 0);

    // DOFs
    std::string dofs = jsGetString(ctx, opts, "dofs");
    if (dofs == "2d" || dofs == "plane2d" || dofs == "Plane2D") {
        out.dofs = JPH::EAllowedDOFs::Plane2D;
    } else if (dofs == "all" || dofs.empty()) {
        out.dofs = JPH::EAllowedDOFs::All;
    } else {
        // Parse bitmask: comma-separated tokens like "tx,ty,rz"
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

    // Layer (string name or numeric index)
    JSValue layerVal = JS_GetPropertyStr(ctx, opts, "layer");
    if (JS_IsString(layerVal)) {
        const char* s = JS_ToCString(ctx, layerVal);
        if (s) { out.layer = s_world->layerIndex(s); JS_FreeCString(ctx, s); }
    } else if (JS_IsNumber(layerVal)) {
        int32_t i = -1; JS_ToInt32(ctx, &i, layerVal); out.layer = i;
    } else {
        out.layer = -1;
    }
    JS_FreeValue(ctx, layerVal);

    // Convex hull points: Float32Array or array of {x,y,z}
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

    // Mesh: positions + indices
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
        out.isStatic = true;  // mesh shapes are always static in Jolt
    }

    // Compound: parts array
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
            if (!readBodyOptions(ctx, p, sub, suberr)) {
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
// Physics.createWorld(opts?)
// ---------------------------------------------------------------------------

static JSValue js_physics_createWorld(JSContext* ctx, JSValueConst, int /*argc*/, JSValueConst* /*argv*/) {
    if (!s_world) return JS_ThrowInternalError(ctx, "Physics not available");
    return JS_TRUE;
}

// ---------------------------------------------------------------------------
// Physics.setGravity / getGravity
// ---------------------------------------------------------------------------

static JSValue js_physics_setGravity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world) return JS_ThrowInternalError(ctx, "Physics not available");
    double x = 0, y = -9.81, z = 0;
    if (argc >= 1) JS_ToFloat64(ctx, &x, argv[0]);
    if (argc >= 2) JS_ToFloat64(ctx, &y, argv[1]);
    if (argc >= 3) JS_ToFloat64(ctx, &z, argv[2]);
    s_world->setGravity((float)x, (float)y, (float)z);
    return JS_UNDEFINED;
}

static JSValue js_physics_getGravity(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!s_world) return JS_ThrowInternalError(ctx, "Physics not available");
    auto g = s_world->gravity();
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, g.GetX()));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, g.GetY()));
    JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, g.GetZ()));
    return obj;
}

// ---------------------------------------------------------------------------
// Physics.setLayers({names: [...], matrix: [...]})
// ---------------------------------------------------------------------------

static JSValue js_physics_setLayers(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1 || !JS_IsObject(argv[0]))
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

    bool ok = s_world->configureLayers(names, matrix);
    return JS_NewBool(ctx, ok);
}

// ---------------------------------------------------------------------------
// Physics.createBody(opts) → tag
// ---------------------------------------------------------------------------

static JSValue js_physics_createBody(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "Physics.createBody(opts) requires an object");

    physics::BodyOptions opts;
    std::string err;
    if (!readBodyOptions(ctx, argv[0], opts, err))
        return JS_ThrowTypeError(ctx, "%s", err.c_str());

    JPH::BodyID id = s_world->createBody(opts);
    if (id.IsInvalid())
        return JS_ThrowInternalError(ctx, "Failed to create body");

    return JS_NewInt32(ctx, registerBody(id));
}

// ---------------------------------------------------------------------------
// Physics.destroyBody(tag)
// ---------------------------------------------------------------------------

static JSValue js_physics_destroyBody(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = PhysicsBindings::bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    s_world->destroyBody(id);
    unregisterBody(tag);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Physics.getTransform(tag)
// ---------------------------------------------------------------------------

static JSValue js_physics_getTransform(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = PhysicsBindings::bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;

    auto pos = s_world->getPosition(id);
    auto rot = s_world->getRotation(id);
    uint64_t udata = s_world->getUserData(id);

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

// ---------------------------------------------------------------------------
// Physics.getVelocity(tag)
// ---------------------------------------------------------------------------

static JSValue js_physics_getVelocity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = PhysicsBindings::bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;

    auto lv = s_world->getLinearVelocity(id);
    auto av = s_world->getAngularVelocity(id);

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

// ---------------------------------------------------------------------------
// Vec3-arg setters/adders
// ---------------------------------------------------------------------------

#define VEC3_BODY_FN(name, call) \
static JSValue js_physics_##name(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) { \
    if (!s_world || argc < 4) return JS_UNDEFINED; \
    int32_t tag; double x, y, z; \
    JS_ToInt32(ctx, &tag, argv[0]); \
    JS_ToFloat64(ctx, &x, argv[1]); JS_ToFloat64(ctx, &y, argv[2]); JS_ToFloat64(ctx, &z, argv[3]); \
    JPH::BodyID id = PhysicsBindings::bodyIdForTag(tag); \
    if (id.IsInvalid()) return JS_UNDEFINED; \
    s_world->call(id, JPH::Vec3((float)x, (float)y, (float)z)); \
    return JS_UNDEFINED; \
}

static JSValue js_physics_setPosition(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 4) return JS_UNDEFINED;
    int32_t tag; double x, y, z;
    JS_ToInt32(ctx, &tag, argv[0]);
    JS_ToFloat64(ctx, &x, argv[1]); JS_ToFloat64(ctx, &y, argv[2]); JS_ToFloat64(ctx, &z, argv[3]);
    JPH::BodyID id = PhysicsBindings::bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    s_world->setPosition(id, JPH::RVec3((float)x, (float)y, (float)z));
    return JS_UNDEFINED;
}

static JSValue js_physics_setRotation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 5) return JS_UNDEFINED;
    int32_t tag; double rx, ry, rz, rw;
    JS_ToInt32(ctx, &tag, argv[0]);
    JS_ToFloat64(ctx, &rx, argv[1]); JS_ToFloat64(ctx, &ry, argv[2]);
    JS_ToFloat64(ctx, &rz, argv[3]); JS_ToFloat64(ctx, &rw, argv[4]);
    JPH::BodyID id = PhysicsBindings::bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    s_world->setRotation(id, JPH::Quat((float)rx, (float)ry, (float)rz, (float)rw).Normalized());
    return JS_UNDEFINED;
}

VEC3_BODY_FN(setLinearVelocity, setLinearVelocity)
VEC3_BODY_FN(setAngularVelocity, setAngularVelocity)
VEC3_BODY_FN(addForce, addForce)
VEC3_BODY_FN(addImpulse, addImpulse)
VEC3_BODY_FN(addTorque, addTorque)

// ---------------------------------------------------------------------------
// User data
// ---------------------------------------------------------------------------

static JSValue js_physics_setUserData(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 2) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = PhysicsBindings::bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    uint64_t data = 0;
    if (JS_IsBigInt(argv[1])) {
        int64_t s; JS_ToBigInt64(ctx, &s, argv[1]); data = (uint64_t)s;
    } else {
        double d = 0; JS_ToFloat64(ctx, &d, argv[1]); data = (uint64_t)d;
    }
    s_world->setUserData(id, data);
    return JS_UNDEFINED;
}

static JSValue js_physics_getUserData(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = PhysicsBindings::bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    return JS_NewBigUint64(ctx, s_world->getUserData(id));
}

// ---------------------------------------------------------------------------
// Raycast
// ---------------------------------------------------------------------------

static JSValue js_physics_raycast(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 6) return JS_NewArray(ctx);
    double ox, oy, oz, dx, dy, dz;
    JS_ToFloat64(ctx, &ox, argv[0]); JS_ToFloat64(ctx, &oy, argv[1]); JS_ToFloat64(ctx, &oz, argv[2]);
    JS_ToFloat64(ctx, &dx, argv[3]); JS_ToFloat64(ctx, &dy, argv[4]); JS_ToFloat64(ctx, &dz, argv[5]);
    double maxDist = 1000.0;
    if (argc >= 7) JS_ToFloat64(ctx, &maxDist, argv[6]);

    auto hits = s_world->raycast(
        JPH::RVec3((float)ox, (float)oy, (float)oz),
        JPH::Vec3((float)dx, (float)dy, (float)dz),
        (float)maxDist);

    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (auto& hit : hits) {
        JSValue obj = JS_NewObject(ctx);
        int32_t tag = PhysicsBindings::tagForBodyId(hit.bodyID);
        JS_SetPropertyStr(ctx, obj, "bodyId", JS_NewInt32(ctx, tag));
        JS_SetPropertyStr(ctx, obj, "fraction", JS_NewFloat64(ctx, hit.fraction));
        JS_SetPropertyStr(ctx, obj, "userData",
            JS_NewBigUint64(ctx, hit.bodyID.IsInvalid() ? 0 : s_world->getUserData(hit.bodyID)));

        JSValue posObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, posObj, "x", JS_NewFloat64(ctx, hit.position.GetX()));
        JS_SetPropertyStr(ctx, posObj, "y", JS_NewFloat64(ctx, hit.position.GetY()));
        JS_SetPropertyStr(ctx, posObj, "z", JS_NewFloat64(ctx, hit.position.GetZ()));
        JS_SetPropertyStr(ctx, obj, "position", posObj);

        JS_SetPropertyUint32(ctx, arr, i++, obj);
    }
    return arr;
}

// ---------------------------------------------------------------------------
// Contacts (now with sensor flag and userData)
// ---------------------------------------------------------------------------

static JSValue js_physics_getContacts(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!s_world) return JS_NewArray(ctx);

    auto events = s_world->drainContactEvents();
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (auto& e : events) {
        JSValue obj = JS_NewObject(ctx);
        const char* typeStr = (e.type == physics::ContactEvent::Added) ? "added" : "removed";
        JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, typeStr));

        int32_t t1 = PhysicsBindings::tagForBodyId(e.body1);
        int32_t t2 = PhysicsBindings::tagForBodyId(e.body2);
        JS_SetPropertyStr(ctx, obj, "body1", JS_NewInt32(ctx, t1));
        JS_SetPropertyStr(ctx, obj, "body2", JS_NewInt32(ctx, t2));
        JS_SetPropertyStr(ctx, obj, "sensor", JS_NewBool(ctx, e.isSensor));

        JS_SetPropertyUint32(ctx, arr, i++, obj);
    }
    return arr;
}

// ---------------------------------------------------------------------------
// Time step / activation
// ---------------------------------------------------------------------------

static JSValue js_physics_setTimeStep(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1) return JS_UNDEFINED;
    double dt; JS_ToFloat64(ctx, &dt, argv[0]);
    s_world->setTimeStep((float)dt);
    return JS_UNDEFINED;
}

static JSValue js_physics_isActive(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1) return JS_FALSE;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = PhysicsBindings::bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_FALSE;
    return JS_NewBool(ctx, s_world->isActive(id));
}

static JSValue js_physics_activate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1) return JS_UNDEFINED;
    int32_t tag; JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id = PhysicsBindings::bodyIdForTag(tag);
    if (id.IsInvalid()) return JS_UNDEFINED;
    s_world->activate(id);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Bulk transforms
// ---------------------------------------------------------------------------

static JSValue js_physics_getAllTransforms(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!s_world) {
        return JS_NewArrayBufferCopy(ctx, nullptr, 0);
    }

    size_t count = s_bodyTags.size();
    size_t stride = 8;
    std::vector<float> buf(count * stride);

    size_t i = 0;
    auto& bi = s_world->system().GetBodyInterfaceNoLock();
    for (auto& [key, tag] : s_bodyTags) {
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

// ---------------------------------------------------------------------------
// Constraints
// ---------------------------------------------------------------------------

static JSValue js_physics_createConstraint(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "Physics.createConstraint(opts) requires an object");

    JSValueConst o = argv[0];
    physics::ConstraintOptions cs;

    std::string type = jsGetString(ctx, o, "type");
    if (type == "distance") cs.type = physics::ConstraintOptions::Distance;
    else if (type == "point") cs.type = physics::ConstraintOptions::Point;
    else if (type == "hinge") cs.type = physics::ConstraintOptions::Hinge;
    else if (type == "fixed") cs.type = physics::ConstraintOptions::Fixed;
    else if (type == "slider") cs.type = physics::ConstraintOptions::Slider;
    else return JS_ThrowTypeError(ctx, "constraint type required (distance|point|hinge|fixed|slider)");

    JSValue b1v = JS_GetPropertyStr(ctx, o, "body1");
    JSValue b2v = JS_GetPropertyStr(ctx, o, "body2");
    int32_t t1 = -1, t2 = -1;
    if (JS_IsNumber(b1v)) JS_ToInt32(ctx, &t1, b1v);
    if (JS_IsNumber(b2v)) JS_ToInt32(ctx, &t2, b2v);
    JS_FreeValue(ctx, b1v);
    JS_FreeValue(ctx, b2v);

    cs.body1 = PhysicsBindings::bodyIdForTag(t1);
    cs.body2 = PhysicsBindings::bodyIdForTag(t2);
    if (cs.body1.IsInvalid())
        return JS_ThrowTypeError(ctx, "constraint body1 tag is invalid");
    // body2 may be invalid → attach to world

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

    uint32_t handle = s_world->createConstraint(cs);
    if (!handle) return JS_ThrowInternalError(ctx, "Failed to create constraint");
    return JS_NewUint32(ctx, handle);
}

static JSValue js_physics_destroyConstraint(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1) return JS_UNDEFINED;
    uint32_t h; JS_ToUint32(ctx, &h, argv[0]);
    s_world->destroyConstraint(h);
    return JS_UNDEFINED;
}

static JSValue js_physics_setConstraintEnabled(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 2) return JS_UNDEFINED;
    uint32_t h; JS_ToUint32(ctx, &h, argv[0]);
    bool en = JS_ToBool(ctx, argv[1]);
    s_world->setConstraintEnabled(h, en);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void PhysicsBindings::install(JSContext* ctx, physics::PhysicsWorld* world) {
    s_world = world;
    s_bodyTags.clear();
    s_tagToBody.clear();
    s_nextTag = 1;

    qjsbind::Namespace(ctx, "Physics")
        .function("createWorld", js_physics_createWorld, 1)
        .function("setGravity", js_physics_setGravity, 3)
        .function("getGravity", js_physics_getGravity, 0)
        .function("setLayers", js_physics_setLayers, 1)
        .function("createBody", js_physics_createBody, 1)
        .function("destroyBody", js_physics_destroyBody, 1)
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
        .function("raycast", js_physics_raycast, 7)
        .function("getContacts", js_physics_getContacts, 0)
        .function("setTimeStep", js_physics_setTimeStep, 1)
        .function("isActive", js_physics_isActive, 1)
        .function("activate", js_physics_activate, 1)
        .function("getAllTransforms", js_physics_getAllTransforms, 0)
        .function("createConstraint", js_physics_createConstraint, 1)
        .function("destroyConstraint", js_physics_destroyConstraint, 1)
        .function("setConstraintEnabled", js_physics_setConstraintEnabled, 2);
}

void PhysicsBindings::cleanup(JSContext* ctx) {
    if (ctx) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSAtom atom = JS_NewAtom(ctx, "Physics");
        JS_DeleteProperty(ctx, global, atom, 0);
        JS_FreeAtom(ctx, atom);
        JS_FreeValue(ctx, global);
    }
    s_world = nullptr;
    s_bodyTags.clear();
    s_tagToBody.clear();
    s_nextTag = 1;
}

} // namespace bro::js
