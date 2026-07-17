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
// NodeWrapper / GraphWrapper come from scene_bindings_internal.h (shared with
// the scene_bindings*.cpp units) so the wrapper layout and liveness scheme
// can never drift between the two modules.

#include "js/ai_bindings.h"
#if BRO_WITH_GAMEAI  // modular-build feature gate
#include "js/scene_bindings.h"
#include "js/scene_bindings_internal.h"
#include "js/terrain_bindings.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/agent_binding.h"
#include "util/log.h"

#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif

#include <brogameagent/brogameagent.h>
#ifdef BROGAMEAGENT_HAS_NAVMESH
#include <brogameagent/nav_mesh.h>
#endif
#include <qjsbind/qjsbind.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <cmath>

namespace bro::js {

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

// Global registry: name → spec. Specs own the (dup'd) JSValue callbacks until
// clearRegisteredCapabilities() frees them at engine teardown (AIBindings::
// cleanup) — they are native-held refs the GC cannot see, so leaving them
// registered would trip JS_FreeRuntime's leaked-object assert.
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

// Generic invocation for JS-registered capabilities (registerCapability).
// Unlike the 5 built-ins above, custom capabilities have no per-name
// accessor — nothing else sets AgentBinding::pending().capId to one of
// them, so a registered {gate,start,advance} spec was previously
// unreachable from any thinkHook_-driven agent (confirmed: gate/start
// never fire even when attached and gate() would return true). Looks the
// capability up by the same name passed to registerCapability.
static JSValue self_useCapability(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* b = bindingFromHandle(ctx, this_val);
    if (!b) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, argv[0]);
    std::string name = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    auto it = registry().byName.find(name);
    if (it == registry().byName.end()) return JS_UNDEFINED;
    auto& a = b->pending();
    a = brogameagent::Action{};
    a.capId = it->second->id;
    if (argc >= 2) { int32_t v = -1; JS_ToInt32(ctx, &v, argv[1]); a.i0 = v; }
    if (argc >= 3) { int32_t v = -1; JS_ToInt32(ctx, &v, argv[2]); a.i1 = v; }
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

    for (const auto& kv : registry().byId) {
        if (caps.has(kv.first)) { addMethod("useCapability", self_useCapability, 1); break; }
    }

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
    if (!w || !w->graph() || argc < 1) return JS_UNDEFINED;
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
    // The graph holds the world's JS wrapper alive for the duration of the
    // attachment (SceneGraph::aiWorldPin_) — a keep-alive on the graph
    // itself, not on this GraphWrapper JS object, so the raw World* can't
    // dangle even if the app drops every JS reference to the world (and the
    // graph wrapper). Released on detachAIWorld / graph teardown, both of
    // which run before JS runtime teardown.
    w->graph()->attachAIWorld(world, stepHz, maxSteps,
        std::make_shared<JSFnRef>(ctx, JS_DupValue(ctx, argv[0])));
    return JS_UNDEFINED;
}

JSValue js_sg_detachAIWorld(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* w = qjsbind::unwrap<GraphWrapper>(ctx, this_val);
    if (!w || !w->graph()) return JS_UNDEFINED;
    w->graph()->detachAIWorld();
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
    if (!nw || !nw->node() || !nw->graph() || argc < 2)
        return JS_ThrowTypeError(ctx, "node.attachAgent(world, agent, opts)");
    auto* world = worldFromJS(ctx, argv[0]);
    if (!world)
        return JS_ThrowTypeError(ctx, "attachAgent: first arg must be a bro.ai.game.createWorld()");
    auto* agent = agentFromJS(ctx, argv[1]);
    if (!agent)
        return JS_ThrowTypeError(ctx, "attachAgent: second arg must be a bro.ai.game.createAgent()");

    // Ensure or reuse a binding on this node.
    auto* binding = nw->graph()->attachAgentBinding(nw->node());
    if (!binding) return JS_ThrowInternalError(ctx, "attachAgent: failed to create binding");
    binding->setAgent(agent);

    // Default configuration.
    binding->setThinkHz(15.0f);
    binding->setYOffset(0.0f);
    binding->setFaceMovement(true);
    binding->setGroundFollow({});  // re-attach resets any previous probe
    binding->stopNavigation();     // ... and any in-flight navmesh route
    binding->setNavMesh(nullptr);
    binding->clearKeepAlives();    // ... and any previous JS-wrapper pins

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

        // avoidance: true|false|{radius?, maxSpeed?, neighborDist?,
        // maxNeighbors?, timeHorizon?, timeHorizonObst?, enabled?} — the
        // agent's ORCA participation, effective while the AI world's
        // avoidance pass is on (world.setAvoidance(true)).
        v = JS_GetPropertyStr(ctx, opts, "avoidance");
        if (!JS_IsUndefined(v) && !JS_IsNull(v))
            applyAgentAvoidanceOpts(ctx, v, *agent);
        JS_FreeValue(ctx, v);

        // navMesh: a bro.ai.game.bakeNavMesh()/loadNavMesh() handle — enables
        // node.navigateTo() route-following on this binding. The binding takes
        // SHARED ownership of the mesh, so it stays valid even if the app
        // drops every JS reference to it.
        v = JS_GetPropertyStr(ctx, opts, "navMesh");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            auto nm = navMeshSharedFromJS(ctx, v);
            if (!nm) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx,
                    "attachAgent: navMesh must be a bro.ai.game.bakeNavMesh()/loadNavMesh() object");
            }
            binding->setNavMesh(std::move(nm));
        }
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

        // groundFollow: { mode: 'terrain'|'raycast', ... } — the node's Y
        // tracks the ground under (x, z) and yOffset becomes a clearance
        // above it. Installed as a native probe; runs once per frame in
        // AgentBinding::syncToNode on the main thread.
        v = JS_GetPropertyStr(ctx, opts, "groundFollow");
        if (JS_IsObject(v)) {
            std::string mode = qjsbind::get_prop_string(ctx, v, "mode");
            float rayStart  = (float)qjsbind::get_prop_number(ctx, v, "rayStart", 100.0);
            float rayLength = (float)qjsbind::get_prop_number(ctx, v, "rayLength", 200.0);

            if (mode == "terrain") {
                JSValue tv = JS_GetPropertyStr(ctx, v, "terrain");
                void* th = terrainHandleFromJS(ctx, tv);
                if (!th) {
                    JS_FreeValue(ctx, tv);
                    JS_FreeValue(ctx, v);
                    return JS_ThrowTypeError(ctx,
                        "attachAgent: groundFollow mode 'terrain' requires a scene.createTerrain() object");
                }
                // Pin the terrain JSValue on the binding itself so the probed
                // TerrainManager outlives the probe even if the app drops its
                // own reference (terrainSampleHeight also verifies liveness
                // per call as a second line of defence).
                binding->addKeepAlive(
                    std::make_shared<JSFnRef>(ctx, JS_DupValue(ctx, tv)));
                JS_FreeValue(ctx, tv);
                binding->setGroundFollow(
                    [th, rayStart, rayLength](float x, float z, float& outY) {
                        return terrainSampleHeight(th, x, z, rayStart, rayLength, outY);
                    });
            } else if (mode == "raycast") {
#if BRO_WITH_PHYSICS
                auto* pw = nw->graph()->physicsWorld();
                if (!pw) {
                    JS_FreeValue(ctx, v);
                    return JS_ThrowTypeError(ctx,
                        "attachAgent: groundFollow mode 'raycast' requires an active physics world");
                }
                physics::QueryFilter filter;
                JSValue lv = JS_GetPropertyStr(ctx, v, "layers");
                if (JS_IsArray(lv)) {
                    uint32_t mask = 0;
                    JSValue lenV = JS_GetPropertyStr(ctx, lv, "length");
                    uint32_t n = 0; JS_ToUint32(ctx, &n, lenV); JS_FreeValue(ctx, lenV);
                    for (uint32_t i = 0; i < n; i++) {
                        JSValue el = JS_GetPropertyUint32(ctx, lv, i);
                        int32_t idx = -1;
                        if (JS_IsString(el)) {
                            const char* s = JS_ToCString(ctx, el);
                            if (s) { idx = pw->layerIndex(s); JS_FreeCString(ctx, s); }
                        } else if (JS_IsNumber(el)) {
                            JS_ToInt32(ctx, &idx, el);
                        }
                        if (idx >= 0 && idx < 32) mask |= 1u << idx;
                        JS_FreeValue(ctx, el);
                    }
                    filter.layerMask = mask;
                }
                JS_FreeValue(ctx, lv);

                // The graph owns the binding, so capturing it raw is safe; the
                // world pointer is re-read per probe (detachable). The probe
                // only runs while the physics phase is idle — syncAgents ticks
                // after consumeStep on the main thread, but a step that
                // overran a frame keeps ownership on the physics thread, in
                // which case we skip and keep the last known ground height.
                scene::SceneGraph* graph = nw->graph();
                binding->setGroundFollow(
                    [graph, filter, rayStart, rayLength](float x, float z, float& outY) {
                        auto* world = graph->physicsWorld();
                        if (!world || !world->isIdle()) return false;
                        physics::RayHit hit;
                        if (!world->raycastClosest(JPH::RVec3(x, rayStart, z),
                                                   JPH::Vec3(0, -1, 0),
                                                   hit, rayLength, filter))
                            return false;
                        outY = hit.position.GetY();
                        return true;
                    });
#else
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx,
                    "attachAgent: groundFollow mode 'raycast' requires a physics-enabled build");
#endif
            } else {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx,
                    "attachAgent: groundFollow.mode must be 'terrain' or 'raycast'");
            }
        }
        JS_FreeValue(ctx, v);
    }

    // If no explicit list, give the binding all built-ins. Towers/minions can
    // trim via opts.capabilities.
    if (!explicitList) {
        addAllBuiltinCapabilities(binding->capabilities());
    }

    // Pin the agent and world JS wrappers on the binding so GC can't free
    // the AgentData/WorldData while the binding references them raw. Pinned
    // on the BINDING (not this node wrapper): wrapNode mints a fresh wrapper
    // per call, so a wrapper-held pin dies with whichever transient wrapper
    // attach happened to be called on. Released with the binding (detach,
    // node/subtree destroy, graph teardown).
    binding->addKeepAlive(std::make_shared<JSFnRef>(ctx, JS_DupValue(ctx, argv[1])));
    binding->addKeepAlive(std::make_shared<JSFnRef>(ctx, JS_DupValue(ctx, argv[0])));

    return JS_DupValue(ctx, this_val);
}

static JSValue js_node_detachAgent(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* nw = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!nw || !nw->node() || !nw->graph()) return JS_UNDEFINED;
    // Destroys the AgentBinding, which releases its JS-wrapper keep-alives.
    nw->graph()->detachAgentBinding(nw->node());
    return JS_UNDEFINED;
}

// ───────────────────────────────────────────────────────────────────────────
// JS-visible: node.navigateTo(target, opts?) / node.stopNavigation()
//
// Navmesh route-following on an attached agent: plans NavMesh::findPath and
// feeds successive XZ waypoints into the agent's existing setTarget steering,
// so the AI world's ORCA avoidance pass composes unchanged. Waypoint Y drives
// the node's height when no groundFollow probe is set (groundFollow wins).
//
// target = {x,y,z} or [x,y,z]; opts = {
//   navMesh,             // bakeNavMesh()/loadNavMesh() handle; optional if
//                        // one was already set via attachAgent({navMesh})
//   extents: {x,y,z},    // findPath snap half-extents (default {2,1,2})
//   repathInterval: 0,   // seconds; > 0 re-plans toward the target
// }
// Returns true when a complete path was found and following started; false
// when either endpoint fails to snap or the goal is unreachable.
// ───────────────────────────────────────────────────────────────────────────

static JSValue js_node_navigateTo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
#ifdef BROGAMEAGENT_HAS_NAVMESH
    auto* nw = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!nw || !nw->node() || !nw->graph() || argc < 1)
        return JS_ThrowTypeError(ctx, "node.navigateTo(target, opts?)");
    auto* binding = nw->graph()->agentBinding(nw->node());
    if (!binding || !binding->agent())
        return JS_ThrowTypeError(ctx, "navigateTo: no agent attached (call node.attachAgent first)");

    bromath::Vec3 target;
    if (!JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "navigateTo: target must be {x,y,z} or [x,y,z]");
    {
        // Accept both object and array forms (same shape as the NavMesh
        // query methods).
        JSValue vx = JS_GetPropertyStr(ctx, argv[0], "x");
        bool isObjForm = JS_IsNumber(vx);
        JS_FreeValue(ctx, vx);
        if (isObjForm) {
            target.x = (float)qjsbind::get_prop_number(ctx, argv[0], "x", 0);
            target.y = (float)qjsbind::get_prop_number(ctx, argv[0], "y", 0);
            target.z = (float)qjsbind::get_prop_number(ctx, argv[0], "z", 0);
        } else {
            double c[3] = {0, 0, 0};
            for (uint32_t i = 0; i < 3; i++) {
                JSValue v = JS_GetPropertyUint32(ctx, argv[0], i);
                if (JS_IsNumber(v)) JS_ToFloat64(ctx, &c[i], v);
                JS_FreeValue(ctx, v);
            }
            target = {(float)c[0], (float)c[1], (float)c[2]};
        }
    }

    bromath::Vec3 extents = brogameagent::NavMesh::kDefaultExtents;
    float repathInterval = 0.0f;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue mv = JS_GetPropertyStr(ctx, argv[1], "navMesh");
        if (!JS_IsUndefined(mv) && !JS_IsNull(mv)) {
            auto nm = navMeshSharedFromJS(ctx, mv);
            if (!nm) {
                JS_FreeValue(ctx, mv);
                return JS_ThrowTypeError(ctx,
                    "navigateTo: navMesh must be a bro.ai.game.bakeNavMesh()/loadNavMesh() object");
            }
            binding->setNavMesh(std::move(nm));
        }
        JS_FreeValue(ctx, mv);

        JSValue ev = JS_GetPropertyStr(ctx, argv[1], "extents");
        if (JS_IsObject(ev)) {
            extents.x = (float)qjsbind::get_prop_number(ctx, ev, "x", extents.x);
            extents.y = (float)qjsbind::get_prop_number(ctx, ev, "y", extents.y);
            extents.z = (float)qjsbind::get_prop_number(ctx, ev, "z", extents.z);
        }
        JS_FreeValue(ctx, ev);

        repathInterval = (float)qjsbind::get_prop_number(ctx, argv[1], "repathInterval", 0.0);
    }

    if (!binding->navMesh())
        return JS_ThrowTypeError(ctx,
            "navigateTo: no navMesh bound (pass one in attachAgent or navigateTo opts)");

    return JS_NewBool(ctx, binding->navigateTo(target, extents, repathInterval));
#else
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx,
        "node.navigateTo is unavailable: this build was compiled without "
        "BROGAMEAGENT_WITH_NAVMESH");
#endif
}

static JSValue js_node_stopNavigation(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* nw = qjsbind::unwrap<NodeWrapper>(ctx, this_val);
    if (!nw || !nw->node() || !nw->graph()) return JS_UNDEFINED;
    if (auto* binding = nw->graph()->agentBinding(nw->node())) binding->stopNavigation();
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
JSValue nodeNavigateTo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return js_node_navigateTo(ctx, this_val, argc, argv);
}
JSValue nodeStopNavigation(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return js_node_stopNavigation(ctx, this_val, argc, argv);
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

void clearRegisteredCapabilities(JSContext* ctx) {
    auto& reg = registry();
    if (reg.ctx != ctx) return;  // registered against a different context (or never)
    for (auto& [id, spec] : reg.byId) {
        // Specs may outlive the registry via the shared_ptr each JsCapability
        // holds, so null the values rather than leaving freed JSValues behind.
        JS_FreeValue(ctx, spec->gateFn);    spec->gateFn    = JS_UNDEFINED;
        JS_FreeValue(ctx, spec->startFn);   spec->startFn   = JS_UNDEFINED;
        JS_FreeValue(ctx, spec->advanceFn); spec->advanceFn = JS_UNDEFINED;
    }
    reg.byName.clear();
    reg.byId.clear();
    reg.ctx = nullptr;
}

} // namespace bro::js

#endif  // BRO_WITH_GAMEAI
