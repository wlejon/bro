#pragma once

// Shared declarations for the scene JS-binding translation units. The scene
// binding surface is split across scene_bindings*.cpp (core/graph
// registration, mesh, anim, fx, view); the wrapper structs, small inline
// helpers, and the raw JS callback functions registered by
// SceneBindings::install() live here so each unit stays cohesive. Mirrors the
// scene_renderer_internal.h convention in src/scene/.
//
// Internal header — only the scene_bindings*.cpp files include this.

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
// SceneNode JS wrapper — wraps a SceneNode* (not owned by JS)
// ---------------------------------------------------------------------------

struct NodeWrapper {
    scene::SceneNode* node;
    scene::SceneGraph* graph;
};

inline JSValue wrapNode(JSContext* ctx, scene::SceneNode* node, scene::SceneGraph* graph) {
    auto* w = new NodeWrapper{node, graph};
    return qjsbind::wrap<NodeWrapper>(ctx, w);
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
// closure holding it dies. Used by the animation player + tween callbacks
// (the sprite registry above predates this pattern).
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
    if (w && w->node && w->node->type() == scene::SceneNode::Type::Mesh)
        return static_cast<scene::MeshNode*>(w->node)->asSkinnedMesh();
    return nullptr;
}

// ---------------------------------------------------------------------------
// SceneGraph wrapper
// ---------------------------------------------------------------------------

struct GraphWrapper {
    scene::SceneGraph* graph;
};

inline scene::SceneGraph* getGraph(JSContext* ctx, JSValueConst val) {
    auto* w = qjsbind::unwrap<GraphWrapper>(ctx, val);
    return w ? w->graph : nullptr;
}

// Tween wrapper — the C++ Tween is owned by the SceneGraph and referenced by
// id, so a destroyed tween (or torn-down graph) can never dangle through a
// live JS wrapper.
struct TweenWrapper {
    scene::SceneGraph* graph;
    uint32_t id;
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
// Sprite animation-end JS callback registry (keyed by node id).
void clearSpriteEndCallback(uint32_t nodeId);
void installSpriteEndCallback(scene::SpriteNode* node, JSContext* ctx, JSValue fn);
// Particles3D one-shot completion callback.
void installParticles3DOnFinished(JSContext* ctx, JSValueConst fnVal,
                                  scene::Particles3DNode* node);

// scene_bindings_view.cpp — camera, lights, environment, post-FX, capture.
JSValue js_sg_raycast(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setCamera(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_createLight(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setToneMap(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setAmbient(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setWind(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setShadowQuality(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setEnvironment(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setFog(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setTiltShift(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setBloom(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setRenderScale(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_setMSAA(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_unprojectLocal(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_toImageData(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_captureFrame(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue js_sg_asTexture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

} // namespace bro::js
