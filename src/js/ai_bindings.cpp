#include "js/ai_bindings.h"
#if BRO_WITH_GAMEAI  // modular-build feature gate
#include "util/log.h"

#if BRO_WITH_PHYSICS
#include "js/physics_bindings.h"
#include "physics/physics_world.h"
#endif

#if BRO_WITH_3D
#include "js/terrain_bindings.h"
#endif

#include <qjsbind/qjsbind.h>
#include <brogameagent/brogameagent.h>
#ifdef BROGAMEAGENT_HAS_NAVMESH
#include <brogameagent/nav_mesh.h>
#endif

#include <cfloat>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstring>

namespace bro::js {

// ═══════════════════════════════════════════════════════════════════════════
// Wrapper structs
// ═══════════════════════════════════════════════════════════════════════════

struct NavGridData {
    std::unique_ptr<brogameagent::NavGrid> grid;
};

#ifdef BROGAMEAGENT_HAS_NAVMESH
// Owns only C++ state (no JSValue refs), so no gc_mark is needed. The mesh is
// held by shared_ptr so C++-side consumers (AgentBinding::setNavMesh) can
// share ownership via navMeshSharedFromJS — the mesh then outlives the JS
// wrapper even if the app drops every JS reference while an agent is still
// routing on it.
struct NavMeshData {
    std::shared_ptr<brogameagent::NavMesh> mesh;
};
#endif

struct UnitData {
    brogameagent::Agent* agentRef;  // non-owning, kept alive by AgentData via JS ref
};

struct AgentData {
    brogameagent::Agent agent;
    // Persistent per-agent unit proxy. Must NOT be a shared static — JS code
    // frequently walks multiple agents in one expression, and a shared proxy
    // would have its agentRef clobbered by the most recent .unit access.
    std::unique_ptr<UnitData> unitProxy;
};

struct WorldData {
    brogameagent::World world;
};

struct RewardTrackerData {
    brogameagent::RewardTracker tracker;
};

struct SimulationData {
    std::unique_ptr<brogameagent::Simulation> sim;
};

struct RecorderData {
    brogameagent::Recorder recorder;
};

struct ReplayReaderData {
    brogameagent::ReplayReader reader;
};

// Mixin so wrappers that hold DupValue'd JSValue callbacks expose them to
// QuickJS's cycle GC. Without this, a JS closure captured as e.g. an Option
// step() that transitively references the wrapper handle forms a cycle the
// GC can't see — JS_FreeRuntime then asserts on shutdown. Each wrapper
// struct holds a std::shared_ptr<JsCallbackHolder> vector populated at
// create-time, then walks it in its registered .gc_mark() trampoline.
struct JsCallbackHolder {
    virtual ~JsCallbackHolder() = default;
    virtual void gc_mark(JSRuntime* rt, JS_MarkFunc* mark) const = 0;
};

template<class T>
static std::shared_ptr<T> track_jcb(std::vector<std::shared_ptr<JsCallbackHolder>>& v,
                                    std::shared_ptr<T> sp) {
    if (sp) {
        if (auto j = std::dynamic_pointer_cast<JsCallbackHolder>(sp)) {
            v.push_back(std::move(j));
        }
    }
    return sp;
}

// gc_mark trampoline for any wrapper struct that has a `jcb` vector.
template<class T>
static void mark_jcb(T* d, JSRuntime* rt, JS_MarkFunc* mark) {
    if (!d) return;
    for (const auto& sp : d->jcb) if (sp) sp->gc_mark(rt, mark);
}

struct MctsData {
    brogameagent::mcts::Mcts mcts;
    std::vector<std::shared_ptr<JsCallbackHolder>> jcb;
};

struct DecoupledMctsData {
    brogameagent::mcts::DecoupledMcts mcts;
    std::vector<std::shared_ptr<JsCallbackHolder>> jcb;
};

struct TeamMctsData {
    brogameagent::mcts::TeamMcts mcts;
    std::vector<std::shared_ptr<JsCallbackHolder>> jcb;
};

struct TacticMctsData {
    brogameagent::mcts::TacticMcts mcts;
    std::vector<std::shared_ptr<JsCallbackHolder>> jcb;
};

struct LayeredPlannerData {
    brogameagent::mcts::LayeredPlanner planner;
    std::vector<std::shared_ptr<JsCallbackHolder>> jcb;
};

struct OptionData {
    std::shared_ptr<brogameagent::mcts::Option> option;
};

struct TeamOptionData {
    std::shared_ptr<brogameagent::mcts::TeamOption> option;
};

// optionJsRefs holds JS_DupValue'd copies of the *original* AIOption/
// AITeamOption wrapper values passed into `options: [...]`, marked directly
// in gc_mark (see below). This is deliberately NOT done by re-walking into
// the shared JsOption/JsTeamOption's own gc_mark (as this file used to via
// track_jcb) — a JS-authored option's canInitiate/step/shouldTerminate
// JSValues are JS_DupValue'd exactly once, in JsOption's constructor, but
// the underlying std::shared_ptr<Option> is legitimately reachable from
// MULTIPLE independent JS-visible objects at once (the original `opt`
// returned by createOption(), *and* every OptionMcts/Commander it's passed
// into). Having each of those independently call into JsOption::gc_mark()
// during QuickJS's cycle-GC decref pass double(or N-)counts a reference
// that was only ever dup'd once, underflowing ref_count and tripping
// "Assertion failed: p->ref_count > 0" the moment GC runs (reproducible
// from ai-arena: create an Option, pass it into createOptionMcts while
// *also* keeping the original handle alive, then advanceTime()). Marking
// the retained wrapper JSValue itself sidesteps this: QuickJS's own
// ref-counting on that JSValue already correctly reflects "N live owners",
// so each owner's mark call is one real edge, not an overcount.
struct OptionMctsData {
    brogameagent::mcts::OptionMcts mcts;
    // Retain shared_ptrs so JS-authored options live as long as the engine
    // even if the JS wrapper is collected. set_options() on the C++ engine
    // stores the same shared_ptrs but this is a defence-in-depth anchor.
    std::vector<std::shared_ptr<brogameagent::mcts::Option>> options;
    std::vector<JSValue> optionJsRefs;
    std::vector<std::shared_ptr<JsCallbackHolder>> jcb;
    JSContext* ctx = nullptr;
    ~OptionMctsData() { for (auto& v : optionJsRefs) JS_FreeValue(ctx, v); }
};

struct TeamOptionMctsData {
    brogameagent::mcts::TeamOptionMcts mcts;
    std::vector<std::shared_ptr<brogameagent::mcts::TeamOption>> options;
    std::vector<JSValue> optionJsRefs;
    std::vector<std::shared_ptr<JsCallbackHolder>> jcb;
    JSContext* ctx = nullptr;
    ~TeamOptionMctsData() { for (auto& v : optionJsRefs) JS_FreeValue(ctx, v); }
};

struct CommanderData {
    brogameagent::mcts::Commander commander;
    // Keep per-role shared_ptrs alive independent of the C++ Commander's
    // internal role list — defence-in-depth against option lifetime bugs
    // when JS-authored options hold JSValue refs.
    std::vector<std::shared_ptr<brogameagent::mcts::Option>> option_refs;
    std::vector<JSValue> optionJsRefs;
    std::vector<std::shared_ptr<JsCallbackHolder>> jcb;
    JSContext* ctx = nullptr;
    ~CommanderData() { for (auto& v : optionJsRefs) JS_FreeValue(ctx, v); }
};

// ═══════════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════════

// Read a double from a JS object property, with a default value
static double getDoubleProp(JSContext* ctx, JSValueConst obj, const char* key, double def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    double out = def;
    if (JS_IsNumber(v)) JS_ToFloat64(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}

static int32_t getInt32Prop(JSContext* ctx, JSValueConst obj, const char* key, int32_t def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    int32_t out = def;
    if (JS_IsNumber(v)) JS_ToInt32(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}

static bool getBoolProp(JSContext* ctx, JSValueConst obj, const char* key, bool def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool out = def;
    if (JS_IsBool(v)) out = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return out;
}

// Parse {x, z, hw, hd} from a JS object into an AABB
static brogameagent::AABB parseAABB(JSContext* ctx, JSValueConst obj) {
    return {
        static_cast<float>(getDoubleProp(ctx, obj, "x", 0)),
        static_cast<float>(getDoubleProp(ctx, obj, "z", 0)),
        static_cast<float>(getDoubleProp(ctx, obj, "hw", 0)),
        static_cast<float>(getDoubleProp(ctx, obj, "hd", 0)),
    };
}

// Parse a JS array of {x, z, hw, hd} into a vector of AABBs
static std::vector<brogameagent::AABB> parseAABBArray(JSContext* ctx, JSValueConst arr) {
    std::vector<brogameagent::AABB> out;
    if (!JS_IsArray(arr)) return out;
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);
    out.reserve(len);
    for (int32_t i = 0; i < len; i++) {
        JSValue ob = JS_GetPropertyUint32(ctx, arr, i);
        out.push_back(parseAABB(ctx, ob));
        JS_FreeValue(ctx, ob);
    }
    return out;
}

// Apply an `avoidance` opts value onto an agent: `true`/`false` toggles
// participation with default params; an object sets {enabled?, radius?,
// maxSpeed?, neighborDist?, maxNeighbors?, timeHorizon?, timeHorizonObst?}
// (radius/maxSpeed omitted or <= 0 derive from the agent). Shared by
// createAgent, agent.setAvoidance and node.attachAgent.
void applyAgentAvoidanceOpts(JSContext* ctx, JSValueConst val, brogameagent::Agent& agent) {
    brogameagent::AgentAvoidance av;  // library defaults
    if (JS_IsBool(val)) {
        av.enabled = JS_ToBool(ctx, val);
    } else if (JS_IsObject(val)) {
        av.enabled         = getBoolProp(ctx, val, "enabled", true);
        av.radius          = (float)getDoubleProp(ctx, val, "radius", -1);
        av.maxSpeed        = (float)getDoubleProp(ctx, val, "maxSpeed", -1);
        av.neighborDist    = (float)getDoubleProp(ctx, val, "neighborDist", av.neighborDist);
        av.maxNeighbors    = getInt32Prop(ctx, val, "maxNeighbors", av.maxNeighbors);
        av.timeHorizon     = (float)getDoubleProp(ctx, val, "timeHorizon", av.timeHorizon);
        av.timeHorizonObst = (float)getDoubleProp(ctx, val, "timeHorizonObst", av.timeHorizonObst);
    } else {
        return;
    }
    agent.setAvoidance(av);
}

// Create a {yaw, pitch} JS object
static JSValue makeAimResult(JSContext* ctx, const brogameagent::AimResult& aim) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "yaw", JS_NewFloat64(ctx, aim.yaw));
    JS_SetPropertyStr(ctx, obj, "pitch", JS_NewFloat64(ctx, aim.pitch));
    return obj;
}

// Create a {fx, fz} JS object
static JSValue makeSteeringOutput(JSContext* ctx, const brogameagent::SteeringOutput& s) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "fx", JS_NewFloat64(ctx, s.fx));
    JS_SetPropertyStr(ctx, obj, "fz", JS_NewFloat64(ctx, s.fz));
    return obj;
}

using qjsbind::make_float32_array;
using qjsbind::make_int32_array;

// Parse DamageKind from string
static brogameagent::DamageKind parseDamageKind(const char* str) {
    if (str && strcmp(str, "magical") == 0) return brogameagent::DamageKind::Magical;
    if (str && strcmp(str, "true") == 0) return brogameagent::DamageKind::True;
    return brogameagent::DamageKind::Physical;
}

static const char* damageKindStr(brogameagent::DamageKind k) {
    switch (k) {
        case brogameagent::DamageKind::Magical: return "magical";
        case brogameagent::DamageKind::True: return "true";
        default: return "physical";
    }
}

// Create a DamageEvent JS object
static JSValue makeDamageEvent(JSContext* ctx, const brogameagent::DamageEvent& e) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "attackerId", JS_NewInt32(ctx, e.attackerId));
    JS_SetPropertyStr(ctx, obj, "targetId", JS_NewInt32(ctx, e.targetId));
    JS_SetPropertyStr(ctx, obj, "amount", JS_NewFloat64(ctx, e.amount));
    JS_SetPropertyStr(ctx, obj, "kind", JS_NewString(ctx, damageKindStr(e.kind)));
    JS_SetPropertyStr(ctx, obj, "killed", JS_NewBool(ctx, e.killed));
    return obj;
}

// Wrap an AgentData* as a JS value (used when returning agents from World queries)
static JSValue wrapAgent(JSContext* ctx, AgentData* d) {
    return qjsbind::wrap<AgentData>(ctx, d);
}

// ═══════════════════════════════════════════════════════════════════════════
// Raw factory/helper functions (complex arg parsing)
// ═══════════════════════════════════════════════════════════════════════════

// bro.ai.game.createNavGrid(opts)
static JSValue js_createNavGrid(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "createNavGrid() requires an options object");

    JSValue opts = argv[0];
    float minX = (float)getDoubleProp(ctx, opts, "minX", -20);
    float minZ = (float)getDoubleProp(ctx, opts, "minZ", -20);
    float maxX = (float)getDoubleProp(ctx, opts, "maxX", 20);
    float maxZ = (float)getDoubleProp(ctx, opts, "maxZ", 20);
    float cellSize = (float)getDoubleProp(ctx, opts, "cellSize", 0.5);

    auto* data = new NavGridData{
        std::make_unique<brogameagent::NavGrid>(minX, minZ, maxX, maxZ, cellSize)
    };

    float padding = (float)getDoubleProp(ctx, opts, "padding", 0);

    // Process obstacles array
    JSValue obsArr = JS_GetPropertyStr(ctx, opts, "obstacles");
    if (JS_IsArray(obsArr)) {
        auto boxes = parseAABBArray(ctx, obsArr);
        for (auto& box : boxes)
            data->grid->addObstacle(box, padding);
    }
    JS_FreeValue(ctx, obsArr);

    // Bake obstacles from a physics world: every static, non-sensor body's
    // world AABB projected to XZ. `fromPhysics` is the default world (the
    // Physics namespace or `true`) or a sandbox handle from
    // Physics.createWorldHandle(). Phase-idle contract: createNavGrid runs
    // on the JS thread while the physics phase is idle, so direct body
    // access is safe (same as the query bindings).
    JSValue fromPhys = JS_GetPropertyStr(ctx, opts, "fromPhysics");
    if (!JS_IsUndefined(fromPhys) && !JS_IsNull(fromPhys) &&
        !(JS_IsBool(fromPhys) && !JS_ToBool(ctx, fromPhys))) {
#if BRO_WITH_PHYSICS
        auto* world = PhysicsBindings::unwrapWorld(ctx, fromPhys);
        if (!world) {
            JS_FreeValue(ctx, fromPhys);
            delete data;
            return JS_ThrowTypeError(ctx, "createNavGrid: fromPhysics world not available");
        }

        // Optional filters: physicsLayers (names/indices → mask) and a Y band
        // ([physicsMinY, physicsMaxY]) a body's AABB must intersect to count.
        uint32_t layerMask = 0xffffffffu;
        JSValue lv = JS_GetPropertyStr(ctx, opts, "physicsLayers");
        if (JS_IsArray(lv)) {
            uint32_t mask = 0;
            JSValue lenV = JS_GetPropertyStr(ctx, lv, "length");
            uint32_t n = 0; JS_ToUint32(ctx, &n, lenV); JS_FreeValue(ctx, lenV);
            for (uint32_t i = 0; i < n; i++) {
                JSValue el = JS_GetPropertyUint32(ctx, lv, i);
                int32_t idx = -1;
                if (JS_IsString(el)) {
                    const char* s = JS_ToCString(ctx, el);
                    if (s) { idx = world->layerIndex(s); JS_FreeCString(ctx, s); }
                } else if (JS_IsNumber(el)) {
                    JS_ToInt32(ctx, &idx, el);
                }
                if (idx >= 0 && idx < 32) mask |= 1u << idx;
                JS_FreeValue(ctx, el);
            }
            layerMask = mask;
        }
        JS_FreeValue(ctx, lv);
        float bandMinY = (float)getDoubleProp(ctx, opts, "physicsMinY", -FLT_MAX);
        float bandMaxY = (float)getDoubleProp(ctx, opts, "physicsMaxY", FLT_MAX);

        for (const auto& b : world->collectStaticBodies()) {
            if (b.isSensor) continue;  // sensors don't block movement
            if (b.layer >= 0 && b.layer < 32 && !(layerMask & (1u << b.layer))) continue;
            if (b.max.GetY() < bandMinY || b.min.GetY() > bandMaxY) continue;
            // Skip bodies whose XZ footprint covers the whole grid (a ground
            // slab would otherwise block every cell). Narrow the grid bounds
            // or use physicsMinY/physicsMaxY to include such geometry.
            if (b.min.GetX() <= minX && b.max.GetX() >= maxX &&
                b.min.GetZ() <= minZ && b.max.GetZ() >= maxZ) continue;
            brogameagent::AABB box{
                0.5f * (b.min.GetX() + b.max.GetX()),
                0.5f * (b.min.GetZ() + b.max.GetZ()),
                0.5f * (b.max.GetX() - b.min.GetX()),
                0.5f * (b.max.GetZ() - b.min.GetZ()),
            };
            data->grid->addObstacle(box, padding);
        }
#else
        JS_FreeValue(ctx, fromPhys);
        delete data;
        return JS_ThrowTypeError(ctx, "createNavGrid: fromPhysics requires a physics-enabled build");
#endif
    }
    JS_FreeValue(ctx, fromPhys);

    return qjsbind::wrap<NavGridData>(ctx, data);
}

#ifdef BROGAMEAGENT_HAS_NAVMESH

// ─── NavMesh helpers ───────────────────────────────────────────────────────

// Parse a point from {x, y, z} or [x, y, z]. Missing components read as 0.
static bool parseVec3Val(JSContext* ctx, JSValueConst val, bromath::Vec3& out) {
    if (!JS_IsObject(val)) return false;
    if (JS_IsArray(val)) {
        double c[3] = {0, 0, 0};
        for (uint32_t i = 0; i < 3; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, val, i);
            if (JS_IsNumber(v)) JS_ToFloat64(ctx, &c[i], v);
            JS_FreeValue(ctx, v);
        }
        out = {(float)c[0], (float)c[1], (float)c[2]};
        return true;
    }
    out = {(float)getDoubleProp(ctx, val, "x", 0),
           (float)getDoubleProp(ctx, val, "y", 0),
           (float)getDoubleProp(ctx, val, "z", 0)};
    return true;
}

static JSValue makeVec3(JSContext* ctx, bromath::Vec3 v) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, v.x));
    JS_SetPropertyStr(ctx, o, "y", JS_NewFloat64(ctx, v.y));
    JS_SetPropertyStr(ctx, o, "z", JS_NewFloat64(ctx, v.z));
    return o;
}

// Snap extents: optional {x,y,z}/[x,y,z] arg, default NavMesh::kDefaultExtents.
static bromath::Vec3 parseExtentsArg(JSContext* ctx, int argc, JSValueConst* argv, int idx) {
    bromath::Vec3 e = brogameagent::NavMesh::kDefaultExtents;
    if (argc > idx) parseVec3Val(ctx, argv[idx], e);
    return e;
}

// Read a numeric array as floats: Float32Array (zero-copy view, copied out)
// or a plain JS array of numbers.
static bool readFloatsAny(JSContext* ctx, JSValueConst val, std::vector<float>& out) {
    size_t n = 0;
    if (const float* view = qjsbind::read_float32_view(ctx, val, n)) {
        out.insert(out.end(), view, view + n);
        return true;
    }
    if (!JS_IsArray(val)) return false;
    JSValue lenV = JS_GetPropertyStr(ctx, val, "length");
    uint32_t len = 0; JS_ToUint32(ctx, &len, lenV); JS_FreeValue(ctx, lenV);
    out.reserve(out.size() + len);
    for (uint32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, val, i);
        double d = 0; JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        out.push_back((float)d);
    }
    return true;
}

// Read a numeric array as u32 indices: Uint32Array/Int32Array or plain array.
static bool readU32Any(JSContext* ctx, JSValueConst val, std::vector<uint32_t>& out) {
    size_t n = 0;
    if (const uint32_t* view = qjsbind::read_typed_array_view<uint32_t>(ctx, val, n)) {
        out.insert(out.end(), view, view + n);
        return true;
    }
    if (!JS_IsArray(val)) return false;
    JSValue lenV = JS_GetPropertyStr(ctx, val, "length");
    uint32_t len = 0; JS_ToUint32(ctx, &len, lenV); JS_FreeValue(ctx, lenV);
    out.reserve(out.size() + len);
    for (uint32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, val, i);
        uint32_t d = 0; JS_ToUint32(ctx, &d, v);
        JS_FreeValue(ctx, v);
        out.push_back(d);
    }
    return true;
}

// ── Dynamic-obstacle pump registry ─────────────────────────────────────────
// Every obstacle-capable mesh (bakeNavMesh({dynamicObstacles: true})) is
// registered here; the engine calls pumpNavMeshObstacles(dt) once per frame
// (engine_frame / headless advanceTime) right before scene agents sync, so
// dtTileCache tile rebuilds progress without the app having to call
// mesh.update() itself, and a finished batch repaths agents the same frame.
// weak_ptr so the pump never extends a mesh's lifetime; expired entries are
// pruned in place. Mutex is fine here: cold control plane, main thread only
// contends with a worker creating a mesh.
static std::mutex g_navMeshPumpMutex;
static std::vector<std::weak_ptr<brogameagent::NavMesh>> g_navMeshPump;

static void registerNavMeshForPump(const std::shared_ptr<brogameagent::NavMesh>& m) {
    std::lock_guard<std::mutex> lock(g_navMeshPumpMutex);
    g_navMeshPump.push_back(m);
}

// Parse `physicsLayers` (array of layer names/indices) into a bitmask.
// Shared shape with createNavGrid's filter.
#if BRO_WITH_PHYSICS
static uint32_t parsePhysicsLayerMask(JSContext* ctx, JSValueConst opts,
                                      physics::PhysicsWorld* world) {
    uint32_t layerMask = 0xffffffffu;
    JSValue lv = JS_GetPropertyStr(ctx, opts, "physicsLayers");
    if (JS_IsArray(lv)) {
        uint32_t mask = 0;
        JSValue lenV = JS_GetPropertyStr(ctx, lv, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lenV); JS_FreeValue(ctx, lenV);
        for (uint32_t i = 0; i < n; i++) {
            JSValue el = JS_GetPropertyUint32(ctx, lv, i);
            int32_t idx = -1;
            if (JS_IsString(el)) {
                const char* s = JS_ToCString(ctx, el);
                if (s) { idx = world->layerIndex(s); JS_FreeCString(ctx, s); }
            } else if (JS_IsNumber(el)) {
                JS_ToInt32(ctx, &idx, el);
            }
            if (idx >= 0 && idx < 32) mask |= 1u << idx;
            JS_FreeValue(ctx, el);
        }
        layerMask = mask;
    }
    JS_FreeValue(ctx, lv);
    return layerMask;
}
#endif

// bro.ai.game.bakeNavMesh(opts) — bake a polygon navmesh from any mix of:
//   positions/indices  raw triangle soup (Float32Array xyz + Uint32Array)
//   fromPhysics        static bodies' actual triangle geometry
//   fromTerrain        height-sampled voxel-terrain surface
// All requested sources are concatenated into one soup and baked once.
static JSValue js_bakeNavMesh(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "bakeNavMesh() requires an options object");
    JSValue opts = argv[0];

    std::vector<float> xyz;
    std::vector<uint32_t> indices;

    // ── Raw soup ──
    {
        JSValue posV = JS_GetPropertyStr(ctx, opts, "positions");
        JSValue idxV = JS_GetPropertyStr(ctx, opts, "indices");
        const bool hasPos = !JS_IsUndefined(posV) && !JS_IsNull(posV);
        const bool hasIdx = !JS_IsUndefined(idxV) && !JS_IsNull(idxV);
        if (hasPos != hasIdx) {
            JS_FreeValue(ctx, posV); JS_FreeValue(ctx, idxV);
            return JS_ThrowTypeError(ctx, "bakeNavMesh: positions and indices must be passed together");
        }
        if (hasPos) {
            std::vector<float> verts;
            std::vector<uint32_t> idx;
            bool okP = readFloatsAny(ctx, posV, verts);
            bool okI = readU32Any(ctx, idxV, idx);
            JS_FreeValue(ctx, posV); JS_FreeValue(ctx, idxV);
            if (!okP || !okI || verts.size() % 3 != 0 || idx.size() % 3 != 0)
                return JS_ThrowTypeError(ctx,
                    "bakeNavMesh: positions must be flat xyz triples and indices a triangle list");
            const uint32_t nVerts = (uint32_t)(verts.size() / 3);
            for (uint32_t i : idx) {
                if (i >= nVerts)
                    return JS_ThrowRangeError(ctx, "bakeNavMesh: index %u out of range (%u vertices)",
                                              i, nVerts);
            }
            const uint32_t base = (uint32_t)(xyz.size() / 3);
            xyz.insert(xyz.end(), verts.begin(), verts.end());
            indices.reserve(indices.size() + idx.size());
            for (uint32_t i : idx) indices.push_back(base + i);
        } else {
            JS_FreeValue(ctx, posV); JS_FreeValue(ctx, idxV);
        }
    }

    // ── fromPhysics: static bodies' triangle geometry ──
    JSValue fromPhys = JS_GetPropertyStr(ctx, opts, "fromPhysics");
    if (!JS_IsUndefined(fromPhys) && !JS_IsNull(fromPhys) &&
        !(JS_IsBool(fromPhys) && !JS_ToBool(ctx, fromPhys))) {
#if BRO_WITH_PHYSICS
        auto* world = PhysicsBindings::unwrapWorld(ctx, fromPhys);
        if (!world) {
            JS_FreeValue(ctx, fromPhys);
            return JS_ThrowTypeError(ctx, "bakeNavMesh: fromPhysics world not available");
        }
        // Phase-idle contract: bake runs on the JS thread while the physics
        // phase is idle, so direct body access is safe (same as createNavGrid).
        const uint32_t layerMask = parsePhysicsLayerMask(ctx, opts, world);
        world->collectStaticTriangles(xyz, indices, layerMask);
#else
        JS_FreeValue(ctx, fromPhys);
        return JS_ThrowTypeError(ctx, "bakeNavMesh: fromPhysics requires a physics-enabled build");
#endif
    }
    JS_FreeValue(ctx, fromPhys);

    // ── fromTerrain: height-sampled surface grid ──
    // Voxel terrain is heightmap-backed; sampling the top surface on a grid
    // (one down-raycast per sample) reproduces slopes and plateaus without
    // pulling chunk render meshes. Overhangs/caves are approximated by the
    // top surface — documented limitation. Samples with no hit become holes.
    JSValue fromTerr = JS_GetPropertyStr(ctx, opts, "fromTerrain");
    if (!JS_IsUndefined(fromTerr) && !JS_IsNull(fromTerr)) {
#if BRO_WITH_3D
        void* th = terrainHandleFromJS(ctx, fromTerr);
        if (!th) {
            JS_FreeValue(ctx, fromTerr);
            return JS_ThrowTypeError(ctx,
                "bakeNavMesh: fromTerrain must be a scene.createTerrain() object");
        }
        JSValue bv = JS_GetPropertyStr(ctx, opts, "terrainBounds");
        if (!JS_IsObject(bv)) {
            JS_FreeValue(ctx, bv);
            JS_FreeValue(ctx, fromTerr);
            return JS_ThrowTypeError(ctx,
                "bakeNavMesh: fromTerrain requires terrainBounds {minX, minZ, maxX, maxZ}");
        }
        const float minX = (float)getDoubleProp(ctx, bv, "minX", 0);
        const float minZ = (float)getDoubleProp(ctx, bv, "minZ", 0);
        const float maxX = (float)getDoubleProp(ctx, bv, "maxX", 0);
        const float maxZ = (float)getDoubleProp(ctx, bv, "maxZ", 0);
        JS_FreeValue(ctx, bv);
        const float step      = (float)getDoubleProp(ctx, opts, "terrainStep", 1.0);
        const float rayStart  = (float)getDoubleProp(ctx, opts, "terrainRayStart", 100.0);
        const float rayLength = (float)getDoubleProp(ctx, opts, "terrainRayLength", 200.0);
        if (!(maxX > minX) || !(maxZ > minZ) || !(step > 0)) {
            JS_FreeValue(ctx, fromTerr);
            return JS_ThrowRangeError(ctx, "bakeNavMesh: invalid terrainBounds/terrainStep");
        }
        const int nx = (int)((maxX - minX) / step) + 1;
        const int nz = (int)((maxZ - minZ) / step) + 1;
        if ((int64_t)nx * nz > 4 * 1024 * 1024) {
            JS_FreeValue(ctx, fromTerr);
            return JS_ThrowRangeError(ctx, "bakeNavMesh: terrain sample grid too large (%dx%d)",
                                      nx, nz);
        }
        std::vector<float> hs((size_t)nx * nz);
        std::vector<uint8_t> valid((size_t)nx * nz, 0);
        for (int iz = 0; iz < nz; iz++) {
            for (int ix = 0; ix < nx; ix++) {
                float y = 0;
                if (terrainSampleHeight(th, minX + ix * step, minZ + iz * step,
                                        rayStart, rayLength, y)) {
                    hs[(size_t)iz * nx + ix] = y;
                    valid[(size_t)iz * nx + ix] = 1;
                }
            }
        }
        const uint32_t base = (uint32_t)(xyz.size() / 3);
        std::vector<int32_t> vidx((size_t)nx * nz, -1);
        for (int iz = 0; iz < nz; iz++) {
            for (int ix = 0; ix < nx; ix++) {
                const size_t s = (size_t)iz * nx + ix;
                if (!valid[s]) continue;
                vidx[s] = (int32_t)(xyz.size() / 3 - base);
                xyz.push_back(minX + ix * step);
                xyz.push_back(hs[s]);
                xyz.push_back(minZ + iz * step);
            }
        }
        // CCW from above: (x0,z0)→(x0,z1)→(x1,z0) and (x1,z0)→(x0,z1)→(x1,z1).
        for (int iz = 0; iz + 1 < nz; iz++) {
            for (int ix = 0; ix + 1 < nx; ix++) {
                const int32_t a = vidx[(size_t)iz * nx + ix];
                const int32_t b = vidx[(size_t)(iz + 1) * nx + ix];
                const int32_t c = vidx[(size_t)iz * nx + ix + 1];
                const int32_t d = vidx[(size_t)(iz + 1) * nx + ix + 1];
                if (a >= 0 && b >= 0 && c >= 0) {
                    indices.push_back(base + a);
                    indices.push_back(base + b);
                    indices.push_back(base + c);
                }
                if (c >= 0 && b >= 0 && d >= 0) {
                    indices.push_back(base + c);
                    indices.push_back(base + b);
                    indices.push_back(base + d);
                }
            }
        }
#else
        JS_FreeValue(ctx, fromTerr);
        return JS_ThrowTypeError(ctx, "bakeNavMesh: fromTerrain requires a 3D-enabled build");
#endif
    }
    JS_FreeValue(ctx, fromTerr);

    if (xyz.empty() || indices.empty())
        return JS_ThrowTypeError(ctx,
            "bakeNavMesh: no geometry (pass positions/indices, fromPhysics or fromTerrain)");

    // ── Bake config ──
    brogameagent::NavMeshBakeConfig cfg;
    cfg.cellSize             = (float)getDoubleProp(ctx, opts, "cellSize", cfg.cellSize);
    cfg.cellHeight           = (float)getDoubleProp(ctx, opts, "cellHeight", cfg.cellHeight);
    cfg.agentRadius          = (float)getDoubleProp(ctx, opts, "agentRadius", cfg.agentRadius);
    cfg.agentHeight          = (float)getDoubleProp(ctx, opts, "agentHeight", cfg.agentHeight);
    cfg.agentMaxClimb        = (float)getDoubleProp(ctx, opts, "agentMaxClimb", cfg.agentMaxClimb);
    cfg.agentMaxSlopeDeg     = (float)getDoubleProp(ctx, opts, "agentMaxSlopeDeg", cfg.agentMaxSlopeDeg);
    cfg.regionMinSize        = (float)getDoubleProp(ctx, opts, "regionMinSize", cfg.regionMinSize);
    cfg.regionMergeSize      = (float)getDoubleProp(ctx, opts, "regionMergeSize", cfg.regionMergeSize);
    cfg.edgeMaxLen           = (float)getDoubleProp(ctx, opts, "edgeMaxLen", cfg.edgeMaxLen);
    cfg.edgeMaxError         = (float)getDoubleProp(ctx, opts, "edgeMaxError", cfg.edgeMaxError);
    cfg.detailSampleDist     = (float)getDoubleProp(ctx, opts, "detailSampleDist", cfg.detailSampleDist);
    cfg.detailSampleMaxError = (float)getDoubleProp(ctx, opts, "detailSampleMaxError", cfg.detailSampleMaxError);

    // Dynamic obstacles: tiled dtTileCache bake with runtime addObstacle/
    // removeObstacle/update. See the obstacle methods on the NavMesh class.
    {
        JSValue v = JS_GetPropertyStr(ctx, opts, "dynamicObstacles");
        cfg.dynamicObstacles = JS_ToBool(ctx, v) == 1;
        JS_FreeValue(ctx, v);
    }
    cfg.tileSize     = (float)getDoubleProp(ctx, opts, "tileSize", cfg.tileSize);
    cfg.maxObstacles = (int)getDoubleProp(ctx, opts, "maxObstacles", cfg.maxObstacles);

    auto mesh = std::make_shared<brogameagent::NavMesh>();
    if (!mesh->bake(xyz.data(), xyz.size() / 3, indices.data(), indices.size(), cfg)) {
        return JS_ThrowInternalError(ctx, "bakeNavMesh: %s", mesh->lastError().c_str());
    }
    // Obstacle-capable meshes get their tile rebuilds pumped by the engine
    // once per frame (pumpNavMeshObstacles).
    if (mesh->supportsObstacles()) registerNavMeshForPump(mesh);
    return qjsbind::wrap<NavMeshData>(ctx, new NavMeshData{std::move(mesh)});
}

// bro.ai.game.loadNavMesh(buffer) — restore a mesh saved with navMesh.save().
// Accepts an ArrayBuffer or any typed-array view over one.
static JSValue js_loadNavMesh(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "loadNavMesh(buffer)");
    size_t size = 0;
    uint8_t* data = JS_GetArrayBuffer(ctx, &size, argv[0]);
    if (!data) {
        // Not an ArrayBuffer — try a typed-array view.
        JS_FreeValue(ctx, JS_GetException(ctx));
        size_t byteOff = 0, byteLen = 0;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &byteLen, nullptr);
        if (JS_IsException(ab)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return JS_ThrowTypeError(ctx, "loadNavMesh: expected an ArrayBuffer or typed array");
        }
        size_t abLen = 0;
        uint8_t* raw = JS_GetArrayBuffer(ctx, &abLen, ab);
        JS_FreeValue(ctx, ab);
        if (!raw) return JS_ThrowTypeError(ctx, "loadNavMesh: detached buffer");
        data = raw + byteOff;
        size = byteLen;
    }
    auto mesh = std::make_unique<brogameagent::NavMesh>();
    if (!mesh->loadFrom(data, size)) {
        return JS_ThrowInternalError(ctx, "loadNavMesh: %s", mesh->lastError().c_str());
    }
    return qjsbind::wrap<NavMeshData>(ctx, new NavMeshData{std::move(mesh)});
}

#endif  // BROGAMEAGENT_HAS_NAVMESH

// bro.ai.game.createAgent(opts)
static JSValue js_createAgent(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* h = new AgentData();

    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue opts = argv[0];
        float x     = (float)getDoubleProp(ctx, opts, "x", 0);
        float z     = (float)getDoubleProp(ctx, opts, "z", 0);
        float speed = (float)getDoubleProp(ctx, opts, "speed", 6);
        float radius = (float)getDoubleProp(ctx, opts, "radius", 0.4);

        h->agent.setPosition(x, z);
        h->agent.setSpeed(speed);
        h->agent.setRadius(radius);

        // Unit stats from opts
        h->agent.unit().id     = getInt32Prop(ctx, opts, "id", 0);
        h->agent.unit().teamId = getInt32Prop(ctx, opts, "teamId", 0);
        double hp = getDoubleProp(ctx, opts, "hp", -1);
        if (hp >= 0) { h->agent.unit().hp = (float)hp; h->agent.unit().maxHp = (float)hp; }
        double maxHp = getDoubleProp(ctx, opts, "maxHp", -1);
        if (maxHp >= 0) h->agent.unit().maxHp = (float)maxHp;
        double damage = getDoubleProp(ctx, opts, "damage", -1);
        if (damage >= 0) h->agent.unit().damage = (float)damage;
        double attackRange = getDoubleProp(ctx, opts, "attackRange", -1);
        if (attackRange >= 0) h->agent.unit().attackRange = (float)attackRange;

        double maxAccel = getDoubleProp(ctx, opts, "maxAccel", -1);
        if (maxAccel >= 0) h->agent.setMaxAccel((float)maxAccel);
        double maxTurnRate = getDoubleProp(ctx, opts, "maxTurnRate", -1);
        if (maxTurnRate >= 0) h->agent.setMaxTurnRate((float)maxTurnRate);

        // avoidance: true|false|{...} — ORCA participation when the world's
        // avoidance pass is on (world.setAvoidance).
        JSValue avoidVal = JS_GetPropertyStr(ctx, opts, "avoidance");
        if (!JS_IsUndefined(avoidVal) && !JS_IsNull(avoidVal))
            applyAgentAvoidanceOpts(ctx, avoidVal, h->agent);
        JS_FreeValue(ctx, avoidVal);

        // navGrid — pin onto the agent JS object below so the JS NavGrid
        // outlives the agent (Agent holds a raw NavGrid* pointer).
        JSValue navGridVal = JS_GetPropertyStr(ctx, opts, "navGrid");
        if (JS_IsObject(navGridVal)) {
            auto* gridData = qjsbind::unwrap<NavGridData>(ctx, navGridVal);
            if (gridData && gridData->grid) {
                h->agent.setNavGrid(gridData->grid.get());
            }
        }

        JSValue agentVal = qjsbind::wrap<AgentData>(ctx, h);
        if (JS_IsObject(navGridVal)) {
            JS_SetPropertyStr(ctx, agentVal, "__navGrid", JS_DupValue(ctx, navGridVal));
        }
        JS_FreeValue(ctx, navGridVal);
        return agentVal;
    }

    return qjsbind::wrap<AgentData>(ctx, h);
}

// bro.ai.game.createWorld()
static JSValue js_createWorld(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return qjsbind::wrap<WorldData>(ctx, new WorldData());
}

// bro.ai.game.hasLineOfSight(fromX, fromZ, toX, toZ, obstacles)
static JSValue js_hasLOS(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_FALSE;
    double fx, fz, tx, tz;
    JS_ToFloat64(ctx, &fx, argv[0]);
    JS_ToFloat64(ctx, &fz, argv[1]);
    JS_ToFloat64(ctx, &tx, argv[2]);
    JS_ToFloat64(ctx, &tz, argv[3]);

    auto boxes = parseAABBArray(ctx, argv[4]);
    bool los = brogameagent::hasLineOfSight(
        {(float)fx, (float)fz}, {(float)tx, (float)tz},
        boxes.data(), (int)boxes.size());
    return JS_NewBool(ctx, los);
}

// bro.ai.game.canSee(fromX, fromZ, toX, toZ, facingYaw, fovRadians, maxRange, obstacles)
static JSValue js_canSee(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 8) return JS_FALSE;
    double fx, fz, tx, tz, facingYaw, fov, maxRange;
    JS_ToFloat64(ctx, &fx, argv[0]);
    JS_ToFloat64(ctx, &fz, argv[1]);
    JS_ToFloat64(ctx, &tx, argv[2]);
    JS_ToFloat64(ctx, &tz, argv[3]);
    JS_ToFloat64(ctx, &facingYaw, argv[4]);
    JS_ToFloat64(ctx, &fov, argv[5]);
    JS_ToFloat64(ctx, &maxRange, argv[6]);

    auto boxes = parseAABBArray(ctx, argv[7]);
    bool result = brogameagent::canSee(
        {(float)fx, (float)fz}, {(float)tx, (float)tz},
        (float)facingYaw, (float)fov, (float)maxRange,
        boxes.data(), (int)boxes.size());
    return JS_NewBool(ctx, result);
}

// bro.ai.game.computeAim(fromX, fromY, fromZ, toX, toY, toZ)
static JSValue js_computeAim(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_NULL;
    double fx, fy, fz, tx, ty, tz;
    JS_ToFloat64(ctx, &fx, argv[0]);
    JS_ToFloat64(ctx, &fy, argv[1]);
    JS_ToFloat64(ctx, &fz, argv[2]);
    JS_ToFloat64(ctx, &tx, argv[3]);
    JS_ToFloat64(ctx, &ty, argv[4]);
    JS_ToFloat64(ctx, &tz, argv[5]);
    auto aim = brogameagent::computeAim((float)fx, (float)fy, (float)fz,
                                         (float)tx, (float)ty, (float)tz);
    return makeAimResult(ctx, aim);
}

// bro.ai.game.computeLeadAim(fromX, fromY, fromZ, tX, tY, tZ, tVX, tVY, tVZ, projectileSpeed)
static JSValue js_computeLeadAim(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 10) return JS_NULL;
    double vals[10];
    for (int i = 0; i < 10; i++) JS_ToFloat64(ctx, &vals[i], argv[i]);
    auto result = brogameagent::computeLeadAim(
        (float)vals[0], (float)vals[1], (float)vals[2],
        (float)vals[3], (float)vals[4], (float)vals[5],
        (float)vals[6], (float)vals[7], (float)vals[8],
        (float)vals[9]);
    JSValue obj = makeAimResult(ctx, result.aim);
    JS_SetPropertyStr(ctx, obj, "valid", JS_NewBool(ctx, result.valid));
    JS_SetPropertyStr(ctx, obj, "timeToHit", JS_NewFloat64(ctx, result.timeToHit));
    return obj;
}

// bro.ai.game.buildObservation(agent, world)
static JSValue js_buildObservation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "buildObservation(agent, world)");
    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[0]);
    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[1]);
    if (!ad || !wd) return JS_ThrowTypeError(ctx, "invalid agent or world");

    float buf[brogameagent::observation::TOTAL];
    brogameagent::observation::build(ad->agent, wd->world, buf);
    return make_float32_array(ctx, buf, brogameagent::observation::TOTAL);
}

// bro.ai.game.buildActionMask(agent, world)
static JSValue js_buildActionMask(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "buildActionMask(agent, world)");
    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[0]);
    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[1]);
    if (!ad || !wd) return JS_ThrowTypeError(ctx, "invalid agent or world");

    float mask[brogameagent::action_mask::TOTAL];
    int enemyIds[brogameagent::action_mask::N_ENEMY_SLOTS];
    brogameagent::action_mask::build(ad->agent, wd->world, mask, enemyIds);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "mask", make_float32_array(ctx, mask, brogameagent::action_mask::TOTAL));
    JS_SetPropertyStr(ctx, obj, "enemyIds", make_int32_array(ctx, reinterpret_cast<const int32_t*>(enemyIds), brogameagent::action_mask::N_ENEMY_SLOTS));
    return obj;
}

// bro.ai.game.createRewardTracker(agent, world)
static JSValue js_createRewardTracker(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "createRewardTracker(agent, world)");
    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[0]);
    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[1]);
    if (!ad || !wd) return JS_ThrowTypeError(ctx, "invalid agent or world");

    auto* data = new RewardTrackerData();
    data->tracker.reset(ad->agent, wd->world);
    return qjsbind::wrap<RewardTrackerData>(ctx, data);
}

// bro.ai.game.createSimulation(world)
static JSValue js_createSimulation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "createSimulation(world)");
    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[0]);
    if (!wd) return JS_ThrowTypeError(ctx, "invalid world");

    auto* data = new SimulationData{
        std::make_unique<brogameagent::Simulation>(wd->world)
    };
    // Hold ref to world so it doesn't get GC'd
    JSValue obj = qjsbind::wrap<SimulationData>(ctx, data);
    JS_SetPropertyStr(ctx, obj, "__world", JS_DupValue(ctx, argv[0]));
    return obj;
}

// bro.ai.game.createRecorder()
static JSValue js_createRecorder(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return qjsbind::wrap<RecorderData>(ctx, new RecorderData());
}

// bro.ai.game.createReplayReader()
static JSValue js_createReplayReader(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return qjsbind::wrap<ReplayReaderData>(ctx, new ReplayReaderData());
}

// ─── MCTS shared parse/marshal helpers ─────────────────────────────────────

brogameagent::mcts::MctsConfig parseMctsConfig(JSContext* ctx, JSValueConst opts) {
    brogameagent::mcts::MctsConfig c{};
    c.iterations              = getInt32Prop(ctx, opts, "iterations", c.iterations);
    c.budget_ms               = getInt32Prop(ctx, opts, "budgetMs", c.budget_ms);
    c.rollout_horizon         = getInt32Prop(ctx, opts, "rolloutHorizon", c.rollout_horizon);
    c.sim_dt                  = (float)getDoubleProp(ctx, opts, "simDt", c.sim_dt);
    c.action_repeat           = getInt32Prop(ctx, opts, "actionRepeat", c.action_repeat);
    c.uct_c                   = (float)getDoubleProp(ctx, opts, "uctC", c.uct_c);
    c.seed                    = (uint64_t)getDoubleProp(ctx, opts, "seed", (double)c.seed);
    c.tactic_window_decisions = getInt32Prop(ctx, opts, "tacticWindowDecisions", c.tactic_window_decisions);
    c.pw_alpha                = (float)getDoubleProp(ctx, opts, "pwAlpha", c.pw_alpha);
    c.prior_c                 = (float)getDoubleProp(ctx, opts, "priorC", c.prior_c);
    c.option_max_windows      = getInt32Prop(ctx, opts, "optionMaxWindows", c.option_max_windows);
    c.use_leaf_value          = getBoolProp(ctx, opts, "useLeafValue", c.use_leaf_value);
    return c;
}

static std::string readStringProp(JSContext* ctx, JSValueConst obj, const char* key) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    std::string out;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { out = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    return out;
}

// Build a plain-object view of one agent's commonly-needed fields. Used by
// JS rollout/prior/evaluator/option callbacks so the callback doesn't need
// access to the C++ Agent wrapper (which would require a reverse lookup
// from Agent* to JSValue). O(1) per call — keep view minimal.
static JSValue buildAgentFields(JSContext* ctx, const brogameagent::Agent& a) {
    const auto& u = a.unit();
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "id",             JS_NewInt32(ctx, u.id));
    JS_SetPropertyStr(ctx, o, "teamId",         JS_NewInt32(ctx, u.teamId));
    JS_SetPropertyStr(ctx, o, "x",              JS_NewFloat64(ctx, a.x()));
    JS_SetPropertyStr(ctx, o, "z",              JS_NewFloat64(ctx, a.z()));
    JS_SetPropertyStr(ctx, o, "yaw",            JS_NewFloat64(ctx, a.yaw()));
    JS_SetPropertyStr(ctx, o, "hp",             JS_NewFloat64(ctx, u.hp));
    JS_SetPropertyStr(ctx, o, "maxHp",          JS_NewFloat64(ctx, u.maxHp));
    JS_SetPropertyStr(ctx, o, "alive",          JS_NewBool(ctx, u.alive()));
    JS_SetPropertyStr(ctx, o, "attackRange",    JS_NewFloat64(ctx, u.attackRange));
    JS_SetPropertyStr(ctx, o, "attackCooldown", JS_NewFloat64(ctx, u.attackCooldown));
    JS_SetPropertyStr(ctx, o, "mana",           JS_NewFloat64(ctx, u.mana));
    JS_SetPropertyStr(ctx, o, "maxMana",        JS_NewFloat64(ctx, u.maxMana));

    // Ability cooldowns — one entry per populated slot (abilitySlot >= 0).
    // Callers read ability[i].cooldown; unpopulated slots are omitted so
    // slot indices still match the original array layout when present.
    JSValue ab = JS_NewArray(ctx);
    for (int i = 0; i < brogameagent::Unit::MAX_ABILITIES; i++) {
        JSValue slot = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, slot, "abilityId", JS_NewInt32(ctx, u.abilitySlot[i]));
        JS_SetPropertyStr(ctx, slot, "cooldown",
                           JS_NewFloat64(ctx, u.abilityCooldowns[i]));
        JS_SetPropertyUint32(ctx, ab, (uint32_t)i, slot);
    }
    JS_SetPropertyStr(ctx, o, "abilities", ab);
    return o;
}

// Build a world view: array of agent fields. JS can filter by teamId /
// alive itself. O(N) per call — keep JS callbacks cheap or use C++ presets.
static JSValue buildWorldView(JSContext* ctx, const brogameagent::World& world) {
    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    for (brogameagent::Agent* a : world.agents()) {
        if (!a) continue;
        JS_SetPropertyUint32(ctx, arr, idx++, buildAgentFields(ctx, *a));
    }
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "agents", arr);
    return o;
}

// Forward decls — defined later in the file, referenced by the JS-callback
// wrappers below (parsing/emitting CombatAction objects to/from JS).
static brogameagent::mcts::CombatAction parseCombatAction(JSContext* ctx, JSValueConst obj);
JSValue makeCombatAction(JSContext* ctx, const brogameagent::mcts::CombatAction& a);
static std::vector<brogameagent::mcts::CombatAction>
parseCombatActionArray(JSContext* ctx, JSValueConst arr);

// ─── JS-callback policy/prior/evaluator wrappers ──────────────────────────
//
// Allow JS authors to pass a function in place of the string presets. The
// wrappers hold a reference to the callback (ref-counted via JS_DupValue)
// and release it in their destructor. MCTS calls these on its thread of
// control; QuickJS contexts are single-threaded so the JS callback runs
// synchronously on the caller's thread — no locking needed.
//
// Performance note: each call allocates a small JS view object. Rollout
// runs many times per search, so a JS rollout is materially slower than
// the C++ "random" / "aggressive" / "scripted" presets. Prefer presets for
// hot paths; use JS callbacks when decision logic is easier in JS (e.g.
// reusing a scripted agent's policy).

namespace {

class JsRolloutPolicy : public brogameagent::mcts::IRolloutPolicy,
                        public JsCallbackHolder {
public:
    JsRolloutPolicy(JSContext* ctx, JSValue fn)
        : ctx_(ctx), fn_(JS_DupValue(ctx, fn)) {}
    ~JsRolloutPolicy() override { JS_FreeValue(ctx_, fn_); }
    void gc_mark(JSRuntime* rt, JS_MarkFunc* mark) const override {
        JS_MarkValue(rt, fn_, mark);
    }

    brogameagent::mcts::CombatAction choose(
        brogameagent::Agent& self, brogameagent::World& world) const override {
        JSValue selfV  = buildAgentFields(ctx_, self);
        JSValue worldV = buildWorldView(ctx_, world);
        JSValue args[2] = { selfV, worldV };
        JSValue res = JS_Call(ctx_, fn_, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx_, selfV);
        JS_FreeValue(ctx_, worldV);
        brogameagent::mcts::CombatAction a{};
        if (!JS_IsException(res) && JS_IsObject(res)) {
            a = parseCombatAction(ctx_, res);
        }
        JS_FreeValue(ctx_, res);
        return a;
    }

private:
    JSContext* ctx_;
    JSValue    fn_;
};

class JsPrior : public brogameagent::mcts::IPrior,
                public JsCallbackHolder {
public:
    JsPrior(JSContext* ctx, JSValue fn)
        : ctx_(ctx), fn_(JS_DupValue(ctx, fn)) {}
    ~JsPrior() override { JS_FreeValue(ctx_, fn_); }
    void gc_mark(JSRuntime* rt, JS_MarkFunc* mark) const override {
        JS_MarkValue(rt, fn_, mark);
    }

    std::vector<float> score(
        const brogameagent::Agent& self, const brogameagent::World& world,
        const std::vector<brogameagent::mcts::CombatAction>& actions) const override {

        JSValue selfV  = buildAgentFields(ctx_, self);
        JSValue worldV = buildWorldView(ctx_, world);
        JSValue actsV  = JS_NewArray(ctx_);
        for (uint32_t i = 0; i < actions.size(); i++) {
            JS_SetPropertyUint32(ctx_, actsV, i, makeCombatAction(ctx_, actions[i]));
        }
        JSValue args[3] = { selfV, worldV, actsV };
        JSValue res = JS_Call(ctx_, fn_, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx_, selfV);
        JS_FreeValue(ctx_, worldV);
        JS_FreeValue(ctx_, actsV);

        std::vector<float> weights(actions.size(), 1.0f);
        if (!JS_IsException(res) && JS_IsArray(res)) {
            JSValue lenVal = JS_GetPropertyStr(ctx_, res, "length");
            int32_t len = 0; JS_ToInt32(ctx_, &len, lenVal);
            JS_FreeValue(ctx_, lenVal);
            int n = std::min(len, (int)actions.size());
            for (int i = 0; i < n; i++) {
                JSValue v = JS_GetPropertyUint32(ctx_, res, i);
                double d = 0.0;
                JS_ToFloat64(ctx_, &d, v);
                JS_FreeValue(ctx_, v);
                weights[i] = (float)std::max(0.0, d);
            }
        }
        JS_FreeValue(ctx_, res);
        return weights;
    }

private:
    JSContext* ctx_;
    JSValue    fn_;
};

class JsEvaluator : public brogameagent::mcts::IEvaluator,
                    public JsCallbackHolder {
public:
    JsEvaluator(JSContext* ctx, JSValue fn)
        : ctx_(ctx), fn_(JS_DupValue(ctx, fn)) {}
    ~JsEvaluator() override { JS_FreeValue(ctx_, fn_); }
    void gc_mark(JSRuntime* rt, JS_MarkFunc* mark) const override {
        JS_MarkValue(rt, fn_, mark);
    }

    float evaluate(const brogameagent::World& world, int heroId) const override {
        JSValue worldV = buildWorldView(ctx_, world);
        JSValue idV    = JS_NewInt32(ctx_, heroId);
        JSValue args[2] = { worldV, idV };
        JSValue res = JS_Call(ctx_, fn_, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx_, worldV);
        JS_FreeValue(ctx_, idV);
        float v = 0.0f;
        if (!JS_IsException(res)) {
            double d = 0.0;
            JS_ToFloat64(ctx_, &d, res);
            v = (float)std::clamp(d, -1.0, 1.0);
        }
        JS_FreeValue(ctx_, res);
        return v;
    }

private:
    JSContext* ctx_;
    JSValue    fn_;
};

class JsTeamEvaluator : public brogameagent::mcts::ITeamEvaluator,
                        public JsCallbackHolder {
public:
    JsTeamEvaluator(JSContext* ctx, JSValue fn)
        : ctx_(ctx), fn_(JS_DupValue(ctx, fn)) {}
    ~JsTeamEvaluator() override { JS_FreeValue(ctx_, fn_); }
    void gc_mark(JSRuntime* rt, JS_MarkFunc* mark) const override {
        JS_MarkValue(rt, fn_, mark);
    }

    float evaluate(const brogameagent::World& world, int teamId) const override {
        JSValue worldV = buildWorldView(ctx_, world);
        JSValue idV    = JS_NewInt32(ctx_, teamId);
        JSValue args[2] = { worldV, idV };
        JSValue res = JS_Call(ctx_, fn_, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx_, worldV);
        JS_FreeValue(ctx_, idV);
        float v = 0.0f;
        if (!JS_IsException(res)) {
            double d = 0.0;
            JS_ToFloat64(ctx_, &d, res);
            v = (float)std::clamp(d, -1.0, 1.0);
        }
        JS_FreeValue(ctx_, res);
        return v;
    }

private:
    JSContext* ctx_;
    JSValue    fn_;
};

// JS-authored single-hero Option. Holds refs to three JS callables +
// a constant name. JS signatures:
//   canInitiate     : (selfView, worldView) -> boolean
//   step            : (selfView, worldView, ticksInOption) -> CombatAction
//   shouldTerminate : (selfView, worldView, ticksInOption) -> boolean
class JsOption : public brogameagent::mcts::Option,
                 public JsCallbackHolder {
public:
    JsOption(JSContext* ctx, std::string name,
             JSValue canInit, JSValue step, JSValue shouldTerm)
        : ctx_(ctx), name_(std::move(name)),
          can_init_(JS_DupValue(ctx, canInit)),
          step_(JS_DupValue(ctx, step)),
          should_term_(JS_DupValue(ctx, shouldTerm)) {}
    ~JsOption() override {
        JS_FreeValue(ctx_, can_init_);
        JS_FreeValue(ctx_, step_);
        JS_FreeValue(ctx_, should_term_);
    }
    void gc_mark(JSRuntime* rt, JS_MarkFunc* mark) const override {
        JS_MarkValue(rt, can_init_, mark);
        JS_MarkValue(rt, step_,     mark);
        JS_MarkValue(rt, should_term_, mark);
    }
    const std::string& name() const override { return name_; }

    bool can_initiate(const brogameagent::Agent& self,
                       const brogameagent::World& world) const override {
        JSValue sv = buildAgentFields(ctx_, self);
        JSValue wv = buildWorldView(ctx_, world);
        JSValue args[2] = { sv, wv };
        JSValue r = JS_Call(ctx_, can_init_, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx_, sv); JS_FreeValue(ctx_, wv);
        bool ok = false;
        if (!JS_IsException(r)) ok = JS_ToBool(ctx_, r) > 0;
        JS_FreeValue(ctx_, r);
        return ok;
    }

    brogameagent::mcts::CombatAction step(
        brogameagent::Agent& self, brogameagent::World& world,
        int ticks_in_option) const override {
        JSValue sv = buildAgentFields(ctx_, self);
        JSValue wv = buildWorldView(ctx_, world);
        JSValue tv = JS_NewInt32(ctx_, ticks_in_option);
        JSValue args[3] = { sv, wv, tv };
        JSValue r = JS_Call(ctx_, step_, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx_, sv); JS_FreeValue(ctx_, wv); JS_FreeValue(ctx_, tv);
        brogameagent::mcts::CombatAction a{};
        if (!JS_IsException(r) && JS_IsObject(r)) a = parseCombatAction(ctx_, r);
        JS_FreeValue(ctx_, r);
        return a;
    }

    bool should_terminate(const brogameagent::Agent& self,
                           const brogameagent::World& world,
                           int ticks_in_option) const override {
        JSValue sv = buildAgentFields(ctx_, self);
        JSValue wv = buildWorldView(ctx_, world);
        JSValue tv = JS_NewInt32(ctx_, ticks_in_option);
        JSValue args[3] = { sv, wv, tv };
        JSValue r = JS_Call(ctx_, should_term_, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx_, sv); JS_FreeValue(ctx_, wv); JS_FreeValue(ctx_, tv);
        bool ok = false;
        if (!JS_IsException(r)) ok = JS_ToBool(ctx_, r) > 0;
        JS_FreeValue(ctx_, r);
        return ok;
    }

private:
    JSContext* ctx_;
    std::string name_;
    JSValue can_init_;
    JSValue step_;
    JSValue should_term_;
};

// JS-authored team option. Signatures are analogous to JsOption but the
// hero arg becomes an array and step returns an array of CombatActions.
class JsTeamOption : public brogameagent::mcts::TeamOption,
                     public JsCallbackHolder {
public:
    JsTeamOption(JSContext* ctx, std::string name,
                 JSValue canInit, JSValue step, JSValue shouldTerm)
        : ctx_(ctx), name_(std::move(name)),
          can_init_(JS_DupValue(ctx, canInit)),
          step_(JS_DupValue(ctx, step)),
          should_term_(JS_DupValue(ctx, shouldTerm)) {}
    ~JsTeamOption() override {
        JS_FreeValue(ctx_, can_init_);
        JS_FreeValue(ctx_, step_);
        JS_FreeValue(ctx_, should_term_);
    }
    void gc_mark(JSRuntime* rt, JS_MarkFunc* mark) const override {
        JS_MarkValue(rt, can_init_, mark);
        JS_MarkValue(rt, step_,     mark);
        JS_MarkValue(rt, should_term_, mark);
    }
    const std::string& name() const override { return name_; }

    JSValue heroesView(const std::vector<brogameagent::Agent*>& heroes) const {
        JSValue arr = JS_NewArray(ctx_);
        for (uint32_t i = 0; i < heroes.size(); i++) {
            if (heroes[i]) JS_SetPropertyUint32(ctx_, arr, i, buildAgentFields(ctx_, *heroes[i]));
            else           JS_SetPropertyUint32(ctx_, arr, i, JS_NULL);
        }
        return arr;
    }

    bool can_initiate(const std::vector<brogameagent::Agent*>& heroes,
                       const brogameagent::World& world) const override {
        JSValue hv = heroesView(heroes);
        JSValue wv = buildWorldView(ctx_, world);
        JSValue args[2] = { hv, wv };
        JSValue r = JS_Call(ctx_, can_init_, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx_, hv); JS_FreeValue(ctx_, wv);
        bool ok = false;
        if (!JS_IsException(r)) ok = JS_ToBool(ctx_, r) > 0;
        JS_FreeValue(ctx_, r);
        return ok;
    }

    std::vector<brogameagent::mcts::CombatAction> step(
        const std::vector<brogameagent::Agent*>& heroes,
        brogameagent::World& world,
        int ticks_in_option) const override {
        JSValue hv = heroesView(heroes);
        JSValue wv = buildWorldView(ctx_, world);
        JSValue tv = JS_NewInt32(ctx_, ticks_in_option);
        JSValue args[3] = { hv, wv, tv };
        JSValue r = JS_Call(ctx_, step_, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx_, hv); JS_FreeValue(ctx_, wv); JS_FreeValue(ctx_, tv);
        std::vector<brogameagent::mcts::CombatAction> out(heroes.size());
        if (!JS_IsException(r) && JS_IsArray(r)) {
            out = parseCombatActionArray(ctx_, r);
            if (out.size() != heroes.size()) out.resize(heroes.size());
        }
        JS_FreeValue(ctx_, r);
        return out;
    }

    bool should_terminate(const std::vector<brogameagent::Agent*>& heroes,
                           const brogameagent::World& world,
                           int ticks_in_option) const override {
        JSValue hv = heroesView(heroes);
        JSValue wv = buildWorldView(ctx_, world);
        JSValue tv = JS_NewInt32(ctx_, ticks_in_option);
        JSValue args[3] = { hv, wv, tv };
        JSValue r = JS_Call(ctx_, should_term_, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx_, hv); JS_FreeValue(ctx_, wv); JS_FreeValue(ctx_, tv);
        bool ok = false;
        if (!JS_IsException(r)) ok = JS_ToBool(ctx_, r) > 0;
        JS_FreeValue(ctx_, r);
        return ok;
    }

private:
    JSContext* ctx_;
    std::string name_;
    JSValue can_init_;
    JSValue step_;
    JSValue should_term_;
};

// JS-authored Commander role-assignment callback. Signature:
//   assign(heroesView[], worldView) -> number[] of role indices
class JsAssigner : public JsCallbackHolder {
public:
    JsAssigner(JSContext* ctx, JSValue fn)
        : ctx_(ctx), fn_(JS_DupValue(ctx, fn)) {}
    ~JsAssigner() override { JS_FreeValue(ctx_, fn_); }
    void gc_mark(JSRuntime* rt, JS_MarkFunc* mark) const override {
        JS_MarkValue(rt, fn_, mark);
    }

    std::vector<int> operator()(const std::vector<brogameagent::Agent*>& heroes,
                                 const brogameagent::World& world) const {
        JSValue hv = JS_NewArray(ctx_);
        for (uint32_t i = 0; i < heroes.size(); i++) {
            if (heroes[i]) JS_SetPropertyUint32(ctx_, hv, i, buildAgentFields(ctx_, *heroes[i]));
            else           JS_SetPropertyUint32(ctx_, hv, i, JS_NULL);
        }
        JSValue wv = buildWorldView(ctx_, world);
        JSValue args[2] = { hv, wv };
        JSValue r = JS_Call(ctx_, fn_, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx_, hv); JS_FreeValue(ctx_, wv);

        std::vector<int> out(heroes.size(), 0);
        if (!JS_IsException(r) && JS_IsArray(r)) {
            JSValue lenVal = JS_GetPropertyStr(ctx_, r, "length");
            int32_t len = 0; JS_ToInt32(ctx_, &len, lenVal);
            JS_FreeValue(ctx_, lenVal);
            int n = std::min(len, (int)heroes.size());
            for (int i = 0; i < n; i++) {
                JSValue v = JS_GetPropertyUint32(ctx_, r, i);
                int32_t idx = 0; JS_ToInt32(ctx_, &idx, v);
                JS_FreeValue(ctx_, v);
                out[i] = idx;
            }
        }
        JS_FreeValue(ctx_, r);
        return out;
    }

private:
    JSContext* ctx_;
    JSValue fn_;
};

} // namespace

static std::shared_ptr<brogameagent::mcts::IRolloutPolicy>
parseRolloutPolicy(JSContext* ctx, JSValueConst opts) {
    JSValue v = JS_GetPropertyStr(ctx, opts, "rolloutPolicy");
    if (auto sp = extractRolloutClassic(ctx, v)) {
        JS_FreeValue(ctx, v);
        return sp;
    }
    if (JS_IsFunction(ctx, v)) {
        auto p = std::make_shared<JsRolloutPolicy>(ctx, v);
        JS_FreeValue(ctx, v);
        return p;
    }
    std::string kind;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { kind = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    if (kind == "aggressive") return std::make_shared<brogameagent::mcts::AggressiveRollout>();
    if (kind == "scripted")   return std::make_shared<brogameagent::mcts::ScriptedRollout>();
    if (kind == "random")     return std::make_shared<brogameagent::mcts::RandomRollout>();
    return nullptr;
}

static brogameagent::mcts::OpponentPolicy
parseOpponentPolicy(JSContext* ctx, JSValueConst opts) {
    std::string kind = readStringProp(ctx, opts, "opponentPolicy");
    if (kind == "aggressive") return brogameagent::mcts::policy_aggressive;
    if (kind == "scripted")   return brogameagent::mcts::policy_scripted;
    if (kind == "idle")       return brogameagent::mcts::policy_idle;
    return {};
}

static brogameagent::mcts::TacticKind parseTacticKindStr(const std::string& s) {
    if (s == "FocusLowestHp") return brogameagent::mcts::TacticKind::FocusLowestHp;
    if (s == "Scatter")       return brogameagent::mcts::TacticKind::Scatter;
    if (s == "Retreat")       return brogameagent::mcts::TacticKind::Retreat;
    return brogameagent::mcts::TacticKind::Hold;
}

static const char* tacticKindStr(brogameagent::mcts::TacticKind k) {
    switch (k) {
        case brogameagent::mcts::TacticKind::FocusLowestHp: return "FocusLowestHp";
        case brogameagent::mcts::TacticKind::Scatter:       return "Scatter";
        case brogameagent::mcts::TacticKind::Retreat:       return "Retreat";
        default:                                            return "Hold";
    }
}

// Accept either { kind: "Hold" } or the bare string "Hold".
static brogameagent::mcts::Tactic parseTactic(JSContext* ctx, JSValueConst v) {
    brogameagent::mcts::Tactic t{};
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { t.kind = parseTacticKindStr(s); JS_FreeCString(ctx, s); }
    } else if (JS_IsObject(v)) {
        std::string k = readStringProp(ctx, v, "kind");
        t.kind = parseTacticKindStr(k);
    }
    return t;
}

static JSValue makeTactic(JSContext* ctx, const brogameagent::mcts::Tactic& t) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "kind", JS_NewString(ctx, tacticKindStr(t.kind)));
    return obj;
}

static brogameagent::mcts::CombatAction parseCombatAction(JSContext* ctx, JSValueConst obj) {
    brogameagent::mcts::CombatAction a;
    a.move_dir     = (brogameagent::mcts::MoveDir)getInt32Prop(ctx, obj, "moveDir", 0);
    a.attack_slot  = (int8_t)getInt32Prop(ctx, obj, "attackSlot", -1);
    a.ability_slot = (int8_t)getInt32Prop(ctx, obj, "abilitySlot", -1);
    return a;
}

JSValue makeCombatAction(JSContext* ctx, const brogameagent::mcts::CombatAction& a) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "moveDir",     JS_NewInt32(ctx, (int)a.move_dir));
    JS_SetPropertyStr(ctx, obj, "attackSlot",  JS_NewInt32(ctx, (int)a.attack_slot));
    JS_SetPropertyStr(ctx, obj, "abilitySlot", JS_NewInt32(ctx, (int)a.ability_slot));
    return obj;
}

static JSValue makeSearchStats(JSContext* ctx, const brogameagent::mcts::SearchStats& s) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "iterations",   JS_NewInt32(ctx, s.iterations));
    JS_SetPropertyStr(ctx, obj, "rootChildren", JS_NewInt32(ctx, s.root_children));
    JS_SetPropertyStr(ctx, obj, "treeSize",     JS_NewInt32(ctx, s.tree_size));
    JS_SetPropertyStr(ctx, obj, "bestMean",     JS_NewFloat64(ctx, s.best_mean));
    JS_SetPropertyStr(ctx, obj, "bestVisits",   JS_NewInt32(ctx, s.best_visits));
    JS_SetPropertyStr(ctx, obj, "elapsedMs",    JS_NewInt32(ctx, s.elapsed_ms));
    JS_SetPropertyStr(ctx, obj, "reusedRoot",   JS_NewBool(ctx, s.reused_root));
    return obj;
}

static std::vector<brogameagent::Agent*>
parseHeroesArray(JSContext* ctx, JSValueConst arr) {
    std::vector<brogameagent::Agent*> out;
    if (!JS_IsArray(arr)) return out;
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);
    out.reserve(len);
    for (int32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        auto* ad = qjsbind::unwrap<AgentData>(ctx, v);
        if (ad) out.push_back(&ad->agent);
        JS_FreeValue(ctx, v);
    }
    return out;
}

static std::vector<brogameagent::mcts::CombatAction>
parseCombatActionArray(JSContext* ctx, JSValueConst arr) {
    std::vector<brogameagent::mcts::CombatAction> out;
    if (!JS_IsArray(arr)) return out;
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);
    out.reserve(len);
    for (int32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        out.push_back(parseCombatAction(ctx, v));
        JS_FreeValue(ctx, v);
    }
    return out;
}

// Build an IPrior from opts.prior. Accepts either a string preset —
// "uniform", "attackBias", "tacticMatch" — or a JS function
// `(selfView, worldView, actions) -> weights[]`.
static std::shared_ptr<brogameagent::mcts::IPrior>
parsePrior(JSContext* ctx, JSValueConst opts) {
    JSValue pv = JS_GetPropertyStr(ctx, opts, "prior");
    if (auto sp = extractPriorShared(ctx, pv)) {
        JS_FreeValue(ctx, pv);
        return sp;
    }
    if (auto sp = extractPriorClassic(ctx, pv)) {
        JS_FreeValue(ctx, pv);
        return sp;
    }
    if (JS_IsFunction(ctx, pv)) {
        auto p = std::make_shared<JsPrior>(ctx, pv);
        JS_FreeValue(ctx, pv);
        return p;
    }
    std::string kind;
    if (JS_IsString(pv)) {
        const char* s = JS_ToCString(ctx, pv);
        if (s) { kind = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, pv);
    if (kind.empty()) return nullptr;
    if (kind == "uniform")    return std::make_shared<brogameagent::mcts::UniformPrior>();
    if (kind == "attackBias") return std::make_shared<brogameagent::mcts::AttackBiasPrior>();
    if (kind == "tacticMatch") {
        auto tp = std::make_shared<brogameagent::mcts::TacticPrior>();
        JSValue tv = JS_GetPropertyStr(ctx, opts, "tactic");
        if (!JS_IsUndefined(tv) && !JS_IsNull(tv)) tp->set_tactic(parseTactic(ctx, tv));
        JS_FreeValue(ctx, tv);
        tp->set_match_weight((float)getDoubleProp(ctx, opts, "tacticMatchWeight", 8.0));
        tp->set_other_weight((float)getDoubleProp(ctx, opts, "tacticOtherWeight", 1.0));
        return tp;
    }
    return nullptr;
}

// Hero-scoped evaluator. Accepts "hpDelta" string or a function
// `(worldView, heroId) -> number in [-1, 1]`.
static std::shared_ptr<brogameagent::mcts::IEvaluator>
parseHeroEvaluator(JSContext* ctx, JSValueConst opts) {
    JSValue v = JS_GetPropertyStr(ctx, opts, "evaluator");
    if (auto se = extractHeroEvaluatorShared(ctx, v)) {
        JS_FreeValue(ctx, v);
        return se;
    }
    if (auto se = extractHeroEvaluatorClassic(ctx, v)) {
        JS_FreeValue(ctx, v);
        return se;
    }
    if (JS_IsFunction(ctx, v)) {
        auto e = std::make_shared<JsEvaluator>(ctx, v);
        JS_FreeValue(ctx, v);
        return e;
    }
    std::string kind;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { kind = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    if (kind == "hpDelta") return std::make_shared<brogameagent::mcts::HpDeltaEvaluator>();
    return nullptr;
}

// Team-scoped evaluator. Accepts "teamHpDelta"/"teamAdvantage"/"teamPosition"
// or a function `(worldView, teamId) -> number in [-1, 1]`.
static std::shared_ptr<brogameagent::mcts::ITeamEvaluator>
parseTeamEvaluator(JSContext* ctx, JSValueConst opts) {
    JSValue v = JS_GetPropertyStr(ctx, opts, "evaluator");
    if (auto sp = extractTeamEvaluatorClassic(ctx, v)) {
        JS_FreeValue(ctx, v);
        return sp;
    }
    if (JS_IsFunction(ctx, v)) {
        auto e = std::make_shared<JsTeamEvaluator>(ctx, v);
        JS_FreeValue(ctx, v);
        return e;
    }
    std::string kind;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { kind = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    if (kind == "teamHpDelta")    return std::make_shared<brogameagent::mcts::TeamHpDeltaEvaluator>();
    if (kind == "teamAdvantage")  return std::make_shared<brogameagent::mcts::TeamAdvantageEvaluator>();
    if (kind == "teamPosition")   return std::make_shared<brogameagent::mcts::TeamPositionEvaluator>();
    return nullptr;
}

// bro.ai.game.createMcts(config?)
//
// Config fields (all optional):
//   iterations, budgetMs, rolloutHorizon, simDt, actionRepeat, uctC, seed,
//   pwAlpha, priorC
//   rolloutPolicy : "random" | "aggressive"
//   opponentPolicy: "idle"   | "aggressive"
//   prior         : "uniform" | "attackBias" | "tacticMatch"
//                   (tacticMatch also reads `tactic`, `tacticMatchWeight`,
//                    `tacticOtherWeight`)
//   evaluator     : "hpDelta"
static JSValue js_createMcts(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* data = new MctsData();
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue opts = argv[0];
        data->mcts.set_config(parseMctsConfig(ctx, opts));
        if (auto p = track_jcb(data->jcb, parseRolloutPolicy(ctx, opts)))
            data->mcts.set_rollout_policy(std::move(p));
        if (auto op = parseOpponentPolicy(ctx, opts)) data->mcts.set_opponent_policy(std::move(op));
        if (auto pr = track_jcb(data->jcb, parsePrior(ctx, opts)))
            data->mcts.set_prior(std::move(pr));
        if (auto ev = track_jcb(data->jcb, parseHeroEvaluator(ctx, opts)))
            data->mcts.set_evaluator(std::move(ev));
    }
    return qjsbind::wrap<MctsData>(ctx, data);
}

// bro.ai.game.createDecoupledMcts(config?)
// Same config surface as createMcts, minus opponentPolicy (both sides are
// searched). Evaluator is hero-scoped (IEvaluator).
static JSValue js_createDecoupledMcts(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* data = new DecoupledMctsData();
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue opts = argv[0];
        data->mcts.set_config(parseMctsConfig(ctx, opts));
        if (auto p = track_jcb(data->jcb, parseRolloutPolicy(ctx, opts)))
            data->mcts.set_rollout_policy(std::move(p));
        if (auto pr = track_jcb(data->jcb, parsePrior(ctx, opts)))
            data->mcts.set_prior(std::move(pr));
        if (auto ev = track_jcb(data->jcb, parseHeroEvaluator(ctx, opts)))
            data->mcts.set_evaluator(std::move(ev));
    }
    return qjsbind::wrap<DecoupledMctsData>(ctx, data);
}

// bro.ai.game.createTeamMcts(config?)
// Cooperative multi-agent MCTS. Evaluator is team-scoped (ITeamEvaluator).
static JSValue js_createTeamMcts(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* data = new TeamMctsData();
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue opts = argv[0];
        data->mcts.set_config(parseMctsConfig(ctx, opts));
        if (auto p = track_jcb(data->jcb, parseRolloutPolicy(ctx, opts)))
            data->mcts.set_rollout_policy(std::move(p));
        if (auto op = parseOpponentPolicy(ctx, opts)) data->mcts.set_opponent_policy(std::move(op));
        if (auto pr = track_jcb(data->jcb, parsePrior(ctx, opts)))
            data->mcts.set_prior(std::move(pr));
        if (auto ev = track_jcb(data->jcb, parseTeamEvaluator(ctx, opts)))
            data->mcts.set_evaluator(std::move(ev));
    }
    return qjsbind::wrap<TeamMctsData>(ctx, data);
}

// bro.ai.game.createTacticMcts(config?)
// Coarse team-tactic planner. Team-scoped evaluator.
static JSValue js_createTacticMcts(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* data = new TacticMctsData();
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue opts = argv[0];
        data->mcts.set_config(parseMctsConfig(ctx, opts));
        if (auto op = parseOpponentPolicy(ctx, opts)) data->mcts.set_opponent_policy(std::move(op));
        if (auto ev = track_jcb(data->jcb, parseTeamEvaluator(ctx, opts)))
            data->mcts.set_evaluator(std::move(ev));
    }
    return qjsbind::wrap<TacticMctsData>(ctx, data);
}

// bro.ai.game.createLayeredPlanner({ tactic?, fine? })
// tactic / fine are MctsConfig objects (same fields as createMcts).
// Top-level opts may also carry rolloutPolicy, opponentPolicy, evaluator
// (team-scoped) — these are shared across both layers.
static JSValue js_createLayeredPlanner(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* data = new LayeredPlannerData();
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue opts = argv[0];
        brogameagent::mcts::LayeredPlanner::Config cfg{};

        JSValue tc = JS_GetPropertyStr(ctx, opts, "tactic");
        if (JS_IsObject(tc)) cfg.tactic_cfg = parseMctsConfig(ctx, tc);
        JS_FreeValue(ctx, tc);

        JSValue fc = JS_GetPropertyStr(ctx, opts, "fine");
        if (JS_IsObject(fc)) cfg.fine_cfg = parseMctsConfig(ctx, fc);
        JS_FreeValue(ctx, fc);

        // Optional TacticPrior weight tuning. Lower match/other ratio lets
        // the fine per-hero search deviate from the committed tactic.
        JSValue tmw = JS_GetPropertyStr(ctx, opts, "tacticMatchWeight");
        if (JS_IsNumber(tmw)) {
            double v = 8.0; JS_ToFloat64(ctx, &v, tmw);
            cfg.tactic_match_weight = (float)v;
        }
        JS_FreeValue(ctx, tmw);
        JSValue tow = JS_GetPropertyStr(ctx, opts, "tacticOtherWeight");
        if (JS_IsNumber(tow)) {
            double v = 1.0; JS_ToFloat64(ctx, &v, tow);
            cfg.tactic_other_weight = (float)v;
        }
        JS_FreeValue(ctx, tow);

        data->planner.set_config(cfg);

        if (auto p = track_jcb(data->jcb, parseRolloutPolicy(ctx, opts)))
            data->planner.set_rollout_policy(std::move(p));
        if (auto op = parseOpponentPolicy(ctx, opts)) data->planner.set_opponent_policy(std::move(op));
        if (auto ev = track_jcb(data->jcb, parseTeamEvaluator(ctx, opts)))
            data->planner.set_team_evaluator(std::move(ev));
    }
    return qjsbind::wrap<LayeredPlannerData>(ctx, data);
}

// bro.ai.game.createOption({ name, canInitiate, step, shouldTerminate })
// Returns an OptionData handle usable in createOptionMcts({ options: [...] }).
// All three callbacks are required; name is required and must be unique
// within an option set (advanceRoot matches by name).
static JSValue js_createOption(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "createOption(spec)");
    }
    std::string name = readStringProp(ctx, argv[0], "name");
    if (name.empty()) return JS_ThrowTypeError(ctx, "createOption: name required");

    JSValue ci = JS_GetPropertyStr(ctx, argv[0], "canInitiate");
    JSValue st = JS_GetPropertyStr(ctx, argv[0], "step");
    JSValue te = JS_GetPropertyStr(ctx, argv[0], "shouldTerminate");
    auto guard = [&](const char* m) {
        JS_FreeValue(ctx, ci); JS_FreeValue(ctx, st); JS_FreeValue(ctx, te);
        return JS_ThrowTypeError(ctx, "%s", m);
    };
    if (!JS_IsFunction(ctx, ci)) return guard("createOption: canInitiate must be a function");
    if (!JS_IsFunction(ctx, st)) return guard("createOption: step must be a function");
    if (!JS_IsFunction(ctx, te)) return guard("createOption: shouldTerminate must be a function");

    auto* d = new OptionData();
    d->option = std::make_shared<JsOption>(ctx, std::move(name), ci, st, te);
    JS_FreeValue(ctx, ci); JS_FreeValue(ctx, st); JS_FreeValue(ctx, te);
    return qjsbind::wrap<OptionData>(ctx, d);
}

// bro.ai.game.createTeamOption({ name, canInitiate, step, shouldTerminate })
// Like createOption but callbacks receive (heroesView, worldView, ticks).
// step returns an array of CombatAction (one per hero, same order).
static JSValue js_createTeamOption(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "createTeamOption(spec)");
    }
    std::string name = readStringProp(ctx, argv[0], "name");
    if (name.empty()) return JS_ThrowTypeError(ctx, "createTeamOption: name required");

    JSValue ci = JS_GetPropertyStr(ctx, argv[0], "canInitiate");
    JSValue st = JS_GetPropertyStr(ctx, argv[0], "step");
    JSValue te = JS_GetPropertyStr(ctx, argv[0], "shouldTerminate");
    auto guard = [&](const char* m) {
        JS_FreeValue(ctx, ci); JS_FreeValue(ctx, st); JS_FreeValue(ctx, te);
        return JS_ThrowTypeError(ctx, "%s", m);
    };
    if (!JS_IsFunction(ctx, ci)) return guard("createTeamOption: canInitiate must be a function");
    if (!JS_IsFunction(ctx, st)) return guard("createTeamOption: step must be a function");
    if (!JS_IsFunction(ctx, te)) return guard("createTeamOption: shouldTerminate must be a function");

    auto* d = new TeamOptionData();
    d->option = std::make_shared<JsTeamOption>(ctx, std::move(name), ci, st, te);
    JS_FreeValue(ctx, ci); JS_FreeValue(ctx, st); JS_FreeValue(ctx, te);
    return qjsbind::wrap<TeamOptionData>(ctx, d);
}

// Parse opts.options as an array of OptionData / TeamOptionData wrappers.
// When jsRefsOut is given, also appends a JS_DupValue'd copy of each
// original wrapper value (the caller owns freeing these) — see the comment
// above OptionMctsData for why marking these, rather than re-walking into
// the shared Option's own gc_mark, is required for correct ref-counting.
template <typename TData, typename TOption>
static std::vector<std::shared_ptr<TOption>>
parseOptionArray(JSContext* ctx, JSValueConst opts, std::vector<JSValue>* jsRefsOut = nullptr) {
    std::vector<std::shared_ptr<TOption>> out;
    JSValue arr = JS_GetPropertyStr(ctx, opts, "options");
    if (JS_IsArray(arr)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        out.reserve(len);
        for (int32_t i = 0; i < len; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, arr, i);
            auto* od = qjsbind::unwrap<TData>(ctx, v);
            if (od && od->option) {
                out.push_back(od->option);
                if (jsRefsOut) jsRefsOut->push_back(JS_DupValue(ctx, v));
            }
            JS_FreeValue(ctx, v);
        }
    }
    JS_FreeValue(ctx, arr);
    return out;
}

// bro.ai.game.createOptionMcts({ options: [Option...], ... })
// Config fields: everything from createMcts plus `options` and
// `optionMaxWindows`. Evaluator is hero-scoped.
static JSValue js_createOptionMcts(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* data = new OptionMctsData();
    data->ctx = ctx;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue opts = argv[0];
        data->mcts.set_config(parseMctsConfig(ctx, opts));
        if (auto op = parseOpponentPolicy(ctx, opts)) data->mcts.set_opponent_policy(std::move(op));
        if (auto ev = track_jcb(data->jcb, parseHeroEvaluator(ctx, opts)))
            data->mcts.set_evaluator(std::move(ev));
        data->options = parseOptionArray<OptionData, brogameagent::mcts::Option>(
            ctx, opts, &data->optionJsRefs);
        if (!data->options.empty()) {
            auto copy = data->options;
            data->mcts.set_options(std::move(copy));
        }
    }
    return qjsbind::wrap<OptionMctsData>(ctx, data);
}

// bro.ai.game.createTeamOptionMcts({ options: [TeamOption...], ... })
// Team-scoped evaluator.
static JSValue js_createTeamOptionMcts(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* data = new TeamOptionMctsData();
    data->ctx = ctx;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue opts = argv[0];
        data->mcts.set_config(parseMctsConfig(ctx, opts));
        if (auto op = parseOpponentPolicy(ctx, opts)) data->mcts.set_opponent_policy(std::move(op));
        if (auto ev = track_jcb(data->jcb, parseTeamEvaluator(ctx, opts)))
            data->mcts.set_evaluator(std::move(ev));
        data->options = parseOptionArray<TeamOptionData, brogameagent::mcts::TeamOption>(
            ctx, opts, &data->optionJsRefs);
        if (!data->options.empty()) {
            auto copy = data->options;
            data->mcts.set_options(std::move(copy));
        }
    }
    return qjsbind::wrap<TeamOptionMctsData>(ctx, data);
}

// bro.ai.game.createCommander({
//   roles: [ { name, options: [Option...], evaluator? }, ... ],
//   replanEveryWindows?: number,
//   roleCfg?: MctsConfig,                 // applied to every per-hero OptionMcts
//   evaluator?: ... | function,           // default hero-scoped evaluator
//   opponentPolicy?: "scripted" | ...,
//   assign?: function(heroesView, worldView) => number[] of role indices
// })
static JSValue js_createCommander(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* data = new CommanderData();
    data->ctx = ctx;
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return qjsbind::wrap<CommanderData>(ctx, data);
    }
    JSValue opts = argv[0];

    brogameagent::mcts::Commander::Config cfg{};
    JSValue rc = JS_GetPropertyStr(ctx, opts, "roleCfg");
    if (JS_IsObject(rc)) cfg.role_cfg = parseMctsConfig(ctx, rc);
    JS_FreeValue(ctx, rc);
    cfg.replan_every_windows = getInt32Prop(ctx, opts, "replanEveryWindows",
                                             cfg.replan_every_windows);
    data->commander.set_config(cfg);

    if (auto op = parseOpponentPolicy(ctx, opts)) data->commander.set_opponent_policy(std::move(op));
    if (auto ev = track_jcb(data->jcb, parseHeroEvaluator(ctx, opts)))
        data->commander.set_default_evaluator(std::move(ev));

    // Roles array.
    JSValue rolesArr = JS_GetPropertyStr(ctx, opts, "roles");
    if (JS_IsArray(rolesArr)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, rolesArr, "length");
        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int32_t i = 0; i < len; i++) {
            JSValue r = JS_GetPropertyUint32(ctx, rolesArr, i);
            if (JS_IsObject(r)) {
                std::string name = readStringProp(ctx, r, "name");
                auto opts_vec = parseOptionArray<OptionData, brogameagent::mcts::Option>(
                    ctx, r, &data->optionJsRefs);
                auto role_eval = track_jcb(data->jcb, parseHeroEvaluator(ctx, r));
                for (auto& sp : opts_vec) data->option_refs.push_back(sp);
                data->commander.add_role(std::move(name), std::move(opts_vec),
                                          std::move(role_eval));
            }
            JS_FreeValue(ctx, r);
        }
    }
    JS_FreeValue(ctx, rolesArr);

    // Optional JS assigner. Stored via shared_ptr so the Commander's
    // std::function captures ownership and the JSValue lives as long as
    // the Commander does. Also tracked in jcb so gc_mark exposes its
    // JSValue callback to QuickJS's cycle GC.
    JSValue assignFn = JS_GetPropertyStr(ctx, opts, "assign");
    if (JS_IsFunction(ctx, assignFn)) {
        auto sp = std::make_shared<JsAssigner>(ctx, assignFn);
        data->jcb.push_back(sp);
        data->commander.set_assigner(
            [sp](const std::vector<brogameagent::Agent*>& heroes,
                 const brogameagent::World& world) {
                return (*sp)(heroes, world);
            });
    }
    JS_FreeValue(ctx, assignFn);

    return qjsbind::wrap<CommanderData>(ctx, data);
}

// bro.ai.game.legalActions(agent, world) → [CombatAction]
static JSValue js_legalActions(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "legalActions(agent, world)");
    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[0]);
    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[1]);
    if (!ad || !wd) return JS_ThrowTypeError(ctx, "invalid agent or world");
    auto acts = brogameagent::mcts::legal_actions(ad->agent, wd->world);
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < acts.size(); i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, makeCombatAction(ctx, acts[i]));
    }
    return arr;
}

// bro.ai.game.legalTactics(world, heroes[]) → [{kind}]
static JSValue js_legalTactics(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "legalTactics(world, heroes)");
    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[0]);
    if (!wd) return JS_ThrowTypeError(ctx, "invalid world");
    auto heroes = parseHeroesArray(ctx, argv[1]);
    auto tactics = brogameagent::mcts::legal_tactics(heroes, wd->world);
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < tactics.size(); i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, makeTactic(ctx, tactics[i]));
    }
    return arr;
}

// bro.ai.game.applyCombatAction(agent, world, action, dt)
//
// Drives one agent through mcts::apply — the exact code path rollouts use.
// Caller is responsible for projectile stepping (handled by the scene's
// auto-ticker calling world.tick each frame). Meant to be called once per
// rAF frame with the real dt; the cached CombatAction is replayed until
// the planner emits a new one.
static JSValue js_applyCombatAction(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "applyCombatAction(agent, world, action, dt)");
    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[0]);
    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[1]);
    if (!ad || !wd) return JS_ThrowTypeError(ctx, "invalid agent or world");
    if (!JS_IsObject(argv[2])) return JS_ThrowTypeError(ctx, "action must be an object");
    double dt = 0;
    JS_ToFloat64(ctx, &dt, argv[3]);
    auto action = parseCombatAction(ctx, argv[2]);
    brogameagent::mcts::apply(ad->agent, wd->world, action, (float)dt);
    return JS_UNDEFINED;
}

// bro.ai.game.tacticToAction(tactic, hero, world) → CombatAction
static JSValue js_tacticToAction(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "tacticToAction(tactic, hero, world)");
    auto t = parseTactic(ctx, argv[0]);
    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[1]);
    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[2]);
    if (!ad || !wd) return JS_ThrowTypeError(ctx, "invalid hero or world");
    auto a = brogameagent::mcts::tactic_to_action(t, ad->agent, wd->world);
    return makeCombatAction(ctx, a);
}

// ═══════════════════════════════════════════════════════════════════════════
// AgentAction parsing helper
// ═══════════════════════════════════════════════════════════════════════════

static brogameagent::AgentAction parseAgentAction(JSContext* ctx, JSValueConst obj) {
    brogameagent::AgentAction a;
    a.moveX = (float)getDoubleProp(ctx, obj, "moveX", 0);
    a.moveZ = (float)getDoubleProp(ctx, obj, "moveZ", 0);
    a.aimYaw = (float)getDoubleProp(ctx, obj, "aimYaw", 0);
    a.aimPitch = (float)getDoubleProp(ctx, obj, "aimPitch", 0);
    a.attackTargetId = getInt32Prop(ctx, obj, "attackTargetId", -1);
    a.useAbilityId = getInt32Prop(ctx, obj, "useAbilityId", -1);
    return a;
}

// ═══════════════════════════════════════════════════════════════════════════
// Install
// ═══════════════════════════════════════════════════════════════════════════

void AIBindings::install(JSContext* ctx) {

    // ─── NavGrid class ─────────────────────────────────────────────────
    {
        qjsbind::Class<NavGridData>(ctx, "AINavGrid", qjsbind::NoGlobal)
            .method("isWalkable",
                [](NavGridData* d, double x, double z) -> bool {
                    return d->grid && d->grid->isWalkable((float)x, (float)z);
                })
            .method("findPath",
                [](NavGridData* d, JSContext* ctx, double fx, double fz, double tx, double tz) -> JSValue {
                    if (!d->grid) return JS_NewArray(ctx);
                    auto path = d->grid->findPath({(float)fx, (float)fz}, {(float)tx, (float)tz});
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < path.size(); i++) {
                        JSValue pt = JS_NewObject(ctx);
                        JS_SetPropertyStr(ctx, pt, "x", JS_NewFloat64(ctx, path[i].x));
                        JS_SetPropertyStr(ctx, pt, "z", JS_NewFloat64(ctx, path[i].y));
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, pt);
                    }
                    return arr;
                })
            .method_raw("addObstacle",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<NavGridData>(ctx, this_val);
                    if (!d || !d->grid || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
                    double padding = 0;
                    if (argc >= 2) JS_ToFloat64(ctx, &padding, argv[1]);
                    d->grid->addObstacle(parseAABB(ctx, argv[0]), (float)padding);
                    return JS_UNDEFINED;
                }, 2);
    }

#ifdef BROGAMEAGENT_HAS_NAVMESH
    // ─── NavMesh class (polygon navmesh — bakeNavMesh / loadNavMesh) ────
    {
        qjsbind::Class<NavMeshData>(ctx, "AINavMesh", qjsbind::NoGlobal)
            .get("valid",
                [](NavMeshData* d) -> bool { return d->mesh && d->mesh->valid(); })
            // findPath(start, end, extents?) → Float32Array of xyz triples,
            // or null when either endpoint fails to snap or no COMPLETE path
            // exists (partial paths are failure, never truncation).
            .method_raw("findPath",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<NavMeshData>(ctx, this_val);
                    if (!d || !d->mesh) return JS_NULL;
                    bromath::Vec3 a, b;
                    if (argc < 2 || !parseVec3Val(ctx, argv[0], a) || !parseVec3Val(ctx, argv[1], b))
                        return JS_ThrowTypeError(ctx, "findPath(start, end, extents?)");
                    auto path = d->mesh->findPath(a, b, parseExtentsArg(ctx, argc, argv, 2));
                    if (path.empty()) return JS_NULL;
                    static_assert(sizeof(bromath::Vec3) == 3 * sizeof(float),
                                  "Vec3 must be tightly packed for the flat copy");
                    return make_float32_array(ctx, &path[0].x, path.size() * 3);
                }, 3)
            // nearestPoint(p, extents?) → {x,y,z} snapped onto the mesh, or
            // null when nothing is within the search extents.
            .method_raw("nearestPoint",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<NavMeshData>(ctx, this_val);
                    if (!d || !d->mesh) return JS_NULL;
                    bromath::Vec3 p;
                    if (argc < 1 || !parseVec3Val(ctx, argv[0], p))
                        return JS_ThrowTypeError(ctx, "nearestPoint(p, extents?)");
                    bromath::Vec3 out;
                    if (!d->mesh->nearestPoint(p, out, parseExtentsArg(ctx, argc, argv, 1)))
                        return JS_NULL;
                    return makeVec3(ctx, out);
                }, 2)
            // raycast(start, end, extents?) → {hit, t, point, normal} — the
            // walkability ray along the mesh surface, not a physics ray.
            .method_raw("raycast",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<NavMeshData>(ctx, this_val);
                    if (!d || !d->mesh) return JS_NULL;
                    bromath::Vec3 a, b;
                    if (argc < 2 || !parseVec3Val(ctx, argv[0], a) || !parseVec3Val(ctx, argv[1], b))
                        return JS_ThrowTypeError(ctx, "raycast(start, end, extents?)");
                    auto hit = d->mesh->raycast(a, b, parseExtentsArg(ctx, argc, argv, 2));
                    JSValue o = JS_NewObject(ctx);
                    JS_SetPropertyStr(ctx, o, "hit", JS_NewBool(ctx, hit.hit));
                    JS_SetPropertyStr(ctx, o, "t", JS_NewFloat64(ctx, hit.t));
                    JS_SetPropertyStr(ctx, o, "point", makeVec3(ctx, hit.point));
                    JS_SetPropertyStr(ctx, o, "normal", makeVec3(ctx, hit.normal));
                    return o;
                }, 3)
            // randomPoint(seed) → {x,y,z} (deterministic per seed), or null
            // when the mesh is empty.
            .method_raw("randomPoint",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<NavMeshData>(ctx, this_val);
                    if (!d || !d->mesh) return JS_NULL;
                    uint32_t seed = 0;
                    if (argc >= 1) JS_ToUint32(ctx, &seed, argv[0]);
                    bromath::Vec3 out;
                    if (!d->mesh->randomPoint(seed, out)) return JS_NULL;
                    return makeVec3(ctx, out);
                }, 1)
            // save() → ArrayBuffer of the baked mesh (cache it to disk;
            // loadNavMesh() restores it without the seconds-scale bake).
            // Dynamic-obstacle (tiled) meshes do not serialize.
            .method_raw("save",
                [](JSContext* ctx, JSValueConst this_val, int, JSValueConst*) -> JSValue {
                    auto* d = qjsbind::unwrap<NavMeshData>(ctx, this_val);
                    if (!d || !d->mesh) return JS_ThrowTypeError(ctx, "save: invalid NavMesh");
                    std::vector<uint8_t> blob;
                    if (!d->mesh->saveTo(blob)) {
                        if (d->mesh->supportsObstacles())
                            return JS_ThrowTypeError(ctx,
                                "save: dynamicObstacles meshes do not serialize");
                        return JS_ThrowInternalError(ctx, "save: NavMesh is not baked");
                    }
                    return JS_NewArrayBufferCopy(ctx, blob.data(), blob.size());
                }, 0)
            // ── Dynamic obstacles (bakeNavMesh({dynamicObstacles: true})) ──
            // True when this mesh can take runtime obstacles.
            .get("supportsObstacles",
                [](NavMeshData* d) -> bool { return d->mesh && d->mesh->supportsObstacles(); })
            // Monotonic surface version: bumps after bake/load and once per
            // applied obstacle batch. Agents repath when it moves.
            .get("generation",
                [](NavMeshData* d) -> double {
                    return d->mesh ? (double)d->mesh->generation() : 0.0; })
            // Active obstacles (added and not removed, including queued ones).
            .get("obstacleCount",
                [](NavMeshData* d) -> int { return d->mesh ? d->mesh->obstacleCount() : 0; })
            // True while queued changes have not been fully applied yet.
            .get("obstaclesPending",
                [](NavMeshData* d) -> bool { return d->mesh && d->mesh->obstaclesPending(); })
            // addObstacle({type:'cylinder', pos, radius, height})
            // addObstacle({type:'box', min, max})
            // addObstacle({type:'box', center, halfExtents, yaw?})   (Y-rotated)
            // → numeric handle for removeObstacle(). Changes apply after the
            // touched tiles rebuild (engine-pumped, or mesh.update()).
            .method_raw("addObstacle",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<NavMeshData>(ctx, this_val);
                    if (!d || !d->mesh) return JS_NULL;
                    if (!d->mesh->supportsObstacles())
                        return JS_ThrowTypeError(ctx,
                            "addObstacle: bake the mesh with dynamicObstacles: true");
                    if (argc < 1 || !JS_IsObject(argv[0]))
                        return JS_ThrowTypeError(ctx, "addObstacle(desc)");
                    const std::string type = qjsbind::get_prop_string(ctx, argv[0], "type");
                    uint32_t id = 0;
                    if (type.empty() || type == "cylinder") {
                        JSValue pv = JS_GetPropertyStr(ctx, argv[0], "pos");
                        bromath::Vec3 pos;
                        const bool okPos = parseVec3Val(ctx, pv, pos);
                        JS_FreeValue(ctx, pv);
                        const double radius = getDoubleProp(ctx, argv[0], "radius", 0);
                        const double height = getDoubleProp(ctx, argv[0], "height", 0);
                        if (!okPos || !(radius > 0) || !(height > 0))
                            return JS_ThrowTypeError(ctx,
                                "addObstacle: cylinder needs {pos, radius > 0, height > 0} "
                                "(pos = center of the base)");
                        id = d->mesh->addObstacle(pos, (float)radius, (float)height);
                    } else if (type == "box") {
                        JSValue minV = JS_GetPropertyStr(ctx, argv[0], "min");
                        JSValue maxV = JS_GetPropertyStr(ctx, argv[0], "max");
                        JSValue ctrV = JS_GetPropertyStr(ctx, argv[0], "center");
                        JSValue extV = JS_GetPropertyStr(ctx, argv[0], "halfExtents");
                        bromath::Vec3 a, b;
                        const bool aabb = parseVec3Val(ctx, minV, a) && parseVec3Val(ctx, maxV, b);
                        const bool obb  = !aabb &&
                            parseVec3Val(ctx, ctrV, a) && parseVec3Val(ctx, extV, b);
                        JS_FreeValue(ctx, minV); JS_FreeValue(ctx, maxV);
                        JS_FreeValue(ctx, ctrV); JS_FreeValue(ctx, extV);
                        if (aabb) {
                            id = d->mesh->addBoxObstacle(a, b);
                        } else if (obb) {
                            const double yaw = getDoubleProp(ctx, argv[0], "yaw", 0);
                            id = d->mesh->addBoxObstacle(a, b, (float)yaw);
                        } else {
                            return JS_ThrowTypeError(ctx,
                                "addObstacle: box needs {min, max} or {center, halfExtents, yaw?}");
                        }
                    } else {
                        return JS_ThrowTypeError(ctx,
                            "addObstacle: type must be 'cylinder' or 'box'");
                    }
                    if (id == 0)
                        return JS_ThrowInternalError(ctx, "addObstacle: %s",
                                                     d->mesh->lastError().c_str());
                    return JS_NewUint32(ctx, id);
                }, 1)
            // removeObstacle(handle) → bool. False for unknown/stale handles
            // (safe to double-remove). The surface restores after the touched
            // tiles rebuild.
            .method_raw("removeObstacle",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<NavMeshData>(ctx, this_val);
                    if (!d || !d->mesh) return JS_FALSE;
                    uint32_t id = 0;
                    if (argc >= 1) JS_ToUint32(ctx, &id, argv[0]);
                    return JS_NewBool(ctx, d->mesh->removeObstacle(id));
                }, 1)
            // update(dt?) → bool: pump pending obstacle changes (one touched-
            // tile rebuild per call); true once fully up to date. The engine
            // pumps automatically every frame — call this only for immediate,
            // synchronous application: while (!mesh.update()) {}
            .method_raw("update",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<NavMeshData>(ctx, this_val);
                    if (!d || !d->mesh) return JS_TRUE;
                    double dt = 1.0 / 60.0;
                    if (argc >= 1 && JS_IsNumber(argv[0])) JS_ToFloat64(ctx, &dt, argv[0]);
                    return JS_NewBool(ctx, d->mesh->update((float)dt));
                }, 1);
    }
#endif  // BROGAMEAGENT_HAS_NAVMESH

    // ─── Unit class (accessor proxy for Agent's Unit) ──────────────────
    {
        qjsbind::Class<UnitData>(ctx, "AIUnit", qjsbind::NoGlobal | qjsbind::NoDestructor)
            .prop("id",
                [](UnitData* d) -> int { return d->agentRef ? d->agentRef->unit().id : 0; },
                [](UnitData* d, int v) { if (d->agentRef) d->agentRef->unit().id = v; })
            .prop("teamId",
                [](UnitData* d) -> int { return d->agentRef ? d->agentRef->unit().teamId : 0; },
                [](UnitData* d, int v) { if (d->agentRef) d->agentRef->unit().teamId = v; })
            .prop("hp",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().hp : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().hp = (float)v; })
            .prop("maxHp",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().maxHp : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().maxHp = (float)v; })
            .prop("mana",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().mana : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().mana = (float)v; })
            .prop("maxMana",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().maxMana : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().maxMana = (float)v; })
            .prop("damage",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().damage : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().damage = (float)v; })
            .prop("attackRange",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().attackRange : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().attackRange = (float)v; })
            .prop("attacksPerSec",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().attacksPerSec : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().attacksPerSec = (float)v; })
            .prop("armor",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().armor : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().armor = (float)v; })
            .prop("magicResist",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().magicResist : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().magicResist = (float)v; })
            .prop("moveSpeed",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().moveSpeed : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().moveSpeed = (float)v; })
            .prop("radius",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().radius : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().radius = (float)v; })
            .prop("manaRegenPerSec",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().manaRegenPerSec : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().manaRegenPerSec = (float)v; })
            .get("alive",
                [](UnitData* d) -> bool { return d->agentRef && d->agentRef->unit().alive(); })
            .get("effectiveArmor",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().effectiveArmor() : 0; })
            .get("effectiveDamage",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().effectiveDamage() : 0; })
            .get("effectiveMoveSpeed",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().effectiveMoveSpeed() : 0; })
            .method("tickCooldowns",
                [](UnitData* d, double dt) { if (d->agentRef) d->agentRef->unit().tickCooldowns((float)dt); })
            .method("setAbilitySlot",
                [](UnitData* d, int slot, int abilityId) {
                    if (!d->agentRef) return;
                    if (slot < 0 || slot >= brogameagent::Unit::MAX_ABILITIES) return;
                    d->agentRef->unit().abilitySlot[slot] = abilityId;
                })
            .method("getAbilitySlot",
                [](UnitData* d, int slot) -> int {
                    if (!d->agentRef) return -1;
                    if (slot < 0 || slot >= brogameagent::Unit::MAX_ABILITIES) return -1;
                    return d->agentRef->unit().abilitySlot[slot];
                })
            .method("getAbilityCooldown",
                [](UnitData* d, int slot) -> double {
                    if (!d->agentRef) return 0.0;
                    if (slot < 0 || slot >= brogameagent::Unit::MAX_ABILITIES) return 0.0;
                    return d->agentRef->unit().abilityCooldowns[slot];
                })
            .method("takeDamage",
                [](UnitData* d, double amount, std::string kind) -> double {
                    if (!d->agentRef) return 0;
                    return d->agentRef->unit().takeDamage((float)amount, parseDamageKind(kind.c_str()));
                })
            // Additive buffs (armor / magic resist): magnitude + remaining duration.
            .prop("armorBonus",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().armorBonus : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().armorBonus = (float)v; })
            .prop("armorBonusRemaining",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().armorBonusRemaining : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().armorBonusRemaining = (float)v; })
            .prop("magicResistBonus",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().magicResistBonus : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().magicResistBonus = (float)v; })
            .prop("magicResistBonusRemaining",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().magicResistBonusRemaining : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().magicResistBonusRemaining = (float)v; })
            // Multiplicative buffs (damage / attacks / move speed).
            .prop("damageMul",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().damageMul : 1.0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().damageMul = (float)v; })
            .prop("damageMulRemaining",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().damageMulRemaining : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().damageMulRemaining = (float)v; })
            .prop("attacksMul",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().attacksMul : 1.0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().attacksMul = (float)v; })
            .prop("attacksMulRemaining",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().attacksMulRemaining : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().attacksMulRemaining = (float)v; })
            .prop("moveSpeedMul",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().moveSpeedMul : 1.0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().moveSpeedMul = (float)v; })
            .prop("moveSpeedMulRemaining",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().moveSpeedMulRemaining : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().moveSpeedMulRemaining = (float)v; })
            // Stealth (dodge chance).
            .prop("stealthChance",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().stealthChance : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().stealthChance = (float)v; })
            .prop("stealthChanceRemaining",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().stealthChanceRemaining : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().stealthChanceRemaining = (float)v; })
            // Damage-over-time.
            .prop("dotDps",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().dotDps : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().dotDps = (float)v; })
            .prop("dotRemaining",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().dotRemaining : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().dotRemaining = (float)v; })
            .prop("dotSourceId",
                [](UnitData* d) -> int { return d->agentRef ? d->agentRef->unit().dotSourceId : -1; },
                [](UnitData* d, int v) { if (d->agentRef) d->agentRef->unit().dotSourceId = v; })
            .prop("dotKind",
                [](UnitData* d) -> std::string {
                    return d->agentRef ? damageKindStr(d->agentRef->unit().dotKind) : "physical";
                },
                [](UnitData* d, std::string s) { if (d->agentRef) d->agentRef->unit().dotKind = parseDamageKind(s.c_str()); })
            // Heal-over-time.
            .prop("hotRate",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().hotRate : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().hotRate = (float)v; })
            .prop("hotRemaining",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().hotRemaining : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().hotRemaining = (float)v; })
            // Effective stats that factor in buffs (read-only helpers).
            .get("effectiveMagicResist",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().effectiveMagicResist() : 0; })
            .get("effectiveAttacksPerSec",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().effectiveAttacksPerSec() : 0; })
            // attackKind (baseline damage type from auto-attacks).
            .prop("attackKind",
                [](UnitData* d) -> std::string {
                    return d->agentRef ? damageKindStr(d->agentRef->unit().attackKind) : "physical";
                },
                [](UnitData* d, std::string s) { if (d->agentRef) d->agentRef->unit().attackKind = parseDamageKind(s.c_str()); })
            // attackCooldown is read/writable.
            .prop("attackCooldown",
                [](UnitData* d) -> double { return d->agentRef ? d->agentRef->unit().attackCooldown : 0; },
                [](UnitData* d, double v) { if (d->agentRef) d->agentRef->unit().attackCooldown = (float)v; });
    }

    // ─── Agent class ───────────────────────────────────────────────────
    {
        qjsbind::Class<AgentData>(ctx, "AIAgent", qjsbind::NoGlobal)
            .method("setTarget",
                [](AgentData* d, double x, double z) { d->agent.setTarget((float)x, (float)z); })
            .method("clearTarget",
                [](AgentData* d) { d->agent.clearTarget(); })
            .method("update",
                [](AgentData* d, double dt) { d->agent.update((float)dt); })
            .method("setPosition",
                [](AgentData* d, double x, double z) { d->agent.setPosition((float)x, (float)z); })
            .method("setYaw",
                [](AgentData* d, double yaw) { d->agent.setYaw((float)yaw); })
            .method("setSpeed",
                [](AgentData* d, double s) { d->agent.setSpeed((float)s); })
            .method("setRadius",
                [](AgentData* d, double r) { d->agent.setRadius((float)r); })
            .method("setMaxAccel",
                [](AgentData* d, double v) { d->agent.setMaxAccel((float)v); })
            .method("setMaxTurnRate",
                [](AgentData* d, double v) { d->agent.setMaxTurnRate((float)v); })
            .method("aimAt",
                [](AgentData* d, JSContext* ctx, double tx, double ty, double tz, double eyeH) -> JSValue {
                    auto aim = d->agent.aimAt((float)tx, (float)ty, (float)tz, (float)eyeH);
                    return makeAimResult(ctx, aim);
                })
            .method_raw("applyAction",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<AgentData>(ctx, this_val);
                    if (!d || argc < 2) return JS_ThrowTypeError(ctx, "applyAction(action, dt)");
                    if (!JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "action must be an object");
                    double dt = 0;
                    JS_ToFloat64(ctx, &dt, argv[1]);
                    d->agent.applyAction(parseAgentAction(ctx, argv[0]), (float)dt);
                    return JS_UNDEFINED;
                }, 2)
            .method_raw("setNavGrid",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<AgentData>(ctx, this_val);
                    if (!d || argc < 1) return JS_UNDEFINED;
                    auto* gd = qjsbind::unwrap<NavGridData>(ctx, argv[0]);
                    if (gd && gd->grid) {
                        d->agent.setNavGrid(gd->grid.get());
                        // Hold a ref to prevent GC
                        JS_SetPropertyStr(ctx, this_val, "__navGrid", JS_DupValue(ctx, argv[0]));
                    }
                    return JS_UNDEFINED;
                }, 1)
            .method_raw("setAvoidance",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<AgentData>(ctx, this_val);
                    if (!d || argc < 1) return JS_UNDEFINED;
                    applyAgentAvoidanceOpts(ctx, argv[0], d->agent);
                    return JS_UNDEFINED;
                }, 1)
            .get("x", [](AgentData* d) -> double { return d->agent.x(); })
            .get("z", [](AgentData* d) -> double { return d->agent.z(); })
            .get("yaw", [](AgentData* d) -> double { return d->agent.yaw(); })
            .get("aimYaw", [](AgentData* d) -> double { return d->agent.aimYaw(); })
            .get("aimPitch", [](AgentData* d) -> double { return d->agent.aimPitch(); })
            .get("hasTarget", [](AgentData* d) -> bool { return d->agent.hasTarget(); })
            .get("atTarget", [](AgentData* d) -> bool { return d->agent.atTarget(); })
            .get("unit",
                [](AgentData* d, JSContext* ctx) -> JSValue {
                    // Lazily allocate a proxy per AgentData. The proxy is
                    // kept alive by AgentData::unitProxy, so wrap_unowned
                    // can safely return a JS handle into it.
                    if (!d->unitProxy) {
                        d->unitProxy = std::make_unique<UnitData>();
                    }
                    d->unitProxy->agentRef = &d->agent;
                    return qjsbind::wrap_unowned<UnitData>(ctx, d->unitProxy.get());
                })
            .get("velocity",
                [](AgentData* d, JSContext* ctx) -> JSValue {
                    auto v = d->agent.velocity();
                    JSValue obj = JS_NewObject(ctx);
                    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, v.x));
                    JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, v.y));
                    return obj;
                })
            .get("path",
                [](AgentData* d, JSContext* ctx) -> JSValue {
                    const auto& path = d->agent.path();
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < path.size(); i++) {
                        JSValue pt = JS_NewObject(ctx);
                        JS_SetPropertyStr(ctx, pt, "x", JS_NewFloat64(ctx, path[i].x));
                        JS_SetPropertyStr(ctx, pt, "z", JS_NewFloat64(ctx, path[i].y));
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, pt);
                    }
                    return arr;
                });
    }

    // ─── World class ───────────────────────────────────────────────────
    {
        qjsbind::Class<WorldData>(ctx, "AIWorld", qjsbind::NoGlobal)
            .method_raw("addAgent",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, this_val);
                    if (!wd || argc < 1) return JS_UNDEFINED;
                    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[0]);
                    if (!ad) return JS_ThrowTypeError(ctx, "expected an Agent");
                    wd->world.addAgent(&ad->agent);
                    // Store a reference to prevent GC of the agent
                    JSValue agents = JS_GetPropertyStr(ctx, this_val, "__agents");
                    if (!JS_IsArray(agents)) {
                        JS_FreeValue(ctx, agents);
                        agents = JS_NewArray(ctx);
                        JS_SetPropertyStr(ctx, this_val, "__agents", JS_DupValue(ctx, agents));
                    }
                    JSValue lenVal = JS_GetPropertyStr(ctx, agents, "length");
                    int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
                    JS_SetPropertyUint32(ctx, agents, (uint32_t)len, JS_DupValue(ctx, argv[0]));
                    JS_FreeValue(ctx, agents);
                    return JS_UNDEFINED;
                }, 1)
            .method_raw("removeAgent",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, this_val);
                    if (!wd || argc < 1) return JS_UNDEFINED;
                    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[0]);
                    if (!ad) return JS_UNDEFINED;
                    wd->world.removeAgent(&ad->agent);
                    return JS_UNDEFINED;
                }, 1)
            .method_raw("addObstacle",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, this_val);
                    if (!wd || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
                    wd->world.addObstacle(parseAABB(ctx, argv[0]));
                    return JS_UNDEFINED;
                }, 1)
            .method("tick",
                [](WorldData* d, double dt) { d->world.tick((float)dt); })
            .method_raw("setAvoidance",
                // world.setAvoidance(true|false) or
                // world.setAvoidance({ enabled?, navGrid? }) — enables the
                // ORCA local-avoidance pass in tick(). Passing a navGrid
                // rebases the world's avoidance-only walls on that grid's
                // obstacle boxes, so agents steer around the same geometry
                // they path around (with createNavGrid({fromPhysics}) that
                // makes avoidance physics-aware for free). Boxes are copied;
                // no reference to the grid is kept.
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, this_val);
                    if (!wd || argc < 1) return JS_UNDEFINED;
                    if (JS_IsBool(argv[0])) {
                        wd->world.setAvoidanceEnabled(JS_ToBool(ctx, argv[0]));
                        return JS_UNDEFINED;
                    }
                    if (!JS_IsObject(argv[0]))
                        return JS_ThrowTypeError(ctx, "setAvoidance(bool | {enabled?, navGrid?})");
                    bool enabled = getBoolProp(ctx, argv[0], "enabled", true);
                    JSValue gv = JS_GetPropertyStr(ctx, argv[0], "navGrid");
                    if (JS_IsObject(gv)) {
                        auto* gd = qjsbind::unwrap<NavGridData>(ctx, gv);
                        if (!gd || !gd->grid) {
                            JS_FreeValue(ctx, gv);
                            return JS_ThrowTypeError(ctx,
                                "setAvoidance: navGrid must be a createNavGrid() object");
                        }
                        wd->world.clearAvoidanceObstacles();
                        for (const auto& box : gd->grid->obstacles())
                            wd->world.addAvoidanceObstacle(box);
                    }
                    JS_FreeValue(ctx, gv);
                    wd->world.setAvoidanceEnabled(enabled);
                    return JS_UNDEFINED;
                }, 1)
            .get("avoidanceEnabled",
                [](WorldData* d) -> bool { return d->world.avoidanceEnabled(); })
            .method_raw("spawnProjectile",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, this_val);
                    if (!wd || argc < 1 || !JS_IsObject(argv[0])) return JS_NewInt32(ctx, -1);
                    JSValue opts = argv[0];
                    brogameagent::Projectile p;
                    p.ownerId = getInt32Prop(ctx, opts, "ownerId", -1);
                    p.teamId  = getInt32Prop(ctx, opts, "teamId", 0);
                    p.targetId = getInt32Prop(ctx, opts, "targetId", -1);
                    p.x = (float)getDoubleProp(ctx, opts, "x", 0);
                    p.z = (float)getDoubleProp(ctx, opts, "z", 0);
                    p.vx = (float)getDoubleProp(ctx, opts, "vx", 0);
                    p.vz = (float)getDoubleProp(ctx, opts, "vz", 0);
                    p.speed = (float)getDoubleProp(ctx, opts, "speed", 20);
                    p.radius = (float)getDoubleProp(ctx, opts, "radius", 0.3);
                    p.damage = (float)getDoubleProp(ctx, opts, "damage", 0);
                    p.remainingLife = (float)getDoubleProp(ctx, opts, "remainingLife", 2);
                    p.splashRadius = (float)getDoubleProp(ctx, opts, "splashRadius", 0);
                    p.maxHits = getInt32Prop(ctx, opts, "maxHits", 0);

                    // Parse kind string
                    JSValue kindVal = JS_GetPropertyStr(ctx, opts, "kind");
                    if (JS_IsString(kindVal)) {
                        const char* s = JS_ToCString(ctx, kindVal);
                        p.kind = parseDamageKind(s);
                        JS_FreeCString(ctx, s);
                    }
                    JS_FreeValue(ctx, kindVal);

                    // Parse mode string
                    JSValue modeVal = JS_GetPropertyStr(ctx, opts, "mode");
                    if (JS_IsString(modeVal)) {
                        const char* s = JS_ToCString(ctx, modeVal);
                        if (s && strcmp(s, "pierce") == 0) p.mode = brogameagent::ProjectileMode::Pierce;
                        else if (s && strcmp(s, "aoe") == 0) p.mode = brogameagent::ProjectileMode::AoE;
                        JS_FreeCString(ctx, s);
                    }
                    JS_FreeValue(ctx, modeVal);

                    int id = wd->world.spawnProjectile(p);
                    return JS_NewInt32(ctx, id);
                }, 1)
            .method_raw("nearestEnemy",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, this_val);
                    if (!wd || argc < 1) return JS_NULL;
                    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[0]);
                    if (!ad) return JS_NULL;
                    auto* enemy = wd->world.nearestEnemy(ad->agent);
                    if (!enemy) return JS_NULL;
                    // Find the matching JS agent from __agents
                    JSValue agents = JS_GetPropertyStr(ctx, this_val, "__agents");
                    if (JS_IsArray(agents)) {
                        JSValue lenVal = JS_GetPropertyStr(ctx, agents, "length");
                        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
                        for (int32_t i = 0; i < len; i++) {
                            JSValue agentVal = JS_GetPropertyUint32(ctx, agents, i);
                            auto* candidate = qjsbind::unwrap<AgentData>(ctx, agentVal);
                            if (candidate && &candidate->agent == enemy) {
                                JS_FreeValue(ctx, agents);
                                return agentVal;
                            }
                            JS_FreeValue(ctx, agentVal);
                        }
                    }
                    JS_FreeValue(ctx, agents);
                    return JS_NULL;
                }, 1)
            .method_raw("enemiesInRange",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, this_val);
                    if (!wd || argc < 2) return JS_NewArray(ctx);
                    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[0]);
                    if (!ad) return JS_NewArray(ctx);
                    double range = 0;
                    JS_ToFloat64(ctx, &range, argv[1]);
                    auto enemies = wd->world.enemiesInRange(ad->agent, (float)range);

                    // Build result array by matching C++ pointers to JS agent refs
                    JSValue result = JS_NewArray(ctx);
                    JSValue agents = JS_GetPropertyStr(ctx, this_val, "__agents");
                    int idx = 0;
                    for (auto* enemy : enemies) {
                        if (JS_IsArray(agents)) {
                            JSValue lenVal = JS_GetPropertyStr(ctx, agents, "length");
                            int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
                            for (int32_t i = 0; i < len; i++) {
                                JSValue agentVal = JS_GetPropertyUint32(ctx, agents, i);
                                auto* candidate = qjsbind::unwrap<AgentData>(ctx, agentVal);
                                if (candidate && &candidate->agent == enemy) {
                                    JS_SetPropertyUint32(ctx, result, idx++, agentVal);
                                    goto next_enemy;
                                }
                                JS_FreeValue(ctx, agentVal);
                            }
                        }
                        next_enemy:;
                    }
                    JS_FreeValue(ctx, agents);
                    return result;
                }, 2)
            .method_raw("findById",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, this_val);
                    if (!wd || argc < 1) return JS_NULL;
                    int32_t id = 0;
                    JS_ToInt32(ctx, &id, argv[0]);
                    auto* found = wd->world.findById(id);
                    if (!found) return JS_NULL;
                    // Find matching JS agent
                    JSValue agents = JS_GetPropertyStr(ctx, this_val, "__agents");
                    if (JS_IsArray(agents)) {
                        JSValue lenVal = JS_GetPropertyStr(ctx, agents, "length");
                        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
                        for (int32_t i = 0; i < len; i++) {
                            JSValue agentVal = JS_GetPropertyUint32(ctx, agents, i);
                            auto* candidate = qjsbind::unwrap<AgentData>(ctx, agentVal);
                            if (candidate && &candidate->agent == found) {
                                JS_FreeValue(ctx, agents);
                                return agentVal;
                            }
                            JS_FreeValue(ctx, agentVal);
                        }
                    }
                    JS_FreeValue(ctx, agents);
                    return JS_NULL;
                }, 1)
            .method_raw("resolveAttack",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, this_val);
                    if (!wd || argc < 2) return JS_FALSE;
                    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[0]);
                    if (!ad) return JS_FALSE;
                    int32_t targetId = 0;
                    JS_ToInt32(ctx, &targetId, argv[1]);
                    return JS_NewBool(ctx, wd->world.resolveAttack(ad->agent, targetId));
                }, 2)
            .method_raw("resolveAbility",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, this_val);
                    if (!wd || argc < 3) return JS_FALSE;
                    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[0]);
                    if (!ad) return JS_FALSE;
                    int32_t slot = 0, targetId = -1;
                    JS_ToInt32(ctx, &slot, argv[1]);
                    JS_ToInt32(ctx, &targetId, argv[2]);
                    return JS_NewBool(ctx, wd->world.resolveAbility(ad->agent, slot, targetId));
                }, 3)
            .method_raw("dealDamage",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, this_val);
                    if (!wd || argc < 3) return JS_NewFloat64(ctx, 0);
                    auto* attacker = qjsbind::unwrap<AgentData>(ctx, argv[0]);
                    auto* target = qjsbind::unwrap<AgentData>(ctx, argv[1]);
                    if (!attacker || !target) return JS_NewFloat64(ctx, 0);
                    double amount = 0;
                    JS_ToFloat64(ctx, &amount, argv[2]);
                    brogameagent::DamageKind kind = brogameagent::DamageKind::Physical;
                    if (argc >= 4 && JS_IsString(argv[3])) {
                        const char* s = JS_ToCString(ctx, argv[3]);
                        kind = parseDamageKind(s);
                        JS_FreeCString(ctx, s);
                    }
                    float result = wd->world.dealDamage(attacker->agent, target->agent, (float)amount, kind);
                    return JS_NewFloat64(ctx, result);
                }, 4)
            .method_raw("registerAbility",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, this_val);
                    if (!wd || argc < 2) return JS_UNDEFINED;
                    int32_t abilityId = 0;
                    JS_ToInt32(ctx, &abilityId, argv[0]);
                    if (!JS_IsObject(argv[1])) return JS_ThrowTypeError(ctx, "spec must be an object");
                    JSValue spec = argv[1];

                    brogameagent::AbilitySpec s;
                    s.cooldown = (float)getDoubleProp(ctx, spec, "cooldown", 1);
                    s.manaCost = (float)getDoubleProp(ctx, spec, "manaCost", 0);
                    s.range    = (float)getDoubleProp(ctx, spec, "range", 0);

                    // JS callback fn(caster, world, targetId)
                    //
                    // Deliberately holds NO owned JSValue refs in the C++
                    // lambda. An earlier version captured JS_DupValue'd refs
                    // to both the callback and this_val; the World (owned by
                    // this wrapper) storing a strong ref back to its own
                    // wrapper is a cycle the GC cannot see, so no AIWorld
                    // with a JS ability could ever be collected — every
                    // match reset leaked a World, and JS_FreeRuntime's
                    // leak assert aborted windowed Debug on exit. Instead
                    // the callback is parked in a __abilityFns table ON the
                    // wrapper (GC-visible, like __agents), and the lambda
                    // captures this_val raw: the lambda can only run while
                    // the World is alive, and the World is owned by the
                    // wrapper, so this_val is valid whenever it fires.
                    JSValue fnVal = JS_GetPropertyStr(ctx, spec, "fn");
                    if (JS_IsFunction(ctx, fnVal)) {
                        JSValue tbl = JS_GetPropertyStr(ctx, this_val, "__abilityFns");
                        if (!JS_IsObject(tbl)) {
                            JS_FreeValue(ctx, tbl);
                            tbl = JS_NewObject(ctx);
                            JS_SetPropertyStr(ctx, this_val, "__abilityFns", JS_DupValue(ctx, tbl));
                        }
                        const std::string fnKey = std::to_string(abilityId);
                        JS_SetPropertyStr(ctx, tbl, fnKey.c_str(), JS_DupValue(ctx, fnVal));
                        JS_FreeValue(ctx, tbl);
                        s.fn = [ctx, this_val, fnKey](
                                   brogameagent::Agent& caster,
                                   brogameagent::World& /*world*/,
                                   int targetId) {
                            // Find the JS agent for the caster
                            JSValue casterVal = JS_UNDEFINED;
                            JSValue agents = JS_GetPropertyStr(ctx, this_val, "__agents");
                            if (JS_IsArray(agents)) {
                                JSValue lenVal2 = JS_GetPropertyStr(ctx, agents, "length");
                                int32_t len2 = 0; JS_ToInt32(ctx, &len2, lenVal2); JS_FreeValue(ctx, lenVal2);
                                for (int32_t i = 0; i < len2; i++) {
                                    JSValue av = JS_GetPropertyUint32(ctx, agents, i);
                                    auto* ad = qjsbind::unwrap<AgentData>(ctx, av);
                                    if (ad && &ad->agent == &caster) {
                                        casterVal = av;
                                        break;
                                    }
                                    JS_FreeValue(ctx, av);
                                }
                            }
                            JS_FreeValue(ctx, agents);

                            JSValue tbl2 = JS_GetPropertyStr(ctx, this_val, "__abilityFns");
                            JSValue fnRef = JS_GetPropertyStr(ctx, tbl2, fnKey.c_str());
                            JS_FreeValue(ctx, tbl2);
                            JSValue args[3] = { casterVal, this_val, JS_NewInt32(ctx, targetId) };
                            JSValue ret = JS_Call(ctx, fnRef, JS_UNDEFINED, 3, args);
                            JS_FreeValue(ctx, ret);
                            JS_FreeValue(ctx, fnRef);
                            JS_FreeValue(ctx, casterVal);
                            JS_FreeValue(ctx, args[2]);
                        };
                    }
                    JS_FreeValue(ctx, fnVal);

                    wd->world.registerAbility(abilityId, std::move(s));
                    return JS_UNDEFINED;
                }, 2)
            .get("events",
                [](WorldData* d, JSContext* ctx) -> JSValue {
                    const auto& events = d->world.events();
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < events.size(); i++)
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, makeDamageEvent(ctx, events[i]));
                    return arr;
                })
            .method("clearEvents",
                [](WorldData* d) { d->world.clearEvents(); })
            .method("seed",
                [](WorldData* d, double s) { d->world.seed((uint64_t)s); })
            .get("agents",
                [](WorldData* d, JSContext* ctx) -> JSValue {
                    const auto& agents = d->world.agents();
                    JSValue arr = JS_NewArray(ctx);
                    JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, (int)agents.size()));
                    return arr;
                })
            .get("agentCount",
                [](WorldData* d) -> int { return (int)d->world.agents().size(); })
            .get("projectiles",
                [](WorldData* d, JSContext* ctx) -> JSValue {
                    const auto& projs = d->world.projectiles();
                    JSValue arr = JS_NewArray(ctx);
                    int idx = 0;
                    for (const auto& p : projs) {
                        if (!p.alive) continue;
                        JSValue obj = JS_NewObject(ctx);
                        JS_SetPropertyStr(ctx, obj, "id", JS_NewInt32(ctx, p.id));
                        JS_SetPropertyStr(ctx, obj, "ownerId", JS_NewInt32(ctx, p.ownerId));
                        JS_SetPropertyStr(ctx, obj, "teamId", JS_NewInt32(ctx, p.teamId));
                        JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, p.x));
                        JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, p.z));
                        JS_SetPropertyStr(ctx, obj, "vx", JS_NewFloat64(ctx, p.vx));
                        JS_SetPropertyStr(ctx, obj, "vz", JS_NewFloat64(ctx, p.vz));
                        JS_SetPropertyStr(ctx, obj, "speed", JS_NewFloat64(ctx, p.speed));
                        JS_SetPropertyStr(ctx, obj, "damage", JS_NewFloat64(ctx, p.damage));
                        JS_SetPropertyStr(ctx, obj, "alive", JS_NewBool(ctx, p.alive));
                        const char* modeStr = "single";
                        if (p.mode == brogameagent::ProjectileMode::Pierce) modeStr = "pierce";
                        else if (p.mode == brogameagent::ProjectileMode::AoE) modeStr = "aoe";
                        JS_SetPropertyStr(ctx, obj, "mode", JS_NewString(ctx, modeStr));
                        JS_SetPropertyUint32(ctx, arr, idx++, obj);
                    }
                    return arr;
                })
            .get("obstacles",
                [](WorldData* d, JSContext* ctx) -> JSValue {
                    const auto& obs = d->world.obstacles();
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < obs.size(); i++) {
                        JSValue obj = JS_NewObject(ctx);
                        JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, obs[i].cx));
                        JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, obs[i].cz));
                        JS_SetPropertyStr(ctx, obj, "hw", JS_NewFloat64(ctx, obs[i].hw));
                        JS_SetPropertyStr(ctx, obj, "hd", JS_NewFloat64(ctx, obs[i].hd));
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, obj);
                    }
                    return arr;
                })
            .method_raw("snapshot",
                [](JSContext* ctx, JSValueConst this_val, int, JSValueConst*) -> JSValue {
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, this_val);
                    if (!wd) return JS_NULL;
                    auto snap = wd->world.snapshot();

                    JSValue obj = JS_NewObject(ctx);
                    // Agents
                    JSValue agentsArr = JS_NewArray(ctx);
                    for (size_t i = 0; i < snap.agents.size(); i++) {
                        const auto& a = snap.agents[i];
                        JSValue ao = JS_NewObject(ctx);
                        JS_SetPropertyStr(ctx, ao, "id", JS_NewInt32(ctx, a.id));
                        JS_SetPropertyStr(ctx, ao, "x", JS_NewFloat64(ctx, a.x));
                        JS_SetPropertyStr(ctx, ao, "z", JS_NewFloat64(ctx, a.z));
                        JS_SetPropertyStr(ctx, ao, "vx", JS_NewFloat64(ctx, a.vx));
                        JS_SetPropertyStr(ctx, ao, "vz", JS_NewFloat64(ctx, a.vz));
                        JS_SetPropertyStr(ctx, ao, "yaw", JS_NewFloat64(ctx, a.yaw));
                        JS_SetPropertyStr(ctx, ao, "aimYaw", JS_NewFloat64(ctx, a.aimYaw));
                        JS_SetPropertyStr(ctx, ao, "aimPitch", JS_NewFloat64(ctx, a.aimPitch));
                        JS_SetPropertyStr(ctx, ao, "speed", JS_NewFloat64(ctx, a.speed));
                        JS_SetPropertyStr(ctx, ao, "radius", JS_NewFloat64(ctx, a.radius));
                        JS_SetPropertyStr(ctx, ao, "hp", JS_NewFloat64(ctx, a.unit.hp));
                        JS_SetPropertyStr(ctx, ao, "maxHp", JS_NewFloat64(ctx, a.unit.maxHp));
                        JS_SetPropertyStr(ctx, ao, "mana", JS_NewFloat64(ctx, a.unit.mana));
                        JS_SetPropertyStr(ctx, ao, "teamId", JS_NewInt32(ctx, a.unit.teamId));
                        JS_SetPropertyStr(ctx, ao, "hasTarget", JS_NewBool(ctx, a.hasTarget));
                        JS_SetPropertyStr(ctx, ao, "targetX", JS_NewFloat64(ctx, a.targetX));
                        JS_SetPropertyStr(ctx, ao, "targetZ", JS_NewFloat64(ctx, a.targetZ));
                        JS_SetPropertyUint32(ctx, agentsArr, (uint32_t)i, ao);
                    }
                    JS_SetPropertyStr(ctx, obj, "agents", agentsArr);
                    JS_SetPropertyStr(ctx, obj, "nextProjectileId", JS_NewInt32(ctx, snap.nextProjectileId));
                    return obj;
                }, 0)
            .method_raw("restore",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, this_val);
                    if (!wd || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

                    brogameagent::WorldSnapshot snap;
                    JSValue obj = argv[0];

                    // Parse agents
                    JSValue agentsArr = JS_GetPropertyStr(ctx, obj, "agents");
                    if (JS_IsArray(agentsArr)) {
                        JSValue lenVal = JS_GetPropertyStr(ctx, agentsArr, "length");
                        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
                        for (int32_t i = 0; i < len; i++) {
                            JSValue ao = JS_GetPropertyUint32(ctx, agentsArr, i);
                            brogameagent::AgentSnapshot as;
                            as.id = getInt32Prop(ctx, ao, "id", 0);
                            as.x = (float)getDoubleProp(ctx, ao, "x", 0);
                            as.z = (float)getDoubleProp(ctx, ao, "z", 0);
                            as.vx = (float)getDoubleProp(ctx, ao, "vx", 0);
                            as.vz = (float)getDoubleProp(ctx, ao, "vz", 0);
                            as.yaw = (float)getDoubleProp(ctx, ao, "yaw", 0);
                            as.aimYaw = (float)getDoubleProp(ctx, ao, "aimYaw", 0);
                            as.aimPitch = (float)getDoubleProp(ctx, ao, "aimPitch", 0);
                            as.speed = (float)getDoubleProp(ctx, ao, "speed", 6);
                            as.radius = (float)getDoubleProp(ctx, ao, "radius", 0.4);
                            as.unit.hp = (float)getDoubleProp(ctx, ao, "hp", 100);
                            as.unit.maxHp = (float)getDoubleProp(ctx, ao, "maxHp", 100);
                            as.unit.mana = (float)getDoubleProp(ctx, ao, "mana", 0);
                            as.unit.teamId = getInt32Prop(ctx, ao, "teamId", 0);
                            as.unit.id = as.id;
                            as.hasTarget = getBoolProp(ctx, ao, "hasTarget", false);
                            as.targetX = (float)getDoubleProp(ctx, ao, "targetX", 0);
                            as.targetZ = (float)getDoubleProp(ctx, ao, "targetZ", 0);
                            snap.agents.push_back(as);
                            JS_FreeValue(ctx, ao);
                        }
                    }
                    JS_FreeValue(ctx, agentsArr);

                    snap.nextProjectileId = getInt32Prop(ctx, obj, "nextProjectileId", 1);
                    wd->world.restore(snap);
                    return JS_UNDEFINED;
                }, 1);
    }

    // ─── RewardTracker class ───────────────────────────────────────────
    {
        qjsbind::Class<RewardTrackerData>(ctx, "AIRewardTracker", qjsbind::NoGlobal)
            .method_raw("consume",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* rd = qjsbind::unwrap<RewardTrackerData>(ctx, this_val);
                    if (!rd || argc < 2) return JS_NULL;
                    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[0]);
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[1]);
                    if (!ad || !wd) return JS_NULL;
                    auto delta = rd->tracker.consume(ad->agent, wd->world);
                    JSValue obj = JS_NewObject(ctx);
                    JS_SetPropertyStr(ctx, obj, "damageDealt", JS_NewFloat64(ctx, delta.damageDealt));
                    JS_SetPropertyStr(ctx, obj, "damageTaken", JS_NewFloat64(ctx, delta.damageTaken));
                    JS_SetPropertyStr(ctx, obj, "kills", JS_NewInt32(ctx, delta.kills));
                    JS_SetPropertyStr(ctx, obj, "deaths", JS_NewInt32(ctx, delta.deaths));
                    JS_SetPropertyStr(ctx, obj, "distanceTravelled", JS_NewFloat64(ctx, delta.distanceTravelled));
                    return obj;
                }, 2)
            .method_raw("reset",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* rd = qjsbind::unwrap<RewardTrackerData>(ctx, this_val);
                    if (!rd || argc < 2) return JS_UNDEFINED;
                    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[0]);
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[1]);
                    if (ad && wd) rd->tracker.reset(ad->agent, wd->world);
                    return JS_UNDEFINED;
                }, 2);
    }

    // ─── Simulation class ──────────────────────────────────────────────
    {
        qjsbind::Class<SimulationData>(ctx, "AISimulation", qjsbind::NoGlobal)
            .method("step",
                [](SimulationData* d, double dt) { if (d->sim) d->sim->step((float)dt); })
            .method("runSteps",
                [](SimulationData* d, double dt, int n) { if (d->sim) d->sim->runSteps((float)dt, n); })
            .get("steps",
                [](SimulationData* d) -> int { return d->sim ? d->sim->steps() : 0; })
            .get("elapsed",
                [](SimulationData* d) -> double { return d->sim ? d->sim->elapsed() : 0; })
            .method("resetCounters",
                [](SimulationData* d) { if (d->sim) d->sim->resetCounters(); })
            .method_raw("addPolicy",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* sd = qjsbind::unwrap<SimulationData>(ctx, this_val);
                    if (!sd || !sd->sim || argc < 2) return JS_UNDEFINED;
                    int32_t agentId = 0;
                    JS_ToInt32(ctx, &agentId, argv[0]);
                    if (!JS_IsFunction(ctx, argv[1])) return JS_ThrowTypeError(ctx, "policy must be a function");

                    JSValue fnRef = JS_DupValue(ctx, argv[1]);
                    JSValue worldRef = JS_GetPropertyStr(ctx, this_val, "__world");

                    sd->sim->addPolicy(agentId, [ctx, fnRef, worldRef](
                            brogameagent::Agent& self,
                            const brogameagent::World& /*world*/) -> brogameagent::AgentAction {
                        // We need to find the JS agent matching 'self'
                        // For simplicity, create a temporary wrapper
                        // This is called from C++ sim step — we need to invoke JS
                        JSValue args[2] = { JS_UNDEFINED, worldRef };

                        // Find the JS agent from the world's __agents
                        JSValue agents = JS_GetPropertyStr(ctx, worldRef, "__agents");
                        if (JS_IsArray(agents)) {
                            JSValue lenVal = JS_GetPropertyStr(ctx, agents, "length");
                            int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
                            for (int32_t i = 0; i < len; i++) {
                                JSValue av = JS_GetPropertyUint32(ctx, agents, i);
                                auto* ad = qjsbind::unwrap<AgentData>(ctx, av);
                                if (ad && &ad->agent == &self) {
                                    args[0] = av;
                                    break;
                                }
                                JS_FreeValue(ctx, av);
                            }
                        }
                        JS_FreeValue(ctx, agents);

                        JSValue ret = JS_Call(ctx, fnRef, JS_UNDEFINED, 2, args);
                        brogameagent::AgentAction action;
                        if (JS_IsObject(ret)) {
                            action = parseAgentAction(ctx, ret);
                        }
                        JS_FreeValue(ctx, ret);
                        JS_FreeValue(ctx, args[0]);
                        return action;
                    });

                    // Store ref to policy function to prevent GC
                    JSValue policies = JS_GetPropertyStr(ctx, this_val, "__policies");
                    if (!JS_IsObject(policies)) {
                        JS_FreeValue(ctx, policies);
                        policies = JS_NewObject(ctx);
                        JS_SetPropertyStr(ctx, this_val, "__policies", JS_DupValue(ctx, policies));
                    }
                    char key[16];
                    snprintf(key, sizeof(key), "%d", agentId);
                    JS_SetPropertyStr(ctx, policies, key, JS_DupValue(ctx, argv[1]));
                    JS_FreeValue(ctx, policies);
                    JS_FreeValue(ctx, worldRef);

                    return JS_UNDEFINED;
                }, 2)
            .method("removePolicy",
                [](SimulationData* d, int agentId) { if (d->sim) d->sim->removePolicy(agentId); });
    }

    // ─── Recorder class ────────────────────────────────────────────────
    {
        qjsbind::Class<RecorderData>(ctx, "AIRecorder", qjsbind::NoGlobal)
            .method("open",
                [](RecorderData* d, std::string path, double episodeId, double seed, double dt) -> bool {
                    return d->recorder.open(path, (uint64_t)episodeId, (uint64_t)seed, (float)dt);
                })
            .get("isOpen",
                [](RecorderData* d) -> bool { return d->recorder.isOpen(); })
            .method_raw("writeRoster",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* rd = qjsbind::unwrap<RecorderData>(ctx, this_val);
                    if (!rd || argc < 1) return JS_UNDEFINED;
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[0]);
                    if (!wd) return JS_ThrowTypeError(ctx, "expected a World");
                    rd->recorder.writeRoster(wd->world.agents());
                    return JS_UNDEFINED;
                }, 1)
            .method_raw("recordFrame",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* rd = qjsbind::unwrap<RecorderData>(ctx, this_val);
                    if (!rd || argc < 3) return JS_UNDEFINED;
                    int32_t stepIdx = 0;
                    JS_ToInt32(ctx, &stepIdx, argv[0]);
                    double elapsed = 0;
                    JS_ToFloat64(ctx, &elapsed, argv[1]);
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[2]);
                    if (!wd) return JS_ThrowTypeError(ctx, "expected a World");
                    rd->recorder.recordFrame((uint32_t)stepIdx, (float)elapsed, wd->world);
                    return JS_UNDEFINED;
                }, 3)
            .method("close",
                [](RecorderData* d) -> bool { return d->recorder.close(); })
            .get("frameCount",
                [](RecorderData* d) -> int { return (int)d->recorder.frameCount(); });
    }

    // ─── ReplayReader class ────────────────────────────────────────────
    {
        qjsbind::Class<ReplayReaderData>(ctx, "AIReplayReader", qjsbind::NoGlobal)
            .method("open",
                [](ReplayReaderData* d, std::string path) -> bool { return d->reader.open(path); })
            .get("errorMessage",
                [](ReplayReaderData* d) -> std::string { return d->reader.errorMessage(); })
            .get("frameCount",
                [](ReplayReaderData* d) -> int { return (int)d->reader.frameCount(); })
            .method("frame",
                [](ReplayReaderData* d, JSContext* ctx, int idx) -> JSValue {
                    if (idx < 0 || idx >= (int)d->reader.frameCount()) return JS_NULL;
                    auto f = d->reader.frame((size_t)idx);
                    JSValue obj = JS_NewObject(ctx);
                    JS_SetPropertyStr(ctx, obj, "stepIdx", JS_NewInt32(ctx, f.header.stepIdx));
                    JS_SetPropertyStr(ctx, obj, "elapsed", JS_NewFloat64(ctx, f.header.elapsed));

                    JSValue agentsArr = JS_NewArray(ctx);
                    for (size_t i = 0; i < f.agents.size(); i++) {
                        const auto& a = f.agents[i];
                        JSValue ao = JS_NewObject(ctx);
                        JS_SetPropertyStr(ctx, ao, "id", JS_NewInt32(ctx, a.id));
                        JS_SetPropertyStr(ctx, ao, "x", JS_NewFloat64(ctx, a.x));
                        JS_SetPropertyStr(ctx, ao, "z", JS_NewFloat64(ctx, a.z));
                        JS_SetPropertyStr(ctx, ao, "hp", JS_NewFloat64(ctx, a.hp));
                        JS_SetPropertyStr(ctx, ao, "mana", JS_NewFloat64(ctx, a.mana));
                        JS_SetPropertyStr(ctx, ao, "yaw", JS_NewFloat64(ctx, a.yaw));
                        JS_SetPropertyStr(ctx, ao, "alive", JS_NewBool(ctx, (a.flags & brogameagent::replay::AGENT_FLAG_ALIVE) != 0));
                        JS_SetPropertyUint32(ctx, agentsArr, (uint32_t)i, ao);
                    }
                    JS_SetPropertyStr(ctx, obj, "agents", agentsArr);

                    JSValue eventsArr = JS_NewArray(ctx);
                    for (size_t i = 0; i < f.events.size(); i++) {
                        const auto& e = f.events[i];
                        JSValue eo = JS_NewObject(ctx);
                        JS_SetPropertyStr(ctx, eo, "attackerId", JS_NewInt32(ctx, e.attackerId));
                        JS_SetPropertyStr(ctx, eo, "targetId", JS_NewInt32(ctx, e.targetId));
                        JS_SetPropertyStr(ctx, eo, "amount", JS_NewFloat64(ctx, e.amount));
                        JS_SetPropertyStr(ctx, eo, "killed", JS_NewBool(ctx, e.killed != 0));
                        JS_SetPropertyUint32(ctx, eventsArr, (uint32_t)i, eo);
                    }
                    JS_SetPropertyStr(ctx, obj, "events", eventsArr);
                    return obj;
                })
            .method("trajectory",
                [](ReplayReaderData* d, JSContext* ctx, int agentId) -> JSValue {
                    auto traj = d->reader.trajectory(agentId);
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < traj.size(); i++) {
                        JSValue pt = JS_NewObject(ctx);
                        JS_SetPropertyStr(ctx, pt, "stepIdx", JS_NewInt32(ctx, traj[i].stepIdx));
                        JS_SetPropertyStr(ctx, pt, "elapsed", JS_NewFloat64(ctx, traj[i].elapsed));
                        JS_SetPropertyStr(ctx, pt, "x", JS_NewFloat64(ctx, traj[i].x));
                        JS_SetPropertyStr(ctx, pt, "z", JS_NewFloat64(ctx, traj[i].z));
                        JS_SetPropertyStr(ctx, pt, "hp", JS_NewFloat64(ctx, traj[i].hp));
                        JS_SetPropertyStr(ctx, pt, "alive", JS_NewBool(ctx, traj[i].alive));
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, pt);
                    }
                    return arr;
                })
            .method("damageSummary",
                [](ReplayReaderData* d, JSContext* ctx) -> JSValue {
                    auto summary = d->reader.damageSummary();
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < summary.size(); i++) {
                        JSValue obj = JS_NewObject(ctx);
                        JS_SetPropertyStr(ctx, obj, "attackerId", JS_NewInt32(ctx, summary[i].attackerId));
                        JS_SetPropertyStr(ctx, obj, "targetId", JS_NewInt32(ctx, summary[i].targetId));
                        JS_SetPropertyStr(ctx, obj, "totalDamage", JS_NewFloat64(ctx, summary[i].totalDamage));
                        JS_SetPropertyStr(ctx, obj, "hits", JS_NewInt32(ctx, (int)summary[i].hits));
                        JS_SetPropertyStr(ctx, obj, "kills", JS_NewInt32(ctx, (int)summary[i].kills));
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, obj);
                    }
                    return arr;
                });
    }

    // ─── Mcts class ────────────────────────────────────────────────────
    {
        qjsbind::Class<MctsData>(ctx, "AIMcts", qjsbind::NoGlobal)
            .gc_mark([](MctsData* d, JSRuntime* rt, JS_MarkFunc* mark) {
                mark_jcb(d, rt, mark);
            })
            .method_raw("search",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* md = qjsbind::unwrap<MctsData>(ctx, this_val);
                    if (!md || argc < 2) return JS_NULL;
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[0]);
                    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[1]);
                    if (!wd || !ad) return JS_ThrowTypeError(ctx, "search(world, hero)");

                    auto action = md->mcts.search(wd->world, ad->agent);
                    JSValue obj = JS_NewObject(ctx);
                    JS_SetPropertyStr(ctx, obj, "moveDir", JS_NewInt32(ctx, (int)action.move_dir));
                    JS_SetPropertyStr(ctx, obj, "attackSlot", JS_NewInt32(ctx, action.attack_slot));
                    JS_SetPropertyStr(ctx, obj, "abilitySlot", JS_NewInt32(ctx, action.ability_slot));
                    return obj;
                }, 2)
            .method_raw("advanceRoot",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* md = qjsbind::unwrap<MctsData>(ctx, this_val);
                    if (!md || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
                    brogameagent::mcts::CombatAction action;
                    action.move_dir = (brogameagent::mcts::MoveDir)getInt32Prop(ctx, argv[0], "moveDir", 0);
                    action.attack_slot = (int8_t)getInt32Prop(ctx, argv[0], "attackSlot", -1);
                    action.ability_slot = (int8_t)getInt32Prop(ctx, argv[0], "abilitySlot", -1);
                    md->mcts.advance_root(action);
                    return JS_UNDEFINED;
                }, 1)
            .method("resetTree",
                [](MctsData* d) { d->mcts.reset_tree(); })
            .get("lastStats",
                [](MctsData* d, JSContext* ctx) -> JSValue {
                    return makeSearchStats(ctx, d->mcts.last_stats());
                });
    }

    // ─── DecoupledMcts class ───────────────────────────────────────────
    {
        qjsbind::Class<DecoupledMctsData>(ctx, "AIDecoupledMcts", qjsbind::NoGlobal)
            .gc_mark([](DecoupledMctsData* d, JSRuntime* rt, JS_MarkFunc* mark) {
                mark_jcb(d, rt, mark);
            })
            .method_raw("search",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* md = qjsbind::unwrap<DecoupledMctsData>(ctx, this_val);
                    if (!md || argc < 3) return JS_ThrowTypeError(ctx, "search(world, hero, opp)");
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[0]);
                    auto* hd = qjsbind::unwrap<AgentData>(ctx, argv[1]);
                    auto* od = qjsbind::unwrap<AgentData>(ctx, argv[2]);
                    if (!wd || !hd || !od) return JS_ThrowTypeError(ctx, "invalid world/hero/opp");
                    auto joint = md->mcts.search(wd->world, hd->agent, od->agent);
                    JSValue obj = JS_NewObject(ctx);
                    JS_SetPropertyStr(ctx, obj, "hero", makeCombatAction(ctx, joint.hero));
                    JS_SetPropertyStr(ctx, obj, "opp",  makeCombatAction(ctx, joint.opp));
                    return obj;
                }, 3)
            .method_raw("advanceRoot",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* md = qjsbind::unwrap<DecoupledMctsData>(ctx, this_val);
                    if (!md || argc < 2) return JS_UNDEFINED;
                    md->mcts.advance_root(parseCombatAction(ctx, argv[0]),
                                          parseCombatAction(ctx, argv[1]));
                    return JS_UNDEFINED;
                }, 2)
            .method("resetTree",
                [](DecoupledMctsData* d) { d->mcts.reset_tree(); })
            .get("lastStats",
                [](DecoupledMctsData* d, JSContext* ctx) -> JSValue {
                    return makeSearchStats(ctx, d->mcts.last_stats());
                });
    }

    // ─── TeamMcts class ────────────────────────────────────────────────
    {
        qjsbind::Class<TeamMctsData>(ctx, "AITeamMcts", qjsbind::NoGlobal)
            .gc_mark([](TeamMctsData* d, JSRuntime* rt, JS_MarkFunc* mark) {
                mark_jcb(d, rt, mark);
            })
            .method_raw("search",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* md = qjsbind::unwrap<TeamMctsData>(ctx, this_val);
                    if (!md || argc < 2) return JS_ThrowTypeError(ctx, "search(world, heroes)");
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[0]);
                    if (!wd) return JS_ThrowTypeError(ctx, "invalid world");
                    auto heroes = parseHeroesArray(ctx, argv[1]);
                    auto joint = md->mcts.search(wd->world, heroes);
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < joint.per_hero.size(); i++) {
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                            makeCombatAction(ctx, joint.per_hero[i]));
                    }
                    return arr;
                }, 2)
            .method_raw("advanceRoot",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* md = qjsbind::unwrap<TeamMctsData>(ctx, this_val);
                    if (!md || argc < 1) return JS_UNDEFINED;
                    brogameagent::mcts::TeamMcts::JointAction j;
                    j.per_hero = parseCombatActionArray(ctx, argv[0]);
                    md->mcts.advance_root(j);
                    return JS_UNDEFINED;
                }, 1)
            .method("resetTree",
                [](TeamMctsData* d) { d->mcts.reset_tree(); })
            .get("lastStats",
                [](TeamMctsData* d, JSContext* ctx) -> JSValue {
                    return makeSearchStats(ctx, d->mcts.last_stats());
                });
    }

    // ─── TacticMcts class ──────────────────────────────────────────────
    {
        qjsbind::Class<TacticMctsData>(ctx, "AITacticMcts", qjsbind::NoGlobal)
            .gc_mark([](TacticMctsData* d, JSRuntime* rt, JS_MarkFunc* mark) {
                mark_jcb(d, rt, mark);
            })
            .method_raw("search",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* md = qjsbind::unwrap<TacticMctsData>(ctx, this_val);
                    if (!md || argc < 2) return JS_ThrowTypeError(ctx, "search(world, heroes)");
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[0]);
                    if (!wd) return JS_ThrowTypeError(ctx, "invalid world");
                    auto heroes = parseHeroesArray(ctx, argv[1]);
                    auto t = md->mcts.search(wd->world, heroes);
                    return makeTactic(ctx, t);
                }, 2)
            .method_raw("advanceRoot",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* md = qjsbind::unwrap<TacticMctsData>(ctx, this_val);
                    if (!md || argc < 1) return JS_UNDEFINED;
                    md->mcts.advance_root(parseTactic(ctx, argv[0]));
                    return JS_UNDEFINED;
                }, 1)
            .method("resetTree",
                [](TacticMctsData* d) { d->mcts.reset_tree(); })
            .get("lastStats",
                [](TacticMctsData* d, JSContext* ctx) -> JSValue {
                    return makeSearchStats(ctx, d->mcts.last_stats());
                });
    }

    // ─── LayeredPlanner class ──────────────────────────────────────────
    {
        qjsbind::Class<LayeredPlannerData>(ctx, "AILayeredPlanner", qjsbind::NoGlobal)
            .gc_mark([](LayeredPlannerData* d, JSRuntime* rt, JS_MarkFunc* mark) {
                mark_jcb(d, rt, mark);
            })
            .method_raw("decide",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* pd = qjsbind::unwrap<LayeredPlannerData>(ctx, this_val);
                    if (!pd || argc < 2) return JS_ThrowTypeError(ctx, "decide(world, heroes)");
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[0]);
                    if (!wd) return JS_ThrowTypeError(ctx, "invalid world");
                    auto heroes = parseHeroesArray(ctx, argv[1]);
                    auto joint = pd->planner.decide(wd->world, heroes);
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < joint.per_hero.size(); i++) {
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                            makeCombatAction(ctx, joint.per_hero[i]));
                    }
                    return arr;
                }, 2)
            .method("reset",
                [](LayeredPlannerData* d) { d->planner.reset(); })
            .get("committedTactic",
                [](LayeredPlannerData* d, JSContext* ctx) -> JSValue {
                    return makeTactic(ctx, d->planner.committed_tactic());
                })
            .get("windowsUntilReplan",
                [](LayeredPlannerData* d) -> int {
                    return d->planner.windows_until_replan();
                })
            .get("lastStats",
                [](LayeredPlannerData* d, JSContext* ctx) -> JSValue {
                    const auto& s = d->planner.last_stats();
                    JSValue obj = JS_NewObject(ctx);
                    JS_SetPropertyStr(ctx, obj, "committedTactic",
                        makeTactic(ctx, s.committed_tactic));
                    JS_SetPropertyStr(ctx, obj, "windowsUntilReplan",
                        JS_NewInt32(ctx, s.windows_until_replan));
                    JS_SetPropertyStr(ctx, obj, "replannedThisCall",
                        JS_NewBool(ctx, s.replanned_this_call));
                    JS_SetPropertyStr(ctx, obj, "tacticStats",
                        makeSearchStats(ctx, s.tactic_stats));
                    JS_SetPropertyStr(ctx, obj, "fineStats",
                        makeSearchStats(ctx, s.fine_stats));
                    return obj;
                });
    }

    // ─── Option class (handle for JS-authored options) ─────────────────
    {
        qjsbind::Class<OptionData>(ctx, "AIOption", qjsbind::NoGlobal)
            .gc_mark([](OptionData* d, JSRuntime* rt, JS_MarkFunc* mark) {
                if (!d || !d->option) return;
                if (auto* j = dynamic_cast<JsCallbackHolder*>(d->option.get()))
                    j->gc_mark(rt, mark);
            })
            .get("name",
                [](OptionData* d, JSContext* ctx) -> JSValue {
                    if (!d->option) return JS_NULL;
                    return JS_NewString(ctx, d->option->name().c_str());
                });
    }

    // ─── TeamOption class ─────────────────────────────────────────────
    {
        qjsbind::Class<TeamOptionData>(ctx, "AITeamOption", qjsbind::NoGlobal)
            .gc_mark([](TeamOptionData* d, JSRuntime* rt, JS_MarkFunc* mark) {
                if (!d || !d->option) return;
                if (auto* j = dynamic_cast<JsCallbackHolder*>(d->option.get()))
                    j->gc_mark(rt, mark);
            })
            .get("name",
                [](TeamOptionData* d, JSContext* ctx) -> JSValue {
                    if (!d->option) return JS_NULL;
                    return JS_NewString(ctx, d->option->name().c_str());
                });
    }

    // ─── OptionMcts class ─────────────────────────────────────────────
    {
        qjsbind::Class<OptionMctsData>(ctx, "AIOptionMcts", qjsbind::NoGlobal)
            .gc_mark([](OptionMctsData* d, JSRuntime* rt, JS_MarkFunc* mark) {
                mark_jcb(d, rt, mark);
                for (const auto& v : d->optionJsRefs) JS_MarkValue(rt, v, mark);
            })
            .method_raw("search",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* md = qjsbind::unwrap<OptionMctsData>(ctx, this_val);
                    if (!md || argc < 2) return JS_ThrowTypeError(ctx, "search(world, hero)");
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[0]);
                    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[1]);
                    if (!wd || !ad) return JS_ThrowTypeError(ctx, "invalid world/hero");
                    const auto* opt = md->mcts.search(wd->world, ad->agent);
                    return opt ? JS_NewString(ctx, opt->name().c_str()) : JS_NULL;
                }, 2)
            .method_raw("advanceRoot",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* md = qjsbind::unwrap<OptionMctsData>(ctx, this_val);
                    if (!md || argc < 1 || !JS_IsString(argv[0])) {
                        if (md) md->mcts.reset_tree();
                        return JS_UNDEFINED;
                    }
                    const char* s = JS_ToCString(ctx, argv[0]);
                    std::string target = s ? s : "";
                    if (s) JS_FreeCString(ctx, s);
                    const brogameagent::mcts::Option* match = nullptr;
                    for (const auto& sp : md->options) {
                        if (sp && sp->name() == target) { match = sp.get(); break; }
                    }
                    md->mcts.advance_root(match);
                    return JS_UNDEFINED;
                }, 1)
            .method_raw("executeOption",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* md = qjsbind::unwrap<OptionMctsData>(ctx, this_val);
                    if (!md || argc < 3) return JS_ThrowTypeError(ctx, "executeOption(world, hero, name)");
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[0]);
                    auto* ad = qjsbind::unwrap<AgentData>(ctx, argv[1]);
                    if (!wd || !ad) return JS_ThrowTypeError(ctx, "invalid world/hero");
                    const char* s = JS_ToCString(ctx, argv[2]);
                    std::string target = s ? s : "";
                    if (s) JS_FreeCString(ctx, s);
                    for (const auto& sp : md->options) {
                        if (sp && sp->name() == target) {
                            int w = md->mcts.execute_option(wd->world, ad->agent, *sp);
                            return JS_NewInt32(ctx, w);
                        }
                    }
                    return JS_NewInt32(ctx, 0);
                }, 3)
            .method("resetTree",
                [](OptionMctsData* d) { d->mcts.reset_tree(); })
            .get("lastStats",
                [](OptionMctsData* d, JSContext* ctx) -> JSValue {
                    return makeSearchStats(ctx, d->mcts.last_stats());
                });
    }

    // ─── TeamOptionMcts class ─────────────────────────────────────────
    {
        qjsbind::Class<TeamOptionMctsData>(ctx, "AITeamOptionMcts", qjsbind::NoGlobal)
            .gc_mark([](TeamOptionMctsData* d, JSRuntime* rt, JS_MarkFunc* mark) {
                mark_jcb(d, rt, mark);
                for (const auto& v : d->optionJsRefs) JS_MarkValue(rt, v, mark);
            })
            .method_raw("search",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* md = qjsbind::unwrap<TeamOptionMctsData>(ctx, this_val);
                    if (!md || argc < 2) return JS_ThrowTypeError(ctx, "search(world, heroes)");
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[0]);
                    if (!wd) return JS_ThrowTypeError(ctx, "invalid world");
                    auto heroes = parseHeroesArray(ctx, argv[1]);
                    const auto* opt = md->mcts.search(wd->world, heroes);
                    return opt ? JS_NewString(ctx, opt->name().c_str()) : JS_NULL;
                }, 2)
            .method_raw("advanceRoot",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* md = qjsbind::unwrap<TeamOptionMctsData>(ctx, this_val);
                    if (!md || argc < 1 || !JS_IsString(argv[0])) {
                        if (md) md->mcts.reset_tree();
                        return JS_UNDEFINED;
                    }
                    const char* s = JS_ToCString(ctx, argv[0]);
                    std::string target = s ? s : "";
                    if (s) JS_FreeCString(ctx, s);
                    const brogameagent::mcts::TeamOption* match = nullptr;
                    for (const auto& sp : md->options) {
                        if (sp && sp->name() == target) { match = sp.get(); break; }
                    }
                    md->mcts.advance_root(match);
                    return JS_UNDEFINED;
                }, 1)
            .method_raw("executeOption",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* md = qjsbind::unwrap<TeamOptionMctsData>(ctx, this_val);
                    if (!md || argc < 3) return JS_ThrowTypeError(ctx, "executeOption(world, heroes, name)");
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[0]);
                    if (!wd) return JS_ThrowTypeError(ctx, "invalid world");
                    auto heroes = parseHeroesArray(ctx, argv[1]);
                    const char* s = JS_ToCString(ctx, argv[2]);
                    std::string target = s ? s : "";
                    if (s) JS_FreeCString(ctx, s);
                    for (const auto& sp : md->options) {
                        if (sp && sp->name() == target) {
                            int w = md->mcts.execute_option(wd->world, heroes, *sp);
                            return JS_NewInt32(ctx, w);
                        }
                    }
                    return JS_NewInt32(ctx, 0);
                }, 3)
            .method("resetTree",
                [](TeamOptionMctsData* d) { d->mcts.reset_tree(); })
            .get("lastStats",
                [](TeamOptionMctsData* d, JSContext* ctx) -> JSValue {
                    return makeSearchStats(ctx, d->mcts.last_stats());
                });
    }

    // ─── Commander class ──────────────────────────────────────────────
    {
        qjsbind::Class<CommanderData>(ctx, "AICommander", qjsbind::NoGlobal)
            .gc_mark([](CommanderData* d, JSRuntime* rt, JS_MarkFunc* mark) {
                mark_jcb(d, rt, mark);
                for (const auto& v : d->optionJsRefs) JS_MarkValue(rt, v, mark);
            })
            .method_raw("decide",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* cd = qjsbind::unwrap<CommanderData>(ctx, this_val);
                    if (!cd || argc < 2) return JS_ThrowTypeError(ctx, "decide(world, heroes)");
                    auto* wd = qjsbind::unwrap<WorldData>(ctx, argv[0]);
                    if (!wd) return JS_ThrowTypeError(ctx, "invalid world");
                    auto heroes = parseHeroesArray(ctx, argv[1]);
                    auto acts = cd->commander.decide(wd->world, heroes);
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < acts.size(); i++) {
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, makeCombatAction(ctx, acts[i]));
                    }
                    return arr;
                }, 2)
            .method("reset",
                [](CommanderData* d) { d->commander.reset(); })
            .method_raw("committedOption",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* cd = qjsbind::unwrap<CommanderData>(ctx, this_val);
                    if (!cd || argc < 1) return JS_NULL;
                    int32_t idx = 0; JS_ToInt32(ctx, &idx, argv[0]);
                    std::string n = cd->commander.committed_option_for_hero((size_t)idx);
                    return n.empty() ? JS_NULL : JS_NewString(ctx, n.c_str());
                }, 1)
            .get("currentAssignments",
                [](CommanderData* d, JSContext* ctx) -> JSValue {
                    const auto& a = d->commander.current_assignments();
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < a.size(); i++) {
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewInt32(ctx, a[i]));
                    }
                    return arr;
                })
            .get("windowsUntilReplan",
                [](CommanderData* d) -> int { return d->commander.windows_until_replan(); })
            .get("roles",
                [](CommanderData* d, JSContext* ctx) -> JSValue {
                    const auto& roles = d->commander.roles();
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < roles.size(); i++) {
                        JSValue o = JS_NewObject(ctx);
                        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, roles[i].name.c_str()));
                        JS_SetPropertyStr(ctx, o, "optionCount",
                                          JS_NewInt32(ctx, (int)roles[i].options.size()));
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
                    }
                    return arr;
                });
    }

    // ═══════════════════════════════════════════════════════════════════
    // Build namespace: bro.ai.game
    // ═══════════════════════════════════════════════════════════════════

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue aiObj = JS_GetPropertyStr(ctx, broObj, "ai");
    if (JS_IsUndefined(aiObj) || JS_IsException(aiObj)) {
        JS_FreeValue(ctx, aiObj);
        aiObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, broObj, "ai", JS_DupValue(ctx, aiObj));
    }

    JSValue gameObj = JS_NewObject(ctx);

    // Factory functions
    JS_SetPropertyStr(ctx, gameObj, "createNavGrid",
        JS_NewCFunction(ctx, js_createNavGrid, "createNavGrid", 1));

    // Polygon navmesh (Recast/Detour via brogameagent) — present when the
    // build was configured with -DBROGAMEAGENT_WITH_NAVMESH=ON. Otherwise the
    // factories throw and navMeshAvailable === false so apps feature-detect.
#ifdef BROGAMEAGENT_HAS_NAVMESH
    JS_SetPropertyStr(ctx, gameObj, "bakeNavMesh",
        JS_NewCFunction(ctx, js_bakeNavMesh, "bakeNavMesh", 1));
    JS_SetPropertyStr(ctx, gameObj, "loadNavMesh",
        JS_NewCFunction(ctx, js_loadNavMesh, "loadNavMesh", 1));
    JS_SetPropertyStr(ctx, gameObj, "navMeshAvailable", JS_NewBool(ctx, true));
#else
    {
        JSCFunction* navStub = [](JSContext* ctx, JSValueConst, int, JSValueConst*) -> JSValue {
            return JS_ThrowTypeError(ctx,
                "bro.ai.game navmesh is unavailable: this build was compiled "
                "without BROGAMEAGENT_WITH_NAVMESH");
        };
        JS_SetPropertyStr(ctx, gameObj, "bakeNavMesh",
            JS_NewCFunction(ctx, navStub, "bakeNavMesh", 1));
        JS_SetPropertyStr(ctx, gameObj, "loadNavMesh",
            JS_NewCFunction(ctx, navStub, "loadNavMesh", 1));
        JS_SetPropertyStr(ctx, gameObj, "navMeshAvailable", JS_NewBool(ctx, false));
    }
#endif
    JS_SetPropertyStr(ctx, gameObj, "createAgent",
        JS_NewCFunction(ctx, js_createAgent, "createAgent", 1));
    JS_SetPropertyStr(ctx, gameObj, "createWorld",
        JS_NewCFunction(ctx, js_createWorld, "createWorld", 0));

    // Perception
    JS_SetPropertyStr(ctx, gameObj, "hasLineOfSight",
        JS_NewCFunction(ctx, js_hasLOS, "hasLineOfSight", 5));
    JS_SetPropertyStr(ctx, gameObj, "canSee",
        JS_NewCFunction(ctx, js_canSee, "canSee", 8));
    JS_SetPropertyStr(ctx, gameObj, "computeAim",
        JS_NewCFunction(ctx, js_computeAim, "computeAim", 6));
    JS_SetPropertyStr(ctx, gameObj, "computeLeadAim",
        JS_NewCFunction(ctx, js_computeLeadAim, "computeLeadAim", 10));

    // Observation / training
    JS_SetPropertyStr(ctx, gameObj, "buildObservation",
        JS_NewCFunction(ctx, js_buildObservation, "buildObservation", 2));
    JS_SetPropertyStr(ctx, gameObj, "buildActionMask",
        JS_NewCFunction(ctx, js_buildActionMask, "buildActionMask", 2));
    JS_SetPropertyStr(ctx, gameObj, "createRewardTracker",
        JS_NewCFunction(ctx, js_createRewardTracker, "createRewardTracker", 2));

    // Simulation
    JS_SetPropertyStr(ctx, gameObj, "createSimulation",
        JS_NewCFunction(ctx, js_createSimulation, "createSimulation", 1));

    // Replay
    JS_SetPropertyStr(ctx, gameObj, "createRecorder",
        JS_NewCFunction(ctx, js_createRecorder, "createRecorder", 0));
    JS_SetPropertyStr(ctx, gameObj, "createReplayReader",
        JS_NewCFunction(ctx, js_createReplayReader, "createReplayReader", 0));

    // MCTS
    JS_SetPropertyStr(ctx, gameObj, "createMcts",
        JS_NewCFunction(ctx, js_createMcts, "createMcts", 1));
    JS_SetPropertyStr(ctx, gameObj, "createDecoupledMcts",
        JS_NewCFunction(ctx, js_createDecoupledMcts, "createDecoupledMcts", 1));
    JS_SetPropertyStr(ctx, gameObj, "createTeamMcts",
        JS_NewCFunction(ctx, js_createTeamMcts, "createTeamMcts", 1));
    JS_SetPropertyStr(ctx, gameObj, "createTacticMcts",
        JS_NewCFunction(ctx, js_createTacticMcts, "createTacticMcts", 1));
    JS_SetPropertyStr(ctx, gameObj, "createLayeredPlanner",
        JS_NewCFunction(ctx, js_createLayeredPlanner, "createLayeredPlanner", 1));
    JS_SetPropertyStr(ctx, gameObj, "createOption",
        JS_NewCFunction(ctx, js_createOption, "createOption", 1));
    JS_SetPropertyStr(ctx, gameObj, "createTeamOption",
        JS_NewCFunction(ctx, js_createTeamOption, "createTeamOption", 1));
    JS_SetPropertyStr(ctx, gameObj, "createOptionMcts",
        JS_NewCFunction(ctx, js_createOptionMcts, "createOptionMcts", 1));
    JS_SetPropertyStr(ctx, gameObj, "createTeamOptionMcts",
        JS_NewCFunction(ctx, js_createTeamOptionMcts, "createTeamOptionMcts", 1));
    JS_SetPropertyStr(ctx, gameObj, "createCommander",
        JS_NewCFunction(ctx, js_createCommander, "createCommander", 1));
    JS_SetPropertyStr(ctx, gameObj, "legalActions",
        JS_NewCFunction(ctx, js_legalActions, "legalActions", 2));
    JS_SetPropertyStr(ctx, gameObj, "legalTactics",
        JS_NewCFunction(ctx, js_legalTactics, "legalTactics", 2));
    JS_SetPropertyStr(ctx, gameObj, "tacticToAction",
        JS_NewCFunction(ctx, js_tacticToAction, "tacticToAction", 3));
    JS_SetPropertyStr(ctx, gameObj, "applyCombatAction",
        JS_NewCFunction(ctx, js_applyCombatAction, "applyCombatAction", 4));

    // Capabilities (JS-authored capability registration)
    installRegisterCapability(ctx, gameObj);

    // NN + Learn subsystems: bro.ai.game.nn / bro.ai.game.learn. These are the
    // brotensor-backed neural layer; only present when it was built in.
#if BRO_WITH_GAMEAI_NN
    installNNBindings(ctx, gameObj);
    installLearnBindings(ctx, gameObj);
#else
    // Compiled out (e.g. the app profile): install { available: false } stubs so
    // apps and tests feature-detect instead of dereferencing undefined. Matches
    // the modular-build stub contract used across the bro.* namespaces.
    {
        JSValue nnStub = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, nnStub, "available", JS_NewBool(ctx, false));
        JS_SetPropertyStr(ctx, gameObj, "nn", nnStub);
        JSValue learnStub = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, learnStub, "available", JS_NewBool(ctx, false));
        JS_SetPropertyStr(ctx, gameObj, "learn", learnStub);
    }
#endif

    // Belief / observability / InfoSetMcts
    installBeliefBindings(ctx, gameObj);

    // Snapshots, projectiles, VecSimulation, classic MCTS primitives
    installExtrasBindings(ctx, gameObj);

    // Env-agnostic GenericMcts (bro.ai.game.createGenericMcts)
    installGenericMctsBindings(ctx, gameObj);

    // Root-parallel search (bro.ai.game.rootParallelSearch[Decoupled])
    installParallelBindings(ctx, gameObj);

    // Grid-world / platformer kit (bro.ai.game.grid.*) — neural training kit,
    // built only with the game-AI neural layer.
#if BRO_WITH_GAMEAI_NN
    installGridBindings(ctx, gameObj);
#else
    {
        JSValue gridStub = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, gridStub, "available", JS_NewBool(ctx, false));
        JS_SetPropertyStr(ctx, gameObj, "grid", gridStub);
    }
#endif

    // ── Steering sub-namespace: bro.ai.game.steer ──────────────────────
    JSValue steerObj = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, steerObj, "seek",
        JS_NewCFunction(ctx, [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
            if (argc < 4) return JS_NULL;
            double px, pz, tx, tz;
            JS_ToFloat64(ctx, &px, argv[0]); JS_ToFloat64(ctx, &pz, argv[1]);
            JS_ToFloat64(ctx, &tx, argv[2]); JS_ToFloat64(ctx, &tz, argv[3]);
            return makeSteeringOutput(ctx, brogameagent::seek({(float)px,(float)pz}, {(float)tx,(float)tz}));
        }, "seek", 4));

    JS_SetPropertyStr(ctx, steerObj, "arrive",
        JS_NewCFunction(ctx, [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
            if (argc < 5) return JS_NULL;
            double px, pz, tx, tz, sr;
            JS_ToFloat64(ctx, &px, argv[0]); JS_ToFloat64(ctx, &pz, argv[1]);
            JS_ToFloat64(ctx, &tx, argv[2]); JS_ToFloat64(ctx, &tz, argv[3]);
            JS_ToFloat64(ctx, &sr, argv[4]);
            return makeSteeringOutput(ctx, brogameagent::arrive({(float)px,(float)pz}, {(float)tx,(float)tz}, (float)sr));
        }, "arrive", 5));

    JS_SetPropertyStr(ctx, steerObj, "flee",
        JS_NewCFunction(ctx, [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
            if (argc < 4) return JS_NULL;
            double px, pz, tx, tz;
            JS_ToFloat64(ctx, &px, argv[0]); JS_ToFloat64(ctx, &pz, argv[1]);
            JS_ToFloat64(ctx, &tx, argv[2]); JS_ToFloat64(ctx, &tz, argv[3]);
            return makeSteeringOutput(ctx, brogameagent::flee({(float)px,(float)pz}, {(float)tx,(float)tz}));
        }, "flee", 4));

    JS_SetPropertyStr(ctx, steerObj, "pursue",
        JS_NewCFunction(ctx, [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
            if (argc < 7) return JS_NULL;
            double px, pz, tx, tz, tvx, tvz, speed;
            JS_ToFloat64(ctx, &px, argv[0]); JS_ToFloat64(ctx, &pz, argv[1]);
            JS_ToFloat64(ctx, &tx, argv[2]); JS_ToFloat64(ctx, &tz, argv[3]);
            JS_ToFloat64(ctx, &tvx, argv[4]); JS_ToFloat64(ctx, &tvz, argv[5]);
            JS_ToFloat64(ctx, &speed, argv[6]);
            return makeSteeringOutput(ctx, brogameagent::pursue(
                {(float)px,(float)pz}, {(float)tx,(float)tz}, {(float)tvx,(float)tvz}, (float)speed));
        }, "pursue", 7));

    JS_SetPropertyStr(ctx, steerObj, "evade",
        JS_NewCFunction(ctx, [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
            if (argc < 7) return JS_NULL;
            double px, pz, tx, tz, tvx, tvz, speed;
            JS_ToFloat64(ctx, &px, argv[0]); JS_ToFloat64(ctx, &pz, argv[1]);
            JS_ToFloat64(ctx, &tx, argv[2]); JS_ToFloat64(ctx, &tz, argv[3]);
            JS_ToFloat64(ctx, &tvx, argv[4]); JS_ToFloat64(ctx, &tvz, argv[5]);
            JS_ToFloat64(ctx, &speed, argv[6]);
            return makeSteeringOutput(ctx, brogameagent::evade(
                {(float)px,(float)pz}, {(float)tx,(float)tz}, {(float)tvx,(float)tvz}, (float)speed));
        }, "evade", 7));

    JS_SetPropertyStr(ctx, gameObj, "steer", steerObj);

    // ── Constants ──
    JS_SetPropertyStr(ctx, gameObj, "OBS_TOTAL",
        JS_NewInt32(ctx, brogameagent::observation::TOTAL));
    JS_SetPropertyStr(ctx, gameObj, "MASK_TOTAL",
        JS_NewInt32(ctx, brogameagent::action_mask::TOTAL));
    JS_SetPropertyStr(ctx, gameObj, "N_ENEMY_SLOTS",
        JS_NewInt32(ctx, brogameagent::action_mask::N_ENEMY_SLOTS));

    // Tactic kind string constants — passed to createLayeredPlanner priors,
    // TacticMcts.advanceRoot, tacticToAction, etc.
    {
        JSValue t = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, t, "Hold",          JS_NewString(ctx, "Hold"));
        JS_SetPropertyStr(ctx, t, "FocusLowestHp", JS_NewString(ctx, "FocusLowestHp"));
        JS_SetPropertyStr(ctx, t, "Scatter",       JS_NewString(ctx, "Scatter"));
        JS_SetPropertyStr(ctx, t, "Retreat",       JS_NewString(ctx, "Retreat"));
        JS_SetPropertyStr(ctx, gameObj, "TACTIC", t);
    }

    // Move direction integer constants — match CombatAction.move_dir values
    // returned by search() and accepted by advanceRoot().
    {
        JSValue m = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, m, "Hold", JS_NewInt32(ctx, (int)brogameagent::mcts::MoveDir::Hold));
        JS_SetPropertyStr(ctx, m, "N",    JS_NewInt32(ctx, (int)brogameagent::mcts::MoveDir::N));
        JS_SetPropertyStr(ctx, m, "NE",   JS_NewInt32(ctx, (int)brogameagent::mcts::MoveDir::NE));
        JS_SetPropertyStr(ctx, m, "E",    JS_NewInt32(ctx, (int)brogameagent::mcts::MoveDir::E));
        JS_SetPropertyStr(ctx, m, "SE",   JS_NewInt32(ctx, (int)brogameagent::mcts::MoveDir::SE));
        JS_SetPropertyStr(ctx, m, "S",    JS_NewInt32(ctx, (int)brogameagent::mcts::MoveDir::S));
        JS_SetPropertyStr(ctx, m, "SW",   JS_NewInt32(ctx, (int)brogameagent::mcts::MoveDir::SW));
        JS_SetPropertyStr(ctx, m, "W",    JS_NewInt32(ctx, (int)brogameagent::mcts::MoveDir::W));
        JS_SetPropertyStr(ctx, m, "NW",   JS_NewInt32(ctx, (int)brogameagent::mcts::MoveDir::NW));
        JS_SetPropertyStr(ctx, gameObj, "MOVE_DIR", m);
    }

    JS_SetPropertyStr(ctx, aiObj, "game", gameObj);
    JS_FreeValue(ctx, aiObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void AIBindings::cleanup(JSContext* ctx) {
    // The one piece of persistent native JSValue storage in this binding
    // family: the registerCapability registry (ai_binding_integration.cpp)
    // holds dup'd gate/start/advance callbacks for the process lifetime.
    // Free them before the runtime goes down; everything else is handled by
    // qjsbind finalizers and the engine-level globalThis sweep.
    clearRegisteredCapabilities(ctx);
}

// ─── Cross-module helpers ──────────────────────────────────────────────────
brogameagent::Agent* agentFromJS(JSContext* ctx, JSValueConst val) {
    auto* d = qjsbind::unwrap<AgentData>(ctx, val);
    return d ? &d->agent : nullptr;
}

brogameagent::World* worldFromJS(JSContext* ctx, JSValueConst val) {
    auto* d = qjsbind::unwrap<WorldData>(ctx, val);
    return d ? &d->world : nullptr;
}

brogameagent::mcts::Mcts* mctsFromJS(JSContext* ctx, JSValueConst val) {
    auto* d = qjsbind::unwrap<MctsData>(ctx, val);
    return d ? &d->mcts : nullptr;
}

brogameagent::NavGrid* navGridFromJS(JSContext* ctx, JSValueConst val) {
    auto* d = qjsbind::unwrap<NavGridData>(ctx, val);
    return d && d->grid ? d->grid.get() : nullptr;
}

brogameagent::NavMesh* navMeshFromJS(JSContext* ctx, JSValueConst val) {
#ifdef BROGAMEAGENT_HAS_NAVMESH
    auto* d = qjsbind::unwrap<NavMeshData>(ctx, val);
    return d && d->mesh ? d->mesh.get() : nullptr;
#else
    (void)ctx; (void)val;
    return nullptr;
#endif
}

std::shared_ptr<brogameagent::NavMesh> navMeshSharedFromJS(JSContext* ctx, JSValueConst val) {
#ifdef BROGAMEAGENT_HAS_NAVMESH
    auto* d = qjsbind::unwrap<NavMeshData>(ctx, val);
    return d ? d->mesh : nullptr;
#else
    (void)ctx; (void)val;
    return nullptr;
#endif
}

void pumpNavMeshObstacles(float dt) {
#ifdef BROGAMEAGENT_HAS_NAVMESH
    // Snapshot the live meshes under the lock, run the (potentially tile-
    // rebuilding) updates outside it. Expired registry entries are pruned by
    // swap-with-back.
    std::vector<std::shared_ptr<brogameagent::NavMesh>> live;
    {
        std::lock_guard<std::mutex> lock(g_navMeshPumpMutex);
        for (size_t i = 0; i < g_navMeshPump.size();) {
            if (auto sp = g_navMeshPump[i].lock()) {
                live.push_back(std::move(sp));
                i++;
            } else {
                g_navMeshPump[i] = std::move(g_navMeshPump.back());
                g_navMeshPump.pop_back();
            }
        }
    }
    for (auto& m : live) {
        if (m->obstaclesPending()) m->update(dt);
    }
#else
    (void)dt;
#endif
}

JSValue createNavGridJS(JSContext* ctx, float minX, float minZ,
                        float maxX, float maxZ, float cellSize) {
    auto* data = new NavGridData{
        std::make_unique<brogameagent::NavGrid>(minX, minZ, maxX, maxZ, cellSize)
    };
    return qjsbind::wrap<NavGridData>(ctx, data);
}

JSValue findAgentJSRef(JSContext* ctx, JSValueConst worldJsRef, brogameagent::Agent* agent) {
    if (!agent || JS_IsUndefined(worldJsRef) || JS_IsNull(worldJsRef)) return JS_NULL;
    JSValue agents = JS_GetPropertyStr(ctx, worldJsRef, "__agents");
    JSValue found = JS_NULL;
    if (JS_IsArray(agents)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, agents, "length");
        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
        for (int32_t i = 0; i < len; i++) {
            JSValue av = JS_GetPropertyUint32(ctx, agents, i);
            auto* ad = qjsbind::unwrap<AgentData>(ctx, av);
            if (ad && &ad->agent == agent) {
                found = av; // transfer ownership to caller
                av = JS_UNDEFINED;
                break;
            }
            JS_FreeValue(ctx, av);
        }
    }
    JS_FreeValue(ctx, agents);
    return found;
}

} // namespace bro::js

#endif  // BRO_WITH_GAMEAI
