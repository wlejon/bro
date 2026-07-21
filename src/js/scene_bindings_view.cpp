// Scene JS bindings — scene-wide view state on the SceneGraph: camera,
// lights, environment/IBL, tone mapping and post-FX (fog, tilt-shift, bloom,
// render scale, MSAA), picking (raycast / unprojectLocal), and frame capture
// (toImageData / captureFrame / asTexture). Shared wrapper structs + helpers
// live in scene_bindings_internal.h.

#include "js/scene_bindings.h"
#if BRO_WITH_3D  // modular-build feature gate
#include "js/scene_bindings_internal.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/mesh_node.h"
#include "scene/light_node.h"

#include <qjsbind/qjsbind.h>

#include <bromesh/analysis/raycast.h>
#include <bromesh/analysis/bvh.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace bro::js {

// raycast(origin, direction, maxDistance) → { hit, point, normal, distance, node } | null
JSValue js_sg_raycast(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 2) return JS_NULL;

    auto parseVec3 = [&](JSValueConst v, bromath::Vec3& out) -> bool {
        if (!JS_IsArray(v)) return false;
        JSValue ex = JS_GetPropertyUint32(ctx, v, 0);
        JSValue ey = JS_GetPropertyUint32(ctx, v, 1);
        JSValue ez = JS_GetPropertyUint32(ctx, v, 2);
        double x = 0, y = 0, z = 0;
        bool ok = !JS_ToFloat64(ctx, &x, ex)
               && !JS_ToFloat64(ctx, &y, ey)
               && !JS_ToFloat64(ctx, &z, ez);
        JS_FreeValue(ctx, ex);
        JS_FreeValue(ctx, ey);
        JS_FreeValue(ctx, ez);
        if (!ok) return false;
        out = {(float)x, (float)y, (float)z};
        return true;
    };

    bromath::Vec3 origin, dir;
    if (!parseVec3(argv[0], origin)) return JS_ThrowTypeError(ctx, "raycast: origin must be [x,y,z]");
    if (!parseVec3(argv[1], dir))    return JS_ThrowTypeError(ctx, "raycast: direction must be [x,y,z]");

    double maxDist = 0.0;
    if (argc >= 3) JS_ToFloat64(ctx, &maxDist, argv[2]);

    dir = bromath::vnorm(dir);
    if (bromath::vlen2(dir) < 1e-12f) return JS_NULL;

    float closestDist = (maxDist > 0.0) ? (float)maxDist : 1e30f;
    scene::MeshNode* closestNode = nullptr;
    scene::LightNode* closestLight = nullptr;
    bromesh::RayHit closestHit;
    bromath::Vec3 closestWorldPoint;
    bromath::Vec3 closestWorldNormal;

    g->root()->traverse([&](scene::SceneNode* node) {
        if (!node || node->type() != scene::SceneNode::Type::Mesh) return;
        if (!node->visible()) return;
        auto* mn = static_cast<scene::MeshNode*>(node);
        const bromesh::MeshData& md = mn->mesh();
        if (md.positions.empty() || md.indices.empty()) return;

        const bromath::Vec3& nodePos = node->position();
        const bromath::Quat& nodeRot = node->rotation();
        const bromath::Vec3& nodeScl = node->scale();

        bromath::Vec3 localOrigin = origin - nodePos;
        localOrigin = bromath::qrotate(bromath::qconjugate(nodeRot), localOrigin);
        if (nodeScl.x != 0.0f) localOrigin.x /= nodeScl.x;
        if (nodeScl.y != 0.0f) localOrigin.y /= nodeScl.y;
        if (nodeScl.z != 0.0f) localOrigin.z /= nodeScl.z;

        bromath::Vec3 localDir = bromath::qrotate(bromath::qconjugate(nodeRot), dir);
        if (nodeScl.x != 0.0f) localDir.x /= nodeScl.x;
        if (nodeScl.y != 0.0f) localDir.y /= nodeScl.y;
        if (nodeScl.z != 0.0f) localDir.z /= nodeScl.z;

        float localDirLen = bromath::vlen(localDir);
        if (localDirLen < 1e-12f) return;
        bromath::Vec3 localDirN = localDir * (1.0f / localDirLen);
        float scale = nodeScl.x != 0.0f ? nodeScl.x : 1.0f;
        float localMaxDist = closestDist / scale;

        // Early-out: local-space AABB slab test
        {
            const bromath::AABB3& lb = mn->localBounds();
            float bmin[3] = { lb.min.x, lb.min.y, lb.min.z };
            float bmax[3] = { lb.max.x, lb.max.y, lb.max.z };
            float invD[3];
            for (int a = 0; a < 3; ++a) {
                float dv = (&localDirN.x)[a];
                invD[a] = (std::fabs(dv) > 1e-30f) ? 1.0f / dv
                                                    : (dv >= 0.0f ?  1e30f : -1e30f);
            }
            float o[3] = { localOrigin.x, localOrigin.y, localOrigin.z };
            float tmin = -1e30f, tmax = 1e30f;
            for (int a = 0; a < 3; ++a) {
                float t1 = (bmin[a] - o[a]) * invD[a];
                float t2 = (bmax[a] - o[a]) * invD[a];
                float lo = t1 < t2 ? t1 : t2;
                float hi = t1 < t2 ? t2 : t1;
                if (lo > tmin) tmin = lo;
                if (hi < tmax) tmax = hi;
            }
            if (tmax < 0.0f || tmin > tmax || tmin > localMaxDist) return;
        }

        float o[3] = { localOrigin.x, localOrigin.y, localOrigin.z };
        float d[3] = { localDirN.x, localDirN.y, localDirN.z };
        bromesh::RayHit hit = mn->bvh().raycast(md, o, d, localMaxDist);
        if (!hit.hit) return;

        bromath::Vec3 localHit{hit.position[0], hit.position[1], hit.position[2]};
        localHit.x *= nodeScl.x;
        localHit.y *= nodeScl.y;
        localHit.z *= nodeScl.z;
        bromath::Vec3 worldHit = bromath::qrotate(nodeRot, localHit) + nodePos;

        bromath::Vec3 toHit = worldHit - origin;
        float worldDist = bromath::vlen(toHit);
        if (worldDist >= closestDist) return;

        bromath::Vec3 localNormal{hit.normal[0], hit.normal[1], hit.normal[2]};
        bromath::Vec3 worldNormal = bromath::vnorm(bromath::qrotate(nodeRot, localNormal));

        closestDist = worldDist;
        closestNode = mn;
        closestLight = nullptr;
        closestHit = hit;
        closestWorldPoint = worldHit;
        closestWorldNormal = worldNormal;
    });

    // Light marker icons are also pickable when showLightIcons is on —
    // treat each as a world-space sphere at the node position matching
    // the largest icon half-extent (directional icon = 0.30). Keeps
    // selection forgiving without needing screen-space math.
    if (g->showLightIcons()) {
        const float lightRadius = 0.32f;
        g->root()->traverse([&](scene::SceneNode* node) {
            if (!node || node->type() != scene::SceneNode::Type::Light) return;
            if (!node->visible()) return;
            const bromath::Mat4& M = node->worldMatrix();
            bromath::Vec3 c{M.at(0, 3), M.at(1, 3), M.at(2, 3)};
            bromath::Vec3 oc = origin - c;
            float b = bromath::vdot(oc, dir);
            float disc = b * b - bromath::vdot(oc, oc) + lightRadius * lightRadius;
            if (disc < 0.0f) return;
            float sq = std::sqrt(disc);
            float t = -b - sq;
            if (t < 0.0f) t = -b + sq;   // origin inside — hit far face
            if (t < 0.0f || t >= closestDist) return;

            closestDist = t;
            closestLight = static_cast<scene::LightNode*>(node);
            closestNode = nullptr;
            closestWorldPoint = origin + dir * t;
            closestWorldNormal = bromath::vnorm(closestWorldPoint - c);
        });
    }

    if (!closestNode && !closestLight) return JS_NULL;

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "hit", JS_TRUE);
    JS_SetPropertyStr(ctx, out, "distance", JS_NewFloat64(ctx, closestDist));

    JSValue position = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, position, 0, JS_NewFloat64(ctx, closestWorldPoint.x));
    JS_SetPropertyUint32(ctx, position, 1, JS_NewFloat64(ctx, closestWorldPoint.y));
    JS_SetPropertyUint32(ctx, position, 2, JS_NewFloat64(ctx, closestWorldPoint.z));
    JS_SetPropertyStr(ctx, out, "position", position);
    JSValue point = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, point, 0, JS_NewFloat64(ctx, closestWorldPoint.x));
    JS_SetPropertyUint32(ctx, point, 1, JS_NewFloat64(ctx, closestWorldPoint.y));
    JS_SetPropertyUint32(ctx, point, 2, JS_NewFloat64(ctx, closestWorldPoint.z));
    JS_SetPropertyStr(ctx, out, "point", point);

    JSValue normal = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, normal, 0, JS_NewFloat64(ctx, closestWorldNormal.x));
    JS_SetPropertyUint32(ctx, normal, 1, JS_NewFloat64(ctx, closestWorldNormal.y));
    JS_SetPropertyUint32(ctx, normal, 2, JS_NewFloat64(ctx, closestWorldNormal.z));
    JS_SetPropertyStr(ctx, out, "normal", normal);

    scene::SceneNode* hitNode = closestNode
        ? static_cast<scene::SceneNode*>(closestNode)
        : static_cast<scene::SceneNode*>(closestLight);
    JS_SetPropertyStr(ctx, out, "node", wrapNode(ctx, hitNode, g));

    return out;
}

// --- Helper: parse a [x, y, z] array into Vec3 ---
static bromath::Vec3 jsGetVec3(JSContext* ctx, JSValueConst obj, const char* prop,
                             float dx = 0, float dy = 0, float dz = 0) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    bromath::Vec3 r{dx, dy, dz};
    if (JS_IsArray(v)) {
        JSValue e0 = JS_GetPropertyUint32(ctx, v, 0);
        JSValue e1 = JS_GetPropertyUint32(ctx, v, 1);
        JSValue e2 = JS_GetPropertyUint32(ctx, v, 2);
        double tx = dx, ty = dy, tz = dz;
        JS_ToFloat64(ctx, &tx, e0);
        JS_ToFloat64(ctx, &ty, e1);
        JS_ToFloat64(ctx, &tz, e2);
        r = {(float)tx, (float)ty, (float)tz};
        JS_FreeValue(ctx, e0);
        JS_FreeValue(ctx, e1);
        JS_FreeValue(ctx, e2);
    }
    JS_FreeValue(ctx, v);
    return r;
}

// Parse a [x, y, z, w] array into Quat
static bromath::Quat jsGetQuat(JSContext* ctx, JSValueConst obj, const char* prop, bool& found) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    bromath::Quat r{0, 0, 0, 1};
    found = false;
    if (JS_IsArray(v)) {
        found = true;
        JSValue e0 = JS_GetPropertyUint32(ctx, v, 0);
        JSValue e1 = JS_GetPropertyUint32(ctx, v, 1);
        JSValue e2 = JS_GetPropertyUint32(ctx, v, 2);
        JSValue e3 = JS_GetPropertyUint32(ctx, v, 3);
        double qx = 0, qy = 0, qz = 0, qw = 1;
        JS_ToFloat64(ctx, &qx, e0);
        JS_ToFloat64(ctx, &qy, e1);
        JS_ToFloat64(ctx, &qz, e2);
        JS_ToFloat64(ctx, &qw, e3);
        r = {(float)qx, (float)qy, (float)qz, (float)qw};
        JS_FreeValue(ctx, e0);
        JS_FreeValue(ctx, e1);
        JS_FreeValue(ctx, e2);
        JS_FreeValue(ctx, e3);
    }
    JS_FreeValue(ctx, v);
    return r;
}

// setCamera({fov, near, far, aspect, position, target|quaternion, up})
JSValue js_sg_setCamera(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

    JSValueConst opts = argv[0];
    double fov = qjsbind::get_prop_number(ctx, opts, "fov", 60.0) * 3.14159265 / 180.0;
    double nearZ = qjsbind::get_prop_number(ctx, opts, "near", 0.1);
    double farZ = qjsbind::get_prop_number(ctx, opts, "far", 1000.0);
    double aspect = qjsbind::get_prop_number(ctx, opts, "aspect", 0.0);

    // Aspect omitted → derive from current canvas and flag the projection
    // to auto-follow on future canvas resizes (setCanvasSize rebuilds it).
    // Explicit aspect pins the projection and disables the follow behavior.
    bool aspectFollowsCanvas = (aspect <= 0);
    if (aspectFollowsCanvas) {
        int cw = g->canvasWidth(), ch = g->canvasHeight();
        aspect = (cw > 0 && ch > 0) ? double(cw) / double(ch) : 4.0 / 3.0;
    }
    g->setCameraAspectFollowsCanvas(aspectFollowsCanvas);

    bromath::Vec3 position = jsGetVec3(ctx, opts, "position", 0, 5, -10);

    bool hasQuat = false;
    bromath::Quat quat = jsGetQuat(ctx, opts, "quaternion", hasQuat);

    if (hasQuat) {
        g->setCameraQuat((float)fov, (float)aspect, (float)nearZ, (float)farZ,
                         position, bromath::qnorm(quat));
    } else {
        bromath::Vec3 target = jsGetVec3(ctx, opts, "target", 0, 0, 0);
        bromath::Vec3 up = jsGetVec3(ctx, opts, "up", 0, 1, 0);

        std::string mode = qjsbind::get_prop_string(ctx, opts, "mode", "perspective");
        if (mode == "orthographic" || mode == "ortho") {
            double size = qjsbind::get_prop_number(ctx, opts, "size", 10.0);
            float halfW = (float)(size * aspect * 0.5);
            float halfH = (float)(size * 0.5);
            g->setCameraOrtho(-halfW, halfW, -halfH, halfH,
                              (float)nearZ, (float)farZ, position, target, up);
        } else {
            g->setCamera((float)fov, (float)aspect, (float)nearZ, (float)farZ,
                         position, target, up);
        }
    }

    return JS_UNDEFINED;
}

// createCamera({ name, fov, near, far, aspect, mode, size, position,
//                quaternion, lookAt }) → CameraNode
// The node's WORLD transform is the view (camera looks down local -Z, +Y
// up); only projection params live on the node. See scene-api.js.
JSValue js_sg_createCamera(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createCamera();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        double fov = qjsbind::get_prop_number(ctx, opts, "fov", 60.0);
        node->setFovY((float)(fov * 3.14159265 / 180.0));
        node->setNearZ((float)qjsbind::get_prop_number(ctx, opts, "near", 0.1));
        node->setFarZ((float)qjsbind::get_prop_number(ctx, opts, "far", 1000.0));
        node->setAspect((float)qjsbind::get_prop_number(ctx, opts, "aspect", 0.0));
        node->setOrthoHeight((float)qjsbind::get_prop_number(ctx, opts, "size", 10.0));

        std::string mode = qjsbind::get_prop_string(ctx, opts, "mode", "perspective");
        node->setPerspective(!(mode == "orthographic" || mode == "ortho"));

        JSValue posVal = JS_GetPropertyStr(ctx, opts, "position");
        if (JS_IsArray(posVal)) {
            bromath::Vec3 p = jsGetVec3(ctx, opts, "position");
            node->setPosition(p);
        }
        JS_FreeValue(ctx, posVal);

        bool hasQuat = false;
        bromath::Quat quat = jsGetQuat(ctx, opts, "quaternion", hasQuat);
        if (hasQuat) {
            node->setRotation(bromath::qnorm(quat));
        } else {
            JSValue laVal = JS_GetPropertyStr(ctx, opts, "lookAt");
            if (JS_IsArray(laVal)) {
                node->lookAt(jsGetVec3(ctx, opts, "lookAt"));
            }
            JS_FreeValue(ctx, laVal);
        }

        JSValue actVal = JS_GetPropertyStr(ctx, opts, "active");
        if (JS_ToBool(ctx, actVal) > 0) g->setActiveCamera(node);
        JS_FreeValue(ctx, actVal);
    }

    return wrapNode(ctx, node, g);
}

// setActiveCamera(cameraNode | null) — activate a camera node (its world
// transform drives the view every frame) or fall back to the last derived
// view with null. Imperative setCamera() calls also deactivate (last call
// wins).
JSValue js_sg_setActiveCamera(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        g->setActiveCamera(nullptr);
        return JS_UNDEFINED;
    }
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, argv[0]);
    scene::SceneNode* n = w ? w->node() : nullptr;
    if (!n || n->type() != scene::SceneNode::Type::Camera)
        return JS_ThrowTypeError(ctx, "setActiveCamera: argument must be a camera node (scene.createCamera) or null");
    g->setActiveCamera(static_cast<scene::CameraNode*>(n));
    return JS_UNDEFINED;
}

// createLight({ type, position, direction, color, intensity, range,
//               innerAngle, outerAngle, name }) → LightNode
JSValue js_sg_createLight(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createLight();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        std::string kindStr = qjsbind::get_prop_string(ctx, opts, "type", "directional");
        if      (kindStr == "point")       node->setKind(scene::LightNode::Kind::Point);
        else if (kindStr == "spot")        node->setKind(scene::LightNode::Kind::Spot);
        else                               node->setKind(scene::LightNode::Kind::Directional);

        // Position lives on the scene node transform (so lights follow
        // parents, gizmos, agents, etc. — same semantics as meshes).
        JSValue posVal = JS_GetPropertyStr(ctx, opts, "position");
        if (JS_IsArray(posVal)) {
            double px = 0, py = 0, pz = 0;
            JSValue e0 = JS_GetPropertyUint32(ctx, posVal, 0);
            JSValue e1 = JS_GetPropertyUint32(ctx, posVal, 1);
            JSValue e2 = JS_GetPropertyUint32(ctx, posVal, 2);
            JS_ToFloat64(ctx, &px, e0);
            JS_ToFloat64(ctx, &py, e1);
            JS_ToFloat64(ctx, &pz, e2);
            node->setPosition((float)px, (float)py, (float)pz);
            JS_FreeValue(ctx, e0);
            JS_FreeValue(ctx, e1);
            JS_FreeValue(ctx, e2);
        }
        JS_FreeValue(ctx, posVal);

        JSValue dirVal = JS_GetPropertyStr(ctx, opts, "direction");
        if (JS_IsArray(dirVal)) {
            double dx = 0, dy = -1, dz = 0;
            JSValue e0 = JS_GetPropertyUint32(ctx, dirVal, 0);
            JSValue e1 = JS_GetPropertyUint32(ctx, dirVal, 1);
            JSValue e2 = JS_GetPropertyUint32(ctx, dirVal, 2);
            JS_ToFloat64(ctx, &dx, e0);
            JS_ToFloat64(ctx, &dy, e1);
            JS_ToFloat64(ctx, &dz, e2);
            node->setDirection({(float)dx, (float)dy, (float)dz});
            JS_FreeValue(ctx, e0);
            JS_FreeValue(ctx, e1);
            JS_FreeValue(ctx, e2);
        }
        JS_FreeValue(ctx, dirVal);

        JSValue colVal = JS_GetPropertyStr(ctx, opts, "color");
        if (JS_IsString(colVal)) {
            uint8_t cr, cg, cb, ca;
            if (parseColor(jsStr(ctx, colVal), cr, cg, cb, ca))
                node->setColor(cr / 255.0f, cg / 255.0f, cb / 255.0f);
        } else if (JS_IsArray(colVal)) {
            double cr = 1, cg = 1, cb = 1;
            JSValue e0 = JS_GetPropertyUint32(ctx, colVal, 0);
            JSValue e1 = JS_GetPropertyUint32(ctx, colVal, 1);
            JSValue e2 = JS_GetPropertyUint32(ctx, colVal, 2);
            JS_ToFloat64(ctx, &cr, e0);
            JS_ToFloat64(ctx, &cg, e1);
            JS_ToFloat64(ctx, &cb, e2);
            node->setColor((float)cr, (float)cg, (float)cb);
            JS_FreeValue(ctx, e0);
            JS_FreeValue(ctx, e1);
            JS_FreeValue(ctx, e2);
        }
        JS_FreeValue(ctx, colVal);

        JSValue iVal = JS_GetPropertyStr(ctx, opts, "intensity");
        if (!JS_IsUndefined(iVal)) node->setIntensity((float)jsNum(ctx, iVal));
        JS_FreeValue(ctx, iVal);

        JSValue rVal = JS_GetPropertyStr(ctx, opts, "range");
        if (!JS_IsUndefined(rVal)) node->setRange((float)jsNum(ctx, rVal));
        JS_FreeValue(ctx, rVal);

        JSValue iaVal = JS_GetPropertyStr(ctx, opts, "innerAngle");
        if (!JS_IsUndefined(iaVal)) node->setInnerAngle((float)jsNum(ctx, iaVal));
        JS_FreeValue(ctx, iaVal);

        JSValue oaVal = JS_GetPropertyStr(ctx, opts, "outerAngle");
        if (!JS_IsUndefined(oaVal)) node->setOuterAngle((float)jsNum(ctx, oaVal));
        JS_FreeValue(ctx, oaVal);
    }

    return wrapNode(ctx, node, g);
}

// setToneMap({ mode:"aces"|"reinhard"|"linear", exposure, gamma })
JSValue js_sg_setToneMap(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
    JSValueConst opts = argv[0];
    std::string modeStr = qjsbind::get_prop_string(ctx, opts, "mode", "aces");
    scene::SceneGraph::ToneMap mode = scene::SceneGraph::ToneMap::ACES;
    if (modeStr == "linear")        mode = scene::SceneGraph::ToneMap::Linear;
    else if (modeStr == "reinhard") mode = scene::SceneGraph::ToneMap::Reinhard;
    double exposure = qjsbind::get_prop_number(ctx, opts, "exposure", 1.0);
    double gamma    = qjsbind::get_prop_number(ctx, opts, "gamma", 2.2);
    g->setToneMap(mode, (float)exposure, (float)gamma);
    return JS_UNDEFINED;
}

// setAmbient({ color:[r,g,b] }) or setAmbient([r,g,b])
JSValue js_sg_setAmbient(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1) return JS_UNDEFINED;
    bromath::Vec3 c{0.03f, 0.03f, 0.03f};
    if (JS_IsObject(argv[0]) && !JS_IsArray(argv[0])) {
        c = jsGetVec3(ctx, argv[0], "color", 0.03f, 0.03f, 0.03f);
    } else if (JS_IsArray(argv[0])) {
        double r = 0, gg = 0, b = 0;
        JSValue e0 = JS_GetPropertyUint32(ctx, argv[0], 0);
        JSValue e1 = JS_GetPropertyUint32(ctx, argv[0], 1);
        JSValue e2 = JS_GetPropertyUint32(ctx, argv[0], 2);
        JS_ToFloat64(ctx, &r, e0);
        JS_ToFloat64(ctx, &gg, e1);
        JS_ToFloat64(ctx, &b, e2);
        c = {(float)r, (float)gg, (float)b};
        JS_FreeValue(ctx, e0);
        JS_FreeValue(ctx, e1);
        JS_FreeValue(ctx, e2);
    }
    g->setAmbient(c.x, c.y, c.z);
    return JS_UNDEFINED;
}

// setWind({direction:[x,y,z], strength, frequency}) — global wind sway.
JSValue js_sg_setWind(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
    bromath::Vec3 d = jsGetVec3(ctx, argv[0], "direction", 1.0f, 0.0f, 0.0f);
    double strength  = qjsbind::get_prop_number(ctx, argv[0], "strength",  0.0);
    double frequency = qjsbind::get_prop_number(ctx, argv[0], "frequency", 1.5);
    g->setWind(d.x, d.y, d.z, (float)strength, (float)frequency);
    return JS_UNDEFINED;
}

// setShadowQuality({atlasSize, pcfTaps}) — atlas side length and PCF kernel.
JSValue js_sg_setShadowQuality(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
    int atlasSize = (int)qjsbind::get_prop_number(ctx, argv[0], "atlasSize", 4096.0);
    int pcfTaps   = (int)qjsbind::get_prop_number(ctx, argv[0], "pcfTaps",   3.0);
    g->setShadowQuality(atlasSize, pcfTaps);
    return JS_UNDEFINED;
}

// setShadowCache({enabled}) — static shadow-tile cache escape hatch. Default
// on; caching is strictly conservative (pixel-identical output), so this
// exists for debugging, bisecting, and exact per-frame shadowDrawn counts.
JSValue js_sg_setShadowCache(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1) return JS_UNDEFINED;
    bool enabled = true;
    if (JS_IsObject(argv[0])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], "enabled");
        if (!JS_IsUndefined(v)) enabled = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
    } else {
        enabled = JS_ToBool(ctx, argv[0]);
    }
    g->setShadowCache(enabled);
    return JS_UNDEFINED;
}

// setEnvironment({hdr, intensity, rotation}) — load HDR equirectangular
// environment map for skybox + IBL. Pass {hdr: ""} or null to clear.
JSValue js_sg_setEnvironment(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        g->clearEnvironment();
        return JS_TRUE;
    }
    if (!JS_IsObject(argv[0])) return JS_FALSE;
    JSValueConst opts = argv[0];

    JSValue hdrVal = JS_GetPropertyStr(ctx, opts, "hdr");
    bool ok = true;
    if (JS_IsString(hdrVal)) {
        const char* path = JS_ToCString(ctx, hdrVal);
        if (path && path[0]) {
            ok = g->loadEnvironment(resolveAppPath(path));
        } else {
            g->clearEnvironment();
        }
        if (path) JS_FreeCString(ctx, path);
    } else if (JS_IsNull(hdrVal) || JS_IsUndefined(hdrVal)) {
        // No path key — leave the cubemap alone, just update intensity/rotation.
    }
    JS_FreeValue(ctx, hdrVal);

    JSValue ivVal = JS_GetPropertyStr(ctx, opts, "intensity");
    if (JS_IsNumber(ivVal)) {
        double v = 1.0; JS_ToFloat64(ctx, &v, ivVal);
        g->setEnvironmentIntensity((float)v);
    }
    JS_FreeValue(ctx, ivVal);

    JSValue rotVal = JS_GetPropertyStr(ctx, opts, "rotation");
    if (JS_IsNumber(rotVal)) {
        double v = 0.0; JS_ToFloat64(ctx, &v, rotVal);
        g->setEnvironmentRotation((float)v);
    }
    JS_FreeValue(ctx, rotVal);

    return ok ? JS_TRUE : JS_FALSE;
}

// setFog({start, end, color, density, heightFalloff, startDistance})
// Two modes sharing one color: the legacy linear start/end ramp, and
// (when density > 0) exponential-squared height fog. Every call resets
// both mode's parameters, so setFog({}) fully disables fog.
JSValue js_sg_setFog(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

    JSValueConst opts = argv[0];
    double start = qjsbind::get_prop_number(ctx, opts, "start", 0.0);
    double end = qjsbind::get_prop_number(ctx, opts, "end", 0.0);
    bromath::Vec3 color = jsGetVec3(ctx, opts, "color", 0.0f, 0.0f, 0.0f);
    double density       = qjsbind::get_prop_number(ctx, opts, "density", 0.0);
    double heightFalloff = qjsbind::get_prop_number(ctx, opts, "heightFalloff", 0.0);
    double startDistance = qjsbind::get_prop_number(ctx, opts, "startDistance", 0.0);
    g->setFog((float)start, (float)end, color.x, color.y, color.z);
    g->setFogExp((float)density, (float)heightFalloff, (float)startDistance);
    return JS_UNDEFINED;
}

// setAtmosphere({enabled, sunDirection, sunColor, planetRadius, thickness,
//                betaRayleigh, betaMie, mieG, scaleHeightRayleigh,
//                scaleHeightMie, seaLevel, sunAngularRadius, sunDiskIntensity})
//
// Omitted fields keep their Earth defaults rather than reading back the current
// value: the parameters only make sense as a set, and a half-applied atmosphere
// is a worse failure than an obviously wrong one.
JSValue js_sg_setAtmosphere(JSContext* ctx, JSValueConst this_val, int argc,
                            JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

    JSValueConst o = argv[0];
    scene::AtmosphereParams a;   // Earth defaults

    a.enabled = qjsbind::get_prop_bool(ctx, o, "enabled", true);

    bromath::Vec3 sd = jsGetVec3(ctx, o, "sunDirection",
                                 a.sunDir[0], a.sunDir[1], a.sunDir[2]);
    a.sunDir[0] = sd.x; a.sunDir[1] = sd.y; a.sunDir[2] = sd.z;

    bromath::Vec3 sc = jsGetVec3(ctx, o, "sunColor",
                                 a.sunColor[0], a.sunColor[1], a.sunColor[2]);
    a.sunColor[0] = sc.x; a.sunColor[1] = sc.y; a.sunColor[2] = sc.z;
    // An explicit sunColor pins the atmosphere to it; otherwise it tracks the
    // scene's directional light so inscatter and lit ground share one scale.
    JSValue scProbe = JS_GetPropertyStr(ctx, o, "sunColor");
    a.sunColorExplicit = !JS_IsUndefined(scProbe);
    JS_FreeValue(ctx, scProbe);

    bromath::Vec3 br = jsGetVec3(ctx, o, "betaRayleigh",
                                 a.betaR[0], a.betaR[1], a.betaR[2]);
    a.betaR[0] = br.x; a.betaR[1] = br.y; a.betaR[2] = br.z;

    auto num = [&](const char* n, float cur) {
        return (float)qjsbind::get_prop_number(ctx, o, n, cur);
    };
    a.planetRadius     = num("planetRadius", a.planetRadius);
    a.thickness        = num("thickness", a.thickness);
    a.betaM            = num("betaMie", a.betaM);
    a.mieG             = num("mieG", a.mieG);
    a.scaleHeightR     = num("scaleHeightRayleigh", a.scaleHeightR);
    a.scaleHeightM     = num("scaleHeightMie", a.scaleHeightM);
    a.seaLevel         = num("seaLevel", a.seaLevel);
    a.sunAngularRadius = num("sunAngularRadius", a.sunAngularRadius);
    a.sunDiskIntensity = num("sunDiskIntensity", a.sunDiskIntensity);

    g->setAtmosphere(a);
    return JS_UNDEFINED;
}

// setStarfield({enabled, intensity, density, rotation}) — additive stars over
// the sky. Omitted fields keep their defaults; setStarfield({enabled:false})
// turns the pass off.
JSValue js_sg_setStarfield(JSContext* ctx, JSValueConst this_val, int argc,
                           JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

    JSValueConst o = argv[0];
    scene::StarfieldParams s;
    s.enabled   = qjsbind::get_prop_bool(ctx, o, "enabled", true);
    s.intensity = (float)qjsbind::get_prop_number(ctx, o, "intensity", s.intensity);
    s.density   = (float)qjsbind::get_prop_number(ctx, o, "density", s.density);
    s.rotation  = (float)qjsbind::get_prop_number(ctx, o, "rotation", s.rotation);
    g->setStarfield(s);
    return JS_UNDEFINED;
}

// setTiltShift({enabled, focusCenter, focusWidth, feather, strength,
//               saturation, contrast}) — screen-space miniature DOF.
// Passing { enabled: false } (or omitting enabled) turns the pass off.
JSValue js_sg_setTiltShift(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

    JSValueConst opts = argv[0];
    bool enabled = false;
    JSValue ev = JS_GetPropertyStr(ctx, opts, "enabled");
    if (!JS_IsUndefined(ev)) enabled = JS_ToBool(ctx, ev);
    JS_FreeValue(ctx, ev);

    double focusCenter = qjsbind::get_prop_number(ctx, opts, "focusCenter", 0.5);
    double focusWidth  = qjsbind::get_prop_number(ctx, opts, "focusWidth",  0.12);
    double feather     = qjsbind::get_prop_number(ctx, opts, "feather",     0.25);
    double strength    = qjsbind::get_prop_number(ctx, opts, "strength",    2.0);
    double saturation  = qjsbind::get_prop_number(ctx, opts, "saturation",  1.0);
    double contrast    = qjsbind::get_prop_number(ctx, opts, "contrast",    1.0);
    g->setTiltShift(enabled, (float)focusCenter, (float)focusWidth,
                    (float)feather, (float)strength, (float)saturation,
                    (float)contrast);
    return JS_UNDEFINED;
}

// setBloom({enabled, threshold, intensity, strength}) — HDR highlight glow.
JSValue js_sg_setBloom(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

    JSValueConst opts = argv[0];
    bool enabled = false;
    JSValue ev = JS_GetPropertyStr(ctx, opts, "enabled");
    if (!JS_IsUndefined(ev)) enabled = JS_ToBool(ctx, ev);
    JS_FreeValue(ctx, ev);

    double threshold = qjsbind::get_prop_number(ctx, opts, "threshold", 1.0);
    double intensity = qjsbind::get_prop_number(ctx, opts, "intensity", 0.6);
    double strength  = qjsbind::get_prop_number(ctx, opts, "strength",  2.0);
    g->setBloom(enabled, (float)threshold, (float)intensity, (float)strength);
    return JS_UNDEFINED;
}

// setSSAO({enabled, radius, intensity, bias}) — screen-space ambient
// occlusion multiplied into the lit HDR image before tonemap.
JSValue js_sg_setSSAO(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

    JSValueConst opts = argv[0];
    bool enabled = false;
    JSValue ev = JS_GetPropertyStr(ctx, opts, "enabled");
    if (!JS_IsUndefined(ev)) enabled = JS_ToBool(ctx, ev);
    JS_FreeValue(ctx, ev);

    double radius    = qjsbind::get_prop_number(ctx, opts, "radius",    0.5);
    double intensity = qjsbind::get_prop_number(ctx, opts, "intensity", 1.0);
    double bias      = qjsbind::get_prop_number(ctx, opts, "bias",      0.025);
    g->setSSAO(enabled, (float)radius, (float)intensity, (float)bias);
    return JS_UNDEFINED;
}

// setSSR({enabled, maxDistance, steps, thickness, intensity, edgeFade}) —
// screen-space reflections marched from the opaque depth buffer, composited
// over the IBL specular before the blended passes.
JSValue js_sg_setSSR(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

    JSValueConst opts = argv[0];
    bool enabled = false;
    JSValue ev = JS_GetPropertyStr(ctx, opts, "enabled");
    if (!JS_IsUndefined(ev)) enabled = JS_ToBool(ctx, ev);
    JS_FreeValue(ctx, ev);

    double maxDistance = qjsbind::get_prop_number(ctx, opts, "maxDistance", 30.0);
    double steps       = qjsbind::get_prop_number(ctx, opts, "steps",       48.0);
    double thickness   = qjsbind::get_prop_number(ctx, opts, "thickness",   0.3);
    double intensity   = qjsbind::get_prop_number(ctx, opts, "intensity",   1.0);
    double edgeFade    = qjsbind::get_prop_number(ctx, opts, "edgeFade",    0.1);
    g->setSSR(enabled, (float)maxDistance, (int)steps, (float)thickness,
              (float)intensity, (float)edgeFade);
    return JS_UNDEFINED;
}

// setDepthOfField({enabled, focusDistance, focusRange, maxBlur}) —
// depth-based DoF on the HDR image before bloom + tonemap.
JSValue js_sg_setDepthOfField(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

    JSValueConst opts = argv[0];
    bool enabled = false;
    JSValue ev = JS_GetPropertyStr(ctx, opts, "enabled");
    if (!JS_IsUndefined(ev)) enabled = JS_ToBool(ctx, ev);
    JS_FreeValue(ctx, ev);

    double focusDistance = qjsbind::get_prop_number(ctx, opts, "focusDistance", 10.0);
    double focusRange    = qjsbind::get_prop_number(ctx, opts, "focusRange",    5.0);
    double maxBlur       = qjsbind::get_prop_number(ctx, opts, "maxBlur",       4.0);
    g->setDepthOfField(enabled, (float)focusDistance, (float)focusRange,
                       (float)maxBlur);
    return JS_UNDEFINED;
}

// setColorLUT({path, size, amount}) — 3D color-grading LUT from a strip
// image, applied after tonemapping. Pass null (or {path: ""}) to clear.
JSValue js_sg_setColorLUT(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        g->clearColorLUT();
        return JS_TRUE;
    }
    if (!JS_IsObject(argv[0])) return JS_FALSE;
    JSValueConst opts = argv[0];

    std::string path = qjsbind::get_prop_string(ctx, opts, "path", "");
    if (path.empty()) {
        g->clearColorLUT();
        return JS_TRUE;
    }
    int size      = (int)qjsbind::get_prop_number(ctx, opts, "size", 0.0);
    double amount = qjsbind::get_prop_number(ctx, opts, "amount", 1.0);
    return g->loadColorLUT(resolveAppPath(path), size, (float)amount)
               ? JS_TRUE : JS_FALSE;
}

// setFXAA(true|false) or setFXAA({enabled}) — FXAA 3.11 on the final LDR
// image, always the last post pass.
JSValue js_sg_setFXAA(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1) return JS_UNDEFINED;
    bool enabled = false;
    if (JS_IsObject(argv[0])) {
        JSValue ev = JS_GetPropertyStr(ctx, argv[0], "enabled");
        if (!JS_IsUndefined(ev)) enabled = JS_ToBool(ctx, ev);
        JS_FreeValue(ctx, ev);
    } else {
        enabled = JS_ToBool(ctx, argv[0]);
    }
    g->setFXAA(enabled);
    return JS_UNDEFINED;
}

// setRenderScale(s) — internal render-resolution multiplier (clamped
// 0.25-2.0). Compositing/picking stay in CSS pixels.
JSValue js_sg_setRenderScale(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1) return JS_UNDEFINED;
    double s = 1.0;
    if (JS_ToFloat64(ctx, &s, argv[0])) return JS_EXCEPTION;
    g->setRenderScale((float)s);
    return JS_UNDEFINED;
}

// setMSAA(samples) — multisampling for the HDR 3D passes. 0/1 = off;
// clamped to the driver's GL_MAX_SAMPLES at allocation.
JSValue js_sg_setMSAA(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1) return JS_UNDEFINED;
    int32_t n = 0;
    if (JS_ToInt32(ctx, &n, argv[0])) return JS_EXCEPTION;
    g->setMSAA(n);
    return JS_UNDEFINED;
}

// unprojectLocal(x, y) → { origin:[x,y,z], dir:[x,y,z] } | null
JSValue js_sg_unprojectLocal(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 2) return JS_NULL;
    double x = 0, y = 0;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &y, argv[1]);
    bromath::Vec3 origin, dir;
    if (!g->unprojectLocal((float)x, (float)y, origin, dir)) return JS_NULL;
    JSValue out = JS_NewObject(ctx);
    JSValue oArr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, oArr, 0, JS_NewFloat64(ctx, origin.x));
    JS_SetPropertyUint32(ctx, oArr, 1, JS_NewFloat64(ctx, origin.y));
    JS_SetPropertyUint32(ctx, oArr, 2, JS_NewFloat64(ctx, origin.z));
    JSValue dArr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, dArr, 0, JS_NewFloat64(ctx, dir.x));
    JS_SetPropertyUint32(ctx, dArr, 1, JS_NewFloat64(ctx, dir.y));
    JS_SetPropertyUint32(ctx, dArr, 2, JS_NewFloat64(ctx, dir.z));
    JS_SetPropertyStr(ctx, out, "origin", oArr);
    JS_SetPropertyStr(ctx, out, "dir", dArr);
    return out;
}

// Read the post-tonemap LDR FBO of this scene as an ImageData-shaped object
// suitable for ctx2d.putImageData(). Returns null if no 3D content has been
// rendered yet (FBO not allocated). Pixels arrive top-down (CSS row order),
// pre-flipped from GL's bottom-up native layout by readTonemapPixelsRGBA().
static JSValue buildImageDataFromPixels(JSContext* ctx,
                                        const std::vector<uint8_t>& pixels,
                                        int w, int h) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, h));

    JSValue abuf = JS_NewArrayBufferCopy(ctx, pixels.data(), pixels.size());
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue u8cCtor = JS_GetPropertyStr(ctx, global, "Uint8ClampedArray");
    JSValue dataArr = JS_CallConstructor(ctx, u8cCtor, 1, &abuf);
    JS_FreeValue(ctx, u8cCtor);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, abuf);
    JS_SetPropertyStr(ctx, obj, "data", dataArr);
    return obj;
}

JSValue js_sg_toImageData(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_NULL;
    int w = 0, h = 0;
    auto pixels = g->readTonemapPixelsRGBA(w, h);
    if (pixels.empty() || w <= 0 || h <= 0) return JS_NULL;
    return buildImageDataFromPixels(ctx, pixels, w, h);
}

// Synchronously render the scene and return its tonemap output as ImageData.
// Unlike toImageData(), this does not depend on the engine's per-tick render
// having already populated the FBO — it drives the render itself, so it works
// in windowed mode where there's no flush() global. Optional width/height
// args resize the scene's render target before rendering, which is what
// off-document capture canvases (e.g. sprite-sheet authoring) need.
JSValue js_sg_captureFrame(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_NULL;
    if (argc >= 2) {
        int32_t w = 0, h = 0;
        if (JS_ToInt32(ctx, &w, argv[0]) || JS_ToInt32(ctx, &h, argv[1])) return JS_NULL;
        if (w > 0 && h > 0 && (w != g->canvasWidth() || h != g->canvasHeight())) {
            g->setCanvasSize(w, h);
        }
    }
    g->render();
    int rw = 0, rh = 0;
    auto pixels = g->readTonemapPixelsRGBA(rw, rh);
    if (pixels.empty() || rw <= 0 || rh <= 0) return JS_NULL;
    return buildImageDataFromPixels(ctx, pixels, rw, rh);
}

// asTexture() → SceneTexture — live-linked handle to this scene's LDR output
// texture, for use as a mesh baseColor map in another scene via
// mesh.setBaseColorTexture(handle). See the SceneTextureHandle comment for
// lifetime semantics.
JSValue js_sg_asTexture(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_NULL;
    return qjsbind::wrap<SceneTextureHandle>(
        ctx, new SceneTextureHandle{g->outputTextureSource()});
}

} // namespace bro::js

#endif  // BRO_WITH_3D
