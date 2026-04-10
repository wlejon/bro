#include "js/scene_bindings.h"
#include "js/mesh_bindings.h"
#include "js/terrain_bindings.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/shape_node.h"
#include "scene/sprite_node.h"
#include "scene/physics_node.h"
#include "scene/mesh_node.h"
#include "physics/physics_world.h"
#include "canvas/canvas_scene.h"
#include "util/log.h"

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

// ---------------------------------------------------------------------------
// Class IDs
// ---------------------------------------------------------------------------

static JSClassID js_scenegraph_class_id = 0;
static JSClassID js_scenenode_class_id = 0;

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

static void js_scenenode_finalizer(JSRuntime*, JSValue val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(val, js_scenenode_class_id));
    delete w;
}

static JSClassDef js_scenenode_class = {
    "SceneNode", js_scenenode_finalizer, nullptr, nullptr, nullptr
};

static JSValue wrapNode(JSContext* ctx, scene::SceneNode* node, scene::SceneGraph* graph);

// ---------------------------------------------------------------------------
// SceneNode properties
// ---------------------------------------------------------------------------

static JSValue js_node_get_id(JSContext* ctx, JSValueConst this_val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    return w ? JS_NewInt32(ctx, w->node->id()) : JS_UNDEFINED;
}

static JSValue js_node_get_name(JSContext* ctx, JSValueConst this_val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    return w ? JS_NewString(ctx, w->node->name().c_str()) : JS_UNDEFINED;
}

static JSValue js_node_set_name(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (w) w->node->setName(jsStr(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_node_get_visible(JSContext* ctx, JSValueConst this_val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    return w ? JS_NewBool(ctx, w->node->visible()) : JS_UNDEFINED;
}

static JSValue js_node_set_visible(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (w) w->node->setVisible(JS_ToBool(ctx, val));
    return JS_UNDEFINED;
}

// --- Position ---

static JSValue js_node_get_x(JSContext* ctx, JSValueConst this_val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    return w ? JS_NewFloat64(ctx, w->node->position().x) : JS_UNDEFINED;
}
static JSValue js_node_set_x(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (w) w->node->setPosition(jsNum(ctx, val), w->node->position().y, w->node->position().z);
    return JS_UNDEFINED;
}

static JSValue js_node_get_y(JSContext* ctx, JSValueConst this_val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    return w ? JS_NewFloat64(ctx, w->node->position().y) : JS_UNDEFINED;
}
static JSValue js_node_set_y(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (w) w->node->setPosition(w->node->position().x, jsNum(ctx, val), w->node->position().z);
    return JS_UNDEFINED;
}

// --- Z position ---
static JSValue js_node_get_z(JSContext* ctx, JSValueConst this_val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    return w ? JS_NewFloat64(ctx, w->node->position().z) : JS_UNDEFINED;
}
static JSValue js_node_set_z(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (w) w->node->setPosition(w->node->position().x, w->node->position().y, jsNum(ctx, val));
    return JS_UNDEFINED;
}

// --- Rotation (Euler angles per axis, radians) ---

static JSValue js_node_get_rotation(JSContext* ctx, JSValueConst this_val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (!w) return JS_UNDEFINED;
    // Backward compat: returns Z-axis rotation in radians
    return JS_NewFloat64(ctx, w->node->rotation().toEuler().z);
}
static JSValue js_node_set_rotation(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (w) w->node->setRotationZ(jsNum(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_node_get_rotationX(JSContext* ctx, JSValueConst this_val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    return w ? JS_NewFloat64(ctx, w->node->rotation().toEuler().x) : JS_UNDEFINED;
}
static JSValue js_node_set_rotationX(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (w) {
        auto e = w->node->rotation().toEuler();
        w->node->setRotationEuler(jsNum(ctx, val), e.y, e.z);
    }
    return JS_UNDEFINED;
}

static JSValue js_node_get_rotationY(JSContext* ctx, JSValueConst this_val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    return w ? JS_NewFloat64(ctx, w->node->rotation().toEuler().y) : JS_UNDEFINED;
}
static JSValue js_node_set_rotationY(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (w) {
        auto e = w->node->rotation().toEuler();
        w->node->setRotationEuler(e.x, jsNum(ctx, val), e.z);
    }
    return JS_UNDEFINED;
}

static JSValue js_node_get_rotationZ(JSContext* ctx, JSValueConst this_val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    return w ? JS_NewFloat64(ctx, w->node->rotation().toEuler().z) : JS_UNDEFINED;
}
static JSValue js_node_set_rotationZ(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (w) {
        auto e = w->node->rotation().toEuler();
        w->node->setRotationEuler(e.x, e.y, jsNum(ctx, val));
    }
    return JS_UNDEFINED;
}

// --- Scale ---

static JSValue js_node_get_scaleX(JSContext* ctx, JSValueConst this_val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    return w ? JS_NewFloat64(ctx, w->node->scale().x) : JS_UNDEFINED;
}
static JSValue js_node_set_scaleX(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (w) w->node->setScale(jsNum(ctx, val), w->node->scale().y, w->node->scale().z);
    return JS_UNDEFINED;
}
static JSValue js_node_get_scaleY(JSContext* ctx, JSValueConst this_val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    return w ? JS_NewFloat64(ctx, w->node->scale().y) : JS_UNDEFINED;
}
static JSValue js_node_set_scaleY(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (w) w->node->setScale(w->node->scale().x, jsNum(ctx, val), w->node->scale().z);
    return JS_UNDEFINED;
}
static JSValue js_node_get_scaleZ(JSContext* ctx, JSValueConst this_val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    return w ? JS_NewFloat64(ctx, w->node->scale().z) : JS_UNDEFINED;
}
static JSValue js_node_set_scaleZ(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (w) w->node->setScale(w->node->scale().x, w->node->scale().y, jsNum(ctx, val));
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// SceneNode methods
// ---------------------------------------------------------------------------

// add(child)
static JSValue js_node_add(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* pw = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (!pw || argc < 1) return JS_UNDEFINED;
    auto* cw = static_cast<NodeWrapper*>(JS_GetOpaque(argv[0], js_scenenode_class_id));
    if (!cw) return JS_ThrowTypeError(ctx, "argument must be a SceneNode");
    pw->node->addChild(cw->node);
    return JS_DupValue(ctx, this_val);
}

// remove(child)
static JSValue js_node_remove(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* pw = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (!pw || argc < 1) return JS_UNDEFINED;
    auto* cw = static_cast<NodeWrapper*>(JS_GetOpaque(argv[0], js_scenenode_class_id));
    if (cw) pw->node->removeChild(cw->node);
    return JS_UNDEFINED;
}

// children → SceneNode[]
//
// Returns a fresh JS array of wrapped child nodes. Each access copies the
// current child list — the array is not live, so callers iterating it can
// safely call destroy()/removeChild() during the loop without surprises.
static JSValue js_node_get_children(JSContext* ctx, JSValueConst this_val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (!w || !w->node) return JS_NewArray(ctx);
    const auto& kids = w->node->children();
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (auto* child : kids) {
        if (child) JS_SetPropertyUint32(ctx, arr, i++, wrapNode(ctx, child, w->graph));
    }
    return arr;
}

// childCount → number — cheap size check that doesn't allocate the array.
static JSValue js_node_get_childCount(JSContext* ctx, JSValueConst this_val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (!w || !w->node) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, (int32_t)w->node->children().size());
}

// destroy()
static JSValue js_node_destroy(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (w && w->graph) {
        w->graph->destroyNode(w->node);
        w->node = nullptr;
    }
    return JS_UNDEFINED;
}

// localToWorld(x, y[, z]) → {x, y, z}
static JSValue js_node_localToWorld(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (!w || argc < 2) return JS_UNDEFINED;
    float z = (argc > 2) ? (float)jsNum(ctx, argv[2]) : 0.0f;
    auto wp = w->node->localToWorld({(float)jsNum(ctx, argv[0]), (float)jsNum(ctx, argv[1]), z});
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, wp.x));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, wp.y));
    JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, wp.z));
    return obj;
}

// ---------------------------------------------------------------------------
// Shape-specific properties (only work on ShapeNodes, silently ignored otherwise)
// ---------------------------------------------------------------------------

static scene::ShapeNode* asShape(JSValueConst val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(val, js_scenenode_class_id));
    if (w && w->node && w->node->type() == scene::SceneNode::Type::Shape)
        return static_cast<scene::ShapeNode*>(w->node);
    return nullptr;
}

static JSValue js_node_get_width(JSContext* ctx, JSValueConst this_val) {
    if (auto* s = asShape(this_val)) return JS_NewFloat64(ctx, s->width());
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (w && w->node && w->node->type() == scene::SceneNode::Type::Sprite)
        return JS_NewFloat64(ctx, static_cast<scene::SpriteNode*>(w->node)->width());
    return JS_UNDEFINED;
}
static JSValue js_node_set_width(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    if (auto* s = asShape(this_val)) s->setSize(jsNum(ctx, val), s->height());
    return JS_UNDEFINED;
}
static JSValue js_node_get_height(JSContext* ctx, JSValueConst this_val) {
    if (auto* s = asShape(this_val)) return JS_NewFloat64(ctx, s->height());
    return JS_UNDEFINED;
}
static JSValue js_node_set_height(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    if (auto* s = asShape(this_val)) s->setSize(s->width(), jsNum(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_node_get_radius(JSContext* ctx, JSValueConst this_val) {
    if (auto* s = asShape(this_val)) return JS_NewFloat64(ctx, s->radius());
    return JS_UNDEFINED;
}
static JSValue js_node_set_radius(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    if (auto* s = asShape(this_val)) s->setRadius(jsNum(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_node_get_fillColor(JSContext* ctx, JSValueConst this_val) {
    if (auto* s = asShape(this_val)) {
        auto c = s->fillColor();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.2f)", c.r, c.g, c.b, c.a / 255.0f);
        return JS_NewString(ctx, buf);
    }
    return JS_UNDEFINED;
}
static JSValue js_node_set_fillColor(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    if (auto* s = asShape(this_val)) {
        uint8_t r, g, b, a;
        if (parseColor(jsStr(ctx, val), r, g, b, a)) {
            s->setFillColor({r, g, b, a});
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_node_get_strokeColor(JSContext* ctx, JSValueConst this_val) {
    if (auto* s = asShape(this_val)) {
        auto c = s->strokeColor();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.2f)", c.r, c.g, c.b, c.a / 255.0f);
        return JS_NewString(ctx, buf);
    }
    return JS_UNDEFINED;
}
static JSValue js_node_set_strokeColor(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    if (auto* s = asShape(this_val)) {
        uint8_t r, g, b, a;
        if (parseColor(jsStr(ctx, val), r, g, b, a)) {
            s->setStrokeColor({r, g, b, a});
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_node_get_strokeWidth(JSContext* ctx, JSValueConst this_val) {
    if (auto* s = asShape(this_val)) return JS_NewFloat64(ctx, s->strokeWidth());
    return JS_UNDEFINED;
}
static JSValue js_node_set_strokeWidth(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    if (auto* s = asShape(this_val)) {
        s->setStrokeWidth(jsNum(ctx, val));
        s->setHasStroke(true);
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Physics-specific properties
// ---------------------------------------------------------------------------

static scene::PhysicsNode* asPhysics(JSValueConst val) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(val, js_scenenode_class_id));
    if (w && w->node && w->node->type() == scene::SceneNode::Type::Physics)
        return static_cast<scene::PhysicsNode*>(w->node);
    return nullptr;
}

static JSValue js_node_get_autoSync(JSContext* ctx, JSValueConst this_val) {
    if (auto* p = asPhysics(this_val)) return JS_NewBool(ctx, p->autoSync());
    return JS_UNDEFINED;
}
static JSValue js_node_set_autoSync(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    if (auto* p = asPhysics(this_val)) p->setAutoSync(JS_ToBool(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_node_get_pixelsPerUnit(JSContext* ctx, JSValueConst this_val) {
    if (auto* p = asPhysics(this_val)) return JS_NewFloat64(ctx, p->pixelsPerUnit());
    return JS_UNDEFINED;
}
static JSValue js_node_set_pixelsPerUnit(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    if (auto* p = asPhysics(this_val)) p->setPixelsPerUnit(jsNum(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_node_get_bodyId(JSContext* ctx, JSValueConst this_val) {
    if (auto* p = asPhysics(this_val)) {
        if (p->hasBody())
            return JS_NewInt32(ctx, (int32_t)p->bodyId().GetIndexAndSequenceNumber());
        return JS_NULL;
    }
    return JS_UNDEFINED;
}

// syncToPhysics()
static JSValue js_node_syncToPhysics(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
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

// updateMesh(meshOrOpts[, opts]) — replace geometry on an existing MeshNode.
//
// Forms:
//   node.updateMesh(meshObj)                         // copy
//   node.updateMesh(meshObj, { transfer: true })     // consume by move
//   node.updateMesh({ mesh: m, transfer: true })     // same as above
//   node.updateMesh({ positions, indices, normals }) // raw vertex data
//
// With `transfer: true`, the Mesh's underlying MeshData is moved out of the JS
// wrapper directly into the MeshNode (zero-copy, matching postMessage
// transferList semantics). The source Mesh is left neutered. Clone first if
// you still need it.
static JSValue js_node_updateMesh(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(this_val, js_scenenode_class_id));
    if (!w || !w->node)
        return JS_ThrowTypeError(ctx, "updateMesh: invalid node");
    if (w->node->type() != scene::SceneNode::Type::Mesh)
        return JS_ThrowTypeError(ctx, "updateMesh: node is not a MeshNode");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "updateMesh: missing argument");

    auto* meshNode = static_cast<scene::MeshNode*>(w->node);
    bromesh::MeshData meshData;
    bool gotData = false;

    // Read the optional second-arg opts object (for positional form) or the
    // transfer flag off the first arg when it's an options object.
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

// ---------------------------------------------------------------------------
// SceneNode prototype
// ---------------------------------------------------------------------------

static const JSCFunctionListEntry js_scenenode_proto[] = {
    // Common properties
    JS_CGETSET_DEF("id", js_node_get_id, nullptr),
    JS_CGETSET_DEF("name", js_node_get_name, js_node_set_name),
    JS_CGETSET_DEF("visible", js_node_get_visible, js_node_set_visible),
    JS_CGETSET_DEF("children", js_node_get_children, nullptr),
    JS_CGETSET_DEF("childCount", js_node_get_childCount, nullptr),

    // Transform
    JS_CGETSET_DEF("x", js_node_get_x, js_node_set_x),
    JS_CGETSET_DEF("y", js_node_get_y, js_node_set_y),
    JS_CGETSET_DEF("z", js_node_get_z, js_node_set_z),
    JS_CGETSET_DEF("rotation", js_node_get_rotation, js_node_set_rotation),
    JS_CGETSET_DEF("rotationX", js_node_get_rotationX, js_node_set_rotationX),
    JS_CGETSET_DEF("rotationY", js_node_get_rotationY, js_node_set_rotationY),
    JS_CGETSET_DEF("rotationZ", js_node_get_rotationZ, js_node_set_rotationZ),
    JS_CGETSET_DEF("scaleX", js_node_get_scaleX, js_node_set_scaleX),
    JS_CGETSET_DEF("scaleY", js_node_get_scaleY, js_node_set_scaleY),
    JS_CGETSET_DEF("scaleZ", js_node_get_scaleZ, js_node_set_scaleZ),

    // Shape properties
    JS_CGETSET_DEF("width", js_node_get_width, js_node_set_width),
    JS_CGETSET_DEF("height", js_node_get_height, js_node_set_height),
    JS_CGETSET_DEF("radius", js_node_get_radius, js_node_set_radius),
    JS_CGETSET_DEF("fillColor", js_node_get_fillColor, js_node_set_fillColor),
    JS_CGETSET_DEF("strokeColor", js_node_get_strokeColor, js_node_set_strokeColor),
    JS_CGETSET_DEF("strokeWidth", js_node_get_strokeWidth, js_node_set_strokeWidth),

    // Physics properties
    JS_CGETSET_DEF("autoSync", js_node_get_autoSync, js_node_set_autoSync),
    JS_CGETSET_DEF("pixelsPerUnit", js_node_get_pixelsPerUnit, js_node_set_pixelsPerUnit),
    JS_CGETSET_DEF("bodyId", js_node_get_bodyId, nullptr),

    // Methods
    JS_CFUNC_DEF("add", 1, js_node_add),
    JS_CFUNC_DEF("remove", 1, js_node_remove),
    JS_CFUNC_DEF("destroy", 0, js_node_destroy),
    JS_CFUNC_DEF("localToWorld", 2, js_node_localToWorld),
    JS_CFUNC_DEF("syncToPhysics", 0, js_node_syncToPhysics),
    JS_CFUNC_DEF("updateMesh", 1, js_node_updateMesh),
};

// ---------------------------------------------------------------------------
// SceneGraph JS wrapper
// ---------------------------------------------------------------------------

struct GraphWrapper {
    scene::SceneGraph* graph;
};

static void js_scenegraph_finalizer(JSRuntime*, JSValue val) {
    auto* w = static_cast<GraphWrapper*>(JS_GetOpaque(val, js_scenegraph_class_id));
    delete w;
}

static JSClassDef js_scenegraph_class = {
    "SceneGraph", js_scenegraph_finalizer, nullptr, nullptr, nullptr
};

static inline scene::SceneGraph* getGraph(JSValueConst val) {
    auto* w = static_cast<GraphWrapper*>(JS_GetOpaque(val, js_scenegraph_class_id));
    return w ? w->graph : nullptr;
}

// ---------------------------------------------------------------------------
// SceneGraph methods
// ---------------------------------------------------------------------------

// root → SceneNode
static JSValue js_sg_get_root(JSContext* ctx, JSValueConst this_val) {
    auto* g = getGraph(this_val);
    if (!g) return JS_UNDEFINED;
    return wrapNode(ctx, g->root(), g);
}

// createNode(name?) → SceneNode
static JSValue js_sg_createNode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(this_val);
    if (!g) return JS_UNDEFINED;
    std::string name = (argc > 0) ? jsStr(ctx, argv[0]) : "";
    auto* node = g->createNode(name);
    g->root()->addChild(node);
    return wrapNode(ctx, node, g);
}

// createShape(opts?) → SceneNode (ShapeNode)
// opts: { shape, width, height, radius, fill, stroke, strokeWidth, ... }
static JSValue js_sg_createShape(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(this_val);
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
    }

    return wrapNode(ctx, node, g);
}

// createSprite(opts?) → SceneNode (SpriteNode)
static JSValue js_sg_createSprite(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(this_val);
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
    }

    return wrapNode(ctx, node, g);
}

// createPhysicsNode(opts) → SceneNode (PhysicsNode)
// opts: { body: bodyTag, pixelsPerUnit, autoSync, name }
static JSValue js_sg_createPhysicsNode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(this_val);
    if (!g) return JS_UNDEFINED;

    auto* node = g->createPhysicsNode();
    g->root()->addChild(node);

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];

        JSValue nameVal = JS_GetPropertyStr(ctx, opts, "name");
        if (JS_IsString(nameVal)) node->setName(jsStr(ctx, nameVal));
        JS_FreeValue(ctx, nameVal);

        // Body ID (raw Jolt BodyID index+sequence number)
        JSValue bodyVal = JS_GetPropertyStr(ctx, opts, "body");
        if (JS_IsNumber(bodyVal)) {
            int32_t bodyRaw;
            JS_ToInt32(ctx, &bodyRaw, bodyVal);
            node->setBody(JPH::BodyID((uint32_t)bodyRaw));
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
// mesh: "box"|"sphere"|"cylinder"|"capsule"|"plane"|"torus" or raw {positions, normals, indices}
static JSValue js_sg_createMesh(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(this_val);
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

        // Emissive
        double emissive = jsGetProp(ctx, opts, "emissive", 0.0);
        node->setEmissive((float)emissive);

        // Depth bias (polygon offset). Pass either a single number for the
        // `units` argument, or an array [factor, units]. Negative values pull
        // the mesh forward in the depth buffer, useful for layering LODs.
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

        // Mesh: either a Mesh object, raw vertex data, or a named primitive.
        //
        // By default a Mesh argument is COPIED into the new MeshNode, leaving
        // the JS Mesh usable. Set `transfer: true` in the options to move the
        // MeshData out of the wrapper instead — the source Mesh is neutered,
        // matching postMessage transferList semantics. This is the zero-copy
        // path the terrain worker pipeline uses.
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

        // Prefer "mesh" key, fall back to legacy "data". If "mesh" is a
        // string ("box", "sphere", ...) the MeshBindings check returns null
        // and we fall through to the named-primitive branch.
        if (tryKey("mesh")) hasRawData = true;
        else if (tryKey("data")) hasRawData = true;

        // Check for raw vertex data (positions + indices arrays)
        if (!hasRawData) {
            std::vector<float> positions, normals;
            std::vector<uint32_t> indices;
            if (jsReadFloatArray(ctx, opts, "positions", positions) &&
                jsReadUint32Array(ctx, opts, "indices", indices)) {
                meshData.positions = std::move(positions);
                meshData.indices = std::move(indices);
                if (jsReadFloatArray(ctx, opts, "normals", normals)) {
                    meshData.normals = std::move(normals);
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

    return wrapNode(ctx, node, g);
}

// findById(id) → SceneNode | null
static JSValue js_sg_findById(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(this_val);
    if (!g || argc < 1) return JS_NULL;
    int32_t id;
    JS_ToInt32(ctx, &id, argv[0]);
    auto* node = g->findById((uint32_t)id);
    return node ? wrapNode(ctx, node, g) : JS_NULL;
}

// findByName(name) → SceneNode | null
static JSValue js_sg_findByName(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(this_val);
    if (!g || argc < 1) return JS_NULL;
    auto* node = g->findByName(jsStr(ctx, argv[0]));
    return node ? wrapNode(ctx, node, g) : JS_NULL;
}

// syncPhysics() — manually sync physics transforms
static JSValue js_sg_syncPhysics(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* g = getGraph(this_val);
    if (g) g->syncPhysics();
    return JS_UNDEFINED;
}

// destroyNode(node)
static JSValue js_sg_destroyNode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(this_val);
    if (!g || argc < 1) return JS_UNDEFINED;
    auto* cw = static_cast<NodeWrapper*>(JS_GetOpaque(argv[0], js_scenenode_class_id));
    if (cw) g->destroyNode(cw->node);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// raycast(origin, direction, maxDistance) → { hit, point, normal, distance, node } | null
//
// Walks every MeshNode in the graph, inverse-transforms the ray into each
// node's local space (via its TRS components), calls bromesh::raycast, and
// keeps the closest hit. Hit position and normal are returned in world space.
//
// Assumes the node's world transform is a composition of translate, rotate,
// and uniform scale (which is what the TRS path in Mat4 produces and what
// every MeshNode in the engine currently uses). Non-uniform scale would need
// a proper inverse-transpose for normal transforms — not worth supporting
// until a caller actually needs it.
// ---------------------------------------------------------------------------
static JSValue js_sg_raycast(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(this_val);
    if (!g || argc < 2) return JS_NULL;

    // Parse origin + direction from JS arrays or object args.
    auto parseVec3 = [&](JSValueConst v, scene::Vec3& out) -> bool {
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

    scene::Vec3 origin, dir;
    if (!parseVec3(argv[0], origin)) return JS_ThrowTypeError(ctx, "raycast: origin must be [x,y,z]");
    if (!parseVec3(argv[1], dir))    return JS_ThrowTypeError(ctx, "raycast: direction must be [x,y,z]");

    double maxDist = 0.0;  // 0 = unlimited per bromesh::raycast
    if (argc >= 3) JS_ToFloat64(ctx, &maxDist, argv[2]);

    // Normalize direction so `distance` in the hit result is in world units
    // regardless of the caller's input magnitude.
    dir = dir.normalized();
    if (dir.lengthSq() < 1e-12f) return JS_NULL;

    // Walk the graph. Track the closest hit across all mesh nodes.
    float closestDist = (maxDist > 0.0) ? (float)maxDist : 1e30f;
    scene::MeshNode* closestNode = nullptr;
    bromesh::RayHit closestHit;
    scene::Vec3 closestWorldPoint;
    scene::Vec3 closestWorldNormal;

    g->root()->traverse([&](scene::SceneNode* node) {
        if (!node || node->type() != scene::SceneNode::Type::Mesh) return;
        if (!node->visible()) return;
        auto* mn = static_cast<scene::MeshNode*>(node);
        const bromesh::MeshData& md = mn->mesh();
        if (md.positions.empty() || md.indices.empty()) return;

        // Build world→local inverse from the node's TRS components. This
        // assumes the node's world matrix is (parent) * T * R * S with
        // uniform scale; good enough for terrain chunks and the current
        // MeshNode usage. For parented nodes we'd need to walk and compose
        // parent inverses — skipped until needed.
        const scene::Vec3& nodePos = node->position();
        const scene::Quat& nodeRot = node->rotation();
        const scene::Vec3& nodeScl = node->scale();

        scene::Vec3 localOrigin = origin - nodePos;
        localOrigin = nodeRot.conjugate().rotate(localOrigin);
        // Uniform scale is the common case; divide component-wise to handle
        // non-uniform without misbehaving (hit normal is still treated as
        // uniform below, which is a known limitation).
        if (nodeScl.x != 0.0f) localOrigin.x /= nodeScl.x;
        if (nodeScl.y != 0.0f) localOrigin.y /= nodeScl.y;
        if (nodeScl.z != 0.0f) localOrigin.z /= nodeScl.z;

        scene::Vec3 localDir = nodeRot.conjugate().rotate(dir);
        if (nodeScl.x != 0.0f) localDir.x /= nodeScl.x;
        if (nodeScl.y != 0.0f) localDir.y /= nodeScl.y;
        if (nodeScl.z != 0.0f) localDir.z /= nodeScl.z;

        // localDir's magnitude affects bromesh raycast's reported distance
        // (it treats the direction as-is). We normalize and rescale the
        // distance-bound accordingly so closestDist stays in world units.
        float localDirLen = localDir.length();
        if (localDirLen < 1e-12f) return;
        scene::Vec3 localDirN = localDir * (1.0f / localDirLen);
        // The closest world hit so far, converted into local-space distance.
        // For a uniform scale, local distance = world distance / scale.
        // We use nodeScl.x as the scale factor (uniform assumption).
        float scale = nodeScl.x != 0.0f ? nodeScl.x : 1.0f;
        float localMaxDist = closestDist / scale;

        // Early-out: local-space AABB slab test against the cached bounds.
        // Prunes most terrain chunks without touching the BVH at all, which
        // is the big win for terrain apps where the raycast walks thousands
        // of mesh nodes per click.
        {
            const bromesh::BBox& lb = mn->localBounds();
            // Expand zero-extent axes slightly so a flat mesh (e.g. a plane)
            // still passes the slab test along its degenerate axis.
            float bmin[3] = { lb.min[0], lb.min[1], lb.min[2] };
            float bmax[3] = { lb.max[0], lb.max[1], lb.max[2] };
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

        // BVH raycast. MeshNode lazily builds + caches the BVH against the
        // current mesh; rebuilt after setMesh.
        float o[3] = { localOrigin.x, localOrigin.y, localOrigin.z };
        float d[3] = { localDirN.x, localDirN.y, localDirN.z };
        bromesh::RayHit hit = mn->bvh().raycast(md, o, d, localMaxDist);
        if (!hit.hit) return;

        // Convert the local hit back to world space.
        scene::Vec3 localHit{hit.position[0], hit.position[1], hit.position[2]};
        localHit.x *= nodeScl.x;
        localHit.y *= nodeScl.y;
        localHit.z *= nodeScl.z;
        scene::Vec3 worldHit = nodeRot.rotate(localHit) + nodePos;

        // Distance in world = distance from world ray origin.
        scene::Vec3 toHit = worldHit - origin;
        float worldDist = toHit.length();
        if (worldDist >= closestDist) return;

        scene::Vec3 localNormal{hit.normal[0], hit.normal[1], hit.normal[2]};
        scene::Vec3 worldNormal = nodeRot.rotate(localNormal).normalized();

        closestDist = worldDist;
        closestNode = mn;
        closestHit = hit;
        closestWorldPoint = worldHit;
        closestWorldNormal = worldNormal;
    });

    if (!closestNode) return JS_NULL;

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "hit", JS_TRUE);
    JS_SetPropertyStr(ctx, out, "distance", JS_NewFloat64(ctx, closestDist));

    // Build the world-space hit point once and expose it under both names:
    // `position` matches Mesh.raycast()'s field name (the natural choice for
    // anyone who learned the API there first); `point` is the original alias
    // and is kept so existing callers don't break.
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

    JS_SetPropertyStr(ctx, out, "node", wrapNode(ctx, closestNode, g));

    return out;
}

// --- Helper: parse a [x, y, z] array into Vec3 ---
static scene::Vec3 jsGetVec3(JSContext* ctx, JSValueConst obj, const char* prop,
                             float dx = 0, float dy = 0, float dz = 0) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    scene::Vec3 r{dx, dy, dz};
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

// Parse a [x, y, z, w] array into Quat. Returns identity if property missing.
static scene::Quat jsGetQuat(JSContext* ctx, JSValueConst obj, const char* prop, bool& found) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    scene::Quat r{0, 0, 0, 1};
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
    auto* g = getGraph(this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

    JSValueConst opts = argv[0];
    double fov = jsGetProp(ctx, opts, "fov", 60.0) * 3.14159265 / 180.0; // degrees to radians
    double nearZ = jsGetProp(ctx, opts, "near", 0.1);
    double farZ = jsGetProp(ctx, opts, "far", 1000.0);
    double aspect = jsGetProp(ctx, opts, "aspect", 0.0);

    // If aspect not provided, default to 4:3 (canvas may not be available here)
    if (aspect <= 0) aspect = 4.0 / 3.0;

    scene::Vec3 position = jsGetVec3(ctx, opts, "position", 0, 5, -10);

    // Check for quaternion-based camera (avoids lookAt precision loss)
    bool hasQuat = false;
    scene::Quat quat = jsGetQuat(ctx, opts, "quaternion", hasQuat);

    if (hasQuat) {
        g->setCameraQuat((float)fov, (float)aspect, (float)nearZ, (float)farZ,
                         position, quat.normalized());
    } else {
        scene::Vec3 target = jsGetVec3(ctx, opts, "target", 0, 0, 0);
        scene::Vec3 up = jsGetVec3(ctx, opts, "up", 0, 1, 0);

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

// Camera properties
static JSValue js_sg_get_cameraX(JSContext* ctx, JSValueConst this_val) {
    auto* g = getGraph(this_val);
    return g ? JS_NewFloat64(ctx, g->cameraX()) : JS_UNDEFINED;
}
static JSValue js_sg_set_cameraX(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* g = getGraph(this_val);
    if (g) g->setCameraPosition((float)jsNum(ctx, val), g->cameraY());
    return JS_UNDEFINED;
}
static JSValue js_sg_get_cameraY(JSContext* ctx, JSValueConst this_val) {
    auto* g = getGraph(this_val);
    return g ? JS_NewFloat64(ctx, g->cameraY()) : JS_UNDEFINED;
}
static JSValue js_sg_set_cameraY(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* g = getGraph(this_val);
    if (g) g->setCameraPosition(g->cameraX(), (float)jsNum(ctx, val));
    return JS_UNDEFINED;
}
static JSValue js_sg_get_cameraZoom(JSContext* ctx, JSValueConst this_val) {
    auto* g = getGraph(this_val);
    return g ? JS_NewFloat64(ctx, g->cameraZoom()) : JS_UNDEFINED;
}
static JSValue js_sg_set_cameraZoom(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* g = getGraph(this_val);
    if (g) g->setCameraZoom((float)jsNum(ctx, val));
    return JS_UNDEFINED;
}

// setFog({start, end, color})
static JSValue js_sg_setFog(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(this_val);
    if (!g || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

    JSValueConst opts = argv[0];
    double start = jsGetProp(ctx, opts, "start", 0.0);
    double end = jsGetProp(ctx, opts, "end", 0.0);
    scene::Vec3 color = jsGetVec3(ctx, opts, "color", 0.0f, 0.0f, 0.0f);
    g->setFog((float)start, (float)end, color.x, color.y, color.z);
    return JS_UNDEFINED;
}

// createTerrain(opts) → Terrain
static JSValue js_sg_createTerrain(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* g = getGraph(this_val);
    if (!g) return JS_NULL;
    JSValueConst opts = (argc >= 1 && JS_IsObject(argv[0])) ? argv[0] : JS_UNDEFINED;
    // If no opts provided, create a temporary empty object for defaults.
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
// SceneGraph prototype
// ---------------------------------------------------------------------------

static const JSCFunctionListEntry js_scenegraph_proto[] = {
    JS_CGETSET_DEF("root", js_sg_get_root, nullptr),
    JS_CGETSET_DEF("cameraX", js_sg_get_cameraX, js_sg_set_cameraX),
    JS_CGETSET_DEF("cameraY", js_sg_get_cameraY, js_sg_set_cameraY),
    JS_CGETSET_DEF("cameraZoom", js_sg_get_cameraZoom, js_sg_set_cameraZoom),

    JS_CFUNC_DEF("createNode", 1, js_sg_createNode),
    JS_CFUNC_DEF("createShape", 1, js_sg_createShape),
    JS_CFUNC_DEF("createSprite", 1, js_sg_createSprite),
    JS_CFUNC_DEF("createPhysicsNode", 1, js_sg_createPhysicsNode),
    JS_CFUNC_DEF("createMesh", 1, js_sg_createMesh),
    JS_CFUNC_DEF("createTerrain", 1, js_sg_createTerrain),
    JS_CFUNC_DEF("findById", 1, js_sg_findById),
    JS_CFUNC_DEF("findByName", 1, js_sg_findByName),
    JS_CFUNC_DEF("destroyNode", 1, js_sg_destroyNode),
    JS_CFUNC_DEF("setCamera", 1, js_sg_setCamera),
    JS_CFUNC_DEF("setFog", 1, js_sg_setFog),
    JS_CFUNC_DEF("syncPhysics", 0, js_sg_syncPhysics),
    JS_CFUNC_DEF("raycast", 2, js_sg_raycast),
};

// ---------------------------------------------------------------------------
// Node wrapping helper
// ---------------------------------------------------------------------------

static JSValue s_scenenode_proto = JS_UNDEFINED;

static JSValue wrapNode(JSContext* ctx, scene::SceneNode* node, scene::SceneGraph* graph) {
    JSValue obj = JS_NewObjectClass(ctx, js_scenenode_class_id);
    JS_SetPrototype(ctx, obj, JS_DupValue(ctx, s_scenenode_proto));
    JS_SetOpaque(obj, new NodeWrapper{node, graph});
    return obj;
}

// ---------------------------------------------------------------------------
// Install / Cleanup
// ---------------------------------------------------------------------------

static JSValue s_scenegraph_proto = JS_UNDEFINED;

void SceneBindings::install(JSContext* ctx) {
    // SceneNode class
    JS_NewClassID(JS_GetRuntime(ctx), &js_scenenode_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_scenenode_class_id, &js_scenenode_class);

    s_scenenode_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, s_scenenode_proto,
                               js_scenenode_proto, sizeof(js_scenenode_proto) / sizeof(js_scenenode_proto[0]));

    // SceneGraph class
    JS_NewClassID(JS_GetRuntime(ctx), &js_scenegraph_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_scenegraph_class_id, &js_scenegraph_class);

    s_scenegraph_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, s_scenegraph_proto,
                               js_scenegraph_proto, sizeof(js_scenegraph_proto) / sizeof(js_scenegraph_proto[0]));
}

JSValue SceneBindings::wrapSceneGraph(JSContext* ctx, scene::SceneGraph* graph) {
    JSValue obj = JS_NewObjectClass(ctx, js_scenegraph_class_id);
    JS_SetPrototype(ctx, obj, JS_DupValue(ctx, s_scenegraph_proto));
    JS_SetOpaque(obj, new GraphWrapper{graph});
    return obj;
}

void SceneBindings::cleanup(JSContext* ctx) {
    JS_FreeValue(ctx, s_scenenode_proto);
    JS_FreeValue(ctx, s_scenegraph_proto);
    s_scenenode_proto = JS_UNDEFINED;
    s_scenegraph_proto = JS_UNDEFINED;
}

} // namespace bro::js
