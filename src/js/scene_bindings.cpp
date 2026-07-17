#include "js/scene_bindings.h"
#if BRO_WITH_3D  // modular-build feature gate
#include "js/scene_bindings_internal.h"
#include "js/ai_bindings.h"
#include "js/mesh_bindings.h"
#include "js/terrain_bindings.h"
#include "js/tile_bindings.h"
#include "js/dom_bindings.h"
#include "js/physics_bindings.h"
#include "js/rigging_bindings.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/shape_node.h"
#include "scene/sprite_node.h"
#include "scene/physics_node.h"
#include "scene/mesh_node.h"
#include "scene/skinned_mesh_node.h"
#include "scene/animation_player.h"
#include "scene/tween.h"
#include "scene/instanced_mesh_node.h"
#include "scene/gaussian_splat_node.h"
#include "scene/html_node.h"
#include "scene/light_node.h"
#include "scene/particle_node.h"
#include "scene/particles3d_node.h"
#include "dom/element.h"
#include "physics/physics_world.h"
#include "canvas/canvas_scene.h"
#include "js/runtime.h"
#include "util/asset_mounts.h"
#include "util/log.h"

#include <qjsbind/qjsbind.h>

#include <bromesh/primitives/primitives.h>
#include <bromesh/analysis/raycast.h>
#include <bromesh/analysis/bvh.h>
#include <bromesh/io/splat_ply.h>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

JPH_SUPPRESS_WARNINGS

namespace bro::js {

// App-relative path resolution context (set per app load by the engine).
static std::string s_basePath;
static const util::AssetMounts* s_mounts = nullptr;

// Resolve a path against the app base directory and engine mounts. Mirrors
// the rules used by image_bindings: absolute paths and Windows drive paths
// pass through; leading-slash paths consult mounts; everything else is taken
// relative to the app directory.
std::string resolveAppPath(const std::string& src) {
    if (src.size() >= 2 && src[1] == ':') return src;
    if (!src.empty() && (src[0] == '/' || src[0] == '\\')) {
        if (s_mounts) {
            std::string m = s_mounts->resolve(src);
            if (!m.empty()) return m;
        }
        return src;
    }
    if (s_basePath.empty()) return src;
    std::string path = s_basePath;
    if (path.back() != '/' && path.back() != '\\') path += '/';
    return path + src;
}

// ---------------------------------------------------------------------------
// SceneNode raw methods (complex arg handling — kept as standalone functions)
// ---------------------------------------------------------------------------

// add(child)
static JSValue js_node_add(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* pw = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    scene::SceneNode* parent = pw ? pw->node() : nullptr;
    if (!parent || argc < 1) return JS_UNDEFINED;
    auto* cw = qjsbind::unwrap<NodeWrapper>(ctx, argv[0]);
    scene::SceneNode* child = cw ? cw->node() : nullptr;
    if (!child) return JS_ThrowTypeError(ctx, "argument must be a SceneNode");
    parent->addChild(child);
    return JS_DupValue(ctx, this_val);
}

// remove(child)
static JSValue js_node_remove(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* pw = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    scene::SceneNode* parent = pw ? pw->node() : nullptr;
    if (!parent || argc < 1) return JS_UNDEFINED;
    auto* cw = qjsbind::unwrap<NodeWrapper>(ctx, argv[0]);
    scene::SceneNode* child = cw ? cw->node() : nullptr;
    if (child) parent->removeChild(child);
    return JS_UNDEFINED;
}

// destroy() — id-resolved wrappers (this one and any other wrapper of the
// same node or its descendants) all read the node as gone afterwards.
static JSValue js_node_destroy(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (w) {
        if (auto* g = w->graph()) g->destroyNode(w->node());
    }
    return JS_UNDEFINED;
}

// localToWorld(x, y[, z]) → {x, y, z}
static JSValue js_node_localToWorld(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    scene::SceneNode* node = w ? w->node() : nullptr;
    if (!node || argc < 2) return JS_UNDEFINED;
    float z = (argc > 2) ? (float)jsNum(ctx, argv[2]) : 0.0f;
    auto wp = node->localToWorld({(float)jsNum(ctx, argv[0]), (float)jsNum(ctx, argv[1]), z});
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, wp.x));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, wp.y));
    JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, wp.z));
    return obj;
}

// syncToPhysics()
static JSValue js_node_syncToPhysics(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Physics) {
        auto* pn = static_cast<scene::PhysicsNode*>(w->node());
        if (w->graph() && w->graph()->physicsWorld())
            pn->syncToPhysics(w->graph()->physicsWorld());
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// SceneGraph raw methods
// ---------------------------------------------------------------------------

// createNode(name?) → SceneNode
static JSValue js_sg_createNode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;
    std::string name = (argc > 0) ? jsStr(ctx, argv[0]) : "";
    auto* node = g->createNode(name);
    g->root()->addChild(node);
    return wrapNode(ctx, node, g);
}

// createPhysicsNode(opts) → SceneNode (PhysicsNode)
static JSValue js_sg_createPhysicsNode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createPhysicsNode();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        // Body tag (JS-side monotonic id; mapped via PhysicsBindings)
        JSValue bodyVal = JS_GetPropertyStr(ctx, opts, "body");
        if (JS_IsNumber(bodyVal)) {
            int32_t bodyTag;
            JS_ToInt32(ctx, &bodyTag, bodyVal);
            JPH::BodyID id = PhysicsBindings::bodyIdForTag(bodyTag);
            if (!id.IsInvalid()) node->setBody(id);
        }
        JS_FreeValue(ctx, bodyVal);

        // Pixels per unit
        JSValue ppuVal = JS_GetPropertyStr(ctx, opts, "pixelsPerUnit");
        if (!JS_IsUndefined(ppuVal)) node->setPixelsPerUnit((float)jsNum(ctx, ppuVal));
        JS_FreeValue(ctx, ppuVal);

        // Auto sync
        JSValue asVal = JS_GetPropertyStr(ctx, opts, "autoSync");
        if (!JS_IsUndefined(asVal)) node->setAutoSync(JS_ToBool(ctx, asVal));
        JS_FreeValue(ctx, asVal);
    }

    return wrapNode(ctx, node, g);
}

// findById(id) → SceneNode | null
static JSValue js_sg_findById(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1) return JS_NULL;
    int32_t id;
    JS_ToInt32(ctx, &id, argv[0]);
    auto* node = g->findById((uint32_t)id);
    return node ? wrapNode(ctx, node, g) : JS_NULL;
}

// findByName(name) → SceneNode | null
static JSValue js_sg_findByName(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1) return JS_NULL;
    auto* node = g->findByName(jsStr(ctx, argv[0]));
    return node ? wrapNode(ctx, node, g) : JS_NULL;
}

// syncPhysics()
// setFrustumCulling(on) — escape hatch for the (default-on) frustum culling
// in the forward + shadow passes. Culling is conservative, so pixels are
// identical either way; turn it off when bisecting a rendering regression.
static JSValue js_sg_setFrustumCulling(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (g && argc >= 1) g->setFrustumCulling(JS_ToBool(ctx, argv[0]));
    return JS_UNDEFINED;
}

// cullStats() — per-category drawn/culled counters from the most recent
// rendered frame. Shadow counts are per caster x atlas tile.
static JSValue js_sg_cullStats(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_NULL;
    const scene::CullStats& s = g->cullStats();
    JSValue o = JS_NewObject(ctx);
    auto num = [&](const char* k, int v) {
        JS_SetPropertyStr(ctx, o, k, JS_NewInt32(ctx, v));
    };
    num("meshDrawn", s.meshDrawn);
    num("meshCulled", s.meshCulled);
    num("instancedDrawn", s.instancedDrawn);
    num("instancedCulled", s.instancedCulled);
    num("splatDrawn", s.splatDrawn);
    num("splatCulled", s.splatCulled);
    num("particlesDrawn", s.particlesDrawn);
    num("particlesCulled", s.particlesCulled);
    num("billboardsDrawn", s.billboardsDrawn);
    num("billboardsCulled", s.billboardsCulled);
    num("shadowDrawn", s.shadowDrawn);
    num("shadowCulled", s.shadowCulled);
    return o;
}

static JSValue js_sg_syncPhysics(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* g = getGraph(ctx, this_val);
    if (g) g->syncPhysics();
    return JS_UNDEFINED;
}

// destroyNode(node)
static JSValue js_sg_destroyNode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1) return JS_UNDEFINED;
    auto* cw = qjsbind::unwrap<NodeWrapper>(ctx, argv[0]);
    if (cw) g->destroyNode(cw->node());
    return JS_UNDEFINED;
}

static JSValue mat4ToJSArray(JSContext* ctx, const bromath::Mat4& mat) {
    JSValue arr = JS_NewArray(ctx);
    int idx = 0;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            JS_SetPropertyUint32(ctx, arr, idx++, JS_NewFloat64(ctx, mat.at(r, c)));
    return arr;
}

static JSValue vec3ToJSArray(JSContext* ctx, const bromath::Vec3& v) {
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, v.x));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, v.y));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, v.z));
    return arr;
}

// createTerrain(opts) → Terrain
static JSValue js_sg_createTerrain(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_NULL;
    JSValueConst opts = (argc >= 1 && JS_IsObject(argv[0])) ? argv[0] : JS_UNDEFINED;
    JSValue tmpOpts = JS_UNDEFINED;
    if (JS_IsUndefined(opts)) {
        tmpOpts = JS_NewObject(ctx);
        opts = tmpOpts;
    }
    JSValue result = createTerrainJS(ctx, g, opts);
    JS_FreeValue(ctx, tmpOpts);
    return result;
}

// createTileWorld(opts) → TileWorld
static JSValue js_sg_createTileWorld(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_NULL;
    JSValueConst opts = (argc >= 1 && JS_IsObject(argv[0])) ? argv[0] : JS_UNDEFINED;
    JSValue tmpOpts = JS_UNDEFINED;
    if (JS_IsUndefined(opts)) {
        tmpOpts = JS_NewObject(ctx);
        opts = tmpOpts;
    }
    JSValue result = createTileWorldJS(ctx, g, opts);
    JS_FreeValue(ctx, tmpOpts);
    return result;
}

// ---------------------------------------------------------------------------
// Install / Cleanup
// ---------------------------------------------------------------------------

void SceneBindings::setAppContext(const std::string& basePath,
                                  const util::AssetMounts* mounts) {
    s_basePath = basePath;
    s_mounts = mounts;
}

void SceneBindings::install(JSContext* ctx) {
    // --- SceneTexture handle (scene-as-texture, minted by asTexture()) ---
    qjsbind::Class<SceneTextureHandle>(ctx, "SceneTexture")
        // True while the source scene still exists. Turning false is exactly
        // the moment consuming meshes fall back to their base color.
        .get("valid", [](SceneTextureHandle* h) -> bool {
            auto s = h ? h->src.lock() : nullptr;
            return s && s->graph;
        });

    // --- Tween class ---
    qjsbind::Class<TweenWrapper>(ctx, "Tween")
        .get("isRunning", [](TweenWrapper* w, JSContext* ctx) -> JSValue {
            auto* t = w->graph() ? w->graph()->findTween(w->id) : nullptr;
            return JS_NewBool(ctx, t && t->isRunning());
        })
        .get("isPaused", [](TweenWrapper* w, JSContext* ctx) -> JSValue {
            auto* t = w->graph() ? w->graph()->findTween(w->id) : nullptr;
            return JS_NewBool(ctx, t && t->isPaused());
        })
        .get("isFinished", [](TweenWrapper* w, JSContext* ctx) -> JSValue {
            auto* t = w->graph() ? w->graph()->findTween(w->id) : nullptr;
            return JS_NewBool(ctx, t && t->isFinished());
        })
        .prop("onFinished",
            [](TweenWrapper*, JSContext*) -> JSValue { return JS_UNDEFINED; },
            [](TweenWrapper* w, JSContext* ctx, JSValue val) {
                auto* t = w->graph() ? w->graph()->findTween(w->id) : nullptr;
                if (!t) return;
                if (JS_IsFunction(ctx, val)) t->setOnFinished(makeVoidCallback(ctx, val));
                else t->setOnFinished(nullptr);
            })
        .method_raw("to", js_tween_to, 3)
        .method_raw("parallel", js_tween_parallel, 0)
        .method_raw("call", js_tween_call, 1)
        .method_raw("loop", js_tween_loop, 1)
        .method_raw("start", js_tween_start, 0)
        .method_raw("stop", js_tween_stop, 0)
        .method_raw("pause", js_tween_pause, 0)
        .method_raw("resume", js_tween_resume, 0)
        .method_raw("destroy", js_tween_destroy, 0);

    // --- SceneNode class ---
    qjsbind::Class<NodeWrapper>(ctx, "SceneNode")
        // Common properties
        .get("id", [](NodeWrapper* w) -> int { return w->node() ? w->node()->id() : 0; })
        .prop("name",
            [](NodeWrapper* w) -> std::string { return w->node() ? w->node()->name() : ""; },
            [](NodeWrapper* w, std::string val) { if (w->node()) w->node()->setName(val); })
        .prop("visible",
            [](NodeWrapper* w) -> bool { return w->node() ? w->node()->visible() : false; },
            [](NodeWrapper* w, bool val) { if (w->node()) w->node()->setVisible(val); })
        .get("childCount", [](NodeWrapper* w) -> int {
            return (w && w->node()) ? (int)w->node()->children().size() : 0;
        })
        .get("instanceCount", [](NodeWrapper* w) -> int {
            if (w && w->node() && w->node()->type() == scene::SceneNode::Type::InstancedMesh)
                return (int)static_cast<scene::InstancedMeshNode*>(w->node())->instanceCount();
            return 0;
        })
        .get("splatCount", [](NodeWrapper* w) -> int {
            if (w && w->node() && w->node()->type() == scene::SceneNode::Type::GaussianSplat)
                return (int)static_cast<scene::GaussianSplatNode*>(w->node())->splatCount();
            return 0;
        })
        .get("atlasCols", [](NodeWrapper* w) -> int {
            if (w && w->node() && w->node()->type() == scene::SceneNode::Type::InstancedMesh)
                return static_cast<scene::InstancedMeshNode*>(w->node())->atlasCols();
            return 0;
        })
        .get("atlasRows", [](NodeWrapper* w) -> int {
            if (w && w->node() && w->node()->type() == scene::SceneNode::Type::InstancedMesh)
                return static_cast<scene::InstancedMeshNode*>(w->node())->atlasRows();
            return 0;
        })
        .prop("alphaCutoff",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::InstancedMesh)
                    return JS_NewFloat64(ctx, (double)static_cast<scene::InstancedMeshNode*>(w->node())->alphaCutoff());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::InstancedMesh)
                    static_cast<scene::InstancedMeshNode*>(w->node())->setAlphaCutoff((float)val);
            })
        .prop("doubleSided",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::InstancedMesh)
                    return JS_NewBool(ctx, static_cast<scene::InstancedMeshNode*>(w->node())->doubleSided() ? 1 : 0);
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, bool val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::InstancedMesh)
                    static_cast<scene::InstancedMeshNode*>(w->node())->setDoubleSided(val);
            })
        .get("boneCount", [](NodeWrapper* w) -> int {
            if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Mesh) {
                if (auto* sm = static_cast<scene::MeshNode*>(w->node())->asSkinnedMesh())
                    return sm->boneCount();
            }
            return 0;
        })
        .get("skinReady", [](NodeWrapper* w) -> bool {
            if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Mesh) {
                if (auto* sm = static_cast<scene::MeshNode*>(w->node())->asSkinnedMesh())
                    return sm->skinReady();
            }
            return false;
        })
        // True while a custom shader (node.setShader) is installed on this
        // MeshNode / InstancedMeshNode; false on other node types.
        .get("hasShader", [](NodeWrapper* w) -> bool {
            if (!w || !w->node()) return false;
            if (w->node()->type() == scene::SceneNode::Type::Mesh)
                return static_cast<scene::MeshNode*>(w->node())->hasCustomShader();
            if (w->node()->type() == scene::SceneNode::Type::InstancedMesh)
                return static_cast<scene::InstancedMeshNode*>(w->node())->hasCustomShader();
            return false;
        })
        // Extra world-space padding on this node's culling bounds — the
        // escape hatch for custom vertex shaders that displace geometry
        // beyond the mesh AABB (culling can't see GLSL). Same contract as
        // Godot's extra_cull_margin. Mesh / InstancedMesh only; undefined
        // elsewhere.
        .prop("cullMargin",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node()) {
                    if (w->node()->type() == scene::SceneNode::Type::Mesh)
                        return JS_NewFloat64(ctx, (double)static_cast<scene::MeshNode*>(w->node())->cullMargin());
                    if (w->node()->type() == scene::SceneNode::Type::InstancedMesh)
                        return JS_NewFloat64(ctx, (double)static_cast<scene::InstancedMeshNode*>(w->node())->cullMargin());
                }
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (!w || !w->node()) return;
                if (w->node()->type() == scene::SceneNode::Type::Mesh)
                    static_cast<scene::MeshNode*>(w->node())->setCullMargin((float)val);
                else if (w->node()->type() == scene::SceneNode::Type::InstancedMesh)
                    static_cast<scene::InstancedMeshNode*>(w->node())->setCullMargin((float)val);
            })
        .get("type", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (!w || !w->node()) return JS_UNDEFINED;
            switch (w->node()->type()) {
                case scene::SceneNode::Type::Mesh:
                    return JS_NewString(ctx,
                        static_cast<scene::MeshNode*>(w->node())->asSkinnedMesh()
                            ? "skinnedMesh" : "mesh");
                case scene::SceneNode::Type::InstancedMesh: return JS_NewString(ctx, "instancedMesh");
                case scene::SceneNode::Type::Light:   return JS_NewString(ctx, "light");
                case scene::SceneNode::Type::Shape:   return JS_NewString(ctx, "shape");
                case scene::SceneNode::Type::Sprite:  return JS_NewString(ctx, "sprite");
                case scene::SceneNode::Type::Physics: return JS_NewString(ctx, "physics");
                case scene::SceneNode::Type::Html:    return JS_NewString(ctx, "html");
                case scene::SceneNode::Type::GaussianSplat: return JS_NewString(ctx, "gaussianSplat");
                case scene::SceneNode::Type::Particles:   return JS_NewString(ctx, "particles");
                case scene::SceneNode::Type::Particles3D: return JS_NewString(ctx, "particles3d");
                case scene::SceneNode::Type::Base:    return JS_NewString(ctx, "group");
                default: break;
            }
            return JS_UNDEFINED;
        })

        // Transform
        .prop("position",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (!w || !w->node()) return JS_UNDEFINED;
                const auto& p = w->node()->position();
                JSValue arr = JS_NewArray(ctx);
                JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, p.x));
                JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, p.y));
                JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, p.z));
                return arr;
            },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (!w || !w->node() || !JS_IsArray(val)) return;
                JSValue e0 = JS_GetPropertyUint32(ctx, val, 0);
                JSValue e1 = JS_GetPropertyUint32(ctx, val, 1);
                JSValue e2 = JS_GetPropertyUint32(ctx, val, 2);
                double x=0, y=0, z=0;
                JS_ToFloat64(ctx, &x, e0);
                JS_ToFloat64(ctx, &y, e1);
                JS_ToFloat64(ctx, &z, e2);
                JS_FreeValue(ctx, e0); JS_FreeValue(ctx, e1); JS_FreeValue(ctx, e2);
                w->node()->setPosition((float)x, (float)y, (float)z);
            })
        .prop("x",
            [](NodeWrapper* w) -> double { return w->node() ? w->node()->position().x : 0; },
            [](NodeWrapper* w, double val) { if (w->node()) w->node()->setPosition((float)val, w->node()->position().y, w->node()->position().z); })
        .prop("y",
            [](NodeWrapper* w) -> double { return w->node() ? w->node()->position().y : 0; },
            [](NodeWrapper* w, double val) { if (w->node()) w->node()->setPosition(w->node()->position().x, (float)val, w->node()->position().z); })
        .prop("z",
            [](NodeWrapper* w) -> double { return w->node() ? w->node()->position().z : 0; },
            [](NodeWrapper* w, double val) { if (w->node()) w->node()->setPosition(w->node()->position().x, w->node()->position().y, (float)val); })
        .prop("rotation",
            [](NodeWrapper* w) -> double { return w->node() ? bromath::qtoEuler(w->node()->rotation()).z : 0; },
            [](NodeWrapper* w, double val) { if (w->node()) w->node()->setRotationZ((float)val); })
        .prop("rotationX",
            [](NodeWrapper* w) -> double { return w->node() ? bromath::qtoEuler(w->node()->rotation()).x : 0; },
            [](NodeWrapper* w, double val) {
                if (w->node()) {
                    auto e = bromath::qtoEuler(w->node()->rotation());
                    w->node()->setRotationEuler((float)val, e.y, e.z);
                }
            })
        .prop("rotationY",
            [](NodeWrapper* w) -> double { return w->node() ? bromath::qtoEuler(w->node()->rotation()).y : 0; },
            [](NodeWrapper* w, double val) {
                if (w->node()) {
                    auto e = bromath::qtoEuler(w->node()->rotation());
                    w->node()->setRotationEuler(e.x, (float)val, e.z);
                }
            })
        .prop("rotationZ",
            [](NodeWrapper* w) -> double { return w->node() ? bromath::qtoEuler(w->node()->rotation()).z : 0; },
            [](NodeWrapper* w, double val) {
                if (w->node()) {
                    auto e = bromath::qtoEuler(w->node()->rotation());
                    w->node()->setRotationEuler(e.x, e.y, (float)val);
                }
            })
        // [x,y,z,w] quaternion. Unlike rotationX/Y/Z (which round-trip
        // through Euler each set), this writes the node orientation
        // atomically — required when assigning arbitrary rotations
        // (e.g. port-to-port mating in the parts DSL).
        .prop("quaternion",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (!w || !w->node()) return JS_UNDEFINED;
                const auto& q = w->node()->rotation();
                JSValue arr = JS_NewArray(ctx);
                JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, q.x));
                JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, q.y));
                JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, q.z));
                JS_SetPropertyUint32(ctx, arr, 3, JS_NewFloat64(ctx, q.w));
                return arr;
            },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (!w || !w->node()) return;
                if (!JS_IsArray(val)) return;
                JSValue e0 = JS_GetPropertyUint32(ctx, val, 0);
                JSValue e1 = JS_GetPropertyUint32(ctx, val, 1);
                JSValue e2 = JS_GetPropertyUint32(ctx, val, 2);
                JSValue e3 = JS_GetPropertyUint32(ctx, val, 3);
                double qx = 0, qy = 0, qz = 0, qw = 1;
                JS_ToFloat64(ctx, &qx, e0);
                JS_ToFloat64(ctx, &qy, e1);
                JS_ToFloat64(ctx, &qz, e2);
                JS_ToFloat64(ctx, &qw, e3);
                JS_FreeValue(ctx, e0);
                JS_FreeValue(ctx, e1);
                JS_FreeValue(ctx, e2);
                JS_FreeValue(ctx, e3);
                bromath::Quat q{(float)qx, (float)qy, (float)qz, (float)qw};
                w->node()->setRotation(bromath::qnorm(q));
            })
        .prop("scaleX",
            [](NodeWrapper* w) -> double { return w->node() ? w->node()->scale().x : 1; },
            [](NodeWrapper* w, double val) { if (w->node()) w->node()->setScale((float)val, w->node()->scale().y, w->node()->scale().z); })
        .prop("scaleY",
            [](NodeWrapper* w) -> double { return w->node() ? w->node()->scale().y : 1; },
            [](NodeWrapper* w, double val) { if (w->node()) w->node()->setScale(w->node()->scale().x, (float)val, w->node()->scale().z); })
        .prop("scaleZ",
            [](NodeWrapper* w) -> double { return w->node() ? w->node()->scale().z : 1; },
            [](NodeWrapper* w, double val) { if (w->node()) w->node()->setScale(w->node()->scale().x, w->node()->scale().y, (float)val); })

        // Mesh material (PBR) — no-op on non-mesh nodes.
        .prop("metallic",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Mesh)
                    return JS_NewFloat64(ctx, static_cast<scene::MeshNode*>(w->node())->metallic());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Mesh)
                    static_cast<scene::MeshNode*>(w->node())->setMetallic((float)val);
            })
        .prop("roughness",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Mesh)
                    return JS_NewFloat64(ctx, static_cast<scene::MeshNode*>(w->node())->roughness());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Mesh)
                    static_cast<scene::MeshNode*>(w->node())->setRoughness((float)val);
            })
        .prop("emissive",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Mesh)
                    return JS_NewFloat64(ctx, static_cast<scene::MeshNode*>(w->node())->emissive());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Mesh)
                    static_cast<scene::MeshNode*>(w->node())->setEmissive((float)val);
            })

        // LightNode properties — no-op on non-light nodes.
        .get("kind", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::Light)
                return JS_UNDEFINED;
            switch (static_cast<scene::LightNode*>(w->node())->kind()) {
                case scene::LightNode::Kind::Directional: return JS_NewString(ctx, "directional");
                case scene::LightNode::Kind::Point:       return JS_NewString(ctx, "point");
                case scene::LightNode::Kind::Spot:        return JS_NewString(ctx, "spot");
            }
            return JS_UNDEFINED;
        })
        .prop("direction",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::Light)
                    return JS_UNDEFINED;
                const auto& d = static_cast<scene::LightNode*>(w->node())->direction();
                JSValue arr = JS_NewArray(ctx);
                JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, d.x));
                JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, d.y));
                JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, d.z));
                return arr;
            },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::Light) return;
                if (!JS_IsArray(val)) return;
                double x = 0, y = -1, z = 0;
                JSValue e0 = JS_GetPropertyUint32(ctx, val, 0);
                JSValue e1 = JS_GetPropertyUint32(ctx, val, 1);
                JSValue e2 = JS_GetPropertyUint32(ctx, val, 2);
                JS_ToFloat64(ctx, &x, e0);
                JS_ToFloat64(ctx, &y, e1);
                JS_ToFloat64(ctx, &z, e2);
                static_cast<scene::LightNode*>(w->node())->setDirection(
                    {(float)x, (float)y, (float)z});
                JS_FreeValue(ctx, e0);
                JS_FreeValue(ctx, e1);
                JS_FreeValue(ctx, e2);
            })
        .prop("color",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (!w || !w->node()) return JS_UNDEFINED;
                if (w->node()->type() == scene::SceneNode::Type::Light) {
                    const auto& c = static_cast<scene::LightNode*>(w->node())->color();
                    JSValue arr = JS_NewArray(ctx);
                    JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, c.x));
                    JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, c.y));
                    JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, c.z));
                    return arr;
                }
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::Light) return;
                auto* L = static_cast<scene::LightNode*>(w->node());
                if (JS_IsString(val)) {
                    uint8_t r, g, b, a;
                    if (parseColor(jsStr(ctx, val), r, g, b, a))
                        L->setColor(r/255.0f, g/255.0f, b/255.0f);
                } else if (JS_IsArray(val)) {
                    double cr = 1, cg = 1, cb = 1;
                    JSValue e0 = JS_GetPropertyUint32(ctx, val, 0);
                    JSValue e1 = JS_GetPropertyUint32(ctx, val, 1);
                    JSValue e2 = JS_GetPropertyUint32(ctx, val, 2);
                    JS_ToFloat64(ctx, &cr, e0);
                    JS_ToFloat64(ctx, &cg, e1);
                    JS_ToFloat64(ctx, &cb, e2);
                    L->setColor((float)cr, (float)cg, (float)cb);
                    JS_FreeValue(ctx, e0);
                    JS_FreeValue(ctx, e1);
                    JS_FreeValue(ctx, e2);
                }
            })
        .prop("intensity",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Light)
                    return JS_NewFloat64(ctx, static_cast<scene::LightNode*>(w->node())->intensity());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node())->setIntensity((float)val);
            })
        .prop("range",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Light)
                    return JS_NewFloat64(ctx, static_cast<scene::LightNode*>(w->node())->range());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node())->setRange((float)val);
            })
        .prop("innerAngle",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Light)
                    return JS_NewFloat64(ctx, static_cast<scene::LightNode*>(w->node())->innerAngle());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node())->setInnerAngle((float)val);
            })
        .prop("outerAngle",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Light)
                    return JS_NewFloat64(ctx, static_cast<scene::LightNode*>(w->node())->outerAngle());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node())->setOuterAngle((float)val);
            })
        .prop("castsShadow",
            [](NodeWrapper* w) -> bool {
                if (!w || !w->node()) return false;
                if (w->node()->type() == scene::SceneNode::Type::Light)
                    return static_cast<scene::LightNode*>(w->node())->castsShadow();
                if (w->node()->type() == scene::SceneNode::Type::Mesh)
                    return static_cast<scene::MeshNode*>(w->node())->castsShadow();
                return false;
            },
            [](NodeWrapper* w, bool val) {
                if (!w || !w->node()) return;
                if (w->node()->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node())->setCastsShadow(val);
                else if (w->node()->type() == scene::SceneNode::Type::Mesh)
                    static_cast<scene::MeshNode*>(w->node())->setCastsShadow(val);
            })
        .prop("receivesShadow",
            [](NodeWrapper* w) -> bool {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Mesh)
                    return static_cast<scene::MeshNode*>(w->node())->receivesShadow();
                return false;
            },
            [](NodeWrapper* w, bool val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Mesh)
                    static_cast<scene::MeshNode*>(w->node())->setReceivesShadow(val);
            })
        .prop("shadowBias",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Light)
                    return JS_NewFloat64(ctx, static_cast<scene::LightNode*>(w->node())->shadowBias());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node())->setShadowBias((float)val);
            })
        .prop("shadowNormalBias",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Light)
                    return JS_NewFloat64(ctx, static_cast<scene::LightNode*>(w->node())->shadowNormalBias());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node())->setShadowNormalBias((float)val);
            })
        .prop("cascadeCount",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Light)
                    return JS_NewInt32(ctx, static_cast<scene::LightNode*>(w->node())->cascadeCount());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, int32_t val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node())->setCascadeCount(val);
            })
        .prop("cascadeSplitLambda",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Light)
                    return JS_NewFloat64(ctx, static_cast<scene::LightNode*>(w->node())->cascadeSplitLambda());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node())->setCascadeSplitLambda((float)val);
            })

        // Shape properties (silently return undefined / no-op for non-shape nodes)
        .prop("width",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node()) {
                    if (w->node()->type() == scene::SceneNode::Type::Shape)
                        return JS_NewFloat64(ctx, static_cast<scene::ShapeNode*>(w->node())->width());
                    if (w->node()->type() == scene::SceneNode::Type::Sprite)
                        return JS_NewFloat64(ctx, static_cast<scene::SpriteNode*>(w->node())->width());
                }
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Shape) {
                    auto* s = static_cast<scene::ShapeNode*>(w->node());
                    s->setSize(val, s->height());
                }
            })
        .prop("height",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Shape)
                    return JS_NewFloat64(ctx, static_cast<scene::ShapeNode*>(w->node())->height());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Shape) {
                    auto* s = static_cast<scene::ShapeNode*>(w->node());
                    s->setSize(s->width(), val);
                }
            })
        .prop("radius",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Shape)
                    return JS_NewFloat64(ctx, static_cast<scene::ShapeNode*>(w->node())->radius());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Shape)
                    static_cast<scene::ShapeNode*>(w->node())->setRadius(val);
            })
        .prop("fillColor",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Shape) {
                    auto c = static_cast<scene::ShapeNode*>(w->node())->fillColor();
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.2f)",
                                  static_cast<int>(c.r * 255.0f + 0.5f),
                                  static_cast<int>(c.g * 255.0f + 0.5f),
                                  static_cast<int>(c.b * 255.0f + 0.5f), c.a);
                    return JS_NewString(ctx, buf);
                }
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Shape) {
                    uint8_t r, g, b, a;
                    if (parseColor(jsStr(ctx, val), r, g, b, a))
                        static_cast<scene::ShapeNode*>(w->node())->setFillColor(bromath::cfromColor8({r, g, b, a}));
                }
            })
        .prop("strokeColor",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Shape) {
                    auto c = static_cast<scene::ShapeNode*>(w->node())->strokeColor();
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.2f)",
                                  static_cast<int>(c.r * 255.0f + 0.5f),
                                  static_cast<int>(c.g * 255.0f + 0.5f),
                                  static_cast<int>(c.b * 255.0f + 0.5f), c.a);
                    return JS_NewString(ctx, buf);
                }
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Shape) {
                    uint8_t r, g, b, a;
                    if (parseColor(jsStr(ctx, val), r, g, b, a))
                        static_cast<scene::ShapeNode*>(w->node())->setStrokeColor(bromath::cfromColor8({r, g, b, a}));
                }
            })
        .prop("strokeWidth",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Shape)
                    return JS_NewFloat64(ctx, static_cast<scene::ShapeNode*>(w->node())->strokeWidth());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Shape) {
                    auto* s = static_cast<scene::ShapeNode*>(w->node());
                    s->setStrokeWidth(val);
                    s->setHasStroke(true);
                }
            })

        // Physics properties
        .prop("autoSync",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Physics)
                    return JS_NewBool(ctx, static_cast<scene::PhysicsNode*>(w->node())->autoSync());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, bool val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Physics)
                    static_cast<scene::PhysicsNode*>(w->node())->setAutoSync(val);
            })
        .prop("pixelsPerUnit",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Physics)
                    return JS_NewFloat64(ctx, static_cast<scene::PhysicsNode*>(w->node())->pixelsPerUnit());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Physics)
                    static_cast<scene::PhysicsNode*>(w->node())->setPixelsPerUnit(val);
            })
        .get("bodyId", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Physics) {
                auto* p = static_cast<scene::PhysicsNode*>(w->node());
                if (p->hasBody())
                    return JS_NewInt32(ctx, (int32_t)p->bodyId().GetIndexAndSequenceNumber());
                return JS_NULL;
            }
            return JS_UNDEFINED;
        })

        // World anchor + billboard (Shape/Sprite/Html only — no-ops elsewhere)
        .prop("worldAnchor",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (!w || !w->node()) return JS_UNDEFINED;
                if (!w->node()->hasWorldAnchor()) return JS_NULL;
                const auto& a = w->node()->worldAnchor();
                JSValue arr = JS_NewArray(ctx);
                JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, a.x));
                JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, a.y));
                JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, a.z));
                return arr;
            },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (!w || !w->node()) return;
                if (JS_IsNull(val) || JS_IsUndefined(val)) {
                    w->node()->clearWorldAnchor();
                    return;
                }
                if (JS_IsArray(val)) {
                    double x = 0, y = 0, z = 0;
                    JSValue e0 = JS_GetPropertyUint32(ctx, val, 0);
                    JSValue e1 = JS_GetPropertyUint32(ctx, val, 1);
                    JSValue e2 = JS_GetPropertyUint32(ctx, val, 2);
                    JS_ToFloat64(ctx, &x, e0);
                    JS_ToFloat64(ctx, &y, e1);
                    JS_ToFloat64(ctx, &z, e2);
                    JS_FreeValue(ctx, e0);
                    JS_FreeValue(ctx, e1);
                    JS_FreeValue(ctx, e2);
                    w->node()->setWorldAnchor({(float)x, (float)y, (float)z});
                }
            })
        .prop("billboard",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (!w || !w->node()) return JS_UNDEFINED;
                return JS_NewString(ctx,
                    w->node()->billboardMode() == scene::SceneNode::BillboardMode::YLock
                        ? "ylock" : "full");
            },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (!w || !w->node()) return;
                std::string s = jsStr(ctx, val);
                if (s == "ylock" || s == "yLock" || s == "y-lock") {
                    w->node()->setBillboardMode(scene::SceneNode::BillboardMode::YLock);
                } else {
                    w->node()->setBillboardMode(scene::SceneNode::BillboardMode::Full);
                }
            })

        // HtmlNode: `root` is the detached DOM Element that JS can mutate
        // imperatively. Mutations automatically mark the DOM dirty; the raster
        // thread re-rasterizes on the next frame.
        .get("root", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (!w || !w->node()) return JS_UNDEFINED;
            if (w->node()->type() != scene::SceneNode::Type::Html) return JS_UNDEFINED;
            auto* hn = static_cast<scene::HtmlNode*>(w->node());
            dom::Element* root = hn->root();
            if (!root) return JS_NULL;
            return DomBindings::wrapElement(ctx, root);
        })

        .get("parent", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (!w || !w->node()) return JS_NULL;
            auto* p = w->node()->parent();
            return p ? wrapNode(ctx, p, w->graph()) : JS_NULL;
        })
        .prop("nearClipDist",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Mesh)
                    return JS_NewFloat64(ctx, static_cast<scene::MeshNode*>(w->node())->nearClipDist());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Mesh)
                    static_cast<scene::MeshNode*>(w->node())->setNearClipDist((float)val);
            })

        // Complex read-only properties
        .get("children", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (!w || !w->node()) return JS_NewArray(ctx);
            const auto& kids = w->node()->children();
            JSValue arr = JS_NewArray(ctx);
            uint32_t i = 0;
            for (auto* child : kids) {
                if (child) JS_SetPropertyUint32(ctx, arr, i++, wrapNode(ctx, child, w->graph()));
            }
            return arr;
        })

        // SpriteNode: frame index + isPlaying + currentAnimation
        .prop("frameIndex",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Sprite)
                    return JS_NewInt32(ctx, static_cast<scene::SpriteNode*>(w->node())->frameIndex());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, int32_t val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Sprite)
                    static_cast<scene::SpriteNode*>(w->node())->setFrameIndex(val);
            })
        .get("isPlaying", [](NodeWrapper* w) -> bool {
            if (!w || !w->node()) return false;
            if (w->node()->type() == scene::SceneNode::Type::Sprite)
                return static_cast<scene::SpriteNode*>(w->node())->isPlaying();
            if (w->node()->type() == scene::SceneNode::Type::Particles)
                return static_cast<scene::ParticleNode*>(w->node())->isPlaying();
            if (w->node()->type() == scene::SceneNode::Type::Particles3D)
                return static_cast<scene::Particles3DNode*>(w->node())->isPlaying();
            if (auto* sm = asSkinnedMesh(w))
                return sm->player() && sm->player()->isPlaying();
            return false;
        })
        .get("currentAnimation", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Sprite) {
                const auto& s = static_cast<scene::SpriteNode*>(w->node())->currentAnimation();
                return JS_NewString(ctx, s.c_str());
            }
            if (auto* sm = asSkinnedMesh(w)) {
                return JS_NewString(ctx,
                    sm->player() ? sm->player()->currentClip().c_str() : "");
            }
            return JS_UNDEFINED;
        })
        // Skinned-mesh animation player: speed multiplier, scrubbable clock,
        // clip duration.
        .prop("animationSpeed",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (auto* sm = asSkinnedMesh(w))
                    return JS_NewFloat64(ctx, sm->player() ? sm->player()->speed() : 1.0);
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (auto* sm = asSkinnedMesh(w))
                    sm->ensurePlayer().setSpeed((float)val);
            })
        .prop("animationTime",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (auto* sm = asSkinnedMesh(w))
                    return JS_NewFloat64(ctx, sm->player() ? sm->player()->time() : 0.0);
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (auto* sm = asSkinnedMesh(w)) {
                    if (auto* player = sm->player()) player->setTime((float)val);
                }
            })
        .get("animationDuration", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (auto* sm = asSkinnedMesh(w))
                return JS_NewFloat64(ctx, sm->player() ? sm->player()->duration() : 0.0);
            return JS_UNDEFINED;
        })
        // Fired once when a non-looping clip (base or layer) reaches its end,
        // with the clip name.
        .prop("onAnimationFinished",
            [](NodeWrapper*, JSContext*) -> JSValue { return JS_UNDEFINED; },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                auto* sm = asSkinnedMesh(w);
                if (!sm) return;
                auto& player = sm->ensurePlayer();
                if (JS_IsFunction(ctx, val)) {
                    auto ref = std::make_shared<JSFnRef>(ctx, JS_DupValue(ctx, val));
                    player.setOnFinished([ref](const std::string& name) {
                        JSValue fn = JS_DupValue(ref->ctx, ref->fn);
                        JSValue arg = JS_NewString(ref->ctx, name.c_str());
                        JSValue r = JS_Call(ref->ctx, fn, JS_UNDEFINED, 1, &arg);
                        if (JS_IsException(r)) Runtime::checkException(ref->ctx, r);
                        JS_FreeValue(ref->ctx, r);
                        JS_FreeValue(ref->ctx, arg);
                        JS_FreeValue(ref->ctx, fn);
                    });
                } else {
                    player.setOnFinished(nullptr);
                }
            })
        // ParticleNode / Particles3DNode: live count + emitter rate.
        // `particleCount` is the documented name; `liveCount` is kept as the
        // original 2D alias.
        .get("liveCount", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Particles)
                return JS_NewInt32(ctx, static_cast<scene::ParticleNode*>(w->node())->liveCount());
            if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Particles3D)
                return JS_NewInt32(ctx, static_cast<scene::Particles3DNode*>(w->node())->liveCount());
            return JS_UNDEFINED;
        })
        .get("particleCount", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Particles)
                return JS_NewInt32(ctx, static_cast<scene::ParticleNode*>(w->node())->liveCount());
            if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Particles3D)
                return JS_NewInt32(ctx, static_cast<scene::Particles3DNode*>(w->node())->liveCount());
            return JS_UNDEFINED;
        })
        .prop("rate",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Particles)
                    return JS_NewFloat64(ctx, static_cast<scene::ParticleNode*>(w->node())->rate());
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Particles3D)
                    return JS_NewFloat64(ctx, static_cast<scene::Particles3DNode*>(w->node())->rate());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Particles)
                    static_cast<scene::ParticleNode*>(w->node())->setRate((float)val);
                else if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Particles3D)
                    static_cast<scene::Particles3DNode*>(w->node())->setRate((float)val);
            })
        // Particles3DNode: soft-particle fade distance (world units, 0 = off)
        .prop("softness",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Particles3D)
                    return JS_NewFloat64(ctx, static_cast<scene::Particles3DNode*>(w->node())->softness());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Particles3D)
                    static_cast<scene::Particles3DNode*>(w->node())->setSoftness((float)val);
            })
        // Particles3DNode: one-shot completion callback (see createParticles3D)
        .prop("onFinished",
            [](NodeWrapper*, JSContext*) -> JSValue { return JS_UNDEFINED; },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (w && w->node() && w->node()->type() == scene::SceneNode::Type::Particles3D)
                    installParticles3DOnFinished(ctx, val,
                        static_cast<scene::Particles3DNode*>(w->node()));
            })
        // SpriteNode: animation-end callback. Setter installs/removes the JS
        // callback in the side registry.
        .prop("onAnimationEnd",
            [](NodeWrapper*, JSContext*) -> JSValue { return JS_UNDEFINED; },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (!w || !w->node() || w->node()->type() != scene::SceneNode::Type::Sprite) return;
                installSpriteEndCallback(static_cast<scene::SpriteNode*>(w->node()), ctx, val);
            })

        // Methods (raw — complex arg handling)
        .method_raw("add", js_node_add, 1)
        .method_raw("remove", js_node_remove, 1)
        .method_raw("destroy", js_node_destroy, 0)
        .method_raw("localToWorld", js_node_localToWorld, 2)
        .method_raw("syncToPhysics", js_node_syncToPhysics, 0)
        .method_raw("updateMesh", js_node_updateMesh, 1)
        .method_raw("setSkinningMatrices", js_node_setSkinningMatrices, 1)
        .method_raw("setInstances", js_node_setInstances, 1)
        .method_raw("setInstancesFromTransforms", js_node_setInstancesFromTransforms, 1)
        .method_raw("updateInstance", js_node_updateInstance, 2)
        .method_raw("setInstancedMesh", js_node_setInstancedMesh, 1)
        .method_raw("setAtlasGrid", js_node_setAtlasGrid, 2)
        .method_raw("setAlphaCutoff", js_node_setAlphaCutoff, 1)
        .method_raw("setDoubleSided", js_node_setDoubleSided, 1)
        .method_raw("setBaseColorTexture", js_node_setBaseColorTexture, 1)
        .method_raw("setShader", js_node_setShader, 1)
        .method_raw("setShaderUniform", js_node_setShaderUniform, 2)
        .method_raw("clearShader", js_node_clearShader, 0)
        .method_raw("setHtml", js_node_setHtml, 1)
        .method_raw("markHtmlDirty", js_node_markHtmlDirty, 0)
        .method_raw("savePly", js_node_savePly, 1)
        .method_raw("attachAgent", nodeAttachAgent, 3)
        .method_raw("detachAgent", nodeDetachAgent, 0)
        .method_raw("navigateTo", nodeNavigateTo, 2)
        .method_raw("stopNavigation", nodeStopNavigation, 0)
        // Sprite animation + Particles + skinned-mesh player control
        .method_raw("play", js_node_play, 1)
        .method_raw("stop", js_node_stop, 0)
        .method_raw("pause", js_node_pause, 0)
        .method_raw("resume", js_node_resume, 0)
        .method_raw("setSkeleton", js_node_setSkeleton, 1)
        .method_raw("addClip", js_node_addClip, 2)
        .method_raw("getBoneWorldMatrix", js_node_getBoneWorldMatrix, 1)
        .method_raw("addAnimation", js_sprite_addAnimation, 2)
        .method_raw("burst", js_particles_burst, 1)
        .method_raw("clear", js_particles_clear, 0)
        .method_raw("configure", js_particles_configure, 1);

    // --- SceneGraph class ---
    qjsbind::Class<GraphWrapper>(ctx, "SceneGraph")
        // Properties
        .get("root", [](GraphWrapper* w, JSContext* ctx) -> JSValue {
            return (w && w->graph()) ? wrapNode(ctx, w->graph()->root(), w->graph()) : JS_UNDEFINED;
        })
        .prop("cameraX",
            [](GraphWrapper* w) -> double { return w && w->graph() ? w->graph()->cameraX() : 0; },
            [](GraphWrapper* w, double val) { if (w && w->graph()) w->graph()->setCameraPosition((float)val, w->graph()->cameraY()); })
        .prop("cameraY",
            [](GraphWrapper* w) -> double { return w && w->graph() ? w->graph()->cameraY() : 0; },
            [](GraphWrapper* w, double val) { if (w && w->graph()) w->graph()->setCameraPosition(w->graph()->cameraX(), (float)val); })
        .prop("cameraZoom",
            [](GraphWrapper* w) -> double { return w && w->graph() ? w->graph()->cameraZoom() : 1; },
            [](GraphWrapper* w, double val) { if (w && w->graph()) w->graph()->setCameraZoom((float)val); })
        .prop("showLightIcons",
            [](GraphWrapper* w) -> bool { return w && w->graph() ? w->graph()->showLightIcons() : false; },
            [](GraphWrapper* w, bool val) { if (w && w->graph()) w->graph()->setShowLightIcons(val); })
        .prop("frustumCulling",
            [](GraphWrapper* w) -> bool { return w && w->graph() ? w->graph()->frustumCulling() : true; },
            [](GraphWrapper* w, bool val) { if (w && w->graph()) w->graph()->setFrustumCulling(val); })
        .prop("renderScale",
            [](GraphWrapper* w) -> double { return w && w->graph() ? w->graph()->renderScale() : 1.0; },
            [](GraphWrapper* w, double val) { if (w && w->graph()) w->graph()->setRenderScale((float)val); })
        .prop("msaa",
            [](GraphWrapper* w) -> double { return w && w->graph() ? w->graph()->msaa() : 0; },
            [](GraphWrapper* w, double val) { if (w && w->graph()) w->graph()->setMSAA((int)val); })

        // Methods (all raw — complex arg handling)
        .method_raw("createNode", js_sg_createNode, 1)
        .method_raw("createShape", js_sg_createShape, 1)
        .method_raw("createSprite", js_sg_createSprite, 1)
        .method_raw("createPhysicsNode", js_sg_createPhysicsNode, 1)
        .method_raw("createMesh", js_sg_createMesh, 1)
        .method_raw("createSkinnedMesh", js_sg_createSkinnedMesh, 1)
        .method_raw("createInstancedMesh", js_sg_createInstancedMesh, 1)
        .method_raw("createGaussianSplat", js_sg_createGaussianSplat, 1)
        .method_raw("createHtmlNode", js_sg_createHtml, 1)
        .method_raw("createLight", js_sg_createLight, 1)
        .method_raw("createParticles", js_sg_createParticles, 1)
        .method_raw("createParticles3D", js_sg_createParticles3D, 1)
        .method_raw("createTween", js_sg_createTween, 0)
        .method_raw("setToneMap", js_sg_setToneMap, 1)
        .method_raw("setAmbient", js_sg_setAmbient, 1)
        .method_raw("setWind", js_sg_setWind, 1)
        .method_raw("setShadowQuality", js_sg_setShadowQuality, 1)
        .method_raw("createTerrain", js_sg_createTerrain, 1)
        .method_raw("createTileWorld", js_sg_createTileWorld, 1)
        .method_raw("findById", js_sg_findById, 1)
        .method_raw("findByName", js_sg_findByName, 1)
        .method_raw("destroyNode", js_sg_destroyNode, 1)
        .method_raw("setCamera", js_sg_setCamera, 1)
        .method_raw("setFog", js_sg_setFog, 1)
        .method_raw("setTiltShift", js_sg_setTiltShift, 1)
        .method_raw("setBloom", js_sg_setBloom, 1)
        .method_raw("setSSAO", js_sg_setSSAO, 1)
        .method_raw("setDepthOfField", js_sg_setDepthOfField, 1)
        .method_raw("setRenderScale", js_sg_setRenderScale, 1)
        .method_raw("setMSAA", js_sg_setMSAA, 1)
        .method_raw("setEnvironment", js_sg_setEnvironment, 1)
        .method_raw("setFrustumCulling", js_sg_setFrustumCulling, 1)
        .method_raw("cullStats", js_sg_cullStats, 0)
        .method_raw("syncPhysics", js_sg_syncPhysics, 0)
        .method_raw("raycast", js_sg_raycast, 2)
        .method_raw("unprojectLocal", js_sg_unprojectLocal, 2)
        .method_raw("toImageData", js_sg_toImageData, 0)
        .method_raw("captureFrame", js_sg_captureFrame, 2)
        .method_raw("asTexture", js_sg_asTexture, 0)
        .get("viewMatrix", [](GraphWrapper* w, JSContext* ctx) -> JSValue {
            if (!w || !w->graph()) return JS_NULL;
            return mat4ToJSArray(ctx, w->graph()->viewMatrix());
        })
        .get("projectionMatrix", [](GraphWrapper* w, JSContext* ctx) -> JSValue {
            if (!w || !w->graph()) return JS_NULL;
            return mat4ToJSArray(ctx, w->graph()->projectionMatrix());
        })
        .get("cameraEye", [](GraphWrapper* w, JSContext* ctx) -> JSValue {
            if (!w || !w->graph()) return JS_NULL;
            return vec3ToJSArray(ctx, w->graph()->cameraEye());
        })
        .method_raw("attachAIWorld", graphAttachAIWorld, 2)
        .method_raw("detachAIWorld", graphDetachAIWorld, 0);
}

JSValue SceneBindings::wrapSceneGraph(JSContext* ctx, scene::SceneGraph* graph) {
    if (!graph) return JS_NULL;
    return qjsbind::wrap<GraphWrapper>(ctx, new GraphWrapper{graph->livenessToken()});
}

void SceneBindings::cleanup(JSContext* ctx) {
    // No persistent JSValue/atom storage in this binding — qjsbind finalizers
    // handle wrappers and the engine-level globalThis sweep drops bro.scene.
    (void)ctx;
}

} // namespace bro::js

#endif  // BRO_WITH_3D
