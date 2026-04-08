#include "js/physics_bindings.h"
#include "js/runtime.h"
#include "physics/physics_world.h"
#include "util/log.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

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
static int32_t s_nextTag = 1;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static double jsGetNum(JSContext* ctx, JSValueConst obj, const char* prop, double def = 0.0) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    double r = def;
    if (!JS_IsUndefined(v)) JS_ToFloat64(ctx, &r, v);
    JS_FreeValue(ctx, v);
    return r;
}

static bool jsGetBool(JSContext* ctx, JSValueConst obj, const char* prop, bool def = false) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    bool r = def;
    if (!JS_IsUndefined(v)) r = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return r;
}

// ---------------------------------------------------------------------------
// Physics.createWorld(opts?) — initialize the physics system
// ---------------------------------------------------------------------------

static JSValue js_physics_createWorld(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world) return JS_ThrowInternalError(ctx, "Physics not available");
    if (s_world->isIdle()) {
        int maxBodies = 4096;
        if (argc > 0 && JS_IsObject(argv[0])) {
            maxBodies = (int)jsGetNum(ctx, argv[0], "maxBodies", 4096);
        }
        // World already initialized by engine — just allow reconfiguration of gravity etc.
    }
    return JS_TRUE;
}

// ---------------------------------------------------------------------------
// Physics.setGravity(x, y, z)
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

// ---------------------------------------------------------------------------
// Physics.getGravity() → {x, y, z}
// ---------------------------------------------------------------------------

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
// Physics.createBody(opts) → bodyId (integer tag)
//
// opts: {
//   shape: "box" | "sphere" | "capsule" | "cylinder",
//   position: {x, y, z},
//   rotation: {x, y, z, w},   // quaternion, default identity
//   // Shape-specific:
//   halfExtents: {x, y, z},   // box
//   radius: number,            // sphere, capsule, cylinder
//   halfHeight: number,        // capsule, cylinder
//   static: boolean,           // default false (dynamic)
//   friction: number,          // default 0.5
//   restitution: number,       // default 0.3
// }
// ---------------------------------------------------------------------------

static JSValue js_physics_createBody(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "Physics.createBody(opts) requires an object");

    JSValueConst opts = argv[0];

    // Shape type
    JSValue shapeVal = JS_GetPropertyStr(ctx, opts, "shape");
    const char* shapeStr = JS_ToCString(ctx, shapeVal);
    JS_FreeValue(ctx, shapeVal);
    if (!shapeStr) return JS_ThrowTypeError(ctx, "shape is required");
    std::string shape(shapeStr);
    JS_FreeCString(ctx, shapeStr);

    // Position
    JSValue posVal = JS_GetPropertyStr(ctx, opts, "position");
    float px = 0, py = 0, pz = 0;
    if (JS_IsObject(posVal)) {
        px = (float)jsGetNum(ctx, posVal, "x");
        py = (float)jsGetNum(ctx, posVal, "y");
        pz = (float)jsGetNum(ctx, posVal, "z");
    }
    JS_FreeValue(ctx, posVal);

    // Rotation (quaternion)
    JSValue rotVal = JS_GetPropertyStr(ctx, opts, "rotation");
    float rx = 0, ry = 0, rz = 0, rw = 1;
    if (JS_IsObject(rotVal)) {
        rx = (float)jsGetNum(ctx, rotVal, "x");
        ry = (float)jsGetNum(ctx, rotVal, "y");
        rz = (float)jsGetNum(ctx, rotVal, "z");
        rw = (float)jsGetNum(ctx, rotVal, "w", 1.0);
    }
    JS_FreeValue(ctx, rotVal);

    bool isStatic = jsGetBool(ctx, opts, "static", false);
    float friction = (float)jsGetNum(ctx, opts, "friction", 0.5);
    float restitution = (float)jsGetNum(ctx, opts, "restitution", 0.3);

    JPH::RVec3 pos(px, py, pz);
    JPH::Quat rot = JPH::Quat(rx, ry, rz, rw).Normalized();
    JPH::BodyID id;

    if (shape == "box") {
        JSValue he = JS_GetPropertyStr(ctx, opts, "halfExtents");
        float hx = 0.5f, hy = 0.5f, hz = 0.5f;
        if (JS_IsObject(he)) {
            hx = (float)jsGetNum(ctx, he, "x", 0.5);
            hy = (float)jsGetNum(ctx, he, "y", 0.5);
            hz = (float)jsGetNum(ctx, he, "z", 0.5);
        }
        JS_FreeValue(ctx, he);
        id = s_world->createBox(pos, rot, JPH::Vec3(hx, hy, hz), isStatic, friction, restitution);
    } else if (shape == "sphere") {
        float radius = (float)jsGetNum(ctx, opts, "radius", 0.5);
        id = s_world->createSphere(pos, rot, radius, isStatic, friction, restitution);
    } else if (shape == "capsule") {
        float halfHeight = (float)jsGetNum(ctx, opts, "halfHeight", 0.5);
        float radius = (float)jsGetNum(ctx, opts, "radius", 0.25);
        id = s_world->createCapsule(pos, rot, halfHeight, radius, isStatic, friction, restitution);
    } else if (shape == "cylinder") {
        float halfHeight = (float)jsGetNum(ctx, opts, "halfHeight", 0.5);
        float radius = (float)jsGetNum(ctx, opts, "radius", 0.5);
        id = s_world->createCylinder(pos, rot, halfHeight, radius, isStatic, friction, restitution);
    } else {
        return JS_ThrowTypeError(ctx, "Unknown shape: %s", shape.c_str());
    }

    if (id.IsInvalid())
        return JS_ThrowInternalError(ctx, "Failed to create body");

    int32_t tag = s_nextTag++;
    s_bodyTags[id.GetIndexAndSequenceNumber()] = tag;
    return JS_NewInt32(ctx, tag);
}

// ---------------------------------------------------------------------------
// Lookup BodyID from tag
// ---------------------------------------------------------------------------

static bool findBodyID(int32_t tag, JPH::BodyID& outID) {
    for (auto& [key, t] : s_bodyTags) {
        if (t == tag) {
            outID = JPH::BodyID(key);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Physics.destroyBody(tag)
// ---------------------------------------------------------------------------

static JSValue js_physics_destroyBody(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1) return JS_UNDEFINED;
    int32_t tag;
    JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id;
    if (!findBodyID(tag, id)) return JS_UNDEFINED;
    s_world->destroyBody(id);
    s_bodyTags.erase(id.GetIndexAndSequenceNumber());
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Physics.getTransform(tag) → {position: {x,y,z}, rotation: {x,y,z,w}}
// ---------------------------------------------------------------------------

static JSValue js_physics_getTransform(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1) return JS_UNDEFINED;
    int32_t tag;
    JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id;
    if (!findBodyID(tag, id)) return JS_UNDEFINED;

    auto pos = s_world->getPosition(id);
    auto rot = s_world->getRotation(id);

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

    return result;
}

// ---------------------------------------------------------------------------
// Physics.getVelocity(tag) → {linear: {x,y,z}, angular: {x,y,z}}
// ---------------------------------------------------------------------------

static JSValue js_physics_getVelocity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1) return JS_UNDEFINED;
    int32_t tag;
    JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id;
    if (!findBodyID(tag, id)) return JS_UNDEFINED;

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
// Physics.setPosition(tag, x, y, z)
// ---------------------------------------------------------------------------

static JSValue js_physics_setPosition(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 4) return JS_UNDEFINED;
    int32_t tag; double x, y, z;
    JS_ToInt32(ctx, &tag, argv[0]);
    JS_ToFloat64(ctx, &x, argv[1]);
    JS_ToFloat64(ctx, &y, argv[2]);
    JS_ToFloat64(ctx, &z, argv[3]);
    JPH::BodyID id;
    if (!findBodyID(tag, id)) return JS_UNDEFINED;
    s_world->setPosition(id, JPH::RVec3((float)x, (float)y, (float)z));
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Physics.setRotation(tag, x, y, z, w)
// ---------------------------------------------------------------------------

static JSValue js_physics_setRotation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 5) return JS_UNDEFINED;
    int32_t tag; double rx, ry, rz, rw;
    JS_ToInt32(ctx, &tag, argv[0]);
    JS_ToFloat64(ctx, &rx, argv[1]);
    JS_ToFloat64(ctx, &ry, argv[2]);
    JS_ToFloat64(ctx, &rz, argv[3]);
    JS_ToFloat64(ctx, &rw, argv[4]);
    JPH::BodyID id;
    if (!findBodyID(tag, id)) return JS_UNDEFINED;
    s_world->setRotation(id, JPH::Quat((float)rx, (float)ry, (float)rz, (float)rw).Normalized());
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Physics.setLinearVelocity(tag, x, y, z)
// ---------------------------------------------------------------------------

static JSValue js_physics_setLinearVelocity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 4) return JS_UNDEFINED;
    int32_t tag; double x, y, z;
    JS_ToInt32(ctx, &tag, argv[0]);
    JS_ToFloat64(ctx, &x, argv[1]);
    JS_ToFloat64(ctx, &y, argv[2]);
    JS_ToFloat64(ctx, &z, argv[3]);
    JPH::BodyID id;
    if (!findBodyID(tag, id)) return JS_UNDEFINED;
    s_world->setLinearVelocity(id, JPH::Vec3((float)x, (float)y, (float)z));
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Physics.addForce(tag, x, y, z)
// ---------------------------------------------------------------------------

static JSValue js_physics_addForce(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 4) return JS_UNDEFINED;
    int32_t tag; double x, y, z;
    JS_ToInt32(ctx, &tag, argv[0]);
    JS_ToFloat64(ctx, &x, argv[1]);
    JS_ToFloat64(ctx, &y, argv[2]);
    JS_ToFloat64(ctx, &z, argv[3]);
    JPH::BodyID id;
    if (!findBodyID(tag, id)) return JS_UNDEFINED;
    s_world->addForce(id, JPH::Vec3((float)x, (float)y, (float)z));
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Physics.addImpulse(tag, x, y, z)
// ---------------------------------------------------------------------------

static JSValue js_physics_addImpulse(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 4) return JS_UNDEFINED;
    int32_t tag; double x, y, z;
    JS_ToInt32(ctx, &tag, argv[0]);
    JS_ToFloat64(ctx, &x, argv[1]);
    JS_ToFloat64(ctx, &y, argv[2]);
    JS_ToFloat64(ctx, &z, argv[3]);
    JPH::BodyID id;
    if (!findBodyID(tag, id)) return JS_UNDEFINED;
    s_world->addImpulse(id, JPH::Vec3((float)x, (float)y, (float)z));
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Physics.addTorque(tag, x, y, z)
// ---------------------------------------------------------------------------

static JSValue js_physics_addTorque(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 4) return JS_UNDEFINED;
    int32_t tag; double x, y, z;
    JS_ToInt32(ctx, &tag, argv[0]);
    JS_ToFloat64(ctx, &x, argv[1]);
    JS_ToFloat64(ctx, &y, argv[2]);
    JS_ToFloat64(ctx, &z, argv[3]);
    JPH::BodyID id;
    if (!findBodyID(tag, id)) return JS_UNDEFINED;
    s_world->addTorque(id, JPH::Vec3((float)x, (float)y, (float)z));
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Physics.raycast(ox, oy, oz, dx, dy, dz, maxDist?) → [{bodyId, fraction, position}]
// ---------------------------------------------------------------------------

static JSValue js_physics_raycast(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 6) return JS_NewArray(ctx);
    double ox, oy, oz, dx, dy, dz;
    JS_ToFloat64(ctx, &ox, argv[0]);
    JS_ToFloat64(ctx, &oy, argv[1]);
    JS_ToFloat64(ctx, &oz, argv[2]);
    JS_ToFloat64(ctx, &dx, argv[3]);
    JS_ToFloat64(ctx, &dy, argv[4]);
    JS_ToFloat64(ctx, &dz, argv[5]);
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
        // Find the tag for this bodyID
        auto it = s_bodyTags.find(hit.bodyID.GetIndexAndSequenceNumber());
        int32_t tag = (it != s_bodyTags.end()) ? it->second : -1;
        JS_SetPropertyStr(ctx, obj, "bodyId", JS_NewInt32(ctx, tag));
        JS_SetPropertyStr(ctx, obj, "fraction", JS_NewFloat64(ctx, hit.fraction));

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
// Physics.getContacts() → [{type, body1, body2}]
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

        auto it1 = s_bodyTags.find(e.body1.GetIndexAndSequenceNumber());
        auto it2 = s_bodyTags.find(e.body2.GetIndexAndSequenceNumber());
        JS_SetPropertyStr(ctx, obj, "body1", JS_NewInt32(ctx, it1 != s_bodyTags.end() ? it1->second : -1));
        JS_SetPropertyStr(ctx, obj, "body2", JS_NewInt32(ctx, it2 != s_bodyTags.end() ? it2->second : -1));

        JS_SetPropertyUint32(ctx, arr, i++, obj);
    }
    return arr;
}

// ---------------------------------------------------------------------------
// Physics.setTimeStep(dt)
// ---------------------------------------------------------------------------

static JSValue js_physics_setTimeStep(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1) return JS_UNDEFINED;
    double dt;
    JS_ToFloat64(ctx, &dt, argv[0]);
    s_world->setTimeStep((float)dt);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Physics.isActive(tag) → boolean
// ---------------------------------------------------------------------------

static JSValue js_physics_isActive(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1) return JS_FALSE;
    int32_t tag;
    JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id;
    if (!findBodyID(tag, id)) return JS_FALSE;
    return JS_NewBool(ctx, s_world->isActive(id));
}

// ---------------------------------------------------------------------------
// Physics.activate(tag)
// ---------------------------------------------------------------------------

static JSValue js_physics_activate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!s_world || argc < 1) return JS_UNDEFINED;
    int32_t tag;
    JS_ToInt32(ctx, &tag, argv[0]);
    JPH::BodyID id;
    if (!findBodyID(tag, id)) return JS_UNDEFINED;
    s_world->activate(id);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Physics.getAllTransforms() → Float32Array
// Packed as [id, x, y, z, rx, ry, rz, rw, id, x, y, z, ...] — 8 floats per body.
// Single allocation, no per-body JS objects.
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
        p[1] = pos.GetX();
        p[2] = pos.GetY();
        p[3] = pos.GetZ();
        p[4] = rot.GetX();
        p[5] = rot.GetY();
        p[6] = rot.GetZ();
        p[7] = rot.GetW();
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
// Install
// ---------------------------------------------------------------------------

void PhysicsBindings::install(JSContext* ctx, physics::PhysicsWorld* world) {
    s_world = world;
    s_bodyTags.clear();
    s_nextTag = 1;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue physics = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, physics, "createWorld",
        JS_NewCFunction(ctx, js_physics_createWorld, "createWorld", 1));
    JS_SetPropertyStr(ctx, physics, "setGravity",
        JS_NewCFunction(ctx, js_physics_setGravity, "setGravity", 3));
    JS_SetPropertyStr(ctx, physics, "getGravity",
        JS_NewCFunction(ctx, js_physics_getGravity, "getGravity", 0));
    JS_SetPropertyStr(ctx, physics, "createBody",
        JS_NewCFunction(ctx, js_physics_createBody, "createBody", 1));
    JS_SetPropertyStr(ctx, physics, "destroyBody",
        JS_NewCFunction(ctx, js_physics_destroyBody, "destroyBody", 1));
    JS_SetPropertyStr(ctx, physics, "getTransform",
        JS_NewCFunction(ctx, js_physics_getTransform, "getTransform", 1));
    JS_SetPropertyStr(ctx, physics, "getVelocity",
        JS_NewCFunction(ctx, js_physics_getVelocity, "getVelocity", 1));
    JS_SetPropertyStr(ctx, physics, "setPosition",
        JS_NewCFunction(ctx, js_physics_setPosition, "setPosition", 4));
    JS_SetPropertyStr(ctx, physics, "setRotation",
        JS_NewCFunction(ctx, js_physics_setRotation, "setRotation", 5));
    JS_SetPropertyStr(ctx, physics, "setLinearVelocity",
        JS_NewCFunction(ctx, js_physics_setLinearVelocity, "setLinearVelocity", 4));
    JS_SetPropertyStr(ctx, physics, "addForce",
        JS_NewCFunction(ctx, js_physics_addForce, "addForce", 4));
    JS_SetPropertyStr(ctx, physics, "addImpulse",
        JS_NewCFunction(ctx, js_physics_addImpulse, "addImpulse", 4));
    JS_SetPropertyStr(ctx, physics, "addTorque",
        JS_NewCFunction(ctx, js_physics_addTorque, "addTorque", 4));
    JS_SetPropertyStr(ctx, physics, "raycast",
        JS_NewCFunction(ctx, js_physics_raycast, "raycast", 7));
    JS_SetPropertyStr(ctx, physics, "getContacts",
        JS_NewCFunction(ctx, js_physics_getContacts, "getContacts", 0));
    JS_SetPropertyStr(ctx, physics, "setTimeStep",
        JS_NewCFunction(ctx, js_physics_setTimeStep, "setTimeStep", 1));
    JS_SetPropertyStr(ctx, physics, "isActive",
        JS_NewCFunction(ctx, js_physics_isActive, "isActive", 1));
    JS_SetPropertyStr(ctx, physics, "activate",
        JS_NewCFunction(ctx, js_physics_activate, "activate", 1));
    JS_SetPropertyStr(ctx, physics, "getAllTransforms",
        JS_NewCFunction(ctx, js_physics_getAllTransforms, "getAllTransforms", 0));

    JS_SetPropertyStr(ctx, global, "Physics", physics);
    JS_FreeValue(ctx, global);
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
    s_nextTag = 1;
}

} // namespace bro::js
