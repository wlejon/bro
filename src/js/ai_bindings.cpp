#include "js/ai_bindings.h"
#include "util/log.h"

#include <qjsbind/qjsbind.h>
#include <brogameagent/brogameagent.h>

#include <memory>
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

struct MctsData {
    brogameagent::mcts::Mcts mcts;
};

struct DecoupledMctsData {
    brogameagent::mcts::DecoupledMcts mcts;
};

struct TeamMctsData {
    brogameagent::mcts::TeamMcts mcts;
};

struct TacticMctsData {
    brogameagent::mcts::TacticMcts mcts;
};

struct LayeredPlannerData {
    brogameagent::mcts::LayeredPlanner planner;
};

struct OptionData {
    std::shared_ptr<brogameagent::mcts::Option> option;
};

struct TeamOptionData {
    std::shared_ptr<brogameagent::mcts::TeamOption> option;
};

struct OptionMctsData {
    brogameagent::mcts::OptionMcts mcts;
    // Retain shared_ptrs so JS-authored options live as long as the engine
    // even if the JS wrapper is collected. set_options() on the C++ engine
    // stores the same shared_ptrs but this is a defence-in-depth anchor.
    std::vector<std::shared_ptr<brogameagent::mcts::Option>> options;
};

struct TeamOptionMctsData {
    brogameagent::mcts::TeamOptionMcts mcts;
    std::vector<std::shared_ptr<brogameagent::mcts::TeamOption>> options;
};

struct CommanderData {
    brogameagent::mcts::Commander commander;
    // Keep per-role shared_ptrs alive independent of the C++ Commander's
    // internal role list — defence-in-depth against option lifetime bugs
    // when JS-authored options hold JSValue refs.
    std::vector<std::shared_ptr<brogameagent::mcts::Option>> option_refs;
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

// Create a Float32Array from a float buffer
static JSValue makeFloat32Array(JSContext* ctx, const float* data, int count) {
    size_t bytes = count * sizeof(float);
    JSValue abuf = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(data), bytes);
    JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, abuf);
    return arr;
}

// Create an Int32Array from an int buffer
static JSValue makeInt32Array(JSContext* ctx, const int* data, int count) {
    size_t bytes = count * sizeof(int);
    JSValue abuf = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(data), bytes);
    JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_INT32);
    JS_FreeValue(ctx, abuf);
    return arr;
}

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

    // Process obstacles array
    JSValue obsArr = JS_GetPropertyStr(ctx, opts, "obstacles");
    if (JS_IsArray(obsArr)) {
        float padding = (float)getDoubleProp(ctx, opts, "padding", 0);
        auto boxes = parseAABBArray(ctx, obsArr);
        for (auto& box : boxes)
            data->grid->addObstacle(box, padding);
    }
    JS_FreeValue(ctx, obsArr);

    return qjsbind::wrap<NavGridData>(ctx, data);
}

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

        // navGrid
        JSValue navGridVal = JS_GetPropertyStr(ctx, opts, "navGrid");
        if (JS_IsObject(navGridVal)) {
            auto* gridData = qjsbind::unwrap<NavGridData>(ctx, navGridVal);
            if (gridData && gridData->grid) {
                h->agent.setNavGrid(gridData->grid.get());
            }
        }
        JS_FreeValue(ctx, navGridVal);
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
    return makeFloat32Array(ctx, buf, brogameagent::observation::TOTAL);
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
    JS_SetPropertyStr(ctx, obj, "mask", makeFloat32Array(ctx, mask, brogameagent::action_mask::TOTAL));
    JS_SetPropertyStr(ctx, obj, "enemyIds", makeInt32Array(ctx, enemyIds, brogameagent::action_mask::N_ENEMY_SLOTS));
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

static brogameagent::mcts::MctsConfig parseMctsConfig(JSContext* ctx, JSValueConst opts) {
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
static JSValue makeCombatAction(JSContext* ctx, const brogameagent::mcts::CombatAction& a);
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

class JsRolloutPolicy : public brogameagent::mcts::IRolloutPolicy {
public:
    JsRolloutPolicy(JSContext* ctx, JSValue fn)
        : ctx_(ctx), fn_(JS_DupValue(ctx, fn)) {}
    ~JsRolloutPolicy() override { JS_FreeValue(ctx_, fn_); }

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

class JsPrior : public brogameagent::mcts::IPrior {
public:
    JsPrior(JSContext* ctx, JSValue fn)
        : ctx_(ctx), fn_(JS_DupValue(ctx, fn)) {}
    ~JsPrior() override { JS_FreeValue(ctx_, fn_); }

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

class JsEvaluator : public brogameagent::mcts::IEvaluator {
public:
    JsEvaluator(JSContext* ctx, JSValue fn)
        : ctx_(ctx), fn_(JS_DupValue(ctx, fn)) {}
    ~JsEvaluator() override { JS_FreeValue(ctx_, fn_); }

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

class JsTeamEvaluator : public brogameagent::mcts::ITeamEvaluator {
public:
    JsTeamEvaluator(JSContext* ctx, JSValue fn)
        : ctx_(ctx), fn_(JS_DupValue(ctx, fn)) {}
    ~JsTeamEvaluator() override { JS_FreeValue(ctx_, fn_); }

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
class JsOption : public brogameagent::mcts::Option {
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
class JsTeamOption : public brogameagent::mcts::TeamOption {
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
class JsAssigner {
public:
    JsAssigner(JSContext* ctx, JSValue fn)
        : ctx_(ctx), fn_(JS_DupValue(ctx, fn)) {}
    ~JsAssigner() { JS_FreeValue(ctx_, fn_); }

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

static JSValue makeCombatAction(JSContext* ctx, const brogameagent::mcts::CombatAction& a) {
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
        if (auto p = parseRolloutPolicy(ctx, opts))   data->mcts.set_rollout_policy(std::move(p));
        if (auto op = parseOpponentPolicy(ctx, opts)) data->mcts.set_opponent_policy(std::move(op));
        if (auto pr = parsePrior(ctx, opts))          data->mcts.set_prior(std::move(pr));
        if (auto ev = parseHeroEvaluator(ctx, opts))  data->mcts.set_evaluator(std::move(ev));
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
        if (auto p = parseRolloutPolicy(ctx, opts))  data->mcts.set_rollout_policy(std::move(p));
        if (auto pr = parsePrior(ctx, opts))         data->mcts.set_prior(std::move(pr));
        if (auto ev = parseHeroEvaluator(ctx, opts)) data->mcts.set_evaluator(std::move(ev));
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
        if (auto p = parseRolloutPolicy(ctx, opts))   data->mcts.set_rollout_policy(std::move(p));
        if (auto op = parseOpponentPolicy(ctx, opts)) data->mcts.set_opponent_policy(std::move(op));
        if (auto pr = parsePrior(ctx, opts))          data->mcts.set_prior(std::move(pr));
        if (auto ev = parseTeamEvaluator(ctx, opts))  data->mcts.set_evaluator(std::move(ev));
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
        if (auto ev = parseTeamEvaluator(ctx, opts))  data->mcts.set_evaluator(std::move(ev));
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

        if (auto p = parseRolloutPolicy(ctx, opts))   data->planner.set_rollout_policy(std::move(p));
        if (auto op = parseOpponentPolicy(ctx, opts)) data->planner.set_opponent_policy(std::move(op));
        if (auto ev = parseTeamEvaluator(ctx, opts))  data->planner.set_team_evaluator(std::move(ev));
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
template <typename TData, typename TOption>
static std::vector<std::shared_ptr<TOption>>
parseOptionArray(JSContext* ctx, JSValueConst opts) {
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
            if (od && od->option) out.push_back(od->option);
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
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue opts = argv[0];
        data->mcts.set_config(parseMctsConfig(ctx, opts));
        if (auto op = parseOpponentPolicy(ctx, opts)) data->mcts.set_opponent_policy(std::move(op));
        if (auto ev = parseHeroEvaluator(ctx, opts))  data->mcts.set_evaluator(std::move(ev));
        data->options = parseOptionArray<OptionData, brogameagent::mcts::Option>(ctx, opts);
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
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue opts = argv[0];
        data->mcts.set_config(parseMctsConfig(ctx, opts));
        if (auto op = parseOpponentPolicy(ctx, opts)) data->mcts.set_opponent_policy(std::move(op));
        if (auto ev = parseTeamEvaluator(ctx, opts))  data->mcts.set_evaluator(std::move(ev));
        data->options = parseOptionArray<TeamOptionData, brogameagent::mcts::TeamOption>(ctx, opts);
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
    if (auto ev = parseHeroEvaluator(ctx, opts))  data->commander.set_default_evaluator(std::move(ev));

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
                auto opts_vec = parseOptionArray<OptionData, brogameagent::mcts::Option>(ctx, r);
                auto role_eval = parseHeroEvaluator(ctx, r);
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
    // the Commander does.
    JSValue assignFn = JS_GetPropertyStr(ctx, opts, "assign");
    if (JS_IsFunction(ctx, assignFn)) {
        auto sp = std::make_shared<JsAssigner>(ctx, assignFn);
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
                        JS_SetPropertyStr(ctx, pt, "z", JS_NewFloat64(ctx, path[i].z));
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
                });
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
                    JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, v.z));
                    return obj;
                })
            .get("path",
                [](AgentData* d, JSContext* ctx) -> JSValue {
                    const auto& path = d->agent.path();
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < path.size(); i++) {
                        JSValue pt = JS_NewObject(ctx);
                        JS_SetPropertyStr(ctx, pt, "x", JS_NewFloat64(ctx, path[i].x));
                        JS_SetPropertyStr(ctx, pt, "z", JS_NewFloat64(ctx, path[i].z));
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
                    JSValue fnVal = JS_GetPropertyStr(ctx, spec, "fn");
                    if (JS_IsFunction(ctx, fnVal)) {
                        JSValue fnRef = JS_DupValue(ctx, fnVal);
                        JSValue worldRef = JS_DupValue(ctx, this_val);
                        s.fn = [ctx, fnRef, worldRef, this_val](
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

                            JSValue args[3] = { casterVal, worldRef, JS_NewInt32(ctx, targetId) };
                            JSValue ret = JS_Call(ctx, fnRef, JS_UNDEFINED, 3, args);
                            JS_FreeValue(ctx, ret);
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
            .get("name",
                [](OptionData* d, JSContext* ctx) -> JSValue {
                    if (!d->option) return JS_NULL;
                    return JS_NewString(ctx, d->option->name().c_str());
                });
    }

    // ─── TeamOption class ─────────────────────────────────────────────
    {
        qjsbind::Class<TeamOptionData>(ctx, "AITeamOption", qjsbind::NoGlobal)
            .get("name",
                [](TeamOptionData* d, JSContext* ctx) -> JSValue {
                    if (!d->option) return JS_NULL;
                    return JS_NewString(ctx, d->option->name().c_str());
                });
    }

    // ─── OptionMcts class ─────────────────────────────────────────────
    {
        qjsbind::Class<OptionMctsData>(ctx, "AIOptionMcts", qjsbind::NoGlobal)
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

void AIBindings::cleanup(JSContext*) {
    // qjsbind handles finalizers automatically
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
