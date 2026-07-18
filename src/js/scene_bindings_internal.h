#pragma once

// Shared declarations for the scene JS-binding translation units. The scene
// binding surface is split across scene_bindings*.cpp (core/graph
// registration, mesh, anim, fx, view); the wrapper structs, small inline
// helpers, and the raw JS callback functions registered by
// SceneBindings::install() live here so each unit stays cohesive. Mirrors the
// scene_renderer_internal.h convention in src/scene/.
//
// Internal header — only the scene_bindings*.cpp files and
// ai_binding_integration.cpp (which shares the wrapper structs) include this.
//
// ── Wrapper liveness design ────────────────────────────────────────────────
// The engine owns every SceneGraph (keyed to its canvas element) and destroys
// it when the element is detached from the DOM, or at engine teardown. JS
// wrappers can outlive that, so no wrapper stores a raw SceneGraph* or
// SceneNode*. Instead:
//   - GraphWrapper holds a weak_ptr to the graph's LivenessToken and
//     re-resolves the SceneGraph* through it on every call. A dead token (or
//     a nulled token->graph) reads as "graph gone" and the binding no-ops or
//     throws a clean JS error.
//   - NodeWrapper holds {weak token, node id} and resolves the node by id
//     through SceneGraph::resolveNode() on every call. Node ids are
//     process-monotonic and never reused, so this is also the per-node
//     validity check: a node destroyed through ANY path (destroy() on another
//     wrapper of the same node, ancestor subtree destroy, graph teardown)
//     makes every wrapper of it resolve to nullptr.
//   - TweenWrapper holds {weak token, tween id}; tweens were already
//     id-resolved through the graph, the token adds the graph-death check.
// Per-call cost is one weak_ptr lock plus one hash lookup — negligible next
// to the JS call overhead itself.

#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/mesh_node.h"
#include "scene/skinned_mesh_node.h"

#include <qjsbind/qjsbind.h>

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>

namespace bro::scene {
class SpriteNode;
class Particles3DNode;
}

namespace bro::js {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

inline double jsNum(JSContext* ctx, JSValueConst val) {
    double v = 0;
    JS_ToFloat64(ctx, &v, val);
    return v;
}

inline std::string jsStr(JSContext* ctx, JSValueConst val) {
    const char* s = JS_ToCString(ctx, val);
    std::string r = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    return r;
}

/// Parse a CSS-style color string into RGBA. Supports: #RGB, #RRGGBB, #RRGGBBAA,
/// rgb(r,g,b), rgba(r,g,b,a), and named colors (basic subset).
inline bool parseColor(const std::string& str, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    a = 255;
    if (str.empty()) return false;
    if (str[0] == '#') {
        unsigned long hex = std::strtoul(str.c_str() + 1, nullptr, 16);
        if (str.size() == 4) { // #RGB
            r = ((hex >> 8) & 0xF) * 17;
            g = ((hex >> 4) & 0xF) * 17;
            b = (hex & 0xF) * 17;
        } else if (str.size() == 7) { // #RRGGBB
            r = (hex >> 16) & 0xFF;
            g = (hex >> 8) & 0xFF;
            b = hex & 0xFF;
        } else if (str.size() == 9) { // #RRGGBBAA
            r = (hex >> 24) & 0xFF;
            g = (hex >> 16) & 0xFF;
            b = (hex >> 8) & 0xFF;
            a = hex & 0xFF;
        } else return false;
        return true;
    }
    // Named colors (small subset)
    if (str == "red")     { r=255; g=0;   b=0;   return true; }
    if (str == "green")   { r=0;   g=128; b=0;   return true; }
    if (str == "blue")    { r=0;   g=0;   b=255; return true; }
    if (str == "white")   { r=255; g=255; b=255; return true; }
    if (str == "black")   { r=0;   g=0;   b=0;   return true; }
    if (str == "yellow")  { r=255; g=255; b=0;   return true; }
    if (str == "cyan")    { r=0;   g=255; b=255; return true; }
    if (str == "magenta") { r=255; g=0;   b=255; return true; }
    if (str == "gray" || str == "grey") { r=128; g=128; b=128; return true; }
    if (str == "orange")  { r=255; g=165; b=0;   return true; }
    if (str == "purple")  { r=128; g=0;   b=128; return true; }
    if (str == "brown")   { r=165; g=42;  b=42;  return true; }
    if (str == "pink")    { r=255; g=192; b=203; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// SceneNode JS wrapper — {weak liveness token, node id}; the node (never
// owned by JS) is re-resolved through the graph on every call. See the
// liveness design note at the top of this header.
// ---------------------------------------------------------------------------

struct NodeWrapper {
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;
    uint32_t id = 0;

    /// Owning graph, or nullptr once the graph has been destroyed.
    scene::SceneGraph* graph() const {
        auto t = token.lock();
        return t ? t->graph : nullptr;
    }
    /// The node, or nullptr once the node (or its graph) has been destroyed.
    scene::SceneNode* node() const {
        auto t = token.lock();
        return (t && t->graph) ? t->graph->resolveNode(id) : nullptr;
    }
};

inline JSValue wrapNode(JSContext* ctx, scene::SceneNode* node, scene::SceneGraph* graph) {
    if (!node || !graph) return JS_NULL;
    return qjsbind::wrap<NodeWrapper>(
        ctx, new NodeWrapper{graph->livenessToken(), node->id()});
}

// ---------------------------------------------------------------------------
// SceneTexture — live-linked handle to a scene's LDR output texture, minted
// by sceneGraph.asTexture() and consumed by mesh.setBaseColorTexture(handle).
// Holds only a weak_ptr to the source graph's liveness token: pure C++-side
// lifetime — no JSValue stored, so no gc_mark hook is needed and finalizer
// order is irrelevant. Weak by design: a handle never extends the source
// scene's lifetime; once the source graph is destroyed the weak_ptr expires
// and consumers resolve to 0 (the mesh falls back to its base color).
// ---------------------------------------------------------------------------
struct SceneTextureHandle {
    std::weak_ptr<scene::SceneGraph::OutputTextureSource> src;
};

// ---------------------------------------------------------------------------
// Shared JS-function reference: dup'd on install, freed when the last C++
// closure holding it dies. Used by the animation player, tween, sprite
// animation-end, and particle callbacks. Also serves as a generic keep-alive
// pin for any JSValue a C++-side consumer must hold (AgentBinding pins the
// agent/world/terrain wrappers this way — see ai_binding_integration.cpp).
// Native-held refs are GC roots QuickJS cannot collect, so holders must be
// destroyed before JS runtime teardown; the engine tears scene graphs down
// before the runtime (engine_lifecycle.cpp) which covers everything owned by
// a graph, a node, or an agent binding.
// ---------------------------------------------------------------------------
struct JSFnRef {
    JSContext* ctx;
    JSValue fn;
    JSFnRef(JSContext* c, JSValue f) : ctx(c), fn(f) {}
    ~JSFnRef() { JS_FreeValue(ctx, fn); }
    JSFnRef(const JSFnRef&) = delete;
    JSFnRef& operator=(const JSFnRef&) = delete;
};

inline scene::SkinnedMeshNode* asSkinnedMesh(NodeWrapper* w) {
    scene::SceneNode* n = w ? w->node() : nullptr;
    if (n && n->type() == scene::SceneNode::Type::Mesh)
        return static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    return nullptr;
}

// ---------------------------------------------------------------------------
// SceneGraph wrapper — weak liveness token only; the SceneGraph* is
// re-resolved through it on every call (nullptr once destroyed).
// ---------------------------------------------------------------------------

struct GraphWrapper {
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;

    scene::SceneGraph* graph() const {
        auto t = token.lock();
        return t ? t->graph : nullptr;
    }
};

inline scene::SceneGraph* getGraph(JSContext* ctx, JSValueConst val) {
    auto* w = qjsbind::unwrap<GraphWrapper>(ctx, val);
    return w ? w->graph() : nullptr;
}

// Tween wrapper — the C++ Tween is owned by the SceneGraph and referenced by
// id; the weak token adds the graph-death check so a torn-down graph can
// never dangle through a live JS wrapper either.
struct TweenWrapper {
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;
    uint32_t id = 0;

    scene::SceneGraph* graph() const {
        auto t = token.lock();
        return t ? t->graph : nullptr;
    }
};

// ---------------------------------------------------------------------------
// Cross-unit functions. Each is defined in the translation unit noted; the
// raw JSValue callbacks are registered on the SceneNode / SceneGraph / Tween
// prototypes by SceneBindings::install() in scene_bindings.cpp.
// ---------------------------------------------------------------------------

// scene_bindings.cpp — resolve a path against the app base dir + mounts.
std::string resolveAppPath(const std::string& src);

// scene_bindings_mesh.cpp — MeshNode / SkinnedMeshNode / InstancedMeshNode.
JSValue js_node_setBaseColorTexture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_updateMesh(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_createMesh(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_createSkinnedMesh(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_setSkinningMatrices(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_createInstancedMesh(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_setInstancedMesh(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_setInstances(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_setInstancesFromTransforms(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_updateInstance(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_setAtlasGrid(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_setAlphaCutoff(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_setDoubleSided(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_setLodMeshes(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_setShader(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_setShaderUniform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_clearShader(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

// scene_bindings_anim.cpp — skinned-mesh animation player + tween.
JSValue js_node_setSkeleton(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_addClip(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_getBoneWorldMatrix(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_play(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_stop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_pause(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_resume(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_tween_to(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_tween_parallel(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_tween_call(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_tween_loop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_tween_start(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_tween_stop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_tween_pause(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_tween_resume(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_tween_destroy(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_createTween(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
// Wrap a JS function into a void() callback (tween.call / onFinished).
std::function<void()> makeVoidCallback(JSContext* ctx, JSValueConst fnVal);

// scene_bindings_fx.cpp — sprites, shapes, HTML nodes, particles, splats.
JSValue js_node_setHtml(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_markHtmlDirty(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_savePly(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sprite_addAnimation(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_particles_burst(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_particles_clear(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_particles_configure(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_createShape(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_createSprite(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_createHtml(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_createParticles(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_createParticles3D(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_createGaussianSplat(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
// Sprite animation-end JS callback. The callback (a shared JSFnRef) is owned
// by the SpriteNode itself, so it is released on any destruction path —
// direct destroy, ancestor subtree destroy, graph teardown — with no side
// registry to leak.
void installSpriteEndCallback(scene::SpriteNode* node, JSContext* ctx, JSValue fn);
// Particles3D one-shot completion callback.
void installParticles3DOnFinished(JSContext* ctx, JSValueConst fnVal,
                                  scene::Particles3DNode* node);

// audio_scene_sync.cpp — scene-attached audio emitters + camera listener.
JSValue js_node_attachAudioEmitter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_node_detachAudioEmitter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_bindAudioListenerToCamera(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

// scene_bindings_view.cpp — camera, lights, environment, post-FX, capture.
JSValue js_sg_raycast(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setCamera(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_createCamera(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setActiveCamera(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_createLight(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setToneMap(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setAmbient(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setWind(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setShadowQuality(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setShadowCache(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setEnvironment(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setFog(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setTiltShift(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setBloom(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setSSAO(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setDepthOfField(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setColorLUT(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setFXAA(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setRenderScale(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setMSAA(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_unprojectLocal(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_toImageData(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_captureFrame(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_asTexture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

} // namespace bro::js
