#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/mesh_node.h"
#include "scene/instanced_mesh_node.h"
#include "scene/light_node.h"
#include "scene/camera_node.h"
#include "scene/physics_node.h"
#include "canvas/canvas2d.h"
#include "engine/scene_audio_sync.h"

#include <bromesh/analysis/raycast.h>
#include <bromesh/analysis/bvh.h>
#include <bromesh/primitives/primitives.h>
#include <bromesh/manipulation/normals.h>

#include <algorithm>
#include <cmath>
#include <vector>
#include <unordered_map>

namespace bro::bronze_host {

HostClass g_sceneGraphClass;
HostClass g_sceneNodeClass;
HostClass g_sceneTextureClass;

namespace {

using NodeWrapKey = std::pair<const scene::SceneGraph::LivenessToken*, uint32_t>;
struct NodeWrapKeyHash {
    size_t operator()(const NodeWrapKey& k) const {
        return std::hash<const void*>()(k.first) ^ (std::hash<uint32_t>()(k.second) << 1);
    }
};

static std::unordered_map<NodeWrapKey, ev::Persistent, NodeWrapKeyHash> s_nodeWrapMap;

double numAtProp(Value obj, const char* key, double defVal) {
    if (!ev::isObject(obj)) return defVal;
    Value v = ev::getProperty(obj, key);
    return ev::isNumber(v) ? ev::toDouble(v) : defVal;
}

// ---------------------------------------------------------------------------
// Raycast picking helpers
// ---------------------------------------------------------------------------

struct PickFrame {
    bromath::Vec3 bx, by, bz, bt;
    bromath::Vec3 r0, r1, r2;
    bool ok = false;

    bromath::Vec3 pointToWorld(const bromath::Vec3& v) const {
        return bx * v.x + by * v.y + bz * v.z + bt;
    }
    bromath::Vec3 dirToLocal(const bromath::Vec3& v) const {
        return {bromath::vdot(v, r0), bromath::vdot(v, r1), bromath::vdot(v, r2)};
    }
    bromath::Vec3 pointToLocal(const bromath::Vec3& p) const {
        return dirToLocal(p - bt);
    }
    bromath::Vec3 normalToWorld(const bromath::Vec3& n) const {
        return r0 * n.x + r1 * n.y + r2 * n.z;
    }
};

PickFrame makePickFrame(const bromath::Vec3& bx, const bromath::Vec3& by,
                        const bromath::Vec3& bz, const bromath::Vec3& bt) {
    PickFrame f;
    f.bx = bx; f.by = by; f.bz = bz; f.bt = bt;
    float det = bromath::vdot(bx, bromath::vcross(by, bz));
    if (std::fabs(det) < 1e-20f) return f;
    float invDet = 1.0f / det;
    f.r0 = bromath::vcross(by, bz) * invDet;
    f.r1 = bromath::vcross(bz, bx) * invDet;
    f.r2 = bromath::vcross(bx, by) * invDet;
    f.ok = true;
    return f;
}

PickFrame pickFrameOf(const bromath::Mat4& m) {
    return makePickFrame({m.at(0, 0), m.at(1, 0), m.at(2, 0)},
                         {m.at(0, 1), m.at(1, 1), m.at(2, 1)},
                         {m.at(0, 2), m.at(1, 2), m.at(2, 2)},
                         {m.at(0, 3), m.at(1, 3), m.at(2, 3)});
}

PickFrame pickFrameOf(const bromath::Mat4& m, const float* rows12) {
    auto mul3 = [&](float x, float y, float z) {
        return bromath::Vec3{m.at(0, 0) * x + m.at(0, 1) * y + m.at(0, 2) * z,
                             m.at(1, 0) * x + m.at(1, 1) * y + m.at(1, 2) * z,
                             m.at(2, 0) * x + m.at(2, 1) * y + m.at(2, 2) * z};
    };
    bromath::Vec3 t = mul3(rows12[3], rows12[7], rows12[11]);
    t.x += m.at(0, 3); t.y += m.at(1, 3); t.z += m.at(2, 3);
    return makePickFrame(mul3(rows12[0], rows12[4], rows12[8]),
                         mul3(rows12[1], rows12[5], rows12[9]),
                         mul3(rows12[2], rows12[6], rows12[10]), t);
}

bool slabHit(const bromath::AABB3& b, const bromath::Vec3& o,
             const bromath::Vec3& d, float& tNear) {
    const float bmin[3] = {b.min.x, b.min.y, b.min.z};
    const float bmax[3] = {b.max.x, b.max.y, b.max.z};
    const float o3[3] = {o.x, o.y, o.z};
    const float d3[3] = {d.x, d.y, d.z};
    float tmin = -1e30f, tmax = 1e30f;
    for (int a = 0; a < 3; ++a) {
        float dv = d3[a];
        float invD = (std::fabs(dv) > 1e-30f) ? 1.0f / dv : (dv >= 0.0f ? 1e30f : -1e30f);
        float t1 = (bmin[a] - o3[a]) * invD;
        float t2 = (bmax[a] - o3[a]) * invD;
        float lo = t1 < t2 ? t1 : t2;
        float hi = t1 < t2 ? t2 : t1;
        if (lo > tmin) tmin = lo;
        if (hi < tmax) tmax = hi;
    }
    if (tmax < 0.0f || tmin > tmax) return false;
    tNear = tmin;
    return true;
}

}  // namespace

Value vec3ToValue(const bromath::Vec3& v) {
    return hostArrayOf(3, [&v](size_t i) {
        if (i == 0) return ev::fromDouble(v.x);
        if (i == 1) return ev::fromDouble(v.y);
        return ev::fromDouble(v.z);
    });
}

Value quatToValue(const bromath::Quat& q) {
    return hostArrayOf(4, [&q](size_t i) {
        if (i == 0) return ev::fromDouble(q.x);
        if (i == 1) return ev::fromDouble(q.y);
        if (i == 2) return ev::fromDouble(q.z);
        return ev::fromDouble(q.w);
    });
}

Value mat4ToValue(const bromath::Mat4& m) {
    return hostArrayOf(16, [&m](size_t i) {
        int col = static_cast<int>(i / 4);
        int row = static_cast<int>(i % 4);
        return ev::fromDouble(m.at(row, col));
    });
}

bool readVec3FromValue(Value v, bromath::Vec3& out) {
    if (!ev::isObject(v)) return false;
    Value lenV = ev::getProperty(v, "length");
    if (ev::isNumber(lenV)) {
        double x = ev::toDouble(ev::getElement(v, 0));
        double y = ev::toDouble(ev::getElement(v, 1));
        double z = ev::toDouble(ev::getElement(v, 2));
        out = {(float)x, (float)y, (float)z};
        return true;
    }
    Value vx = ev::getProperty(v, "x");
    Value vy = ev::getProperty(v, "y");
    Value vz = ev::getProperty(v, "z");
    if (ev::isNumber(vx) && ev::isNumber(vy)) {
        double x = ev::toDouble(vx);
        double y = ev::toDouble(vy);
        double z = ev::isNumber(vz) ? ev::toDouble(vz) : 0.0;
        out = {(float)x, (float)y, (float)z};
        return true;
    }
    return false;
}

bool readQuatFromValue(Value v, bromath::Quat& out) {
    if (!ev::isObject(v)) return false;
    Value lenV = ev::getProperty(v, "length");
    if (ev::isNumber(lenV)) {
        double x = ev::toDouble(ev::getElement(v, 0));
        double y = ev::toDouble(ev::getElement(v, 1));
        double z = ev::toDouble(ev::getElement(v, 2));
        double w = ev::toDouble(ev::getElement(v, 3));
        out = {(float)x, (float)y, (float)z, (float)w};
        return true;
    }
    Value vx = ev::getProperty(v, "x");
    Value vy = ev::getProperty(v, "y");
    Value vz = ev::getProperty(v, "z");
    Value vw = ev::getProperty(v, "w");
    if (ev::isNumber(vx) && ev::isNumber(vy) && ev::isNumber(vz) && ev::isNumber(vw)) {
        out = {(float)ev::toDouble(vx), (float)ev::toDouble(vy),
               (float)ev::toDouble(vz), (float)ev::toDouble(vw)};
        return true;
    }
    return false;
}

bool parseColorValue(Value v, float& r, float& g, float& b, float& a) {
    if (ev::isString(v)) {
        uint8_t cr = 255, cg = 255, cb = 255, ca = 255;
        if (canvas::parseCSSColor(ev::toUtf8(v), cr, cg, cb, ca)) {
            r = cr / 255.0f;
            g = cg / 255.0f;
            b = cb / 255.0f;
            a = ca / 255.0f;
            return true;
        }
        return false;
    }
    if (ev::isObject(v)) {
        Value lenV = ev::getProperty(v, "length");
        if (ev::isNumber(lenV)) {
            int len = static_cast<int>(ev::toDouble(lenV));
            if (len >= 3) {
                r = static_cast<float>(ev::toDouble(ev::getElement(v, 0)));
                g = static_cast<float>(ev::toDouble(ev::getElement(v, 1)));
                b = static_cast<float>(ev::toDouble(ev::getElement(v, 2)));
                if (len >= 4) {
                    Value va = ev::getElement(v, 3);
                    if (!ev::isUndefined(va)) a = static_cast<float>(ev::toDouble(va));
                }
                return true;
            }
        }
    }
    return false;
}

Value buildImageDataValue(int w, int h, const std::vector<uint8_t>& pixels) {
    ObjectBuilder img;
    img.set("width", ev::fromDouble(w));
    img.set("height", ev::fromDouble(h));
    Value arr = ev::createTypedArray(bronze::embed::elements::Uint8Clamped,
                                     static_cast<uint32_t>(pixels.size()));
    if (!pixels.empty()) {
        ev::fillTypedArray(arr, std::span<const uint8_t>(pixels.data(), pixels.size()));
    }
    img.set("data", arr);
    return img.get();
}

void pruneSceneNodeWrapper(const scene::SceneGraph::LivenessToken* tok, uint32_t id) {
    s_nodeWrapMap.erase(NodeWrapKey{tok, id});
}

Value wrapSceneNode(scene::SceneNode* node, scene::SceneGraph* graph) {
    if (!node || !graph) return ev::null();
    auto tok = graph->livenessToken();
    if (!tok) return ev::null();
    NodeWrapKey key{tok.get(), node->id()};
    auto it = s_nodeWrapMap.find(key);
    if (it != s_nodeWrapMap.end()) {
        Value existing = it->second.get();
        if (ev::isObject(existing)) return existing;
    }
    ensureSceneClassesInstalled();
    auto* cell = new HostSceneNodeCell{kHostSceneNodeTag, tok, node->id()};
    Value obj = g_sceneNodeClass.make(cell, [](void* p) {
        delete static_cast<HostSceneNodeCell*>(p);
    });
    s_nodeWrapMap[key].set(obj);
    return obj;
}

Value wrapSceneTexture(std::shared_ptr<scene::SceneGraph::OutputTextureSource> src) {
    ensureSceneClassesInstalled();
    auto* cell = new HostSceneTextureCell{kHostSceneTextureTag, src};
    return g_sceneTextureClass.make(cell, [](void* p) {
        delete static_cast<HostSceneTextureCell*>(p);
    });
}

Value createSceneGraphValue(scene::SceneGraph* sg, dom::Element* canvas) {
    if (!sg) return ev::null();
    ensureSceneClassesInstalled();
    auto* cell = new HostSceneGraphCell{kHostSceneGraphTag, sg->livenessToken(), canvas};
    return g_sceneGraphClass.make(cell, [](void* p) {
        delete static_cast<HostSceneGraphCell*>(p);
    });
}

static bool s_classesInstalled = false;

void ensureSceneClassesInstalled() {
    if (s_classesInstalled) return;
    s_classesInstalled = true;

    g_sceneTextureClass.install("SceneTexture", 0, nullptr, [](ObjectBuilder& b) {
        b.accessor("valid", [](Value self_, std::span<const Value>) {
            auto* c = sceneTextureCellOf(self_);
            if (!c) return ev::fromBool(false);
            auto s = c->src.lock();
            return ev::fromBool(s && s->graph);
        }, nullptr);
    });

    g_sceneNodeClass.install("SceneNode", 0, nullptr, [](ObjectBuilder& b) {
        installSceneNodeCore(b);
        installSceneNodeMesh(b);
        installSceneNodeLights(b);
        installSceneNodeFx(b);
        installSceneNode2D(b);
    });

    g_sceneGraphClass.install("SceneGraph", 0, nullptr, [](ObjectBuilder& b) {
        installSceneGraphCore(b);
        installSceneGraphEnv(b);
        installSceneGraphMesh(b);
        installSceneGraphLights(b);
        installSceneGraphFx(b);
        installSceneGraph2D(b);
    });
}

void installSceneGraphCore(ObjectBuilder& b) {
    b.accessor("root", [](Value self_, std::span<const Value>) {
        auto* c = sceneGraphCellOf(self_);
        auto* g = c ? c->graph() : nullptr;
        return (g && g->root()) ? wrapSceneNode(g->root(), g) : ev::null();
    }, nullptr);

    b.def("createNode", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        std::string name = a.empty() || !ev::isString(a[0]) ? "" : ev::toUtf8(a[0]);
        auto* n = g->createNode(name);
        g->root()->addChild(n);
        return wrapSceneNode(n, g);
    });

    b.def("createPhysicsNode", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        auto* n = g->createPhysicsNode();
        g->root()->addChild(n);
        if (!a.empty() && ev::isObject(a[0])) {
            Value opts = a[0];
            Value nameV = ev::getProperty(opts, "name");
            if (ev::isString(nameV)) n->setName(ev::toUtf8(nameV));
            Value ppuV = ev::getProperty(opts, "pixelsPerUnit");
            if (ev::isNumber(ppuV)) n->setPixelsPerUnit((float)ev::toDouble(ppuV));
            Value asV = ev::getProperty(opts, "autoSync");
            if (!ev::isUndefined(asV)) n->setAutoSync(ev::toBool(asV));
        }
        return wrapSceneNode(n, g);
    });

    b.def("destroyNode", 1, [](Value self_, std::span<const Value> a) {
        auto* c = sceneGraphCellOf(self_);
        auto* g = c ? c->graph() : nullptr;
        if (!g || a.empty()) return ev::undefined();
        auto* nodeCell = sceneNodeCellOf(a[0]);
        auto* n = nodeCell ? nodeCell->node() : nullptr;
        if (n) {
            auto tok = c->token.lock();
            if (tok) {
                n->traverse([&](scene::SceneNode* kid) {
                    pruneSceneNodeWrapper(tok.get(), kid->id());
                });
            }
            g->destroyNode(n);
        }
        return ev::undefined();
    });

    b.def("findById", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty()) return ev::null();
        uint32_t id = static_cast<uint32_t>(ev::toDouble(a[0]));
        auto* n = g->findById(id);
        return n ? wrapSceneNode(n, g) : ev::null();
    });

    b.def("findByName", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty()) return ev::null();
        auto* n = g->findByName(ev::toUtf8(a[0]));
        return n ? wrapSceneNode(n, g) : ev::null();
    });

    b.def("setFrustumCulling", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (g && !a.empty()) g->setFrustumCulling(ev::toBool(a[0]));
        return ev::undefined();
    });

    b.accessor("frustumCulling",
        [](Value self_, std::span<const Value>) {
            auto* g = sceneGraphOf(self_);
            return ev::fromBool(g ? g->frustumCulling() : true);
        },
        [](Value self_, std::span<const Value> a) {
            auto* g = sceneGraphOf(self_);
            if (g && !a.empty()) g->setFrustumCulling(ev::toBool(a[0]));
            return ev::undefined();
        });

    b.def("cullStats", 0, [](Value self_, std::span<const Value>) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::null();
        const auto& s = g->cullStats();
        ObjectBuilder o;
        o.set("meshDrawn", ev::fromDouble(s.meshDrawn));
        o.set("meshCulled", ev::fromDouble(s.meshCulled));
        o.set("instancedDrawn", ev::fromDouble(s.instancedDrawn));
        o.set("instancedCulled", ev::fromDouble(s.instancedCulled));
        o.set("splatDrawn", ev::fromDouble(s.splatDrawn));
        o.set("splatCulled", ev::fromDouble(s.splatCulled));
        o.set("particlesDrawn", ev::fromDouble(s.particlesDrawn));
        o.set("particlesCulled", ev::fromDouble(s.particlesCulled));
        o.set("billboardsDrawn", ev::fromDouble(s.billboardsDrawn));
        o.set("billboardsCulled", ev::fromDouble(s.billboardsCulled));
        o.set("decalsDrawn", ev::fromDouble(s.decalsDrawn));
        o.set("decalsCulled", ev::fromDouble(s.decalsCulled));
        o.set("shadowDrawn", ev::fromDouble(s.shadowDrawn));
        o.set("shadowCulled", ev::fromDouble(s.shadowCulled));
        o.set("shadowTilesTotal", ev::fromDouble(s.shadowTilesTotal));
        o.set("shadowTilesRendered", ev::fromDouble(s.shadowTilesRendered));
        o.set("shadowTilesCached", ev::fromDouble(s.shadowTilesCached));
        return o.get();
    });

    b.def("syncPhysics", 0, [](Value self_, std::span<const Value>) {
        auto* g = sceneGraphOf(self_);
        if (g) g->syncPhysics();
        return ev::undefined();
    });

    b.def("bindAudioListenerToCamera", 1, [](Value self_, std::span<const Value> a) {
        auto* cell = sceneGraphCellOf(self_);
        if (!cell) return ev::undefined();
        auto* g = cell->graph();
        if (!g) return ev::undefined();
        bool enable = a.empty() || ev::toBool(a[0]);
        bro::engine::SceneAudioSync::bindAudioListenerToCamera(cell->token, g, enable);
        return ev::undefined();
    });

    b.def("setCamera", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty() || !ev::isObject(a[0])) return ev::undefined();
        Value opts = a[0];
        double fovDeg = numAtProp(opts, "fov", 60.0);
        float fov = static_cast<float>(fovDeg * 3.141592653589793 / 180.0);
        float nearZ = static_cast<float>(numAtProp(opts, "near", 0.1));
        float farZ = static_cast<float>(numAtProp(opts, "far", 1000.0));
        double aspectD = numAtProp(opts, "aspect", 0.0);
        bool aspectFollowsCanvas = (aspectD <= 0);
        if (aspectFollowsCanvas) {
            int cw = g->canvasWidth(), ch = g->canvasHeight();
            aspectD = (cw > 0 && ch > 0) ? static_cast<double>(cw) / static_cast<double>(ch) : (4.0 / 3.0);
        }
        g->setCameraAspectFollowsCanvas(aspectFollowsCanvas);
        float aspect = static_cast<float>(aspectD);

        bromath::Vec3 pos{0, 5, -10};
        readVec3FromValue(ev::getProperty(opts, "position"), pos);

        bromath::Quat quat{0, 0, 0, 1};
        bool hasQuat = readQuatFromValue(ev::getProperty(opts, "quaternion"), quat);

        if (hasQuat) {
            g->setCameraQuat(fov, aspect, nearZ, farZ, pos, bromath::qnorm(quat));
        } else {
            bromath::Vec3 target{0, 0, 0};
            readVec3FromValue(ev::getProperty(opts, "target"), target);
            bromath::Vec3 up{0, 1, 0};
            readVec3FromValue(ev::getProperty(opts, "up"), up);

            Value modeV = ev::getProperty(opts, "mode");
            std::string mode = ev::isString(modeV) ? ev::toUtf8(modeV) : "perspective";
            if (mode == "orthographic" || mode == "ortho") {
                float size = static_cast<float>(numAtProp(opts, "size", 10.0));
                float halfW = size * aspect * 0.5f;
                float halfH = size * 0.5f;
                g->setCameraOrtho(-halfW, halfW, -halfH, halfH, nearZ, farZ, pos, target, up);
            } else {
                g->setCamera(fov, aspect, nearZ, farZ, pos, target, up);
            }
        }
        return ev::undefined();
    });

    b.def("createCamera", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        auto* node = g->createCamera();
        g->root()->addChild(node);
        if (!a.empty() && ev::isObject(a[0])) {
            Value opts = a[0];
            Value nameV = ev::getProperty(opts, "name");
            if (ev::isString(nameV)) node->setName(ev::toUtf8(nameV));
            double fovDeg = numAtProp(opts, "fov", 60.0);
            node->setFovY(static_cast<float>(fovDeg * 3.141592653589793 / 180.0));
            node->setNearZ(static_cast<float>(numAtProp(opts, "near", 0.1)));
            node->setFarZ(static_cast<float>(numAtProp(opts, "far", 1000.0)));
            node->setAspect(static_cast<float>(numAtProp(opts, "aspect", 0.0)));
            node->setOrthoHeight(static_cast<float>(numAtProp(opts, "size", 10.0)));
            Value modeV = ev::getProperty(opts, "mode");
            std::string mode = ev::isString(modeV) ? ev::toUtf8(modeV) : "perspective";
            node->setPerspective(!(mode == "orthographic" || mode == "ortho"));

            bromath::Vec3 pos;
            if (readVec3FromValue(ev::getProperty(opts, "position"), pos)) node->setPosition(pos);

            bromath::Quat quat;
            if (readQuatFromValue(ev::getProperty(opts, "quaternion"), quat)) {
                node->setRotation(bromath::qnorm(quat));
            } else {
                bromath::Vec3 la;
                if (readVec3FromValue(ev::getProperty(opts, "lookAt"), la)) node->lookAt(la);
            }

            Value actV = ev::getProperty(opts, "active");
            if (!ev::isUndefined(actV) && ev::toBool(actV)) g->setActiveCamera(node);
        }
        return wrapSceneNode(node, g);
    });

    b.def("setActiveCamera", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        if (a.empty() || ev::isNull(a[0]) || ev::isUndefined(a[0])) {
            g->setActiveCamera(nullptr);
            return ev::undefined();
        }
        auto* node = sceneNodeOf(a[0]);
        if (!node || node->type() != scene::SceneNode::Type::Camera)
            return ev::throwTypeError("setActiveCamera: argument must be a CameraNode or null");
        g->setActiveCamera(static_cast<scene::CameraNode*>(node));
        return ev::undefined();
    });

    b.accessor("activeCamera",
        [](Value self_, std::span<const Value>) {
            auto* g = sceneGraphOf(self_);
            if (!g) return ev::null();
            auto* cam = g->activeCamera();
            return cam ? wrapSceneNode(cam, g) : ev::null();
        },
        [](Value self_, std::span<const Value> a) {
            auto* g = sceneGraphOf(self_);
            if (!g) return ev::undefined();
            if (a.empty() || ev::isNull(a[0]) || ev::isUndefined(a[0])) {
                g->setActiveCamera(nullptr);
                return ev::undefined();
            }
            auto* node = sceneNodeOf(a[0]);
            if (node && node->type() == scene::SceneNode::Type::Camera)
                g->setActiveCamera(static_cast<scene::CameraNode*>(node));
            return ev::undefined();
        });

    b.accessor("viewMatrix", [](Value self_, std::span<const Value>) {
        auto* g = sceneGraphOf(self_);
        return g ? mat4ToValue(g->viewMatrix()) : ev::null();
    }, nullptr);

    b.accessor("projectionMatrix", [](Value self_, std::span<const Value>) {
        auto* g = sceneGraphOf(self_);
        return g ? mat4ToValue(g->projectionMatrix()) : ev::null();
    }, nullptr);

    b.accessor("cameraEye", [](Value self_, std::span<const Value>) {
        auto* g = sceneGraphOf(self_);
        return g ? vec3ToValue(g->cameraEye()) : ev::null();
    }, nullptr);

    b.def("raycast", 3, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.size() < 2) return ev::null();

        bromath::Vec3 origin, dir;
        if (!readVec3FromValue(a[0], origin)) return ev::throwTypeError("raycast: origin must be [x,y,z]");
        if (!readVec3FromValue(a[1], dir)) return ev::throwTypeError("raycast: direction must be [x,y,z]");

        double maxDist = 0.0;
        if (a.size() >= 3 && ev::isNumber(a[2])) maxDist = ev::toDouble(a[2]);

        dir = bromath::vnorm(dir);
        if (bromath::vlen2(dir) < 1e-12f) return ev::null();

        float closestDist = (maxDist > 0.0) ? static_cast<float>(maxDist) : 1e30f;
        scene::MeshNode* closestNode = nullptr;
        scene::LightNode* closestLight = nullptr;
        scene::InstancedMeshNode* closestInstanced = nullptr;
        int closestInstance = -1;
        bromath::Vec3 closestWorldPoint;
        bromath::Vec3 closestWorldNormal;

        auto tryMesh = [&](const PickFrame& f, const bromesh::MeshData& md,
                           const bromath::AABB3& lb, auto&& bvhOf) -> bool {
            if (!f.ok) return false;
            bromath::Vec3 lo = f.pointToLocal(origin);
            bromath::Vec3 ld = f.dirToLocal(dir);
            float ldLen = bromath::vlen(ld);
            if (ldLen < 1e-20f) return false;
            bromath::Vec3 ldN = ld * (1.0f / ldLen);
            float localMax = closestDist * ldLen;

            float tNear = 0.0f;
            if (!slabHit(lb, lo, ldN, tNear) || tNear > localMax) return false;

            const float o[3] = {lo.x, lo.y, lo.z};
            const float d[3] = {ldN.x, ldN.y, ldN.z};
            bromesh::RayHit hit = bvhOf().raycast(md, o, d, localMax);
            if (!hit.hit) return false;

            bromath::Vec3 worldHit = f.pointToWorld({hit.position[0], hit.position[1], hit.position[2]});
            float worldDist = bromath::vlen(worldHit - origin);
            if (worldDist >= closestDist) return false;

            closestDist = worldDist;
            closestWorldPoint = worldHit;
            closestWorldNormal = bromath::vnorm(f.normalToWorld({hit.normal[0], hit.normal[1], hit.normal[2]}));
            return true;
        };

        g->root()->traverse([&](scene::SceneNode* node) {
            if (!node || node->type() != scene::SceneNode::Type::Mesh || !node->visible()) return;
            auto* mn = static_cast<scene::MeshNode*>(node);
            const bromesh::MeshData& md = mn->mesh();
            if (md.positions.empty() || md.indices.empty()) return;
            auto bvhOf = [&]() -> const bromesh::MeshBVH& { return mn->bvh(); };
            if (tryMesh(pickFrameOf(node->worldMatrix()), md, mn->localBounds(), bvhOf)) {
                closestNode = mn;
                closestLight = nullptr;
                closestInstanced = nullptr;
                closestInstance = -1;
            }
        });

        g->root()->traverse([&](scene::SceneNode* node) {
            if (!node || node->type() != scene::SceneNode::Type::InstancedMesh || !node->visible()) return;
            auto* in = static_cast<scene::InstancedMeshNode*>(node);
            const bromesh::MeshData& md = in->mesh();
            if (md.positions.empty() || md.indices.empty()) return;
            const size_t count = in->instanceCount();
            if (count == 0) return;
            const bromath::Mat4& nodeM = node->worldMatrix();
            const bromath::AABB3& lb = in->localBounds();
            auto bvhOf = [&]() -> const bromesh::MeshBVH& { return in->bvh(); };

            for (size_t i = 0; i < count; ++i) {
                float rows[12];
                if (!in->instanceRows(i, rows)) continue;
                if (tryMesh(pickFrameOf(nodeM, rows), md, lb, bvhOf)) {
                    closestNode = nullptr;
                    closestLight = nullptr;
                    closestInstanced = in;
                    closestInstance = static_cast<int>(i);
                }
            }
        });

        if (g->showLightIcons()) {
            const float lightRadius = 0.32f;
            g->root()->traverse([&](scene::SceneNode* node) {
                if (!node || node->type() != scene::SceneNode::Type::Light || !node->visible()) return;
                const bromath::Mat4& M = node->worldMatrix();
                bromath::Vec3 c{M.at(0, 3), M.at(1, 3), M.at(2, 3)};
                bromath::Vec3 oc = origin - c;
                float b_val = bromath::vdot(oc, dir);
                float disc = b_val * b_val - bromath::vdot(oc, oc) + lightRadius * lightRadius;
                if (disc < 0.0f) return;
                float sq = std::sqrt(disc);
                float t = -b_val - sq;
                if (t < 0.0f) t = -b_val + sq;
                if (t < 0.0f || t >= closestDist) return;

                closestDist = t;
                closestLight = static_cast<scene::LightNode*>(node);
                closestNode = nullptr;
                closestInstanced = nullptr;
                closestInstance = -1;
                closestWorldPoint = origin + dir * t;
                closestWorldNormal = bromath::vnorm(closestWorldPoint - c);
            });
        }

        if (!closestNode && !closestLight && !closestInstanced) return ev::null();

        ObjectBuilder out;
        out.set("hit", ev::fromBool(true));
        out.set("distance", ev::fromDouble(closestDist));
        out.set("position", vec3ToValue(closestWorldPoint));
        out.set("point", vec3ToValue(closestWorldPoint));
        out.set("normal", vec3ToValue(closestWorldNormal));

        scene::SceneNode* hitNode = closestNode
            ? static_cast<scene::SceneNode*>(closestNode)
            : (closestInstanced ? static_cast<scene::SceneNode*>(closestInstanced)
                                : static_cast<scene::SceneNode*>(closestLight));
        out.set("node", wrapSceneNode(hitNode, g));
        if (closestInstance >= 0) {
            out.set("instance", ev::fromDouble(closestInstance));
        }
        return out.get();
    });

    b.def("unprojectLocal", 2, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.size() < 2) return ev::null();
        float x = static_cast<float>(ev::toDouble(a[0]));
        float y = static_cast<float>(ev::toDouble(a[1]));
        bromath::Vec3 origin, dir;
        if (!g->unprojectLocal(x, y, origin, dir)) return ev::null();
        ObjectBuilder out;
        out.set("origin", vec3ToValue(origin));
        out.set("dir", vec3ToValue(dir));
        return out.get();
    });

    b.def("toImageData", 0, [](Value self_, std::span<const Value>) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::null();
        int w = 0, h = 0;
        auto pixels = g->readTonemapPixelsRGBA(w, h);
        if (pixels.empty() || w <= 0 || h <= 0) return ev::null();
        return buildImageDataValue(w, h, pixels);
    });

    b.def("captureFrame", 2, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::null();
        if (a.size() >= 2 && ev::isNumber(a[0]) && ev::isNumber(a[1])) {
            int w = static_cast<int>(ev::toDouble(a[0]));
            int h = static_cast<int>(ev::toDouble(a[1]));
            if (w > 0 && h > 0 && (w != g->canvasWidth() || h != g->canvasHeight())) {
                g->setCanvasSize(w, h);
            }
        }
        g->render();
        int rw = 0, rh = 0;
        auto pixels = g->readTonemapPixelsRGBA(rw, rh);
        if (pixels.empty() || rw <= 0 || rh <= 0) return ev::null();
        return buildImageDataValue(rw, rh, pixels);
    });

    b.def("asTexture", 0, [](Value self_, std::span<const Value>) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::null();
        return wrapSceneTexture(g->outputTextureSource());
    });
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
