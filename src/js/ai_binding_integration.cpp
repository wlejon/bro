// Integration glue wiring brogameagent capability/policy + scene AgentBinding
// through to JS. Ships:
//   - bro.ai.game.registerCapability(name, {gate, start, advance, id?})
//   - sceneGraph.attachAIWorld(world, {stepHz, maxStepsPerFrame})
//   - sceneGraph.detachAIWorld()
//   - node.attachAgent(agent, {capabilities, think, thinkHz, yOffset, faceMovement, policy, laneWaypoints})
//   - node.detachAgent()
// plus the per-tick `self` proxy whose methods reflect the binding's enabled
// capabilities.
//
// NodeWrapper / GraphWrapper live in scene_bindings.cpp; we forward-declare
// the shape of those structs here. Access is through thin helpers exposed via
// headers so this file doesn't need to know qjsbind internals of the scene
// module.

#include "js/ai_bindings.h"
#include "js/scene_bindings.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/agent_binding.h"
#include "util/log.h"

#include <brogameagent/brogameagent.h>
#include <qjsbind/qjsbind.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <cmath>

namespace bro::js {

// ───────────────────────────────────────────────────────────────────────────
// Forward-declared wrapper shapes (kept in sync with scene_bindings.cpp).
// qjsbind::unwrap<T> works purely on JS class id; the struct layout here
// just has to match.
// ───────────────────────────────────────────────────────────────────────────

struct NodeWrapper {
    scene::SceneNode* node;
    scene::SceneGraph* graph;
};

struct GraphWrapper {
    scene::SceneGraph* graph;
};

// ───────────────────────────────────────────────────────────────────────────
// JsCapability — wraps JS-authored {gate, start, advance} callbacks behind
// the brogameagent::Capability interface.
// ───────────────────────────────────────────────────────────────────────────

namespace {

struct JsCapSpec {
    int id;
    std::string name;
    JSValue gateFn    = JS_UNDEFINED;
    JSValue startFn   = JS_UNDEFINED;
    JSValue advanceFn = JS_UNDEFINED;
};

// Global registry: name → spec. Specs own the (dup'd) JSValue callbacks for
// the lifetime of the process; registered JS caps are effectively permanent.
struct JsCapRegistry {
    JSContext* ctx = nullptr;
    int nextId = brogameagent::kJsCapFirst;
    std::unordered_map<std::string, std::shared_ptr<JsCapSpec>> byName;
    std::unordered_map<int, std::shared_ptr<JsCapSpec>> byId;
};

static JsCapRegistry& registry() {
    static JsCapRegistry r;
    return r;
}

// --- self proxy construction --------------------------------------------

// Encode binding* as a Number for transport via JS property. 53-bit safe on
// 64-bit systems (ptrs here are 48-bit effectively).
static JSValue bindingHandleOf(JSContext* ctx, scene::AgentBinding* b) {
    return JS_NewInt64(ctx, reinterpret_cast<int64_t>(b));
}
static scene::AgentBinding* bindingFromHandle(JSContext* ctx, JSValueConst self) {
    JSValue h = JS_GetPropertyStr(ctx, self, "__bind");
    int64_t v = 0;
    JS_ToInt64(ctx, &v, h);
    JS_FreeValue(ctx, h);
    return reinterpret_cast<scene::AgentBinding*>(static_cast<intptr_t>(v));
}

// --- self method implementations ---------------------------------------

static JSValue self_moveTo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b) return JS_UNDEFINED;
    double x = 0, z = 0;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &z, argv[1]);
    auto& a = b->pending();
    a = brogameagent::Action{};
    a.capId = brogameagent::kCapMoveTo;
    a.fx = (float)x; a.fz = (float)z;
    return JS_UNDEFINED;
}

static JSValue self_laneWalk(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b) return JS_UNDEFINED;
    auto& a = b->pending();
    a = brogameagent::Action{};
    a.capId = brogameagent::kCapLaneWalk;
    return JS_UNDEFINED;
}

static JSValue self_attack(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b) return JS_UNDEFINED;
    int32_t tid = -1;
    JS_ToInt32(ctx, &tid, argv[0]);
    auto& a = b->pending();
    a = brogameagent::Action{};
    a.capId = brogameagent::kCapBasicAttack;
    a.i0 = tid;
    return JS_UNDEFINED;
}

static JSValue self_cast(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b) return JS_UNDEFINED;
    int32_t slot = -1, tid = -1;
    JS_ToInt32(ctx, &slot, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &tid, argv[1]);
    auto& a = b->pending();
    a = brogameagent::Action{};
    a.capId = brogameagent::kCapCastAbility;
    a.i0 = slot; a.i1 = tid;
    return JS_UNDEFINED;
}

static JSValue self_flee(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b) return JS_UNDEFINED;
    auto& a = b->pending();
    a = brogameagent::Action{};
    a.capId = brogameagent::kCapFlee;
    if (argc >= 2) {
        double x = 0, z = 0;
        JS_ToFloat64(ctx, &x, argv[0]);
        JS_ToFloat64(ctx, &z, argv[1]);
        a.fx = (float)x; a.fz = (float)z;
    }
    return JS_UNDEFINED;
}

static JSValue self_hold(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b) return JS_UNDEFINED;
    auto& a = b->pending();
    a = brogameagent::Action{};
    a.capId = brogameagent::kCapHold;
    if (argc >= 1) {
        double d = 0;
        JS_ToFloat64(ctx, &d, argv[0]);
        a.dur = (float)d;
    }
    return JS_UNDEFINED;
}

// Read-only getters on self.
static JSValue self_get_hp(JSContext* ctx, JSValueConst this_val) {
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b || !b->agent()) return JS_NewFloat64(ctx, 0);
    return JS_NewFloat64(ctx, b->agent()->unit().hp);
}
static JSValue self_get_mana(JSContext* ctx, JSValueConst this_val) {
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b || !b->agent()) return JS_NewFloat64(ctx, 0);
    return JS_NewFloat64(ctx, b->agent()->unit().mana);
}
static JSValue self_get_x(JSContext* ctx, JSValueConst this_val) {
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b || !b->agent()) return JS_NewFloat64(ctx, 0);
    return JS_NewFloat64(ctx, b->agent()->x());
}
static JSValue self_get_z(JSContext* ctx, JSValueConst this_val) {
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b || !b->agent()) return JS_NewFloat64(ctx, 0);
    return JS_NewFloat64(ctx, b->agent()->z());
}
static JSValue self_get_id(JSContext* ctx, JSValueConst this_val) {
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b || !b->agent()) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, b->agent()->unit().id);
}
static JSValue self_get_teamId(JSContext* ctx, JSValueConst this_val) {
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b || !b->agent()) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, b->agent()->unit().teamId);
}
static JSValue self_get_attackRange(JSContext* ctx, JSValueConst this_val) {
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b || !b->agent()) return JS_NewFloat64(ctx, 0);
    return JS_NewFloat64(ctx, b->agent()->unit().attackRange);
}
static JSValue self_get_alive(JSContext* ctx, JSValueConst this_val) {
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b || !b->agent()) return JS_NewBool(ctx, false);
    return JS_NewBool(ctx, b->agent()->unit().alive());
}

// Helper: self.distanceTo(other) — other may be {x,z}, an AI agent, or a self
// proxy. Returns 2D Euclidean distance.
static bool extractXZ(JSContext* ctx, JSValueConst val, float& x, float& z) {
    if (JS_IsObject(val)) {
        // Try AIAgent (created via bro.ai.game.createAgent).
        if (auto* ag = agentFromJS(ctx, val)) {
            x = ag->x(); z = ag->z(); return true;
        }
        JSValue vx = JS_GetPropertyStr(ctx, val, "x");
        JSValue vz = JS_GetPropertyStr(ctx, val, "z");
        double dx = 0, dz = 0;
        bool ok = JS_IsNumber(vx) && JS_IsNumber(vz);
        if (ok) { JS_ToFloat64(ctx, &dx, vx); JS_ToFloat64(ctx, &dz, vz); }
        JS_FreeValue(ctx, vx); JS_FreeValue(ctx, vz);
        if (ok) { x = (float)dx; z = (float)dz; return true; }
    }
    return false;
}

static JSValue self_distanceTo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewFloat64(ctx, 0);
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b || !b->agent()) return JS_NewFloat64(ctx, 0);
    float ox = 0, oz = 0;
    if (!extractXZ(ctx, argv[0], ox, oz)) return JS_NewFloat64(ctx, 0);
    float dx = b->agent()->x() - ox;
    float dz = b->agent()->z() - oz;
    return JS_NewFloat64(ctx, std::sqrt(dx*dx + dz*dz));
}

static JSValue self_inRange(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewBool(ctx, false);
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b || !b->agent()) return JS_NewBool(ctx, false);
    float ox = 0, oz = 0;
    if (!extractXZ(ctx, argv[0], ox, oz)) return JS_NewBool(ctx, false);
    float dx = b->agent()->x() - ox;
    float dz = b->agent()->z() - oz;
    float range = b->agent()->unit().attackRange;
    if (argc >= 2) { double r = 0; JS_ToFloat64(ctx, &r, argv[1]); range = (float)r; }
    return JS_NewBool(ctx, (dx*dx + dz*dz) <= range*range);
}

// Build a fresh per-tick self proxy. Exposes only methods whose capabilities
// are present on the binding's set. Always exposes the read-only getters and
// `agent` (the underlying AIAgent JSValue) for use with world queries.
static JSValue buildSelfProxy(JSContext* ctx, scene::AgentBinding* b,
                              JSValueConst agentRef) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "__bind", bindingHandleOf(ctx, b));
    JS_SetPropertyStr(ctx, obj, "agent", JS_DupValue(ctx, agentRef));

    auto addMethod = [&](const char* name, JSCFunction* fn, int nargs) {
        JS_SetPropertyStr(ctx, obj, name,
            JS_NewCFunction(ctx, fn, name, nargs));
    };
    auto addGetter = [&](const char* name, JSValue (*fn)(JSContext*, JSValueConst)) {
        // Simple approach: install a plain value (not a true JS getter) by
        // snapshotting the value now. Cheaper than a getter and matches the
        // read-only-per-tick contract.
        JS_SetPropertyStr(ctx, obj, name, fn(ctx, obj));
    };

    addGetter("hp",          self_get_hp);
    addGetter("mana",        self_get_mana);
    addGetter("x",           self_get_x);
    addGetter("z",           self_get_z);
    addGetter("id",          self_get_id);
    addGetter("teamId",      self_get_teamId);
    addGetter("attackRange", self_get_attackRange);
    addGetter("alive",       self_get_alive);

    // Universal helpers (don't require any cap).
    addMethod("distanceTo", self_distanceTo, 1);
    addMethod("inRange",    self_inRange,    2);
    addMethod("hold",       self_hold,       1); // hold is always allowed

    const auto& caps = b->capabilities();
    if (caps.has(brogameagent::kCapMoveTo))      addMethod("moveTo",   self_moveTo,   2);
    if (caps.has(brogameagent::kCapLaneWalk))    addMethod("laneWalk", self_laneWalk, 0);
    if (caps.has(brogameagent::kCapBasicAttack)) addMethod("attack",   self_attack,   1);
    if (caps.has(brogameagent::kCapCastAbility)) addMethod("cast",     self_cast,     2);
    if (caps.has(brogameagent::kCapFlee))        addMethod("flee",     self_flee,     2);

    return obj;
}

// ───────────────────────────────────────────────────────────────────────────
// JsThinkHook — scene::ThinkHook that defers to a JS function.
// ───────────────────────────────────────────────────────────────────────────

class JsThinkHook : public scene::ThinkHook {
public:
    JsThinkHook(JSContext* ctx, JSValue thinkFn, JSValue worldRef, JSValue agentRef)
        : ctx_(ctx),
          thinkFn_(JS_DupValue(ctx, thinkFn)),
          worldRef_(JS_DupValue(ctx, worldRef)),
          agentRef_(JS_DupValue(ctx, agentRef)) {}
    ~JsThinkHook() override {
        JS_FreeValue(ctx_, thinkFn_);
        JS_FreeValue(ctx_, worldRef_);
        JS_FreeValue(ctx_, agentRef_);
    }

    void think(const brogameagent::CapContext&,
               const brogameagent::CapabilitySet&,
               brogameagent::Action&) override {
        if (!binding_) return;
        JSValue self = buildSelfProxy(ctx_, binding_, agentRef_);
        JSValue args[2] = { self, JS_DupValue(ctx_, worldRef_) };
        JSValue ret = JS_Call(ctx_, thinkFn_, JS_UNDEFINED, 2, args);
        if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(ctx_);
            const char* msg = JS_ToCString(ctx_, exc);
            LOG_WARN("[agent think] exception: %s", msg ? msg : "?");
            if (msg) JS_FreeCString(ctx_, msg);
            JS_FreeValue(ctx_, exc);
        }
        JS_FreeValue(ctx_, ret);
        JS_FreeValue(ctx_, args[0]);
        JS_FreeValue(ctx_, args[1]);
    }

    void setBinding(scene::AgentBinding* b) { binding_ = b; }

private:
    JSContext* ctx_;
    JSValue thinkFn_;
    JSValue worldRef_;
    JSValue agentRef_;
    scene::AgentBinding* binding_ = nullptr;
};

// ───────────────────────────────────────────────────────────────────────────
// JsCapability — brogameagent::Capability delegating to JS callbacks.
// ───────────────────────────────────────────────────────────────────────────

class JsCapability : public brogameagent::Capability {
public:
    JsCapability(std::shared_ptr<JsCapSpec> spec, JSContext* ctx)
        : spec_(std::move(spec)), ctx_(ctx) {}
    int id() const override { return spec_->id; }
    const char* name() const override { return spec_->name.c_str(); }

    bool gate(const brogameagent::CapContext&) const override {
        if (JS_IsUndefined(spec_->gateFn)) return true;
        JSValue ret = JS_Call(ctx_, spec_->gateFn, JS_UNDEFINED, 0, nullptr);
        bool ok = JS_IsException(ret) ? false : JS_ToBool(ctx_, ret);
        JS_FreeValue(ctx_, ret);
        return ok;
    }
    void start(const brogameagent::CapContext&, brogameagent::Action& a) override {
        if (!JS_IsUndefined(spec_->startFn)) {
            JSValue ret = JS_Call(ctx_, spec_->startFn, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx_, ret);
        }
        a.elapsed = 0;
        a.done = (a.dur <= 0.0f);
    }
    void advance(const brogameagent::CapContext&, brogameagent::Action& a, float dt) override {
        a.elapsed += dt;
        if (!JS_IsUndefined(spec_->advanceFn)) {
            JSValue ret = JS_Call(ctx_, spec_->advanceFn, JS_UNDEFINED, 0, nullptr);
            if (JS_IsBool(ret)) a.done = JS_ToBool(ctx_, ret);
            JS_FreeValue(ctx_, ret);
        } else if (a.elapsed >= a.dur) {
            a.done = true;
        }
    }

private:
    std::shared_ptr<JsCapSpec> spec_;
    JSContext* ctx_;
};

// ───────────────────────────────────────────────────────────────────────────
// JS-visible: bro.ai.game.registerCapability(name, spec)
// ───────────────────────────────────────────────────────────────────────────

static JSValue js_registerCapability(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "registerCapability(name, spec)");
    const char* cname = JS_ToCString(ctx, argv[0]);
    std::string name = cname ? cname : "";
    if (cname) JS_FreeCString(ctx, cname);
    if (name.empty()) return JS_ThrowTypeError(ctx, "capability name required");

    auto& reg = registry();
    reg.ctx = ctx;
    auto spec = std::make_shared<JsCapSpec>();
    spec->name = name;
    // Explicit id wins; else allocate from the JS-capability range.
    JSValue idVal = JS_GetPropertyStr(ctx, argv[1], "id");
    if (JS_IsNumber(idVal)) {
        int32_t v = 0; JS_ToInt32(ctx, &v, idVal);
        spec->id = v;
    } else {
        spec->id = reg.nextId++;
    }
    JS_FreeValue(ctx, idVal);

    auto dup = [&](const char* k) -> JSValue {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], k);
        if (JS_IsFunction(ctx, v)) return v; // already holds a ref from GetProperty
        JS_FreeValue(ctx, v);
        return JS_UNDEFINED;
    };
    spec->gateFn    = dup("gate");
    spec->startFn   = dup("start");
    spec->advanceFn = dup("advance");

    reg.byName[name] = spec;
    reg.byId[spec->id] = spec;
    return JS_NewInt32(ctx, spec->id);
}

// ───────────────────────────────────────────────────────────────────────────
// Helpers: parse capability list from JS opts → add to CapabilitySet.
// ───────────────────────────────────────────────────────────────────────────

static void addBuiltinByName(brogameagent::CapabilitySet& set, const std::string& name) {
    using namespace brogameagent;
    if (name == "move_to")      set.add(makeMoveToCapability());
    else if (name == "lane_walk")    set.add(makeLaneWalkCapability());
    else if (name == "basic_attack") set.add(makeBasicAttackCapability());
    else if (name == "cast_ability") set.add(makeCastAbilityCapability());
    else if (name == "flee")         set.add(makeFleeCapability());
    else if (name == "hold")         set.add(makeHoldCapability());
}

static void parseCapabilitiesList(JSContext* ctx, JSValueConst arr,
                                  brogameagent::CapabilitySet& set) {
    if (!JS_IsArray(arr)) return;
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
    for (int32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsString(v)) {
            const char* s = JS_ToCString(ctx, v);
            std::string name = s ? s : "";
            if (s) JS_FreeCString(ctx, s);
            addBuiltinByName(set, name);
            // Fall back to registered JS caps.
            auto it = registry().byName.find(name);
            if (it != registry().byName.end()) {
                set.add(std::make_unique<JsCapability>(it->second, ctx));
            }
        }
        JS_FreeValue(ctx, v);
    }
}

static void parseLaneWaypoints(JSContext* ctx, JSValueConst arr,
                               brogameagent::CapabilitySet& set) {
    if (!JS_IsArray(arr)) return;
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
    std::vector<bromath::Vec2> wps;
    wps.reserve(len);
    for (int32_t i = 0; i < len; i++) {
        JSValue pt = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsObject(pt)) {
            double x = 0, z = 0;
            JSValue vx = JS_GetPropertyStr(ctx, pt, "x");
            JSValue vz = JS_GetPropertyStr(ctx, pt, "z");
            if (JS_IsNumber(vx)) JS_ToFloat64(ctx, &x, vx);
            if (JS_IsNumber(vz)) JS_ToFloat64(ctx, &z, vz);
            JS_FreeValue(ctx, vx); JS_FreeValue(ctx, vz);
            wps.push_back({(float)x, (float)z});
        }
        JS_FreeValue(ctx, pt);
    }
    set.setLaneWaypoints(std::move(wps));
}

// ───────────────────────────────────────────────────────────────────────────
// JS-visible: graph.attachAIWorld(world, opts?)
// ───────────────────────────────────────────────────────────────────────────

JSValue js_sg_attachAIWorld(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<GraphWrapper>(ctx, this_val);
    if (!w || !w->graph || argc < 1) return JS_UNDEFINED;
    auto* world = worldFromJS(ctx, argv[0]);
    if (!world) return JS_ThrowTypeError(ctx, "attachAIWorld: expected a bro.ai.game.createWorld()");
    float stepHz = 60.0f;
    int   maxSteps = 8;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "stepHz");
        if (JS_IsNumber(v)) { double d = 0; JS_ToFloat64(ctx, &d, v); stepHz = (float)d; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "maxStepsPerFrame");
        if (JS_IsNumber(v)) { int32_t d = 0; JS_ToInt32(ctx, &d, v); maxSteps = d; }
        JS_FreeValue(ctx, v);
    }
    w->graph->attachAIWorld(world, stepHz, maxSteps);
    // Hold the world JSValue alive on the GraphWrapper for the think loop
    // (so __agents queries and the think's world param keep pointing at
    // something GC-safe).
    JS_SetPropertyStr(ctx, this_val, "__aiWorld", JS_DupValue(ctx, argv[0]));
    return JS_UNDEFINED;
}

JSValue js_sg_detachAIWorld(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<GraphWrapper>(ctx, this_val);
    if (!w || !w->graph) return JS_UNDEFINED;
    w->graph->detachAIWorld();
    JS_SetPropertyStr(ctx, this_val, "__aiWorld", JS_UNDEFINED);
    return JS_UNDEFINED;
}

// ───────────────────────────────────────────────────────────────────────────
// JS-visible: node.attachAgent(world, agent, opts)
//
// The `world` arg is required so the think() callback's `world` parameter has
// a stable, GC-safe JSValue to pass through each tick without traversing up
// the scene graph.
//
// opts = {
//   capabilities: ["lane_walk","basic_attack"],   // default: all built-ins
//   think(self, world) {...},                      // primary JS path
//   thinkHz: 15,                                   // default 15
//   yOffset: 1.0,                                  // default 0
//   faceMovement: true,                            // default true
//   laneWaypoints: [{x,z}, ...],                   // optional
//   policy: "scripted_minion",                     // alternative to think
// }
// ───────────────────────────────────────────────────────────────────────────

static JSValue js_node_attachAgent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* nw = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!nw || !nw->node || !nw->graph || argc < 2)
        return JS_ThrowTypeError(ctx, "node.attachAgent(world, agent, opts)");
    auto* world = worldFromJS(ctx, argv[0]);
    if (!world)
        return JS_ThrowTypeError(ctx, "attachAgent: first arg must be a bro.ai.game.createWorld()");
    auto* agent = agentFromJS(ctx, argv[1]);
    if (!agent)
        return JS_ThrowTypeError(ctx, "attachAgent: second arg must be a bro.ai.game.createAgent()");

    // Ensure or reuse a binding on this node.
    auto* binding = nw->graph->attachAgentBinding(nw->node);
    if (!binding) return JS_ThrowInternalError(ctx, "attachAgent: failed to create binding");
    binding->setAgent(agent);

    // Default configuration.
    binding->setThinkHz(15.0f);
    binding->setYOffset(0.0f);
    binding->setFaceMovement(true);

    // Default capability set: all six built-ins (trimmed to "basic_attack +
    // hold" if caller passes an explicit list).
    binding->capabilities() = brogameagent::CapabilitySet{};
    bool explicitList = false;

    JSValueConst opts = (argc >= 3 && JS_IsObject(argv[2])) ? argv[2] : JS_UNDEFINED;
    if (!JS_IsUndefined(opts)) {
        JSValue v;

        v = JS_GetPropertyStr(ctx, opts, "capabilities");
        if (JS_IsArray(v)) {
            explicitList = true;
            parseCapabilitiesList(ctx, v, binding->capabilities());
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "thinkHz");
        if (JS_IsNumber(v)) { double d = 15; JS_ToFloat64(ctx, &d, v); binding->setThinkHz((float)d); }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "yOffset");
        if (JS_IsNumber(v)) { double d = 0; JS_ToFloat64(ctx, &d, v); binding->setYOffset((float)d); }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "faceMovement");
        if (JS_IsBool(v)) binding->setFaceMovement(JS_ToBool(ctx, v));
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "laneWaypoints");
        if (JS_IsArray(v)) parseLaneWaypoints(ctx, v, binding->capabilities());
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "policy");
        if (JS_IsString(v)) {
            const char* s = JS_ToCString(ctx, v);
            std::string name = s ? s : "";
            if (s) JS_FreeCString(ctx, s);
            if (name == "scripted_minion") {
                binding->setPolicy(brogameagent::makeScriptedMinionPolicy());
            }
        }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, opts, "think");
        if (JS_IsFunction(ctx, v)) {
            // argv[0] is the world JS wrapper; argv[1] is the agent. The
            // JsThinkHook dups them internally.
            auto hook = std::make_unique<JsThinkHook>(ctx, v, argv[0], argv[1]);
            hook->setBinding(binding);
            binding->setThinkHook(std::move(hook));
        }
        JS_FreeValue(ctx, v);
    }

    // If no explicit list, give the binding all built-ins. Towers/minions can
    // trim via opts.capabilities.
    if (!explicitList) {
        addAllBuiltinCapabilities(binding->capabilities());
    }

    // Hold a JSValue dup to the agent and world on the node wrapper so GC
    // can't free the AgentData/WorldData while the binding references them.
    JS_SetPropertyStr(ctx, this_val, "__agent", JS_DupValue(ctx, argv[1]));
    JS_SetPropertyStr(ctx, this_val, "__aiWorld", JS_DupValue(ctx, argv[0]));

    return JS_DupValue(ctx, this_val);
}

static JSValue js_node_detachAgent(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* nw = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!nw || !nw->node || !nw->graph) return JS_UNDEFINED;
    nw->graph->detachAgentBinding(nw->node);
    JS_SetPropertyStr(ctx, this_val, "__agent", JS_UNDEFINED);
    return JS_UNDEFINED;
}

} // namespace

// ───────────────────────────────────────────────────────────────────────────
// Public entry points — called from ai_bindings.cpp / scene_bindings.cpp.
// ───────────────────────────────────────────────────────────────────────────

JSValue nodeAttachAgent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return js_node_attachAgent(ctx, this_val, argc, argv);
}
JSValue nodeDetachAgent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return js_node_detachAgent(ctx, this_val, argc, argv);
}
JSValue graphAttachAIWorld(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return js_sg_attachAIWorld(ctx, this_val, argc, argv);
}
JSValue graphDetachAIWorld(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return js_sg_detachAIWorld(ctx, this_val, argc, argv);
}

void installRegisterCapability(JSContext* ctx, JSValue gameObj) {
    JS_SetPropertyStr(ctx, gameObj, "registerCapability",
        JS_NewCFunction(ctx, js_registerCapability, "registerCapability", 2));
}

} // namespace bro::js
