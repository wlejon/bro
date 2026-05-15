#include "js/scene_bindings.h"
#include "js/ai_bindings.h"
#include "js/mesh_bindings.h"
#include "js/terrain_bindings.h"
#include "js/dom_bindings.h"
#include "js/physics_bindings.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/shape_node.h"
#include "scene/sprite_node.h"
#include "scene/physics_node.h"
#include "scene/mesh_node.h"
#include "scene/instanced_mesh_node.h"
#include "scene/html_node.h"
#include "scene/light_node.h"
#include "scene/particle_node.h"
#include "scene/tilemap_node.h"
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

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

#include <cmath>
#include <cstring>
#include <string>

JPH_SUPPRESS_WARNINGS

namespace bro::js {

// App-relative path resolution context (set per app load by the engine).
static std::string s_basePath;
static const util::AssetMounts* s_mounts = nullptr;

// Resolve a path against the app base directory and engine mounts. Mirrors
// the rules used by image_bindings: absolute paths and Windows drive paths
// pass through; leading-slash paths consult mounts; everything else is taken
// relative to the app directory.
static std::string resolveAppPath(const std::string& src) {
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
// Helpers
// ---------------------------------------------------------------------------

static double jsNum(JSContext* ctx, JSValueConst val) {
    double v = 0;
    JS_ToFloat64(ctx, &v, val);
    return v;
}

static double jsGetProp(JSContext* ctx, JSValueConst obj, const char* prop, double def = 0.0) {
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

static std::string jsStr(JSContext* ctx, JSValueConst val) {
    const char* s = JS_ToCString(ctx, val);
    std::string r = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    return r;
}

static std::string jsGetStr(JSContext* ctx, JSValueConst obj, const char* prop, const char* def = "") {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    std::string r = def;
    if (JS_IsString(v)) r = jsStr(ctx, v);
    JS_FreeValue(ctx, v);
    return r;
}

/// Parse a CSS-style color string into RGBA. Supports: #RGB, #RRGGBB, #RRGGBBAA,
/// rgb(r,g,b), rgba(r,g,b,a), and named colors (basic subset).
static bool parseColor(const std::string& str, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
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

// Parse a `worldAnchor: [x, y, z]` or `{x, y, z}` option into a Vec3.
// Returns true if the option was present (even if partially specified).
static bool parseWorldAnchor(JSContext* ctx, JSValueConst opts, bromath::Vec3& out) {
    JSValue v = JS_GetPropertyStr(ctx, opts, "worldAnchor");
    bool found = false;
    if (JS_IsArray(v)) {
        found = true;
        JSValue e0 = JS_GetPropertyUint32(ctx, v, 0);
        JSValue e1 = JS_GetPropertyUint32(ctx, v, 1);
        JSValue e2 = JS_GetPropertyUint32(ctx, v, 2);
        double x = 0, y = 0, z = 0;
        JS_ToFloat64(ctx, &x, e0);
        JS_ToFloat64(ctx, &y, e1);
        JS_ToFloat64(ctx, &z, e2);
        out = {(float)x, (float)y, (float)z};
        JS_FreeValue(ctx, e0);
        JS_FreeValue(ctx, e1);
        JS_FreeValue(ctx, e2);
    } else if (JS_IsObject(v)) {
        found = true;
        double x = 0, y = 0, z = 0;
        JSValue ex = JS_GetPropertyStr(ctx, v, "x");
        JSValue ey = JS_GetPropertyStr(ctx, v, "y");
        JSValue ez = JS_GetPropertyStr(ctx, v, "z");
        if (!JS_IsUndefined(ex)) JS_ToFloat64(ctx, &x, ex);
        if (!JS_IsUndefined(ey)) JS_ToFloat64(ctx, &y, ey);
        if (!JS_IsUndefined(ez)) JS_ToFloat64(ctx, &z, ez);
        out = {(float)x, (float)y, (float)z};
        JS_FreeValue(ctx, ex);
        JS_FreeValue(ctx, ey);
        JS_FreeValue(ctx, ez);
    }
    JS_FreeValue(ctx, v);
    return found;
}

// Apply worldAnchor + billboard options to any SceneNode. Safe to call on
// nodes that don't support billboarding — the fields are harmless.
static void applyBillboardOpts(JSContext* ctx, JSValueConst opts, scene::SceneNode* node) {
    if (!node) return;

    bromath::Vec3 anchor;
    if (parseWorldAnchor(ctx, opts, anchor)) {
        node->setWorldAnchor(anchor);
    }

    JSValue bbVal = JS_GetPropertyStr(ctx, opts, "billboard");
    if (JS_IsString(bbVal)) {
        std::string mode = jsStr(ctx, bbVal);
        if (mode == "ylock" || mode == "yLock" || mode == "y-lock") {
            node->setBillboardMode(scene::SceneNode::BillboardMode::YLock);
        } else {
            node->setBillboardMode(scene::SceneNode::BillboardMode::Full);
        }
    }
    JS_FreeValue(ctx, bbVal);
}

static scene::Color parseColorProp(JSContext* ctx, JSValueConst obj, const char* prop) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    scene::Color c;
    if (JS_IsString(v)) {
        parseColor(jsStr(ctx, v), c.r, c.g, c.b, c.a);
    } else if (JS_IsObject(v)) {
        c.r = (uint8_t)jsGetProp(ctx, v, "r", 255);
        c.g = (uint8_t)jsGetProp(ctx, v, "g", 255);
        c.b = (uint8_t)jsGetProp(ctx, v, "b", 255);
        c.a = (uint8_t)jsGetProp(ctx, v, "a", 255);
    }
    JS_FreeValue(ctx, v);
    return c;
}

// ---------------------------------------------------------------------------
// SceneNode JS wrapper — wraps a SceneNode* (not owned by JS)
// ---------------------------------------------------------------------------

struct NodeWrapper {
    scene::SceneNode* node;
    scene::SceneGraph* graph;
};

static JSValue wrapNode(JSContext* ctx, scene::SceneNode* node, scene::SceneGraph* graph) {
    auto* w = new NodeWrapper{node, graph};
    return qjsbind::wrap<NodeWrapper>(ctx, w);
}

// ---------------------------------------------------------------------------
// Shape / Physics helpers (cast node to subclass)
// ---------------------------------------------------------------------------

static scene::ShapeNode* asShape(JSContext* ctx, JSValueConst val) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, val);
    if (w && w->node && w->node->type() == scene::SceneNode::Type::Shape)
        return static_cast<scene::ShapeNode*>(w->node);
    return nullptr;
}

static scene::PhysicsNode* asPhysics(JSContext* ctx, JSValueConst val) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, val);
    if (w && w->node && w->node->type() == scene::SceneNode::Type::Physics)
        return static_cast<scene::PhysicsNode*>(w->node);
    return nullptr;
}

// ---------------------------------------------------------------------------
// SceneNode raw methods (complex arg handling — kept as standalone functions)
// ---------------------------------------------------------------------------

// add(child)
static JSValue js_node_add(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* pw = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!pw || argc < 1) return JS_UNDEFINED;
    auto* cw = qjsbind::unwrap<NodeWrapper>(ctx, argv[0]);
    if (!cw) return JS_ThrowTypeError(ctx, "argument must be a SceneNode");
    pw->node->addChild(cw->node);
    return JS_DupValue(ctx, this_val);
}

// remove(child)
static JSValue js_node_remove(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* pw = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!pw || argc < 1) return JS_UNDEFINED;
    auto* cw = qjsbind::unwrap<NodeWrapper>(ctx, argv[0]);
    if (cw) pw->node->removeChild(cw->node);
    return JS_UNDEFINED;
}

// Forward decl — clears the JS animation-end callback for a SpriteNode id.
static void clearSpriteEndCallback(uint32_t nodeId);

// destroy()
static JSValue js_node_destroy(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (w && w->graph) {
        if (w->node) clearSpriteEndCallback(w->node->id());
        w->graph->destroyNode(w->node);
        w->node = nullptr;
    }
    return JS_UNDEFINED;
}

// localToWorld(x, y[, z]) → {x, y, z}
static JSValue js_node_localToWorld(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || argc < 2) return JS_UNDEFINED;
    float z = (argc > 2) ? (float)jsNum(ctx, argv[2]) : 0.0f;
    auto wp = w->node->localToWorld({(float)jsNum(ctx, argv[0]), (float)jsNum(ctx, argv[1]), z});
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, wp.x));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, wp.y));
    JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, wp.z));
    return obj;
}

// syncToPhysics()
static JSValue js_node_syncToPhysics(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (w && w->node && w->node->type() == scene::SceneNode::Type::Physics) {
        auto* pn = static_cast<scene::PhysicsNode*>(w->node);
        if (w->graph && w->graph->physicsWorld())
            pn->syncToPhysics(w->graph->physicsWorld());
    }
    return JS_UNDEFINED;
}

// --- Forward decls used by updateMesh (defined later) ---
static bool jsReadFloatArray(JSContext* ctx, JSValueConst obj, const char* prop,
                             std::vector<float>& out);
static bool jsReadUint32Array(JSContext* ctx, JSValueConst obj, const char* prop,
                              std::vector<uint32_t>& out);

// setHtml(htmlString) — HtmlNode only
static JSValue js_node_setHtml(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node || argc < 1) return JS_UNDEFINED;
    if (w->node->type() != scene::SceneNode::Type::Html)
        return JS_ThrowTypeError(ctx, "setHtml: node is not an HtmlNode");
    auto* hn = static_cast<scene::HtmlNode*>(w->node);
    hn->setHtml(jsStr(ctx, argv[0]));
    return JS_UNDEFINED;
}

// markHtmlDirty() — HtmlNode only; force a re-raster on the next frame.
// Useful after imperative DOM mutation via node.root.
static JSValue js_node_markHtmlDirty(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node) return JS_UNDEFINED;
    if (w->node->type() != scene::SceneNode::Type::Html)
        return JS_ThrowTypeError(ctx, "markHtmlDirty: node is not an HtmlNode");
    static_cast<scene::HtmlNode*>(w->node)->markHtmlDirty();
    return JS_UNDEFINED;
}

// setBaseColorTexture(tex|null) — replace or clear the baseColor texture at
// runtime. `tex` shape matches createMesh's `texture` option:
// { width, height, data: Uint8Array(rgba8) }. Pass null/undefined to clear
// so the mesh falls back to `uColor` (and vertex colors if present).
static JSValue js_node_setBaseColorTexture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node || w->node->type() != scene::SceneNode::Type::Mesh)
        return JS_ThrowTypeError(ctx, "setBaseColorTexture: not a MeshNode");
    auto* meshNode = static_cast<scene::MeshNode*>(w->node);

    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        meshNode->clearBaseColorTexture();
        return JS_UNDEFINED;
    }
    if (!JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "setBaseColorTexture: expected { width, height, data } or null");

    int w_ = (int)jsGetProp(ctx, argv[0], "width",  0);
    int h_ = (int)jsGetProp(ctx, argv[0], "height", 0);
    JSValue dataVal = JS_GetPropertyStr(ctx, argv[0], "data");
    size_t off = 0, len = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, dataVal, &off, &len, nullptr);
    if (!JS_IsException(ab)) {
        size_t bytes = 0;
        uint8_t* base = JS_GetArrayBuffer(ctx, &bytes, ab);
        if (base && w_ > 0 && h_ > 0 && len >= (size_t)w_ * (size_t)h_ * 4) {
            meshNode->setBaseColorTexture(w_, h_, base + off);
        }
        JS_FreeValue(ctx, ab);
    }
    JS_FreeValue(ctx, dataVal);
    return JS_UNDEFINED;
}

// updateMesh(meshOrOpts[, opts])
static JSValue js_node_updateMesh(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node)
        return JS_ThrowTypeError(ctx, "updateMesh: invalid node");
    if (w->node->type() != scene::SceneNode::Type::Mesh)
        return JS_ThrowTypeError(ctx, "updateMesh: node is not a MeshNode");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "updateMesh: missing argument");

    auto* meshNode = static_cast<scene::MeshNode*>(w->node);
    bromesh::MeshData meshData;
    bool gotData = false;

    bool transfer = false;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        transfer = jsGetBool(ctx, argv[1], "transfer", false);
    }

    // Path 1: argument is a Mesh object directly.
    if (MeshBindings::getMeshData(ctx, argv[0])) {
        if (transfer) {
            if (auto taken = MeshBindings::takeMeshData(ctx, argv[0])) {
                meshData = std::move(*taken);
                gotData = true;
            }
        } else {
            meshData = *MeshBindings::getMeshData(ctx, argv[0]);
            gotData = true;
        }
    }

    // Path 2: options object with `mesh:`/`data:` (Mesh) or raw typed arrays.
    if (!gotData && JS_IsObject(argv[0])) {
        bool transferOpt = jsGetBool(ctx, argv[0], "transfer", false);

        auto tryKey = [&](const char* key) -> bool {
            JSValue v = JS_GetPropertyStr(ctx, argv[0], key);
            bool took = false;
            if (!JS_IsUndefined(v) && MeshBindings::getMeshData(ctx, v)) {
                if (transferOpt) {
                    if (auto taken = MeshBindings::takeMeshData(ctx, v)) {
                        meshData = std::move(*taken);
                        took = true;
                    }
                } else {
                    meshData = *MeshBindings::getMeshData(ctx, v);
                    took = true;
                }
            }
            JS_FreeValue(ctx, v);
            return took;
        };
        if (tryKey("mesh") || tryKey("data"))
            gotData = true;

        if (!gotData) {
            std::vector<float> positions, normals;
            std::vector<uint32_t> indices;
            if (jsReadFloatArray(ctx, argv[0], "positions", positions) &&
                jsReadUint32Array(ctx, argv[0], "indices", indices)) {
                meshData.positions = std::move(positions);
                meshData.indices = std::move(indices);
                if (jsReadFloatArray(ctx, argv[0], "normals", normals)) {
                    meshData.normals = std::move(normals);
                }
                gotData = true;
            }
        }
    }

    if (!gotData)
        return JS_ThrowTypeError(ctx, "updateMesh: argument must be a Mesh or {positions,indices}");

    meshNode->setMesh(std::move(meshData));
    return JS_DupValue(ctx, this_val);
}

// Forward declarations — parseAnimSpec and getGraph are defined further down
// (alongside the SceneGraph wrapper / sprite createSprite handler), but the
// sprite/particle/tilemap helpers below need them. The sprite-sheet related
// types referenced are also forward-declared above via scene_node.h.
struct GraphWrapper;
static inline scene::SceneGraph* getGraph(JSContext* ctx, JSValueConst val);
static scene::SpriteNode::AnimationSpec parseAnimSpec(JSContext* ctx, JSValueConst obj);

// ---------------------------------------------------------------------------
// Sprite animation-end JS callback registry (keyed by node id).
// ---------------------------------------------------------------------------

namespace {
struct SpriteEndCB { JSContext* ctx; JSValue fn; };
}
static std::unordered_map<uint32_t, SpriteEndCB>& spriteEndCallbacks() {
    static std::unordered_map<uint32_t, SpriteEndCB> map;
    return map;
}
static void clearSpriteEndCallback(uint32_t nodeId) {
    auto& m = spriteEndCallbacks();
    auto it = m.find(nodeId);
    if (it != m.end()) {
        JS_FreeValue(it->second.ctx, it->second.fn);
        m.erase(it);
    }
}
static void installSpriteEndCallback(scene::SpriteNode* node, JSContext* ctx, JSValue fn) {
    uint32_t id = node->id();
    clearSpriteEndCallback(id);
    if (JS_IsFunction(ctx, fn)) {
        spriteEndCallbacks()[id] = { ctx, JS_DupValue(ctx, fn) };
        node->setOnAnimationEnd([id](const std::string& name) {
            auto& m = spriteEndCallbacks();
            auto it = m.find(id);
            if (it == m.end()) return;
            JSContext* c = it->second.ctx;
            JSValue dup = JS_DupValue(c, it->second.fn);
            JSValue arg = JS_NewString(c, name.c_str());
            JSValue ret = JS_Call(c, dup, JS_UNDEFINED, 1, &arg);
            if (JS_IsException(ret)) {
                Runtime::checkException(c, ret);
            }
            JS_FreeValue(c, ret);
            JS_FreeValue(c, arg);
            JS_FreeValue(c, dup);
        });
    } else {
        node->setOnAnimationEnd(nullptr);
    }
}

// ---------------------------------------------------------------------------
// Sprite node methods
// ---------------------------------------------------------------------------

// Unified play() for SpriteNode and ParticleNode. Sprite: optional animation
// name. Particles: ignores any args.
static JSValue js_node_play(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node) return JS_UNDEFINED;
    if (w->node->type() == scene::SceneNode::Type::Sprite) {
        auto* s = static_cast<scene::SpriteNode*>(w->node);
        if (argc > 0 && JS_IsString(argv[0])) s->play(jsStr(ctx, argv[0]));
        else                                  s->resume();
    } else if (w->node->type() == scene::SceneNode::Type::Particles) {
        static_cast<scene::ParticleNode*>(w->node)->play();
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue js_node_stop(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node) return JS_UNDEFINED;
    if (w->node->type() == scene::SceneNode::Type::Sprite) {
        static_cast<scene::SpriteNode*>(w->node)->stop();
    } else if (w->node->type() == scene::SceneNode::Type::Particles) {
        static_cast<scene::ParticleNode*>(w->node)->stop();
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue js_sprite_addAnimation(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node || w->node->type() != scene::SceneNode::Type::Sprite || argc < 2)
        return JS_UNDEFINED;
    auto* s = static_cast<scene::SpriteNode*>(w->node);
    std::string name = jsStr(ctx, argv[0]);
    if (JS_IsObject(argv[1])) {
        s->addAnimation(name, parseAnimSpec(ctx, argv[1]));
    }
    return JS_DupValue(ctx, this_val);
}

// ---------------------------------------------------------------------------
// Particle node helpers + methods
// ---------------------------------------------------------------------------

static scene::Color colorFromJS(JSContext* ctx, JSValueConst v, scene::Color def) {
    if (JS_IsString(v)) {
        uint8_t r, g, b, a;
        std::string s = jsStr(ctx, v);
        if (parseColor(s, r, g, b, a)) return {r, g, b, a};
    }
    return def;
}

// Read a property that can be a number or {min,max}; returns chosen min,max.
static void parseRange(JSContext* ctx, JSValueConst opts, const char* key,
                       float& outMin, float& outMax) {
    JSValue v = JS_GetPropertyStr(ctx, opts, key);
    if (JS_IsNumber(v)) {
        double n = 0; JS_ToFloat64(ctx, &n, v);
        outMin = outMax = (float)n;
    } else if (JS_IsObject(v)) {
        outMin = (float)jsGetProp(ctx, v, "min", outMin);
        outMax = (float)jsGetProp(ctx, v, "max", outMax);
    }
    JS_FreeValue(ctx, v);
}

static void applyParticleOpts(JSContext* ctx, JSValueConst opts, scene::ParticleNode* node) {
    // maxParticles (must run before any burst/play)
    JSValue mpVal = JS_GetPropertyStr(ctx, opts, "maxParticles");
    if (JS_IsNumber(mpVal)) {
        int32_t n = 256; JS_ToInt32(ctx, &n, mpVal);
        node->setMaxParticles(n);
    }
    JS_FreeValue(ctx, mpVal);

    // texture
    JSValue texVal = JS_GetPropertyStr(ctx, opts, "texture");
    if (JS_IsString(texVal)) node->setTexturePath(jsStr(ctx, texVal));
    JS_FreeValue(ctx, texVal);

    // blend
    JSValue blendVal = JS_GetPropertyStr(ctx, opts, "blend");
    if (JS_IsString(blendVal)) {
        std::string s = jsStr(ctx, blendVal);
        node->setBlend(s == "additive" ? scene::ParticleNode::Blend::Additive
                                       : scene::ParticleNode::Blend::Normal);
    }
    JS_FreeValue(ctx, blendVal);

    // rate
    JSValue rateVal = JS_GetPropertyStr(ctx, opts, "rate");
    if (JS_IsNumber(rateVal)) node->setRate((float)jsNum(ctx, rateVal));
    JS_FreeValue(ctx, rateVal);

    // lifetime
    {
        float lo = 0.5f, hi = 1.0f;
        parseRange(ctx, opts, "lifetime", lo, hi);
        node->setLifetime(lo, hi);
    }

    // velocity: { angle, angleSpread, speed, speedSpread }
    JSValue velVal = JS_GetPropertyStr(ctx, opts, "velocity");
    if (JS_IsObject(velVal)) {
        node->setVelocity(
            (float)jsGetProp(ctx, velVal, "angle", -90),
            (float)jsGetProp(ctx, velVal, "angleSpread", 360),
            (float)jsGetProp(ctx, velVal, "speed", 100),
            (float)jsGetProp(ctx, velVal, "speedSpread", 0));
    }
    JS_FreeValue(ctx, velVal);

    // gravity: { x, y } or [x, y]
    JSValue gravVal = JS_GetPropertyStr(ctx, opts, "gravity");
    if (JS_IsObject(gravVal)) {
        if (JS_IsArray(gravVal)) {
            JSValue gx = JS_GetPropertyUint32(ctx, gravVal, 0);
            JSValue gy = JS_GetPropertyUint32(ctx, gravVal, 1);
            node->setGravity((float)jsNum(ctx, gx), (float)jsNum(ctx, gy));
            JS_FreeValue(ctx, gx); JS_FreeValue(ctx, gy);
        } else {
            node->setGravity(
                (float)jsGetProp(ctx, gravVal, "x", 0),
                (float)jsGetProp(ctx, gravVal, "y", 0));
        }
    }
    JS_FreeValue(ctx, gravVal);

    // size: { start, end }
    JSValue sizeVal = JS_GetPropertyStr(ctx, opts, "size");
    if (JS_IsObject(sizeVal)) {
        node->setSize(
            (float)jsGetProp(ctx, sizeVal, "start", 6),
            (float)jsGetProp(ctx, sizeVal, "end", 0));
    } else if (JS_IsNumber(sizeVal)) {
        float v = (float)jsNum(ctx, sizeVal);
        node->setSize(v, v);
    }
    JS_FreeValue(ctx, sizeVal);

    // color: { start, end }
    JSValue colorVal = JS_GetPropertyStr(ctx, opts, "color");
    if (JS_IsObject(colorVal)) {
        JSValue cs = JS_GetPropertyStr(ctx, colorVal, "start");
        JSValue ce = JS_GetPropertyStr(ctx, colorVal, "end");
        scene::Color start = colorFromJS(ctx, cs, {255,255,255,255});
        scene::Color end   = colorFromJS(ctx, ce, {start.r, start.g, start.b, 0});
        node->setColors(start, end);
        JS_FreeValue(ctx, cs); JS_FreeValue(ctx, ce);
    } else if (JS_IsString(colorVal)) {
        scene::Color c = colorFromJS(ctx, colorVal, {255,255,255,255});
        scene::Color end = c; end.a = 0;
        node->setColors(c, end);
    }
    JS_FreeValue(ctx, colorVal);

    // rotation: { start, spinSpeed, spinSpread }
    JSValue rotVal = JS_GetPropertyStr(ctx, opts, "rotation");
    if (JS_IsObject(rotVal)) {
        node->setRotation(
            (float)jsGetProp(ctx, rotVal, "start", 0),
            (float)jsGetProp(ctx, rotVal, "spinSpeed", 0),
            (float)jsGetProp(ctx, rotVal, "spinSpread", 0));
    }
    JS_FreeValue(ctx, rotVal);

    // drag (per-second multiplier)
    JSValue dragVal = JS_GetPropertyStr(ctx, opts, "drag");
    if (JS_IsNumber(dragVal)) node->setDrag((float)jsNum(ctx, dragVal));
    JS_FreeValue(ctx, dragVal);

    // initial burst
    JSValue burstVal = JS_GetPropertyStr(ctx, opts, "burst");
    if (JS_IsNumber(burstVal)) {
        int32_t n = 0; JS_ToInt32(ctx, &n, burstVal);
        node->burst(n);
    }
    JS_FreeValue(ctx, burstVal);

    // autoplay (default true)
    JSValue apVal = JS_GetPropertyStr(ctx, opts, "autoplay");
    bool autoplay = JS_IsUndefined(apVal) ? true : JS_ToBool(ctx, apVal);
    JS_FreeValue(ctx, apVal);
    if (autoplay) node->play(); else node->stop();
}

// createParticles(opts?) → SceneNode (ParticleNode)
static JSValue js_sg_createParticles(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;
    auto* node = g->createParticles();
    g->root()->addChild(node);
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];
        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);
        JSValue xVal = JS_GetPropertyStr(ctx, opts, "x");
        JSValue yVal = JS_GetPropertyStr(ctx, opts, "y");
        if (!JS_IsUndefined(xVal) || !JS_IsUndefined(yVal))
            node->setPosition((float)jsNum(ctx, xVal), (float)jsNum(ctx, yVal));
        JS_FreeValue(ctx, xVal);
        JS_FreeValue(ctx, yVal);
        applyParticleOpts(ctx, opts, node);
    }
    return wrapNode(ctx, node, g);
}

static JSValue js_particles_burst(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node || w->node->type() != scene::SceneNode::Type::Particles) return JS_UNDEFINED;
    int32_t n = 1;
    if (argc > 0) JS_ToInt32(ctx, &n, argv[0]);
    static_cast<scene::ParticleNode*>(w->node)->burst(n);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_particles_clear(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (w && w->node && w->node->type() == scene::SceneNode::Type::Particles)
        static_cast<scene::ParticleNode*>(w->node)->clear();
    return JS_DupValue(ctx, this_val);
}

static JSValue js_particles_configure(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node || w->node->type() != scene::SceneNode::Type::Particles) return JS_UNDEFINED;
    if (argc > 0 && JS_IsObject(argv[0])) {
        applyParticleOpts(ctx, argv[0], static_cast<scene::ParticleNode*>(w->node));
    }
    return JS_DupValue(ctx, this_val);
}

// ---------------------------------------------------------------------------
// Tilemap node
// ---------------------------------------------------------------------------

static bool readUint16Array(JSContext* ctx, JSValueConst v, std::vector<uint16_t>& out) {
    if (!JS_IsObject(v)) return false;
    size_t off = 0, byteLen = 0, bpe = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &byteLen, &bpe);
    if (JS_IsException(ab)) { JS_FreeValue(ctx, ab); return false; }
    size_t abufLen = 0;
    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, ab);
    JS_FreeValue(ctx, ab);
    if (!raw) {
        // Fall back to plain Array of numbers.
        if (!JS_IsArray(v)) return false;
        JSValue lenVal = JS_GetPropertyStr(ctx, v, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        out.resize(len);
        for (int32_t i = 0; i < len; ++i) {
            JSValue elem = JS_GetPropertyUint32(ctx, v, i);
            int32_t n = 0; JS_ToInt32(ctx, &n, elem);
            out[i] = (uint16_t)n;
            JS_FreeValue(ctx, elem);
        }
        return true;
    }
    // Treat as Uint16Array (bpe should be 2).
    if (bpe == 2) {
        const uint16_t* data = reinterpret_cast<const uint16_t*>(raw + off);
        size_t count = byteLen / sizeof(uint16_t);
        out.assign(data, data + count);
        return true;
    }
    if (bpe == 4) {
        const uint32_t* data = reinterpret_cast<const uint32_t*>(raw + off);
        size_t count = byteLen / sizeof(uint32_t);
        out.resize(count);
        for (size_t i = 0; i < count; ++i) out[i] = (uint16_t)data[i];
        return true;
    }
    if (bpe == 1) {
        size_t count = byteLen;
        out.resize(count);
        for (size_t i = 0; i < count; ++i) out[i] = raw[off + i];
        return true;
    }
    return false;
}

// createTilemap({tileWidth, tileHeight, columns, rows, tileset:{src, tileWidth, tileHeight, columns}, data|layers})
static JSValue js_sg_createTilemap(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;
    auto* node = g->createTilemap();
    g->root()->addChild(node);
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        int tw = (int)jsGetProp(ctx, opts, "tileWidth", 32);
        int th = (int)jsGetProp(ctx, opts, "tileHeight", 32);
        node->setTileSize(tw, th);

        int cols = (int)jsGetProp(ctx, opts, "columns", 0);
        int rows = (int)jsGetProp(ctx, opts, "rows", 0);
        node->setMapSize(cols, rows);

        JSValue xVal = JS_GetPropertyStr(ctx, opts, "x");
        JSValue yVal = JS_GetPropertyStr(ctx, opts, "y");
        if (!JS_IsUndefined(xVal) || !JS_IsUndefined(yVal))
            node->setPosition((float)jsNum(ctx, xVal), (float)jsNum(ctx, yVal));
        JS_FreeValue(ctx, xVal); JS_FreeValue(ctx, yVal);

        // tileset
        JSValue tsVal = JS_GetPropertyStr(ctx, opts, "tileset");
        if (JS_IsObject(tsVal)) {
            std::string src = jsGetStr(ctx, tsVal, "src", "");
            int sw = (int)jsGetProp(ctx, tsVal, "tileWidth", tw);
            int sh = (int)jsGetProp(ctx, tsVal, "tileHeight", th);
            int sc = (int)jsGetProp(ctx, tsVal, "columns", 0);
            node->setTileset(src, sw, sh, sc);
        }
        JS_FreeValue(ctx, tsVal);

        // layers (multi) or data (single layer)
        JSValue layersVal = JS_GetPropertyStr(ctx, opts, "layers");
        if (JS_IsArray(layersVal)) {
            JSValue lenVal = JS_GetPropertyStr(ctx, layersVal, "length");
            int32_t len = 0; JS_ToInt32(ctx, &len, lenVal);
            JS_FreeValue(ctx, lenVal);
            for (int32_t i = 0; i < len; ++i) {
                JSValue lo = JS_GetPropertyUint32(ctx, layersVal, i);
                std::string lname = jsGetStr(ctx, lo, "name", "");
                if (lname.empty()) lname = "layer" + std::to_string(i);
                int idx = node->addLayer(lname);
                JSValue dv = JS_GetPropertyStr(ctx, lo, "data");
                std::vector<uint16_t> buf;
                if (readUint16Array(ctx, dv, buf)) {
                    node->setLayerData(idx, buf.data(), buf.size());
                }
                JS_FreeValue(ctx, dv);
                JS_FreeValue(ctx, lo);
            }
        } else {
            JSValue dataVal = JS_GetPropertyStr(ctx, opts, "data");
            if (!JS_IsUndefined(dataVal)) {
                int idx = node->addLayer("default");
                std::vector<uint16_t> buf;
                if (readUint16Array(ctx, dataVal, buf)) {
                    node->setLayerData(idx, buf.data(), buf.size());
                }
            }
            JS_FreeValue(ctx, dataVal);
        }
        JS_FreeValue(ctx, layersVal);
    }
    return wrapNode(ctx, node, g);
}

static int resolveLayerArg(JSContext* ctx, JSValueConst v, scene::TilemapNode* tm) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return 0;
    if (JS_IsString(v)) return tm->layerIndex(jsStr(ctx, v));
    int32_t n = 0; JS_ToInt32(ctx, &n, v);
    return n;
}

static JSValue js_tilemap_setTile(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node || w->node->type() != scene::SceneNode::Type::Tilemap || argc < 3) return JS_UNDEFINED;
    auto* tm = static_cast<scene::TilemapNode*>(w->node);
    int32_t col = 0, row = 0; int32_t tile = 0;
    JS_ToInt32(ctx, &col, argv[0]);
    JS_ToInt32(ctx, &row, argv[1]);
    JS_ToInt32(ctx, &tile, argv[2]);
    int layer = (argc > 3) ? resolveLayerArg(ctx, argv[3], tm) : 0;
    tm->setTile(col, row, (uint16_t)tile, layer);
    return JS_UNDEFINED;
}

static JSValue js_tilemap_getTile(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node || w->node->type() != scene::SceneNode::Type::Tilemap || argc < 2) return JS_NewInt32(ctx, 0);
    auto* tm = static_cast<scene::TilemapNode*>(w->node);
    int32_t col = 0, row = 0;
    JS_ToInt32(ctx, &col, argv[0]);
    JS_ToInt32(ctx, &row, argv[1]);
    int layer = (argc > 2) ? resolveLayerArg(ctx, argv[2], tm) : 0;
    return JS_NewInt32(ctx, tm->getTile(col, row, layer));
}

static JSValue js_tilemap_tileAtWorld(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node || w->node->type() != scene::SceneNode::Type::Tilemap || argc < 2) return JS_NULL;
    auto* tm = static_cast<scene::TilemapNode*>(w->node);
    int col = 0, row = 0;
    if (!tm->tileAtWorld((float)jsNum(ctx, argv[0]), (float)jsNum(ctx, argv[1]), col, row))
        return JS_NULL;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "col", JS_NewInt32(ctx, col));
    JS_SetPropertyStr(ctx, obj, "row", JS_NewInt32(ctx, row));
    return obj;
}

// ---------------------------------------------------------------------------
// SceneGraph wrapper
// ---------------------------------------------------------------------------

struct GraphWrapper {
    scene::SceneGraph* graph;
};

static inline scene::SceneGraph* getGraph(JSContext* ctx, JSValueConst val) {
    auto* w = qjsbind::unwrap<GraphWrapper>(ctx, val);
    return w ? w->graph : nullptr;
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

// createShape(opts?) → SceneNode (ShapeNode)
static JSValue js_sg_createShape(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createShape();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        // Shape type
        std::string shapeStr = jsGetStr(ctx, opts, "shape", "rect");
        if (shapeStr == "rect")        node->setShape(scene::ShapeNode::Shape::Rect);
        else if (shapeStr == "roundrect") node->setShape(scene::ShapeNode::Shape::RoundRect);
        else if (shapeStr == "circle")  node->setShape(scene::ShapeNode::Shape::Circle);
        else if (shapeStr == "ellipse") node->setShape(scene::ShapeNode::Shape::Ellipse);
        else if (shapeStr == "polygon") node->setShape(scene::ShapeNode::Shape::Polygon);
        else if (shapeStr == "line")    node->setShape(scene::ShapeNode::Shape::Line);

        // Name
        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        // Dimensions
        JSValue wVal = JS_GetPropertyStr(ctx, opts, "width");
        JSValue hVal = JS_GetPropertyStr(ctx, opts, "height");
        if (!JS_IsUndefined(wVal) || !JS_IsUndefined(hVal))
            node->setSize(jsNum(ctx, wVal), jsNum(ctx, hVal));
        JS_FreeValue(ctx, wVal);
        JS_FreeValue(ctx, hVal);

        // Radius
        JSValue rVal = JS_GetPropertyStr(ctx, opts, "radius");
        if (!JS_IsUndefined(rVal)) node->setRadius(jsNum(ctx, rVal));
        JS_FreeValue(ctx, rVal);

        // Corner radius
        JSValue crVal = JS_GetPropertyStr(ctx, opts, "cornerRadius");
        if (!JS_IsUndefined(crVal)) node->setCornerRadius(jsNum(ctx, crVal));
        JS_FreeValue(ctx, crVal);

        // Radii (ellipse)
        JSValue rxVal = JS_GetPropertyStr(ctx, opts, "radiusX");
        JSValue ryVal = JS_GetPropertyStr(ctx, opts, "radiusY");
        if (!JS_IsUndefined(rxVal) || !JS_IsUndefined(ryVal))
            node->setRadii(jsNum(ctx, rxVal), jsNum(ctx, ryVal));
        JS_FreeValue(ctx, rxVal);
        JS_FreeValue(ctx, ryVal);

        // Fill color
        JSValue fillVal = JS_GetPropertyStr(ctx, opts, "fill");
        if (!JS_IsUndefined(fillVal)) {
            uint8_t r, g, b, a;
            if (JS_IsString(fillVal) && parseColor(jsStr(ctx, fillVal), r, g, b, a)) {
                node->setFillColor({r, g, b, a});
            }
        } else {
            node->setHasFill(true); // default white fill
        }
        JS_FreeValue(ctx, fillVal);

        // Stroke color
        JSValue strokeVal = JS_GetPropertyStr(ctx, opts, "stroke");
        if (!JS_IsUndefined(strokeVal)) {
            uint8_t r, g, b, a;
            if (JS_IsString(strokeVal) && parseColor(jsStr(ctx, strokeVal), r, g, b, a)) {
                node->setStrokeColor({r, g, b, a});
            }
        }
        JS_FreeValue(ctx, strokeVal);

        // Stroke width
        JSValue swVal = JS_GetPropertyStr(ctx, opts, "strokeWidth");
        if (!JS_IsUndefined(swVal)) {
            node->setStrokeWidth(jsNum(ctx, swVal));
            node->setHasStroke(true);
        }
        JS_FreeValue(ctx, swVal);

        // Anchor
        JSValue axVal = JS_GetPropertyStr(ctx, opts, "anchorX");
        JSValue ayVal = JS_GetPropertyStr(ctx, opts, "anchorY");
        if (!JS_IsUndefined(axVal) || !JS_IsUndefined(ayVal))
            node->setAnchor(
                JS_IsUndefined(axVal) ? 0.5f : (float)jsNum(ctx, axVal),
                JS_IsUndefined(ayVal) ? 0.5f : (float)jsNum(ctx, ayVal));
        JS_FreeValue(ctx, axVal);
        JS_FreeValue(ctx, ayVal);

        // Position
        JSValue xVal = JS_GetPropertyStr(ctx, opts, "x");
        JSValue yVal = JS_GetPropertyStr(ctx, opts, "y");
        if (!JS_IsUndefined(xVal) || !JS_IsUndefined(yVal))
            node->setPosition(
                JS_IsUndefined(xVal) ? 0.0f : (float)jsNum(ctx, xVal),
                JS_IsUndefined(yVal) ? 0.0f : (float)jsNum(ctx, yVal));
        JS_FreeValue(ctx, xVal);
        JS_FreeValue(ctx, yVal);

        // Points (polygon)
        JSValue ptsVal = JS_GetPropertyStr(ctx, opts, "points");
        if (JS_IsArray(ptsVal)) {
            JSValue lenVal = JS_GetPropertyStr(ctx, ptsVal, "length");
            int32_t len = 0;
            JS_ToInt32(ctx, &len, lenVal);
            JS_FreeValue(ctx, lenVal);
            std::vector<float> pts(len);
            for (int32_t i = 0; i < len; i++) {
                JSValue elem = JS_GetPropertyUint32(ctx, ptsVal, i);
                pts[i] = (float)jsNum(ctx, elem);
                JS_FreeValue(ctx, elem);
            }
            node->setPoints(pts);
        }
        JS_FreeValue(ctx, ptsVal);

        applyBillboardOpts(ctx, opts, node);
    }

    return wrapNode(ctx, node, g);
}

// Parse a `sheet` option onto a SpriteNode. Two forms:
//   { frameWidth, frameHeight, columns, rows }     -- uniform grid
//   { frames: [{x,y,w,h}, ...] }                  -- explicit list
static void applySpriteSheet(JSContext* ctx, JSValueConst opts, scene::SpriteNode* node) {
    JSValue sheetVal = JS_GetPropertyStr(ctx, opts, "sheet");
    if (!JS_IsObject(sheetVal)) { JS_FreeValue(ctx, sheetVal); return; }

    JSValue framesVal = JS_GetPropertyStr(ctx, sheetVal, "frames");
    if (JS_IsArray(framesVal)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, framesVal, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        std::vector<scene::SpriteNode::Frame> frames;
        frames.reserve(len);
        for (int32_t i = 0; i < len; ++i) {
            JSValue f = JS_GetPropertyUint32(ctx, framesVal, i);
            scene::SpriteNode::Frame fr{};
            fr.x = (float)jsGetProp(ctx, f, "x", 0);
            fr.y = (float)jsGetProp(ctx, f, "y", 0);
            fr.w = (float)jsGetProp(ctx, f, "w", 0);
            fr.h = (float)jsGetProp(ctx, f, "h", 0);
            frames.push_back(fr);
            JS_FreeValue(ctx, f);
        }
        node->setSheetFrames(std::move(frames));
    } else {
        int fw = (int)jsGetProp(ctx, sheetVal, "frameWidth", 0);
        int fh = (int)jsGetProp(ctx, sheetVal, "frameHeight", 0);
        int cols = (int)jsGetProp(ctx, sheetVal, "columns", 0);
        int rows = (int)jsGetProp(ctx, sheetVal, "rows", 0);
        node->setSheetGrid(fw, fh, cols, rows);
    }
    JS_FreeValue(ctx, framesVal);
    JS_FreeValue(ctx, sheetVal);
}

// Parse an animation spec object: { frames: [...], fps, loop, next }.
static scene::SpriteNode::AnimationSpec parseAnimSpec(JSContext* ctx, JSValueConst obj) {
    scene::SpriteNode::AnimationSpec spec;
    JSValue framesVal = JS_GetPropertyStr(ctx, obj, "frames");
    if (JS_IsArray(framesVal)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, framesVal, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        spec.frames.reserve(len);
        for (int32_t i = 0; i < len; ++i) {
            JSValue elem = JS_GetPropertyUint32(ctx, framesVal, i);
            int32_t v = 0;
            JS_ToInt32(ctx, &v, elem);
            spec.frames.push_back(v);
            JS_FreeValue(ctx, elem);
        }
    }
    JS_FreeValue(ctx, framesVal);

    JSValue fpsVal = JS_GetPropertyStr(ctx, obj, "fps");
    if (!JS_IsUndefined(fpsVal)) spec.fps = (float)jsNum(ctx, fpsVal);
    JS_FreeValue(ctx, fpsVal);

    JSValue loopVal = JS_GetPropertyStr(ctx, obj, "loop");
    if (!JS_IsUndefined(loopVal)) spec.loop = JS_ToBool(ctx, loopVal);
    JS_FreeValue(ctx, loopVal);

    JSValue nextVal = JS_GetPropertyStr(ctx, obj, "next");
    if (JS_IsString(nextVal)) spec.next = jsStr(ctx, nextVal);
    JS_FreeValue(ctx, nextVal);

    return spec;
}

// Parse `animations: { name: spec, ... }` into the SpriteNode.
static void applySpriteAnimations(JSContext* ctx, JSValueConst opts, scene::SpriteNode* node) {
    JSValue animsVal = JS_GetPropertyStr(ctx, opts, "animations");
    if (!JS_IsObject(animsVal)) { JS_FreeValue(ctx, animsVal); return; }

    JSPropertyEnum* tab = nullptr;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &count, animsVal,
                                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (uint32_t i = 0; i < count; ++i) {
            const char* keyStr = JS_AtomToCString(ctx, tab[i].atom);
            if (!keyStr) continue;
            JSValue v = JS_GetProperty(ctx, animsVal, tab[i].atom);
            if (JS_IsObject(v)) {
                node->addAnimation(keyStr, parseAnimSpec(ctx, v));
            }
            JS_FreeValue(ctx, v);
            JS_FreeCString(ctx, keyStr);
        }
        for (uint32_t i = 0; i < count; ++i) JS_FreeAtom(ctx, tab[i].atom);
        js_free(ctx, tab);
    }
    JS_FreeValue(ctx, animsVal);
}

// createSprite(opts?) → SceneNode (SpriteNode)
static JSValue js_sg_createSprite(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createSprite();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        // Image path
        JSValue srcVal = JS_GetPropertyStr(ctx, opts, "src");
        if (JS_IsString(srcVal)) node->setImagePath(jsStr(ctx, srcVal));
        JS_FreeValue(ctx, srcVal);

        // Size
        JSValue wVal = JS_GetPropertyStr(ctx, opts, "width");
        JSValue hVal = JS_GetPropertyStr(ctx, opts, "height");
        if (!JS_IsUndefined(wVal) || !JS_IsUndefined(hVal))
            node->setSize((float)jsNum(ctx, wVal), (float)jsNum(ctx, hVal));
        JS_FreeValue(ctx, wVal);
        JS_FreeValue(ctx, hVal);

        // Position
        JSValue xVal = JS_GetPropertyStr(ctx, opts, "x");
        JSValue yVal = JS_GetPropertyStr(ctx, opts, "y");
        if (!JS_IsUndefined(xVal) || !JS_IsUndefined(yVal))
            node->setPosition((float)jsNum(ctx, xVal), (float)jsNum(ctx, yVal));
        JS_FreeValue(ctx, xVal);
        JS_FreeValue(ctx, yVal);

        // Opacity
        JSValue oVal = JS_GetPropertyStr(ctx, opts, "opacity");
        if (!JS_IsUndefined(oVal)) node->setOpacity((float)jsNum(ctx, oVal));
        JS_FreeValue(ctx, oVal);

        // Anchor
        JSValue axVal = JS_GetPropertyStr(ctx, opts, "anchorX");
        JSValue ayVal = JS_GetPropertyStr(ctx, opts, "anchorY");
        if (!JS_IsUndefined(axVal) || !JS_IsUndefined(ayVal))
            node->setAnchor(
                JS_IsUndefined(axVal) ? 0.5f : (float)jsNum(ctx, axVal),
                JS_IsUndefined(ayVal) ? 0.5f : (float)jsNum(ctx, ayVal));
        JS_FreeValue(ctx, axVal);
        JS_FreeValue(ctx, ayVal);

        // Spritesheet config + named animations + initial play.
        applySpriteSheet(ctx, opts, node);
        applySpriteAnimations(ctx, opts, node);
        JSValue playVal = JS_GetPropertyStr(ctx, opts, "play");
        if (JS_IsString(playVal)) node->play(jsStr(ctx, playVal));
        JS_FreeValue(ctx, playVal);
        JSValue fiVal = JS_GetPropertyStr(ctx, opts, "frameIndex");
        if (JS_IsNumber(fiVal)) {
            int32_t idx = 0; JS_ToInt32(ctx, &idx, fiVal);
            node->setFrameIndex(idx);
        }
        JS_FreeValue(ctx, fiVal);

        applyBillboardOpts(ctx, opts, node);
    }

    return wrapNode(ctx, node, g);
}

// createHtmlNode({html, width, height, pxPerUnit, worldAnchor, billboard, name?})
static JSValue js_sg_createHtml(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createHtml();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        float w = 200.0f, h = 50.0f;
        JSValue wVal = JS_GetPropertyStr(ctx, opts, "width");
        JSValue hVal = JS_GetPropertyStr(ctx, opts, "height");
        if (!JS_IsUndefined(wVal)) w = (float)jsNum(ctx, wVal);
        if (!JS_IsUndefined(hVal)) h = (float)jsNum(ctx, hVal);
        JS_FreeValue(ctx, wVal);
        JS_FreeValue(ctx, hVal);
        node->setLayoutSize(w, h);

        JSValue ppuVal = JS_GetPropertyStr(ctx, opts, "pxPerUnit");
        if (!JS_IsUndefined(ppuVal)) node->setPxPerUnit((float)jsNum(ctx, ppuVal));
        JS_FreeValue(ctx, ppuVal);

        JSValue htmlVal = JS_GetPropertyStr(ctx, opts, "html");
        if (JS_IsString(htmlVal)) node->setHtml(jsStr(ctx, htmlVal));
        JS_FreeValue(ctx, htmlVal);

        applyBillboardOpts(ctx, opts, node);
    }

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

// --- Helper: read a typed array property into a vector<float> or vector<uint32_t> ---
static bool jsReadFloatArray(JSContext* ctx, JSValueConst obj, const char* prop,
                             std::vector<float>& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return false; }

    size_t offset = 0, byteLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &offset, &byteLen, &bpe);
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, abuf);
        JS_FreeValue(ctx, v);
        return false;
    }
    size_t abufLen = 0;
    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!raw) { JS_FreeValue(ctx, v); return false; }

    const float* data = reinterpret_cast<const float*>(raw + offset);
    size_t count = byteLen / sizeof(float);
    out.assign(data, data + count);
    JS_FreeValue(ctx, v);
    return true;
}

static bool jsReadUint32Array(JSContext* ctx, JSValueConst obj, const char* prop,
                              std::vector<uint32_t>& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return false; }

    size_t offset = 0, byteLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &offset, &byteLen, &bpe);
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, abuf);
        JS_FreeValue(ctx, v);
        return false;
    }
    size_t abufLen = 0;
    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!raw) { JS_FreeValue(ctx, v); return false; }

    const uint32_t* data = reinterpret_cast<const uint32_t*>(raw + offset);
    size_t count = byteLen / sizeof(uint32_t);
    out.assign(data, data + count);
    JS_FreeValue(ctx, v);
    return true;
}

// createMesh({mesh, color, name, x, y, z, ...})
static JSValue js_sg_createMesh(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createMesh();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        // Position
        double x = jsGetProp(ctx, opts, "x", 0);
        double y = jsGetProp(ctx, opts, "y", 0);
        double z = jsGetProp(ctx, opts, "z", 0);
        node->setPosition((float)x, (float)y, (float)z);

        // Scale (uniform or per-axis)
        JSValue scaleVal = JS_GetPropertyStr(ctx, opts, "scale");
        if (!JS_IsUndefined(scaleVal)) {
            double s = 1;
            JS_ToFloat64(ctx, &s, scaleVal);
            node->setScale((float)s, (float)s, (float)s);
        }
        JS_FreeValue(ctx, scaleVal);

        // Rotation (Euler degrees for convenience)
        JSValue rxVal = JS_GetPropertyStr(ctx, opts, "rx");
        JSValue ryVal = JS_GetPropertyStr(ctx, opts, "ry");
        JSValue rzVal = JS_GetPropertyStr(ctx, opts, "rz");
        if (!JS_IsUndefined(rxVal) || !JS_IsUndefined(ryVal) || !JS_IsUndefined(rzVal)) {
            double rx = 0, ry = 0, rz = 0;
            if (!JS_IsUndefined(rxVal)) JS_ToFloat64(ctx, &rx, rxVal);
            if (!JS_IsUndefined(ryVal)) JS_ToFloat64(ctx, &ry, ryVal);
            if (!JS_IsUndefined(rzVal)) JS_ToFloat64(ctx, &rz, rzVal);
            float toRad = 3.14159265f / 180.0f;
            node->setRotationEuler((float)rx * toRad, (float)ry * toRad, (float)rz * toRad);
        }
        JS_FreeValue(ctx, rxVal);
        JS_FreeValue(ctx, ryVal);
        JS_FreeValue(ctx, rzVal);

        // Color
        JSValue colorVal = JS_GetPropertyStr(ctx, opts, "color");
        if (JS_IsString(colorVal)) {
            uint8_t r, g2, b, a;
            if (parseColor(jsStr(ctx, colorVal), r, g2, b, a)) {
                node->setColor(r / 255.0f, g2 / 255.0f, b / 255.0f, a / 255.0f);
            }
        } else if (JS_IsArray(colorVal)) {
            double cr = 1, cg = 1, cb = 1, ca = 1;
            JSValue e0 = JS_GetPropertyUint32(ctx, colorVal, 0);
            JSValue e1 = JS_GetPropertyUint32(ctx, colorVal, 1);
            JSValue e2 = JS_GetPropertyUint32(ctx, colorVal, 2);
            JSValue e3 = JS_GetPropertyUint32(ctx, colorVal, 3);
            JS_ToFloat64(ctx, &cr, e0);
            JS_ToFloat64(ctx, &cg, e1);
            JS_ToFloat64(ctx, &cb, e2);
            if (!JS_IsUndefined(e3)) JS_ToFloat64(ctx, &ca, e3);
            node->setColor((float)cr, (float)cg, (float)cb, (float)ca);
            JS_FreeValue(ctx, e0);
            JS_FreeValue(ctx, e1);
            JS_FreeValue(ctx, e2);
            JS_FreeValue(ctx, e3);
        }
        JS_FreeValue(ctx, colorVal);

        // Emissive intensity (scalar multiplier against emissiveColor)
        double emissive = jsGetProp(ctx, opts, "emissive", 0.0);
        node->setEmissive((float)emissive);

        // Emissive color (defaults to baseColor if unspecified — mimics
        // glTF "emissiveFactor applied to base" for single-field ergonomics).
        JSValue emColVal = JS_GetPropertyStr(ctx, opts, "emissiveColor");
        if (JS_IsString(emColVal)) {
            uint8_t er, eg, eb, ea;
            if (parseColor(jsStr(ctx, emColVal), er, eg, eb, ea))
                node->setEmissiveColor(er/255.0f, eg/255.0f, eb/255.0f);
        } else if (JS_IsArray(emColVal)) {
            double er = 1, eg = 1, eb = 1;
            JSValue e0 = JS_GetPropertyUint32(ctx, emColVal, 0);
            JSValue e1 = JS_GetPropertyUint32(ctx, emColVal, 1);
            JSValue e2 = JS_GetPropertyUint32(ctx, emColVal, 2);
            JS_ToFloat64(ctx, &er, e0);
            JS_ToFloat64(ctx, &eg, e1);
            JS_ToFloat64(ctx, &eb, e2);
            node->setEmissiveColor((float)er, (float)eg, (float)eb);
            JS_FreeValue(ctx, e0);
            JS_FreeValue(ctx, e1);
            JS_FreeValue(ctx, e2);
        } else if (emissive > 0.0) {
            // Default: reuse baseColor so `{color:'#ff0', emissive:2}` glows yellow.
            const float* c = node->color();
            node->setEmissiveColor(c[0], c[1], c[2]);
        }
        JS_FreeValue(ctx, emColVal);

        // PBR material params (glTF metallic/roughness workflow).
        JSValue matVal = JS_GetPropertyStr(ctx, opts, "material");
        auto applyMat = [&](JSValueConst obj) {
            JSValue mv = JS_GetPropertyStr(ctx, obj, "metallic");
            if (!JS_IsUndefined(mv)) node->setMetallic((float)jsNum(ctx, mv));
            JS_FreeValue(ctx, mv);
            JSValue rv = JS_GetPropertyStr(ctx, obj, "roughness");
            if (!JS_IsUndefined(rv)) node->setRoughness((float)jsNum(ctx, rv));
            JS_FreeValue(ctx, rv);
        };
        if (JS_IsObject(matVal)) applyMat(matVal);
        // Flat shortcuts: {metallic:0.9, roughness:0.2}
        applyMat(opts);
        JS_FreeValue(ctx, matVal);

        // Unlit flag — skip lighting for this mesh (output baseColor only).
        JSValue unlitVal = JS_GetPropertyStr(ctx, opts, "unlit");
        if (!JS_IsUndefined(unlitVal)) {
            node->setUnlit(JS_ToBool(ctx, unlitVal) == 1);
        }
        JS_FreeValue(ctx, unlitVal);

        JSValue tsVal = JS_GetPropertyStr(ctx, opts, "twoSided");
        if (!JS_IsUndefined(tsVal)) node->setTwoSided(JS_ToBool(ctx, tsVal) == 1);
        JS_FreeValue(ctx, tsVal);

        JSValue ssVal = JS_GetPropertyStr(ctx, opts, "subsurface");
        if (!JS_IsUndefined(ssVal)) {
            double s = 0;
            JS_ToFloat64(ctx, &s, ssVal);
            node->setSubsurface((float)s);
        }
        JS_FreeValue(ctx, ssVal);

        JSValue acVal = JS_GetPropertyStr(ctx, opts, "alphaCutoff");
        if (!JS_IsUndefined(acVal)) {
            double s = 0;
            JS_ToFloat64(ctx, &s, acVal);
            node->setAlphaCutoff((float)s);
        }
        JS_FreeValue(ctx, acVal);

        JSValue csVal = JS_GetPropertyStr(ctx, opts, "castsShadow");
        if (!JS_IsUndefined(csVal)) node->setCastsShadow(JS_ToBool(ctx, csVal) == 1);
        JS_FreeValue(ctx, csVal);
        JSValue rsVal = JS_GetPropertyStr(ctx, opts, "receivesShadow");
        if (!JS_IsUndefined(rsVal)) node->setReceivesShadow(JS_ToBool(ctx, rsVal) == 1);
        JS_FreeValue(ctx, rsVal);

        // Depth bias
        JSValue dbVal = JS_GetPropertyStr(ctx, opts, "depthBias");
        if (!JS_IsUndefined(dbVal)) {
            if (JS_IsArray(dbVal)) {
                double f = 0, u = 0;
                JSValue e0 = JS_GetPropertyUint32(ctx, dbVal, 0);
                JSValue e1 = JS_GetPropertyUint32(ctx, dbVal, 1);
                JS_ToFloat64(ctx, &f, e0);
                JS_ToFloat64(ctx, &u, e1);
                node->setDepthBias((float)f, (float)u);
                JS_FreeValue(ctx, e0);
                JS_FreeValue(ctx, e1);
            } else {
                double u = 0;
                JS_ToFloat64(ctx, &u, dbVal);
                node->setDepthBias(0.0f, (float)u);
            }
        }
        JS_FreeValue(ctx, dbVal);

        // Mesh data
        bromesh::MeshData meshData;
        bool hasRawData = false;

        bool transfer = jsGetBool(ctx, opts, "transfer", false);

        auto tryKey = [&](const char* key) -> bool {
            JSValue v = JS_GetPropertyStr(ctx, opts, key);
            bool took = false;
            if (!JS_IsUndefined(v) && MeshBindings::getMeshData(ctx, v)) {
                if (transfer) {
                    if (auto taken = MeshBindings::takeMeshData(ctx, v)) {
                        meshData = std::move(*taken);
                        took = true;
                    }
                } else {
                    meshData = *MeshBindings::getMeshData(ctx, v);
                    took = true;
                }
            }
            JS_FreeValue(ctx, v);
            return took;
        };

        if (tryKey("mesh")) hasRawData = true;
        else if (tryKey("data")) hasRawData = true;

        // Check for raw vertex data (positions + indices arrays)
        if (!hasRawData) {
            std::vector<float> positions, normals, colors;
            std::vector<uint32_t> indices;
            if (jsReadFloatArray(ctx, opts, "positions", positions) &&
                jsReadUint32Array(ctx, opts, "indices", indices)) {
                meshData.positions = std::move(positions);
                meshData.indices = std::move(indices);
                if (jsReadFloatArray(ctx, opts, "normals", normals)) {
                    meshData.normals = std::move(normals);
                }
                if (jsReadFloatArray(ctx, opts, "colors", colors)) {
                    meshData.colors = std::move(colors);
                }
                hasRawData = true;
            }
        }

        if (!hasRawData) {
            std::string meshType = jsGetStr(ctx, opts, "mesh", "box");

            if (meshType == "box") {
                double hw = jsGetProp(ctx, opts, "halfW", 0.5);
                double hh = jsGetProp(ctx, opts, "halfH", 0.5);
                double hd = jsGetProp(ctx, opts, "halfD", 0.5);
                meshData = bromesh::box((float)hw, (float)hh, (float)hd);
            } else if (meshType == "sphere") {
                double radius = jsGetProp(ctx, opts, "radius", 0.5);
                int segments = (int)jsGetProp(ctx, opts, "segments", 16);
                int rings = (int)jsGetProp(ctx, opts, "rings", 12);
                meshData = bromesh::sphere((float)radius, segments, rings);
            } else if (meshType == "cylinder") {
                double radius = jsGetProp(ctx, opts, "radius", 0.5);
                double halfH = jsGetProp(ctx, opts, "halfHeight", 0.5);
                int segments = (int)jsGetProp(ctx, opts, "segments", 16);
                meshData = bromesh::cylinder((float)radius, (float)halfH, segments);
            } else if (meshType == "capsule") {
                double radius = jsGetProp(ctx, opts, "radius", 0.5);
                double halfH = jsGetProp(ctx, opts, "halfHeight", 0.5);
                int segments = (int)jsGetProp(ctx, opts, "segments", 16);
                int rings = (int)jsGetProp(ctx, opts, "rings", 8);
                meshData = bromesh::capsule((float)radius, (float)halfH, segments, rings);
            } else if (meshType == "plane") {
                double hw = jsGetProp(ctx, opts, "halfW", 5.0);
                double hd = jsGetProp(ctx, opts, "halfD", 5.0);
                int sx = (int)jsGetProp(ctx, opts, "subdivX", 1);
                int sz = (int)jsGetProp(ctx, opts, "subdivZ", 1);
                meshData = bromesh::plane((float)hw, (float)hd, sx, sz);
            } else if (meshType == "torus") {
                double major = jsGetProp(ctx, opts, "majorRadius", 1.0);
                double minor = jsGetProp(ctx, opts, "minorRadius", 0.3);
                int majSeg = (int)jsGetProp(ctx, opts, "majorSegments", 24);
                int minSeg = (int)jsGetProp(ctx, opts, "minorSegments", 12);
                meshData = bromesh::torus((float)major, (float)minor, majSeg, minSeg);
            }
        }

        node->setMesh(std::move(meshData));
    }

    // Optional texture maps: each is { width, height, data: Uint8Array(rgba8) }.
    // Keys: texture (baseColor), normalTexture, metallicRoughnessTexture,
    //       occlusionTexture, emissiveTexture.
    if (argc > 0 && JS_IsObject(argv[0])) {
        auto applyTex = [&](const char* key, void (scene::MeshNode::*setter)(int, int, const uint8_t*)) {
            JSValue tex = JS_GetPropertyStr(ctx, argv[0], key);
            if (JS_IsObject(tex)) {
                int w = (int)jsGetProp(ctx, tex, "width",  0);
                int h = (int)jsGetProp(ctx, tex, "height", 0);
                JSValue dataVal = JS_GetPropertyStr(ctx, tex, "data");
                size_t bytes = 0;
                size_t off = 0, len = 0;
                JSValue ab = JS_GetTypedArrayBuffer(ctx, dataVal, &off, &len, nullptr);
                if (!JS_IsException(ab)) {
                    uint8_t* base = JS_GetArrayBuffer(ctx, &bytes, ab);
                    if (base && w > 0 && h > 0 && len >= (size_t)w * (size_t)h * 4) {
                        (node->*setter)(w, h, base + off);
                    }
                    JS_FreeValue(ctx, ab);
                }
                JS_FreeValue(ctx, dataVal);
            }
            JS_FreeValue(ctx, tex);
        };
        applyTex("texture",                  &scene::MeshNode::setBaseColorTexture);
        applyTex("normalTexture",            &scene::MeshNode::setNormalTexture);
        applyTex("metallicRoughnessTexture", &scene::MeshNode::setMetallicRoughnessTexture);
        applyTex("occlusionTexture",         &scene::MeshNode::setOcclusionTexture);
        applyTex("emissiveTexture",          &scene::MeshNode::setEmissiveTexture);
    }

    return wrapNode(ctx, node, g);
}

// createInstancedMesh({mesh, instances|instancesFromTransforms, color, ...})
// Mirrors createMesh's material + texture surface but renders N copies of
// `mesh` in a single draw via hardware instancing. Per-instance state is
// either:
//   - `instances`: Float32Array of 16*count floats (canonical layout —
//     4x3 affine transform rows + RGBA tint), or
//   - `instancesFromTransforms`: Float32Array of 9*count floats
//     (px py pz, qx qy qz qw, scale, variantIndex), converted internally.
static JSValue js_sg_createInstancedMesh(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createInstancedMesh();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        double x = jsGetProp(ctx, opts, "x", 0);
        double y = jsGetProp(ctx, opts, "y", 0);
        double z = jsGetProp(ctx, opts, "z", 0);
        node->setPosition((float)x, (float)y, (float)z);

        // Color
        JSValue colorVal = JS_GetPropertyStr(ctx, opts, "color");
        if (JS_IsString(colorVal)) {
            uint8_t r, g2, b, a;
            if (parseColor(jsStr(ctx, colorVal), r, g2, b, a)) {
                node->setColor(r/255.0f, g2/255.0f, b/255.0f, a/255.0f);
            }
        } else if (JS_IsArray(colorVal)) {
            double cr = 1, cg = 1, cb = 1, ca = 1;
            JSValue e0 = JS_GetPropertyUint32(ctx, colorVal, 0);
            JSValue e1 = JS_GetPropertyUint32(ctx, colorVal, 1);
            JSValue e2 = JS_GetPropertyUint32(ctx, colorVal, 2);
            JSValue e3 = JS_GetPropertyUint32(ctx, colorVal, 3);
            JS_ToFloat64(ctx, &cr, e0); JS_ToFloat64(ctx, &cg, e1);
            JS_ToFloat64(ctx, &cb, e2);
            if (!JS_IsUndefined(e3)) JS_ToFloat64(ctx, &ca, e3);
            node->setColor((float)cr, (float)cg, (float)cb, (float)ca);
            JS_FreeValue(ctx, e0); JS_FreeValue(ctx, e1);
            JS_FreeValue(ctx, e2); JS_FreeValue(ctx, e3);
        }
        JS_FreeValue(ctx, colorVal);

        double emissive = jsGetProp(ctx, opts, "emissive", 0.0);
        node->setEmissive((float)emissive);

        JSValue emColVal = JS_GetPropertyStr(ctx, opts, "emissiveColor");
        if (JS_IsString(emColVal)) {
            uint8_t er, eg, eb, ea;
            if (parseColor(jsStr(ctx, emColVal), er, eg, eb, ea))
                node->setEmissiveColor(er/255.0f, eg/255.0f, eb/255.0f);
        } else if (JS_IsArray(emColVal)) {
            double er = 1, eg = 1, eb = 1;
            JSValue e0 = JS_GetPropertyUint32(ctx, emColVal, 0);
            JSValue e1 = JS_GetPropertyUint32(ctx, emColVal, 1);
            JSValue e2 = JS_GetPropertyUint32(ctx, emColVal, 2);
            JS_ToFloat64(ctx, &er, e0); JS_ToFloat64(ctx, &eg, e1); JS_ToFloat64(ctx, &eb, e2);
            node->setEmissiveColor((float)er, (float)eg, (float)eb);
            JS_FreeValue(ctx, e0); JS_FreeValue(ctx, e1); JS_FreeValue(ctx, e2);
        } else if (emissive > 0.0) {
            const float* c = node->color();
            node->setEmissiveColor(c[0], c[1], c[2]);
        }
        JS_FreeValue(ctx, emColVal);

        JSValue mv = JS_GetPropertyStr(ctx, opts, "metallic");
        if (!JS_IsUndefined(mv)) node->setMetallic((float)jsNum(ctx, mv));
        JS_FreeValue(ctx, mv);
        JSValue rv = JS_GetPropertyStr(ctx, opts, "roughness");
        if (!JS_IsUndefined(rv)) node->setRoughness((float)jsNum(ctx, rv));
        JS_FreeValue(ctx, rv);

        JSValue unlitVal = JS_GetPropertyStr(ctx, opts, "unlit");
        if (!JS_IsUndefined(unlitVal)) node->setUnlit(JS_ToBool(ctx, unlitVal) == 1);
        JS_FreeValue(ctx, unlitVal);

        JSValue acV = JS_GetPropertyStr(ctx, opts, "alphaCutoff");
        if (!JS_IsUndefined(acV)) node->setAlphaCutoff((float)jsNum(ctx, acV));
        JS_FreeValue(ctx, acV);

        JSValue dsV = JS_GetPropertyStr(ctx, opts, "doubleSided");
        if (!JS_IsUndefined(dsV)) node->setDoubleSided(JS_ToBool(ctx, dsV) == 1);
        JS_FreeValue(ctx, dsV);

        JSValue csVal = JS_GetPropertyStr(ctx, opts, "castsShadow");
        if (!JS_IsUndefined(csVal)) node->setCastsShadow(JS_ToBool(ctx, csVal) == 1);
        JS_FreeValue(ctx, csVal);
        JSValue rsVal = JS_GetPropertyStr(ctx, opts, "receivesShadow");
        if (!JS_IsUndefined(rsVal)) node->setReceivesShadow(JS_ToBool(ctx, rsVal) == 1);
        JS_FreeValue(ctx, rsVal);

        // Mesh source — accept a Mesh handle from MeshBindings.
        bool transfer = jsGetBool(ctx, opts, "transfer", false);
        JSValue meshVal = JS_GetPropertyStr(ctx, opts, "mesh");
        if (!JS_IsUndefined(meshVal) && MeshBindings::getMeshData(ctx, meshVal)) {
            if (transfer) {
                if (auto taken = MeshBindings::takeMeshData(ctx, meshVal))
                    node->setMesh(std::move(*taken));
            } else {
                node->setMesh(*MeshBindings::getMeshData(ctx, meshVal));
            }
        }
        JS_FreeValue(ctx, meshVal);

        // Instance buffer.
        std::vector<float> raw;
        if (jsReadFloatArray(ctx, opts, "instances", raw)) {
            size_t count = raw.size() / 16;
            node->setInstances(raw.data(), count);
        } else if (jsReadFloatArray(ctx, opts, "instancesFromTransforms", raw)) {
            size_t count = raw.size() / 9;
            node->setInstancesFromPosQuatScale(raw.data(), count);
        }

        // Texture maps — same shape as createMesh.
        auto applyTex = [&](const char* key, void (scene::InstancedMeshNode::*setter)(int, int, const uint8_t*)) {
            JSValue tex = JS_GetPropertyStr(ctx, opts, key);
            if (JS_IsObject(tex)) {
                int w = (int)jsGetProp(ctx, tex, "width",  0);
                int h = (int)jsGetProp(ctx, tex, "height", 0);
                JSValue dataVal = JS_GetPropertyStr(ctx, tex, "data");
                size_t bytes = 0, off = 0, len = 0;
                JSValue ab = JS_GetTypedArrayBuffer(ctx, dataVal, &off, &len, nullptr);
                if (!JS_IsException(ab)) {
                    uint8_t* base = JS_GetArrayBuffer(ctx, &bytes, ab);
                    if (base && w > 0 && h > 0 && len >= (size_t)w * (size_t)h * 4) {
                        (node->*setter)(w, h, base + off);
                    }
                    JS_FreeValue(ctx, ab);
                }
                JS_FreeValue(ctx, dataVal);
            }
            JS_FreeValue(ctx, tex);
        };
        applyTex("texture",                  &scene::InstancedMeshNode::setBaseColorTexture);
        applyTex("normalTexture",            &scene::InstancedMeshNode::setNormalTexture);
        applyTex("metallicRoughnessTexture", &scene::InstancedMeshNode::setMetallicRoughnessTexture);
        applyTex("occlusionTexture",         &scene::InstancedMeshNode::setOcclusionTexture);
        applyTex("emissiveTexture",          &scene::InstancedMeshNode::setEmissiveTexture);

        JSValue acVal = JS_GetPropertyStr(ctx, opts, "atlasCols");
        JSValue arVal = JS_GetPropertyStr(ctx, opts, "atlasRows");
        if (!JS_IsUndefined(acVal) || !JS_IsUndefined(arVal)) {
            int ac = JS_IsUndefined(acVal) ? 1 : (int)jsNum(ctx, acVal);
            int ar = JS_IsUndefined(arVal) ? 1 : (int)jsNum(ctx, arVal);
            node->setAtlasGrid(ac, ar);
        }
        JS_FreeValue(ctx, acVal);
        JS_FreeValue(ctx, arVal);
    }

    return wrapNode(ctx, node, g);
}

// Per-node setters for InstancedMeshNode — setMesh / setInstances /
// setInstancesFromTransforms / updateInstance.
static JSValue js_node_setInstancedMesh(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node || w->node->type() != scene::SceneNode::Type::InstancedMesh) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    auto* node = static_cast<scene::InstancedMeshNode*>(w->node);
    if (MeshBindings::getMeshData(ctx, argv[0])) {
        node->setMesh(*MeshBindings::getMeshData(ctx, argv[0]));
    }
    return JS_UNDEFINED;
}

static JSValue js_node_setInstances(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node || w->node->type() != scene::SceneNode::Type::InstancedMesh) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    size_t off = 0, len = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, nullptr);
    if (JS_IsException(ab)) { JS_FreeValue(ctx, ab); return JS_UNDEFINED; }
    size_t bytes = 0;
    uint8_t* base = JS_GetArrayBuffer(ctx, &bytes, ab);
    if (base) {
        const float* data = reinterpret_cast<const float*>(base + off);
        size_t count = (len / sizeof(float)) / 16;
        static_cast<scene::InstancedMeshNode*>(w->node)->setInstances(data, count);
    }
    JS_FreeValue(ctx, ab);
    return JS_UNDEFINED;
}

static JSValue js_node_setInstancesFromTransforms(JSContext* ctx, JSValueConst this_val,
                                                  int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node || w->node->type() != scene::SceneNode::Type::InstancedMesh) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    size_t off = 0, len = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, nullptr);
    if (JS_IsException(ab)) { JS_FreeValue(ctx, ab); return JS_UNDEFINED; }
    size_t bytes = 0;
    uint8_t* base = JS_GetArrayBuffer(ctx, &bytes, ab);
    if (base) {
        const float* data = reinterpret_cast<const float*>(base + off);
        size_t count = (len / sizeof(float)) / 9;
        static_cast<scene::InstancedMeshNode*>(w->node)->setInstancesFromPosQuatScale(data, count);
    }
    JS_FreeValue(ctx, ab);
    return JS_UNDEFINED;
}

static JSValue js_node_updateInstance(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node || w->node->type() != scene::SceneNode::Type::InstancedMesh) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    size_t off = 0, len = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[1], &off, &len, nullptr);
    if (JS_IsException(ab)) { JS_FreeValue(ctx, ab); return JS_UNDEFINED; }
    size_t bytes = 0;
    uint8_t* base = JS_GetArrayBuffer(ctx, &bytes, ab);
    if (base && len >= sizeof(float) * 16 && idx >= 0) {
        const float* data = reinterpret_cast<const float*>(base + off);
        static_cast<scene::InstancedMeshNode*>(w->node)->updateInstance((size_t)idx, data);
    }
    JS_FreeValue(ctx, ab);
    return JS_UNDEFINED;
}

static JSValue js_node_setAtlasGrid(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node || w->node->type() != scene::SceneNode::Type::InstancedMesh) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t cols = 1, rows = 1;
    JS_ToInt32(ctx, &cols, argv[0]);
    JS_ToInt32(ctx, &rows, argv[1]);
    static_cast<scene::InstancedMeshNode*>(w->node)->setAtlasGrid(cols, rows);
    return JS_UNDEFINED;
}

static JSValue js_node_setAlphaCutoff(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node || w->node->type() != scene::SceneNode::Type::InstancedMesh) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    static_cast<scene::InstancedMeshNode*>(w->node)->setAlphaCutoff((float)jsNum(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_node_setDoubleSided(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!w || !w->node || w->node->type() != scene::SceneNode::Type::InstancedMesh) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    static_cast<scene::InstancedMeshNode*>(w->node)->setDoubleSided(JS_ToBool(ctx, argv[0]) == 1);
    return JS_UNDEFINED;
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
    if (cw) {
        if (cw->node) clearSpriteEndCallback(cw->node->id());
        g->destroyNode(cw->node);
    }
    return JS_UNDEFINED;
}

// raycast(origin, direction, maxDistance) → { hit, point, normal, distance, node } | null
static JSValue js_sg_raycast(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
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
static JSValue js_sg_setCamera(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

    JSValueConst opts = argv[0];
    double fov = jsGetProp(ctx, opts, "fov", 60.0) * 3.14159265 / 180.0;
    double nearZ = jsGetProp(ctx, opts, "near", 0.1);
    double farZ = jsGetProp(ctx, opts, "far", 1000.0);
    double aspect = jsGetProp(ctx, opts, "aspect", 0.0);

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

        std::string mode = jsGetStr(ctx, opts, "mode", "perspective");
        if (mode == "orthographic" || mode == "ortho") {
            double size = jsGetProp(ctx, opts, "size", 10.0);
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

// createLight({ type, position, direction, color, intensity, range,
//               innerAngle, outerAngle, name }) → LightNode
static JSValue js_sg_createLight(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createLight();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        std::string kindStr = jsGetStr(ctx, opts, "type", "directional");
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
static JSValue js_sg_setToneMap(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
    JSValueConst opts = argv[0];
    std::string modeStr = jsGetStr(ctx, opts, "mode", "aces");
    scene::SceneGraph::ToneMap mode = scene::SceneGraph::ToneMap::ACES;
    if (modeStr == "linear")        mode = scene::SceneGraph::ToneMap::Linear;
    else if (modeStr == "reinhard") mode = scene::SceneGraph::ToneMap::Reinhard;
    double exposure = jsGetProp(ctx, opts, "exposure", 1.0);
    double gamma    = jsGetProp(ctx, opts, "gamma", 2.2);
    g->setToneMap(mode, (float)exposure, (float)gamma);
    return JS_UNDEFINED;
}

// setAmbient({ color:[r,g,b] }) or setAmbient([r,g,b])
static JSValue js_sg_setAmbient(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
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
static JSValue js_sg_setWind(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
    bromath::Vec3 d = jsGetVec3(ctx, argv[0], "direction", 1.0f, 0.0f, 0.0f);
    double strength  = jsGetProp(ctx, argv[0], "strength",  0.0);
    double frequency = jsGetProp(ctx, argv[0], "frequency", 1.5);
    g->setWind(d.x, d.y, d.z, (float)strength, (float)frequency);
    return JS_UNDEFINED;
}

// setShadowQuality({atlasSize, pcfTaps}) — atlas side length and PCF kernel.
static JSValue js_sg_setShadowQuality(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
    int atlasSize = (int)jsGetProp(ctx, argv[0], "atlasSize", 4096.0);
    int pcfTaps   = (int)jsGetProp(ctx, argv[0], "pcfTaps",   3.0);
    g->setShadowQuality(atlasSize, pcfTaps);
    return JS_UNDEFINED;
}

// setEnvironment({hdr, intensity, rotation}) — load HDR equirectangular
// environment map for skybox + IBL. Pass {hdr: ""} or null to clear.
static JSValue js_sg_setEnvironment(JSContext* ctx, JSValueConst this_val,
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

// setFog({start, end, color})
static JSValue js_sg_setFog(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(ctx, this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

    JSValueConst opts = argv[0];
    double start = jsGetProp(ctx, opts, "start", 0.0);
    double end = jsGetProp(ctx, opts, "end", 0.0);
    bromath::Vec3 color = jsGetVec3(ctx, opts, "color", 0.0f, 0.0f, 0.0f);
    g->setFog((float)start, (float)end, color.x, color.y, color.z);
    return JS_UNDEFINED;
}

// unprojectLocal(x, y) → { origin:[x,y,z], dir:[x,y,z] } | null
static JSValue js_sg_unprojectLocal(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
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

static JSValue js_sg_toImageData(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
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
static JSValue js_sg_captureFrame(JSContext* ctx, JSValueConst this_val,
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

// ---------------------------------------------------------------------------
// Install / Cleanup
// ---------------------------------------------------------------------------

void SceneBindings::setAppContext(const std::string& basePath,
                                  const util::AssetMounts* mounts) {
    s_basePath = basePath;
    s_mounts = mounts;
}

void SceneBindings::install(JSContext* ctx) {
    // --- SceneNode class ---
    qjsbind::Class<NodeWrapper>(ctx, "SceneNode")
        // Common properties
        .get("id", [](NodeWrapper* w) -> int { return w->node ? w->node->id() : 0; })
        .prop("name",
            [](NodeWrapper* w) -> std::string { return w->node ? w->node->name() : ""; },
            [](NodeWrapper* w, std::string val) { if (w->node) w->node->setName(val); })
        .prop("visible",
            [](NodeWrapper* w) -> bool { return w->node ? w->node->visible() : false; },
            [](NodeWrapper* w, bool val) { if (w->node) w->node->setVisible(val); })
        .get("childCount", [](NodeWrapper* w) -> int {
            return (w && w->node) ? (int)w->node->children().size() : 0;
        })
        .get("instanceCount", [](NodeWrapper* w) -> int {
            if (w && w->node && w->node->type() == scene::SceneNode::Type::InstancedMesh)
                return (int)static_cast<scene::InstancedMeshNode*>(w->node)->instanceCount();
            return 0;
        })
        .get("atlasCols", [](NodeWrapper* w) -> int {
            if (w && w->node && w->node->type() == scene::SceneNode::Type::InstancedMesh)
                return static_cast<scene::InstancedMeshNode*>(w->node)->atlasCols();
            return 0;
        })
        .get("atlasRows", [](NodeWrapper* w) -> int {
            if (w && w->node && w->node->type() == scene::SceneNode::Type::InstancedMesh)
                return static_cast<scene::InstancedMeshNode*>(w->node)->atlasRows();
            return 0;
        })
        .prop("alphaCutoff",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::InstancedMesh)
                    return JS_NewFloat64(ctx, (double)static_cast<scene::InstancedMeshNode*>(w->node)->alphaCutoff());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::InstancedMesh)
                    static_cast<scene::InstancedMeshNode*>(w->node)->setAlphaCutoff((float)val);
            })
        .prop("doubleSided",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::InstancedMesh)
                    return JS_NewBool(ctx, static_cast<scene::InstancedMeshNode*>(w->node)->doubleSided() ? 1 : 0);
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, bool val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::InstancedMesh)
                    static_cast<scene::InstancedMeshNode*>(w->node)->setDoubleSided(val);
            })
        .get("type", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (!w || !w->node) return JS_UNDEFINED;
            switch (w->node->type()) {
                case scene::SceneNode::Type::Mesh:    return JS_NewString(ctx, "mesh");
                case scene::SceneNode::Type::InstancedMesh: return JS_NewString(ctx, "instancedMesh");
                case scene::SceneNode::Type::Light:   return JS_NewString(ctx, "light");
                case scene::SceneNode::Type::Shape:   return JS_NewString(ctx, "shape");
                case scene::SceneNode::Type::Sprite:  return JS_NewString(ctx, "sprite");
                case scene::SceneNode::Type::Physics: return JS_NewString(ctx, "physics");
                case scene::SceneNode::Type::Html:    return JS_NewString(ctx, "html");
                case scene::SceneNode::Type::Base:    return JS_NewString(ctx, "group");
            }
            return JS_UNDEFINED;
        })

        // Transform
        .prop("position",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (!w || !w->node) return JS_UNDEFINED;
                const auto& p = w->node->position();
                JSValue arr = JS_NewArray(ctx);
                JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, p.x));
                JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, p.y));
                JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, p.z));
                return arr;
            },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (!w || !w->node || !JS_IsArray(val)) return;
                JSValue e0 = JS_GetPropertyUint32(ctx, val, 0);
                JSValue e1 = JS_GetPropertyUint32(ctx, val, 1);
                JSValue e2 = JS_GetPropertyUint32(ctx, val, 2);
                double x=0, y=0, z=0;
                JS_ToFloat64(ctx, &x, e0);
                JS_ToFloat64(ctx, &y, e1);
                JS_ToFloat64(ctx, &z, e2);
                JS_FreeValue(ctx, e0); JS_FreeValue(ctx, e1); JS_FreeValue(ctx, e2);
                w->node->setPosition((float)x, (float)y, (float)z);
            })
        .prop("x",
            [](NodeWrapper* w) -> double { return w->node ? w->node->position().x : 0; },
            [](NodeWrapper* w, double val) { if (w->node) w->node->setPosition((float)val, w->node->position().y, w->node->position().z); })
        .prop("y",
            [](NodeWrapper* w) -> double { return w->node ? w->node->position().y : 0; },
            [](NodeWrapper* w, double val) { if (w->node) w->node->setPosition(w->node->position().x, (float)val, w->node->position().z); })
        .prop("z",
            [](NodeWrapper* w) -> double { return w->node ? w->node->position().z : 0; },
            [](NodeWrapper* w, double val) { if (w->node) w->node->setPosition(w->node->position().x, w->node->position().y, (float)val); })
        .prop("rotation",
            [](NodeWrapper* w) -> double { return w->node ? bromath::qtoEuler(w->node->rotation()).z : 0; },
            [](NodeWrapper* w, double val) { if (w->node) w->node->setRotationZ((float)val); })
        .prop("rotationX",
            [](NodeWrapper* w) -> double { return w->node ? bromath::qtoEuler(w->node->rotation()).x : 0; },
            [](NodeWrapper* w, double val) {
                if (w->node) {
                    auto e = bromath::qtoEuler(w->node->rotation());
                    w->node->setRotationEuler((float)val, e.y, e.z);
                }
            })
        .prop("rotationY",
            [](NodeWrapper* w) -> double { return w->node ? bromath::qtoEuler(w->node->rotation()).y : 0; },
            [](NodeWrapper* w, double val) {
                if (w->node) {
                    auto e = bromath::qtoEuler(w->node->rotation());
                    w->node->setRotationEuler(e.x, (float)val, e.z);
                }
            })
        .prop("rotationZ",
            [](NodeWrapper* w) -> double { return w->node ? bromath::qtoEuler(w->node->rotation()).z : 0; },
            [](NodeWrapper* w, double val) {
                if (w->node) {
                    auto e = bromath::qtoEuler(w->node->rotation());
                    w->node->setRotationEuler(e.x, e.y, (float)val);
                }
            })
        // [x,y,z,w] quaternion. Unlike rotationX/Y/Z (which round-trip
        // through Euler each set), this writes the node orientation
        // atomically — required when assigning arbitrary rotations
        // (e.g. port-to-port mating in the parts DSL).
        .prop("quaternion",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (!w || !w->node) return JS_UNDEFINED;
                const auto& q = w->node->rotation();
                JSValue arr = JS_NewArray(ctx);
                JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, q.x));
                JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, q.y));
                JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, q.z));
                JS_SetPropertyUint32(ctx, arr, 3, JS_NewFloat64(ctx, q.w));
                return arr;
            },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (!w || !w->node) return;
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
                w->node->setRotation(bromath::qnorm(q));
            })
        .prop("scaleX",
            [](NodeWrapper* w) -> double { return w->node ? w->node->scale().x : 1; },
            [](NodeWrapper* w, double val) { if (w->node) w->node->setScale((float)val, w->node->scale().y, w->node->scale().z); })
        .prop("scaleY",
            [](NodeWrapper* w) -> double { return w->node ? w->node->scale().y : 1; },
            [](NodeWrapper* w, double val) { if (w->node) w->node->setScale(w->node->scale().x, (float)val, w->node->scale().z); })
        .prop("scaleZ",
            [](NodeWrapper* w) -> double { return w->node ? w->node->scale().z : 1; },
            [](NodeWrapper* w, double val) { if (w->node) w->node->setScale(w->node->scale().x, w->node->scale().y, (float)val); })

        // Mesh material (PBR) — no-op on non-mesh nodes.
        .prop("metallic",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Mesh)
                    return JS_NewFloat64(ctx, static_cast<scene::MeshNode*>(w->node)->metallic());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Mesh)
                    static_cast<scene::MeshNode*>(w->node)->setMetallic((float)val);
            })
        .prop("roughness",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Mesh)
                    return JS_NewFloat64(ctx, static_cast<scene::MeshNode*>(w->node)->roughness());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Mesh)
                    static_cast<scene::MeshNode*>(w->node)->setRoughness((float)val);
            })
        .prop("emissive",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Mesh)
                    return JS_NewFloat64(ctx, static_cast<scene::MeshNode*>(w->node)->emissive());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Mesh)
                    static_cast<scene::MeshNode*>(w->node)->setEmissive((float)val);
            })

        // LightNode properties — no-op on non-light nodes.
        .get("kind", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (!w || !w->node || w->node->type() != scene::SceneNode::Type::Light)
                return JS_UNDEFINED;
            switch (static_cast<scene::LightNode*>(w->node)->kind()) {
                case scene::LightNode::Kind::Directional: return JS_NewString(ctx, "directional");
                case scene::LightNode::Kind::Point:       return JS_NewString(ctx, "point");
                case scene::LightNode::Kind::Spot:        return JS_NewString(ctx, "spot");
            }
            return JS_UNDEFINED;
        })
        .prop("direction",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (!w || !w->node || w->node->type() != scene::SceneNode::Type::Light)
                    return JS_UNDEFINED;
                const auto& d = static_cast<scene::LightNode*>(w->node)->direction();
                JSValue arr = JS_NewArray(ctx);
                JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, d.x));
                JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, d.y));
                JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, d.z));
                return arr;
            },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (!w || !w->node || w->node->type() != scene::SceneNode::Type::Light) return;
                if (!JS_IsArray(val)) return;
                double x = 0, y = -1, z = 0;
                JSValue e0 = JS_GetPropertyUint32(ctx, val, 0);
                JSValue e1 = JS_GetPropertyUint32(ctx, val, 1);
                JSValue e2 = JS_GetPropertyUint32(ctx, val, 2);
                JS_ToFloat64(ctx, &x, e0);
                JS_ToFloat64(ctx, &y, e1);
                JS_ToFloat64(ctx, &z, e2);
                static_cast<scene::LightNode*>(w->node)->setDirection(
                    {(float)x, (float)y, (float)z});
                JS_FreeValue(ctx, e0);
                JS_FreeValue(ctx, e1);
                JS_FreeValue(ctx, e2);
            })
        .prop("color",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (!w || !w->node) return JS_UNDEFINED;
                if (w->node->type() == scene::SceneNode::Type::Light) {
                    const auto& c = static_cast<scene::LightNode*>(w->node)->color();
                    JSValue arr = JS_NewArray(ctx);
                    JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, c.x));
                    JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, c.y));
                    JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, c.z));
                    return arr;
                }
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (!w || !w->node || w->node->type() != scene::SceneNode::Type::Light) return;
                auto* L = static_cast<scene::LightNode*>(w->node);
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
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Light)
                    return JS_NewFloat64(ctx, static_cast<scene::LightNode*>(w->node)->intensity());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node)->setIntensity((float)val);
            })
        .prop("range",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Light)
                    return JS_NewFloat64(ctx, static_cast<scene::LightNode*>(w->node)->range());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node)->setRange((float)val);
            })
        .prop("innerAngle",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Light)
                    return JS_NewFloat64(ctx, static_cast<scene::LightNode*>(w->node)->innerAngle());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node)->setInnerAngle((float)val);
            })
        .prop("outerAngle",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Light)
                    return JS_NewFloat64(ctx, static_cast<scene::LightNode*>(w->node)->outerAngle());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node)->setOuterAngle((float)val);
            })
        .prop("castsShadow",
            [](NodeWrapper* w) -> bool {
                if (!w || !w->node) return false;
                if (w->node->type() == scene::SceneNode::Type::Light)
                    return static_cast<scene::LightNode*>(w->node)->castsShadow();
                if (w->node->type() == scene::SceneNode::Type::Mesh)
                    return static_cast<scene::MeshNode*>(w->node)->castsShadow();
                return false;
            },
            [](NodeWrapper* w, bool val) {
                if (!w || !w->node) return;
                if (w->node->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node)->setCastsShadow(val);
                else if (w->node->type() == scene::SceneNode::Type::Mesh)
                    static_cast<scene::MeshNode*>(w->node)->setCastsShadow(val);
            })
        .prop("receivesShadow",
            [](NodeWrapper* w) -> bool {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Mesh)
                    return static_cast<scene::MeshNode*>(w->node)->receivesShadow();
                return false;
            },
            [](NodeWrapper* w, bool val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Mesh)
                    static_cast<scene::MeshNode*>(w->node)->setReceivesShadow(val);
            })
        .prop("shadowBias",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Light)
                    return JS_NewFloat64(ctx, static_cast<scene::LightNode*>(w->node)->shadowBias());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node)->setShadowBias((float)val);
            })
        .prop("shadowNormalBias",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Light)
                    return JS_NewFloat64(ctx, static_cast<scene::LightNode*>(w->node)->shadowNormalBias());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node)->setShadowNormalBias((float)val);
            })
        .prop("cascadeCount",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Light)
                    return JS_NewInt32(ctx, static_cast<scene::LightNode*>(w->node)->cascadeCount());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, int32_t val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node)->setCascadeCount(val);
            })
        .prop("cascadeSplitLambda",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Light)
                    return JS_NewFloat64(ctx, static_cast<scene::LightNode*>(w->node)->cascadeSplitLambda());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Light)
                    static_cast<scene::LightNode*>(w->node)->setCascadeSplitLambda((float)val);
            })

        // Shape properties (silently return undefined / no-op for non-shape nodes)
        .prop("width",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node) {
                    if (w->node->type() == scene::SceneNode::Type::Shape)
                        return JS_NewFloat64(ctx, static_cast<scene::ShapeNode*>(w->node)->width());
                    if (w->node->type() == scene::SceneNode::Type::Sprite)
                        return JS_NewFloat64(ctx, static_cast<scene::SpriteNode*>(w->node)->width());
                }
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Shape) {
                    auto* s = static_cast<scene::ShapeNode*>(w->node);
                    s->setSize(val, s->height());
                }
            })
        .prop("height",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Shape)
                    return JS_NewFloat64(ctx, static_cast<scene::ShapeNode*>(w->node)->height());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Shape) {
                    auto* s = static_cast<scene::ShapeNode*>(w->node);
                    s->setSize(s->width(), val);
                }
            })
        .prop("radius",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Shape)
                    return JS_NewFloat64(ctx, static_cast<scene::ShapeNode*>(w->node)->radius());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Shape)
                    static_cast<scene::ShapeNode*>(w->node)->setRadius(val);
            })
        .prop("fillColor",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Shape) {
                    auto c = static_cast<scene::ShapeNode*>(w->node)->fillColor();
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.2f)", c.r, c.g, c.b, c.a / 255.0f);
                    return JS_NewString(ctx, buf);
                }
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Shape) {
                    uint8_t r, g, b, a;
                    if (parseColor(jsStr(ctx, val), r, g, b, a))
                        static_cast<scene::ShapeNode*>(w->node)->setFillColor({r, g, b, a});
                }
            })
        .prop("strokeColor",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Shape) {
                    auto c = static_cast<scene::ShapeNode*>(w->node)->strokeColor();
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.2f)", c.r, c.g, c.b, c.a / 255.0f);
                    return JS_NewString(ctx, buf);
                }
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Shape) {
                    uint8_t r, g, b, a;
                    if (parseColor(jsStr(ctx, val), r, g, b, a))
                        static_cast<scene::ShapeNode*>(w->node)->setStrokeColor({r, g, b, a});
                }
            })
        .prop("strokeWidth",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Shape)
                    return JS_NewFloat64(ctx, static_cast<scene::ShapeNode*>(w->node)->strokeWidth());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Shape) {
                    auto* s = static_cast<scene::ShapeNode*>(w->node);
                    s->setStrokeWidth(val);
                    s->setHasStroke(true);
                }
            })

        // Physics properties
        .prop("autoSync",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Physics)
                    return JS_NewBool(ctx, static_cast<scene::PhysicsNode*>(w->node)->autoSync());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, bool val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Physics)
                    static_cast<scene::PhysicsNode*>(w->node)->setAutoSync(val);
            })
        .prop("pixelsPerUnit",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Physics)
                    return JS_NewFloat64(ctx, static_cast<scene::PhysicsNode*>(w->node)->pixelsPerUnit());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Physics)
                    static_cast<scene::PhysicsNode*>(w->node)->setPixelsPerUnit(val);
            })
        .get("bodyId", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (w && w->node && w->node->type() == scene::SceneNode::Type::Physics) {
                auto* p = static_cast<scene::PhysicsNode*>(w->node);
                if (p->hasBody())
                    return JS_NewInt32(ctx, (int32_t)p->bodyId().GetIndexAndSequenceNumber());
                return JS_NULL;
            }
            return JS_UNDEFINED;
        })

        // World anchor + billboard (Shape/Sprite/Html only — no-ops elsewhere)
        .prop("worldAnchor",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (!w || !w->node) return JS_UNDEFINED;
                if (!w->node->hasWorldAnchor()) return JS_NULL;
                const auto& a = w->node->worldAnchor();
                JSValue arr = JS_NewArray(ctx);
                JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, a.x));
                JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, a.y));
                JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, a.z));
                return arr;
            },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (!w || !w->node) return;
                if (JS_IsNull(val) || JS_IsUndefined(val)) {
                    w->node->clearWorldAnchor();
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
                    w->node->setWorldAnchor({(float)x, (float)y, (float)z});
                }
            })
        .prop("billboard",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (!w || !w->node) return JS_UNDEFINED;
                return JS_NewString(ctx,
                    w->node->billboardMode() == scene::SceneNode::BillboardMode::YLock
                        ? "ylock" : "full");
            },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (!w || !w->node) return;
                std::string s = jsStr(ctx, val);
                if (s == "ylock" || s == "yLock" || s == "y-lock") {
                    w->node->setBillboardMode(scene::SceneNode::BillboardMode::YLock);
                } else {
                    w->node->setBillboardMode(scene::SceneNode::BillboardMode::Full);
                }
            })

        // HtmlNode: `root` is the detached DOM Element that JS can mutate
        // imperatively. Mutations automatically mark the DOM dirty; the raster
        // thread re-rasterizes on the next frame.
        .get("root", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (!w || !w->node) return JS_UNDEFINED;
            if (w->node->type() != scene::SceneNode::Type::Html) return JS_UNDEFINED;
            auto* hn = static_cast<scene::HtmlNode*>(w->node);
            dom::Element* root = hn->root();
            if (!root) return JS_NULL;
            return DomBindings::wrapElement(ctx, root);
        })

        .get("parent", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (!w || !w->node) return JS_NULL;
            auto* p = w->node->parent();
            return p ? wrapNode(ctx, p, w->graph) : JS_NULL;
        })
        .prop("nearClipDist",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Mesh)
                    return JS_NewFloat64(ctx, static_cast<scene::MeshNode*>(w->node)->nearClipDist());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Mesh)
                    static_cast<scene::MeshNode*>(w->node)->setNearClipDist((float)val);
            })

        // Complex read-only properties
        .get("children", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (!w || !w->node) return JS_NewArray(ctx);
            const auto& kids = w->node->children();
            JSValue arr = JS_NewArray(ctx);
            uint32_t i = 0;
            for (auto* child : kids) {
                if (child) JS_SetPropertyUint32(ctx, arr, i++, wrapNode(ctx, child, w->graph));
            }
            return arr;
        })

        // SpriteNode: frame index + isPlaying + currentAnimation
        .prop("frameIndex",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Sprite)
                    return JS_NewInt32(ctx, static_cast<scene::SpriteNode*>(w->node)->frameIndex());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, int32_t val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Sprite)
                    static_cast<scene::SpriteNode*>(w->node)->setFrameIndex(val);
            })
        .get("isPlaying", [](NodeWrapper* w) -> bool {
            if (!w || !w->node) return false;
            if (w->node->type() == scene::SceneNode::Type::Sprite)
                return static_cast<scene::SpriteNode*>(w->node)->isPlaying();
            if (w->node->type() == scene::SceneNode::Type::Particles)
                return static_cast<scene::ParticleNode*>(w->node)->isPlaying();
            return false;
        })
        .get("currentAnimation", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (w && w->node && w->node->type() == scene::SceneNode::Type::Sprite) {
                const auto& s = static_cast<scene::SpriteNode*>(w->node)->currentAnimation();
                return JS_NewString(ctx, s.c_str());
            }
            return JS_UNDEFINED;
        })
        // ParticleNode: live count + emitter rate
        .get("liveCount", [](NodeWrapper* w, JSContext* ctx) -> JSValue {
            if (w && w->node && w->node->type() == scene::SceneNode::Type::Particles)
                return JS_NewInt32(ctx, static_cast<scene::ParticleNode*>(w->node)->liveCount());
            return JS_UNDEFINED;
        })
        .prop("rate",
            [](NodeWrapper* w, JSContext* ctx) -> JSValue {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Particles)
                    return JS_NewFloat64(ctx, static_cast<scene::ParticleNode*>(w->node)->rate());
                return JS_UNDEFINED;
            },
            [](NodeWrapper* w, double val) {
                if (w && w->node && w->node->type() == scene::SceneNode::Type::Particles)
                    static_cast<scene::ParticleNode*>(w->node)->setRate((float)val);
            })
        // SpriteNode: animation-end callback. Setter installs/removes the JS
        // callback in the side registry.
        .prop("onAnimationEnd",
            [](NodeWrapper*, JSContext*) -> JSValue { return JS_UNDEFINED; },
            [](NodeWrapper* w, JSContext* ctx, JSValue val) {
                if (!w || !w->node || w->node->type() != scene::SceneNode::Type::Sprite) return;
                installSpriteEndCallback(static_cast<scene::SpriteNode*>(w->node), ctx, val);
            })

        // Methods (raw — complex arg handling)
        .method_raw("add", js_node_add, 1)
        .method_raw("remove", js_node_remove, 1)
        .method_raw("destroy", js_node_destroy, 0)
        .method_raw("localToWorld", js_node_localToWorld, 2)
        .method_raw("syncToPhysics", js_node_syncToPhysics, 0)
        .method_raw("updateMesh", js_node_updateMesh, 1)
        .method_raw("setInstances", js_node_setInstances, 1)
        .method_raw("setInstancesFromTransforms", js_node_setInstancesFromTransforms, 1)
        .method_raw("updateInstance", js_node_updateInstance, 2)
        .method_raw("setInstancedMesh", js_node_setInstancedMesh, 1)
        .method_raw("setAtlasGrid", js_node_setAtlasGrid, 2)
        .method_raw("setAlphaCutoff", js_node_setAlphaCutoff, 1)
        .method_raw("setDoubleSided", js_node_setDoubleSided, 1)
        .method_raw("setBaseColorTexture", js_node_setBaseColorTexture, 1)
        .method_raw("setHtml", js_node_setHtml, 1)
        .method_raw("markHtmlDirty", js_node_markHtmlDirty, 0)
        .method_raw("attachAgent", nodeAttachAgent, 3)
        .method_raw("detachAgent", nodeDetachAgent, 0)
        // Sprite animation + Particles control
        .method_raw("play", js_node_play, 1)
        .method_raw("stop", js_node_stop, 0)
        .method_raw("addAnimation", js_sprite_addAnimation, 2)
        .method_raw("burst", js_particles_burst, 1)
        .method_raw("clear", js_particles_clear, 0)
        .method_raw("configure", js_particles_configure, 1)
        // Tilemap
        .method_raw("setTile", js_tilemap_setTile, 4)
        .method_raw("getTile", js_tilemap_getTile, 3)
        .method_raw("tileAtWorld", js_tilemap_tileAtWorld, 2);

    // --- SceneGraph class ---
    qjsbind::Class<GraphWrapper>(ctx, "SceneGraph")
        // Properties
        .get("root", [](GraphWrapper* w, JSContext* ctx) -> JSValue {
            return (w && w->graph) ? wrapNode(ctx, w->graph->root(), w->graph) : JS_UNDEFINED;
        })
        .prop("cameraX",
            [](GraphWrapper* w) -> double { return w && w->graph ? w->graph->cameraX() : 0; },
            [](GraphWrapper* w, double val) { if (w && w->graph) w->graph->setCameraPosition((float)val, w->graph->cameraY()); })
        .prop("cameraY",
            [](GraphWrapper* w) -> double { return w && w->graph ? w->graph->cameraY() : 0; },
            [](GraphWrapper* w, double val) { if (w && w->graph) w->graph->setCameraPosition(w->graph->cameraX(), (float)val); })
        .prop("cameraZoom",
            [](GraphWrapper* w) -> double { return w && w->graph ? w->graph->cameraZoom() : 1; },
            [](GraphWrapper* w, double val) { if (w && w->graph) w->graph->setCameraZoom((float)val); })
        .prop("showLightIcons",
            [](GraphWrapper* w) -> bool { return w && w->graph ? w->graph->showLightIcons() : false; },
            [](GraphWrapper* w, bool val) { if (w && w->graph) w->graph->setShowLightIcons(val); })

        // Methods (all raw — complex arg handling)
        .method_raw("createNode", js_sg_createNode, 1)
        .method_raw("createShape", js_sg_createShape, 1)
        .method_raw("createSprite", js_sg_createSprite, 1)
        .method_raw("createPhysicsNode", js_sg_createPhysicsNode, 1)
        .method_raw("createMesh", js_sg_createMesh, 1)
        .method_raw("createInstancedMesh", js_sg_createInstancedMesh, 1)
        .method_raw("createHtmlNode", js_sg_createHtml, 1)
        .method_raw("createLight", js_sg_createLight, 1)
        .method_raw("createParticles", js_sg_createParticles, 1)
        .method_raw("createTilemap", js_sg_createTilemap, 1)
        .method_raw("setToneMap", js_sg_setToneMap, 1)
        .method_raw("setAmbient", js_sg_setAmbient, 1)
        .method_raw("setWind", js_sg_setWind, 1)
        .method_raw("setShadowQuality", js_sg_setShadowQuality, 1)
        .method_raw("createTerrain", js_sg_createTerrain, 1)
        .method_raw("findById", js_sg_findById, 1)
        .method_raw("findByName", js_sg_findByName, 1)
        .method_raw("destroyNode", js_sg_destroyNode, 1)
        .method_raw("setCamera", js_sg_setCamera, 1)
        .method_raw("setFog", js_sg_setFog, 1)
        .method_raw("setEnvironment", js_sg_setEnvironment, 1)
        .method_raw("syncPhysics", js_sg_syncPhysics, 0)
        .method_raw("raycast", js_sg_raycast, 2)
        .method_raw("unprojectLocal", js_sg_unprojectLocal, 2)
        .method_raw("toImageData", js_sg_toImageData, 0)
        .method_raw("captureFrame", js_sg_captureFrame, 2)
        .get("viewMatrix", [](GraphWrapper* w, JSContext* ctx) -> JSValue {
            if (!w || !w->graph) return JS_NULL;
            return mat4ToJSArray(ctx, w->graph->viewMatrix());
        })
        .get("projectionMatrix", [](GraphWrapper* w, JSContext* ctx) -> JSValue {
            if (!w || !w->graph) return JS_NULL;
            return mat4ToJSArray(ctx, w->graph->projectionMatrix());
        })
        .get("cameraEye", [](GraphWrapper* w, JSContext* ctx) -> JSValue {
            if (!w || !w->graph) return JS_NULL;
            return vec3ToJSArray(ctx, w->graph->cameraEye());
        })
        .method_raw("attachAIWorld", graphAttachAIWorld, 2)
        .method_raw("detachAIWorld", graphDetachAIWorld, 0);
}

JSValue SceneBindings::wrapSceneGraph(JSContext* ctx, scene::SceneGraph* graph) {
    return qjsbind::wrap<GraphWrapper>(ctx, new GraphWrapper{graph});
}

void SceneBindings::cleanup(JSContext* ctx) {
    // No persistent JSValue/atom storage in this binding — qjsbind finalizers
    // handle wrappers and the engine-level globalThis sweep drops bro.scene.
    (void)ctx;
}

} // namespace bro::js
