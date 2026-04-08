#include "js/scene_bindings.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/shape_node.h"
#include "scene/sprite_node.h"
#include "scene/physics_node.h"
#include "physics/physics_world.h"
#include "canvas/canvas_scene.h"
#include "util/log.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

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

// ---------------------------------------------------------------------------
// SceneNode prototype
// ---------------------------------------------------------------------------

static const JSCFunctionListEntry js_scenenode_proto[] = {
    // Common properties
    JS_CGETSET_DEF("id", js_node_get_id, nullptr),
    JS_CGETSET_DEF("name", js_node_get_name, js_node_set_name),
    JS_CGETSET_DEF("visible", js_node_get_visible, js_node_set_visible),

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

// render() — manually trigger scene graph render
static JSValue js_sg_render(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* g = getGraph(this_val);
    if (g) g->render();
    return JS_UNDEFINED;
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

// setCamera({fov, near, far, aspect, position, target, up})
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
    JS_CFUNC_DEF("findById", 1, js_sg_findById),
    JS_CFUNC_DEF("findByName", 1, js_sg_findByName),
    JS_CFUNC_DEF("destroyNode", 1, js_sg_destroyNode),
    JS_CFUNC_DEF("setCamera", 1, js_sg_setCamera),
    JS_CFUNC_DEF("render", 0, js_sg_render),
    JS_CFUNC_DEF("syncPhysics", 0, js_sg_syncPhysics),
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
