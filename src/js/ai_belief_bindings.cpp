// JS bindings for belief / observability / Information-Set MCTS.
//
// Installed onto bro.ai.game: createTeamBelief(), observe(), merge(),
// createInfoSetMcts(), createInfoSetTeamMcts(), patchSnapshotWithParticles().

#include "js/ai_bindings.h"

#include <qjsbind/qjsbind.h>
#include <brogameagent/brogameagent.h>
#include <brogameagent/belief.h>
#include <brogameagent/observability.h>
#include <brogameagent/info_set_mcts.h>
#include <brogameagent/mcts.h>

#include <cstring>
#include <memory>
#include <unordered_map>

namespace bro::js {

namespace mcts   = brogameagent::mcts;
namespace belief = brogameagent::belief;
namespace obs    = brogameagent::obs;

// Unwrap NavGrid* — forward declaration. We need access to the pointer stored
// in NavGridData, which is defined (anonymously) in ai_bindings.cpp. Rather
// than exposing the struct, we add a tiny accessor there.
brogameagent::NavGrid* navGridFromJS(JSContext* ctx, JSValueConst v);

// ─── Wrapper structs ───────────────────────────────────────────────────────
struct TeamBeliefData {
    std::shared_ptr<belief::TeamBelief> b;
};

struct InfoSetMctsData {
    std::unique_ptr<mcts::InfoSetMcts> m;
    std::shared_ptr<belief::TeamBelief> beliefRef;
};

struct InfoSetTeamMctsData {
    std::unique_ptr<mcts::InfoSetTeamMcts> m;
    std::shared_ptr<belief::TeamBelief> beliefRef;
};

// ─── Helpers ───────────────────────────────────────────────────────────────
static double getDouble(JSContext* ctx, JSValueConst obj, const char* k, double def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, k);
    double out = def;
    if (JS_IsNumber(v)) JS_ToFloat64(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}
static int32_t getInt(JSContext* ctx, JSValueConst obj, const char* k, int32_t def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, k);
    int32_t out = def;
    if (JS_IsNumber(v)) JS_ToInt32(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}
static bool getBool(JSContext* ctx, JSValueConst obj, const char* k, bool def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, k);
    bool out = def;
    if (JS_IsBool(v)) out = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return out;
}
static uint64_t readSeedVal(JSContext* ctx, JSValueConst v, uint64_t def) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return def;
    int64_t s = 0;
    if (JS_ToBigInt64(ctx, &s, v) == 0) return (uint64_t)s;
    double d = 0;
    if (JS_ToFloat64(ctx, &d, v) == 0) return (uint64_t)(int64_t)d;
    return def;
}

static obs::VisibilityConfig parseVisibilityConfig(JSContext* ctx, JSValueConst o) {
    obs::VisibilityConfig c{};
    if (!JS_IsObject(o)) return c;
    c.fov_radians = (float)getDouble(ctx, o, "fovRadians", c.fov_radians);
    c.max_range   = (float)getDouble(ctx, o, "maxRange", c.max_range);
    c.check_los   = getBool(ctx, o, "checkLos", c.check_los);
    return c;
}

static JSValue makeAgentObservation(JSContext* ctx, const obs::AgentObservation& a) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "id",              JS_NewInt32(ctx, a.id));
    JS_SetPropertyStr(ctx, o, "teamId",          JS_NewInt32(ctx, a.team_id));
    JS_SetPropertyStr(ctx, o, "x",               JS_NewFloat64(ctx, a.pos.x));
    JS_SetPropertyStr(ctx, o, "z",               JS_NewFloat64(ctx, a.pos.y));
    JS_SetPropertyStr(ctx, o, "vx",              JS_NewFloat64(ctx, a.vel.x));
    JS_SetPropertyStr(ctx, o, "vz",              JS_NewFloat64(ctx, a.vel.y));
    JS_SetPropertyStr(ctx, o, "hp",              JS_NewFloat64(ctx, a.hp));
    JS_SetPropertyStr(ctx, o, "maxHp",           JS_NewFloat64(ctx, a.max_hp));
    JS_SetPropertyStr(ctx, o, "heading",         JS_NewFloat64(ctx, a.heading));
    JS_SetPropertyStr(ctx, o, "alive",           JS_NewBool(ctx, a.alive));
    JS_SetPropertyStr(ctx, o, "visible",         JS_NewBool(ctx, a.visible));
    JS_SetPropertyStr(ctx, o, "lastSeenElapsed", JS_NewFloat64(ctx, a.last_seen_elapsed));
    return o;
}

static obs::AgentObservation parseAgentObservation(JSContext* ctx, JSValueConst v) {
    obs::AgentObservation a{};
    a.id      = getInt(ctx, v, "id", 0);
    a.team_id = getInt(ctx, v, "teamId", 0);
    a.pos.x   = (float)getDouble(ctx, v, "x", 0);
    a.pos.y   = (float)getDouble(ctx, v, "z", 0);
    a.vel.x   = (float)getDouble(ctx, v, "vx", 0);
    a.vel.y   = (float)getDouble(ctx, v, "vz", 0);
    a.hp      = (float)getDouble(ctx, v, "hp", 0);
    a.max_hp  = (float)getDouble(ctx, v, "maxHp", 0);
    a.heading = (float)getDouble(ctx, v, "heading", 0);
    a.alive   = getBool(ctx, v, "alive", false);
    a.visible = getBool(ctx, v, "visible", false);
    a.last_seen_elapsed = (float)getDouble(ctx, v, "lastSeenElapsed", 0);
    return a;
}

static JSValue makeTeamObservation(JSContext* ctx, const obs::TeamObservation& t) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "teamId",    JS_NewInt32(ctx, t.team_id));
    JS_SetPropertyStr(ctx, o, "timestamp", JS_NewFloat64(ctx, t.timestamp));
    JSValue aa = JS_NewArray(ctx);
    for (size_t i = 0; i < t.allies.size(); ++i)
        JS_SetPropertyUint32(ctx, aa, (uint32_t)i, makeAgentObservation(ctx, t.allies[i]));
    JSValue ee = JS_NewArray(ctx);
    for (size_t i = 0; i < t.enemies.size(); ++i)
        JS_SetPropertyUint32(ctx, ee, (uint32_t)i, makeAgentObservation(ctx, t.enemies[i]));
    JS_SetPropertyStr(ctx, o, "allies", aa);
    JS_SetPropertyStr(ctx, o, "enemies", ee);
    return o;
}

static obs::TeamObservation parseTeamObservation(JSContext* ctx, JSValueConst v) {
    obs::TeamObservation t{};
    if (!JS_IsObject(v)) return t;
    t.team_id   = getInt(ctx, v, "teamId", 0);
    t.timestamp = (float)getDouble(ctx, v, "timestamp", 0);
    auto readArr = [&](const char* key, std::vector<obs::AgentObservation>& dst) {
        JSValue a = JS_GetPropertyStr(ctx, v, key);
        if (JS_IsArray(a)) {
            JSValue lv = JS_GetPropertyStr(ctx, a, "length");
            int32_t n = 0; JS_ToInt32(ctx, &n, lv); JS_FreeValue(ctx, lv);
            for (int32_t i = 0; i < n; i++) {
                JSValue e = JS_GetPropertyUint32(ctx, a, i);
                dst.push_back(parseAgentObservation(ctx, e));
                JS_FreeValue(ctx, e);
            }
        }
        JS_FreeValue(ctx, a);
    };
    readArr("allies", t.allies);
    readArr("enemies", t.enemies);
    return t;
}

static JSValue makeEnemyParticle(JSContext* ctx, const belief::EnemyParticle& p) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "x",       JS_NewFloat64(ctx, p.pos.x));
    JS_SetPropertyStr(ctx, o, "z",       JS_NewFloat64(ctx, p.pos.y));
    JS_SetPropertyStr(ctx, o, "vx",      JS_NewFloat64(ctx, p.vel.x));
    JS_SetPropertyStr(ctx, o, "vz",      JS_NewFloat64(ctx, p.vel.y));
    JS_SetPropertyStr(ctx, o, "hp",      JS_NewFloat64(ctx, p.hp));
    JS_SetPropertyStr(ctx, o, "heading", JS_NewFloat64(ctx, p.heading));
    JS_SetPropertyStr(ctx, o, "weight",  JS_NewFloat64(ctx, p.weight));
    return o;
}

static belief::EnemyParticle parseEnemyParticle(JSContext* ctx, JSValueConst v) {
    belief::EnemyParticle p{};
    p.pos.x   = (float)getDouble(ctx, v, "x", 0);
    p.pos.y   = (float)getDouble(ctx, v, "z", 0);
    p.vel.x   = (float)getDouble(ctx, v, "vx", 0);
    p.vel.y   = (float)getDouble(ctx, v, "vz", 0);
    p.hp      = (float)getDouble(ctx, v, "hp", 0);
    p.heading = (float)getDouble(ctx, v, "heading", 0);
    p.weight  = (float)getDouble(ctx, v, "weight", 1.0);
    return p;
}

static JSValue makeParticleMap(JSContext* ctx, const std::unordered_map<int, belief::EnemyParticle>& m) {
    JSValue o = JS_NewObject(ctx);
    for (const auto& kv : m) {
        char key[32]; std::snprintf(key, sizeof(key), "%d", kv.first);
        JS_SetPropertyStr(ctx, o, key, makeEnemyParticle(ctx, kv.second));
    }
    return o;
}

static std::unordered_map<int, belief::EnemyParticle>
parseParticleMap(JSContext* ctx, JSValueConst o) {
    std::unordered_map<int, belief::EnemyParticle> m;
    if (!JS_IsObject(o)) return m;
    JSPropertyEnum* props = nullptr;
    uint32_t plen = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &plen, o, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return m;
    for (uint32_t i = 0; i < plen; i++) {
        JSValue key = JS_AtomToValue(ctx, props[i].atom);
        const char* keyStr = JS_ToCString(ctx, key);
        int id = keyStr ? atoi(keyStr) : 0;
        if (keyStr) JS_FreeCString(ctx, keyStr);
        JS_FreeValue(ctx, key);
        JSValue val = JS_GetProperty(ctx, o, props[i].atom);
        m[id] = parseEnemyParticle(ctx, val);
        JS_FreeValue(ctx, val);
        JS_FreeAtom(ctx, props[i].atom);
    }
    js_free(ctx, props);
    return m;
}

// ═══════════════════════════════════════════════════════════════════════════
// Class registration
// ═══════════════════════════════════════════════════════════════════════════

static void registerClasses(JSContext* ctx) {
    // ── TeamBelief ─────────────────────────────────────────────────────────
    {
        qjsbind::Class<TeamBeliefData>(ctx, "AITeamBelief", qjsbind::NoGlobal)
            .get("teamId",       [](TeamBeliefData* d) -> int { return d->b ? d->b->team_id() : 0; })
            .get("numParticles", [](TeamBeliefData* d) -> int { return d->b ? d->b->num_particles() : 0; })
            .get("ess",
                [](TeamBeliefData* d) -> double { return d->b ? d->b->effective_sample_size() : 0.0; })
            .method("clear", [](TeamBeliefData* d) { if (d->b) d->b->clear(); })
            .method_raw("registerEnemy",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<TeamBeliefData>(ctx, this_val);
                    if (!d || !d->b || argc < 2) return JS_UNDEFINED;
                    int32_t id = 0; JS_ToInt32(ctx, &id, argv[0]);
                    double maxHp = 0; JS_ToFloat64(ctx, &maxHp, argv[1]);
                    bromath::Vec2 pos;
                    const bromath::Vec2* posPtr = nullptr;
                    if (argc >= 3 && JS_IsObject(argv[2])) {
                        pos.x = (float)getDouble(ctx, argv[2], "x", 0);
                        pos.y = (float)getDouble(ctx, argv[2], "z", 0);
                        posPtr = &pos;
                    }
                    d->b->register_enemy(id, (float)maxHp, posPtr);
                    return JS_UNDEFINED;
                }, 3)
            .method_raw("propagate",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<TeamBeliefData>(ctx, this_val);
                    if (!d || !d->b || argc < 3) return JS_UNDEFINED;
                    auto* w = worldFromJS(ctx, argv[0]);
                    if (!w) return JS_ThrowTypeError(ctx, "expected world");
                    obs::VisibilityConfig vis = parseVisibilityConfig(ctx, argv[1]);
                    double dt = 0; JS_ToFloat64(ctx, &dt, argv[2]);
                    d->b->propagate(*w, vis, (float)dt);
                    return JS_UNDEFINED;
                }, 3)
            .method("update",
                [](TeamBeliefData* d, JSContext* ctx, JSValueConst obsV) {
                    if (!d->b) return;
                    auto t = parseTeamObservation(ctx, obsV);
                    d->b->update(t);
                })
            .method("sample",
                [](TeamBeliefData* d, JSContext* ctx) -> JSValue {
                    if (!d->b) return JS_NewObject(ctx);
                    auto m = d->b->sample(d->b->rng());
                    return makeParticleMap(ctx, m);
                })
            .method("mean",
                [](TeamBeliefData* d, JSContext* ctx) -> JSValue {
                    if (!d->b) return JS_NewObject(ctx);
                    return makeParticleMap(ctx, d->b->mean());
                })
            .method("enemies",
                [](TeamBeliefData* d, JSContext* ctx) -> JSValue {
                    JSValue arr = JS_NewArray(ctx);
                    if (!d->b) return arr;
                    const auto& v = d->b->enemies();
                    for (size_t i = 0; i < v.size(); ++i) {
                        JSValue o = JS_NewObject(ctx);
                        JS_SetPropertyStr(ctx, o, "enemyId",          JS_NewInt32(ctx, v[i].enemy_id));
                        JS_SetPropertyStr(ctx, o, "maxHp",            JS_NewFloat64(ctx, v[i].max_hp));
                        JS_SetPropertyStr(ctx, o, "everSeen",         JS_NewBool(ctx, v[i].ever_seen));
                        JS_SetPropertyStr(ctx, o, "visible",          JS_NewBool(ctx, v[i].visible));
                        JS_SetPropertyStr(ctx, o, "lastSeenElapsed",  JS_NewFloat64(ctx, v[i].last_seen_elapsed));
                        JS_SetPropertyStr(ctx, o, "particleCount",    JS_NewInt32(ctx, (int)v[i].particles.size()));
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
                    }
                    return arr;
                });
    }

    // ── InfoSetMcts ────────────────────────────────────────────────────────
    {
        qjsbind::Class<InfoSetMctsData>(ctx, "AIInfoSetMcts", qjsbind::NoGlobal)
            .method_raw("setBelief",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<InfoSetMctsData>(ctx, this_val);
                    if (!d || !d->m || argc < 1) return JS_UNDEFINED;
                    auto* bd = qjsbind::unwrap<TeamBeliefData>(ctx, argv[0]);
                    if (!bd || !bd->b) return JS_ThrowTypeError(ctx, "expected TeamBelief");
                    d->beliefRef = bd->b;
                    d->m->set_belief(bd->b);
                    JS_SetPropertyStr(ctx, this_val, "__belief", JS_DupValue(ctx, argv[0]));
                    return JS_UNDEFINED;
                }, 1)
            .method_raw("setEvaluator",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<InfoSetMctsData>(ctx, this_val);
                    if (!d || !d->m || argc < 1) return JS_UNDEFINED;
                    auto ev = extractHeroEvaluatorShared(ctx, argv[0]);
                    if (!ev) {
                        std::string kind;
                        if (JS_IsString(argv[0])) {
                            const char* s = JS_ToCString(ctx, argv[0]);
                            if (s) { kind = s; JS_FreeCString(ctx, s); }
                        }
                        if (kind == "hpDelta") ev = std::make_shared<mcts::HpDeltaEvaluator>();
                    }
                    if (ev) { d->m->set_evaluator(ev);
                              JS_SetPropertyStr(ctx, this_val, "__ev", JS_DupValue(ctx, argv[0])); }
                    return JS_UNDEFINED;
                }, 1)
            .method_raw("setPrior",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<InfoSetMctsData>(ctx, this_val);
                    if (!d || !d->m || argc < 1) return JS_UNDEFINED;
                    auto pr = extractPriorShared(ctx, argv[0]);
                    if (!pr) {
                        std::string kind;
                        if (JS_IsString(argv[0])) {
                            const char* s = JS_ToCString(ctx, argv[0]);
                            if (s) { kind = s; JS_FreeCString(ctx, s); }
                        }
                        if (kind == "uniform")    pr = std::make_shared<mcts::UniformPrior>();
                        else if (kind == "attackBias") pr = std::make_shared<mcts::AttackBiasPrior>();
                    }
                    if (pr) { d->m->set_prior(pr);
                              JS_SetPropertyStr(ctx, this_val, "__pr", JS_DupValue(ctx, argv[0])); }
                    return JS_UNDEFINED;
                }, 1)
            .method("setConfig",
                [](InfoSetMctsData* d, JSContext* ctx, JSValueConst cfg) {
                    if (!d->m || !JS_IsObject(cfg)) return;
                    mcts::MctsConfig c = d->m->config();
                    c.iterations     = getInt(ctx, cfg, "iterations", c.iterations);
                    c.budget_ms      = getInt(ctx, cfg, "budgetMs", c.budget_ms);
                    c.rollout_horizon= getInt(ctx, cfg, "rolloutHorizon", c.rollout_horizon);
                    c.sim_dt         = (float)getDouble(ctx, cfg, "simDt", c.sim_dt);
                    c.action_repeat  = getInt(ctx, cfg, "actionRepeat", c.action_repeat);
                    c.uct_c          = (float)getDouble(ctx, cfg, "uctC", c.uct_c);
                    JSValue sv = JS_GetPropertyStr(ctx, cfg, "seed");
                    c.seed = readSeedVal(ctx, sv, c.seed); JS_FreeValue(ctx, sv);
                    c.pw_alpha       = (float)getDouble(ctx, cfg, "pwAlpha", c.pw_alpha);
                    c.prior_c        = (float)getDouble(ctx, cfg, "priorC", c.prior_c);
                    c.use_leaf_value = getBool(ctx, cfg, "useLeafValue", c.use_leaf_value);
                    d->m->set_config(c);
                })
            .method_raw("search",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<InfoSetMctsData>(ctx, this_val);
                    if (!d || !d->m || argc < 2) return JS_ThrowTypeError(ctx, "search(world,hero)");
                    auto* w = worldFromJS(ctx, argv[0]);
                    auto* a = agentFromJS(ctx, argv[1]);
                    if (!w || !a) return JS_ThrowTypeError(ctx, "expected world,hero");
                    auto act = d->m->search(*w, *a);
                    JSValue obj = JS_NewObject(ctx);
                    JS_SetPropertyStr(ctx, obj, "moveDir",     JS_NewInt32(ctx, (int)act.move_dir));
                    JS_SetPropertyStr(ctx, obj, "attackSlot",  JS_NewInt32(ctx, (int)act.attack_slot));
                    JS_SetPropertyStr(ctx, obj, "abilitySlot", JS_NewInt32(ctx, (int)act.ability_slot));
                    return obj;
                }, 2)
            .method("advanceRoot",
                [](InfoSetMctsData* d, JSContext* ctx, JSValueConst actV) {
                    if (!d->m || !JS_IsObject(actV)) return;
                    mcts::CombatAction a{};
                    a.move_dir     = (mcts::MoveDir)getInt(ctx, actV, "moveDir", 0);
                    a.attack_slot  = (int8_t)getInt(ctx, actV, "attackSlot", -1);
                    a.ability_slot = (int8_t)getInt(ctx, actV, "abilitySlot", -1);
                    d->m->advance_root(a);
                })
            .method("resetTree", [](InfoSetMctsData* d) { if (d->m) d->m->reset_tree(); })
            .get("lastStats",
                [](InfoSetMctsData* d, JSContext* ctx) -> JSValue {
                    JSValue o = JS_NewObject(ctx);
                    if (!d->m) return o;
                    const auto& s = d->m->last_stats();
                    JS_SetPropertyStr(ctx, o, "iterations",   JS_NewInt32(ctx, s.iterations));
                    JS_SetPropertyStr(ctx, o, "rootChildren", JS_NewInt32(ctx, s.root_children));
                    JS_SetPropertyStr(ctx, o, "treeSize",     JS_NewInt32(ctx, s.tree_size));
                    JS_SetPropertyStr(ctx, o, "bestMean",     JS_NewFloat64(ctx, s.best_mean));
                    JS_SetPropertyStr(ctx, o, "bestVisits",   JS_NewInt32(ctx, s.best_visits));
                    JS_SetPropertyStr(ctx, o, "elapsedMs",    JS_NewInt32(ctx, s.elapsed_ms));
                    JS_SetPropertyStr(ctx, o, "reusedRoot",   JS_NewBool(ctx, s.reused_root));
                    JS_SetPropertyStr(ctx, o, "meanEss",      JS_NewFloat64(ctx, s.mean_ess));
                    return o;
                });
    }

    // ── InfoSetTeamMcts ────────────────────────────────────────────────────
    {
        qjsbind::Class<InfoSetTeamMctsData>(ctx, "AIInfoSetTeamMcts", qjsbind::NoGlobal)
            .method_raw("setBelief",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<InfoSetTeamMctsData>(ctx, this_val);
                    if (!d || !d->m || argc < 1) return JS_UNDEFINED;
                    auto* bd = qjsbind::unwrap<TeamBeliefData>(ctx, argv[0]);
                    if (!bd || !bd->b) return JS_ThrowTypeError(ctx, "expected TeamBelief");
                    d->beliefRef = bd->b;
                    d->m->set_belief(bd->b);
                    JS_SetPropertyStr(ctx, this_val, "__belief", JS_DupValue(ctx, argv[0]));
                    return JS_UNDEFINED;
                }, 1)
            .method("setConfig",
                [](InfoSetTeamMctsData* d, JSContext* ctx, JSValueConst cfg) {
                    if (!d->m || !JS_IsObject(cfg)) return;
                    mcts::MctsConfig c = d->m->config();
                    c.iterations     = getInt(ctx, cfg, "iterations", c.iterations);
                    c.budget_ms      = getInt(ctx, cfg, "budgetMs", c.budget_ms);
                    c.rollout_horizon= getInt(ctx, cfg, "rolloutHorizon", c.rollout_horizon);
                    c.sim_dt         = (float)getDouble(ctx, cfg, "simDt", c.sim_dt);
                    c.action_repeat  = getInt(ctx, cfg, "actionRepeat", c.action_repeat);
                    c.uct_c          = (float)getDouble(ctx, cfg, "uctC", c.uct_c);
                    JSValue sv = JS_GetPropertyStr(ctx, cfg, "seed");
                    c.seed = readSeedVal(ctx, sv, c.seed); JS_FreeValue(ctx, sv);
                    d->m->set_config(c);
                })
            .method_raw("search",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<InfoSetTeamMctsData>(ctx, this_val);
                    if (!d || !d->m || argc < 2) return JS_ThrowTypeError(ctx, "search(world,heroes)");
                    auto* w = worldFromJS(ctx, argv[0]);
                    if (!w) return JS_ThrowTypeError(ctx, "expected world");
                    std::vector<brogameagent::Agent*> heroes;
                    if (JS_IsArray(argv[1])) {
                        JSValue lv = JS_GetPropertyStr(ctx, argv[1], "length");
                        int32_t n = 0; JS_ToInt32(ctx, &n, lv); JS_FreeValue(ctx, lv);
                        for (int32_t i = 0; i < n; i++) {
                            JSValue e = JS_GetPropertyUint32(ctx, argv[1], i);
                            auto* a = agentFromJS(ctx, e);
                            if (a) heroes.push_back(a);
                            JS_FreeValue(ctx, e);
                        }
                    }
                    auto out = d->m->search(*w, heroes);
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < out.per_hero.size(); ++i) {
                        JSValue obj = JS_NewObject(ctx);
                        JS_SetPropertyStr(ctx, obj, "moveDir",     JS_NewInt32(ctx, (int)out.per_hero[i].move_dir));
                        JS_SetPropertyStr(ctx, obj, "attackSlot",  JS_NewInt32(ctx, (int)out.per_hero[i].attack_slot));
                        JS_SetPropertyStr(ctx, obj, "abilitySlot", JS_NewInt32(ctx, (int)out.per_hero[i].ability_slot));
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, obj);
                    }
                    return arr;
                }, 2)
            .method("resetTree", [](InfoSetTeamMctsData* d) { if (d->m) d->m->reset_tree(); })
            .get("lastStats",
                [](InfoSetTeamMctsData* d, JSContext* ctx) -> JSValue {
                    JSValue o = JS_NewObject(ctx);
                    if (!d->m) return o;
                    const auto& s = d->m->last_stats();
                    JS_SetPropertyStr(ctx, o, "iterations", JS_NewInt32(ctx, s.iterations));
                    JS_SetPropertyStr(ctx, o, "treeSize",   JS_NewInt32(ctx, s.tree_size));
                    JS_SetPropertyStr(ctx, o, "bestMean",   JS_NewFloat64(ctx, s.best_mean));
                    JS_SetPropertyStr(ctx, o, "bestVisits", JS_NewInt32(ctx, s.best_visits));
                    JS_SetPropertyStr(ctx, o, "elapsedMs",  JS_NewInt32(ctx, s.elapsed_ms));
                    JS_SetPropertyStr(ctx, o, "meanEss",    JS_NewFloat64(ctx, s.mean_ess));
                    return o;
                });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Factory + free functions
// ═══════════════════════════════════════════════════════════════════════════

static JSValue js_createTeamBelief(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int teamId = 0;
    int numParticles = 32;
    brogameagent::NavGrid* nav = nullptr;
    belief::MotionParams mp{};
    uint64_t seed = 0xBE11EFCAFEULL;

    if (argc >= 1 && JS_IsObject(argv[0])) {
        teamId        = getInt(ctx, argv[0], "teamId", teamId);
        numParticles  = getInt(ctx, argv[0], "numParticles", numParticles);
        JSValue navV = JS_GetPropertyStr(ctx, argv[0], "navGrid");
        nav = navGridFromJS(ctx, navV);
        JS_FreeValue(ctx, navV);
        JSValue mpV = JS_GetPropertyStr(ctx, argv[0], "motion");
        if (JS_IsObject(mpV)) {
            mp.max_speed      = (float)getDouble(ctx, mpV, "maxSpeed", mp.max_speed);
            mp.accel_std      = (float)getDouble(ctx, mpV, "accelStd", mp.accel_std);
            mp.spread_on_loss = (float)getDouble(ctx, mpV, "spreadOnLoss", mp.spread_on_loss);
        }
        JS_FreeValue(ctx, mpV);
        JSValue sv = JS_GetPropertyStr(ctx, argv[0], "seed");
        seed = readSeedVal(ctx, sv, seed); JS_FreeValue(ctx, sv);
    }

    auto b = std::make_shared<belief::TeamBelief>(teamId, numParticles, nav, mp, seed);
    return qjsbind::wrap<TeamBeliefData>(ctx, new TeamBeliefData{ b });
}

static JSValue js_observe(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "observe(world, teamId, visCfg, now)");
    auto* w = worldFromJS(ctx, argv[0]);
    if (!w) return JS_ThrowTypeError(ctx, "expected world");
    int32_t team = 0; JS_ToInt32(ctx, &team, argv[1]);
    obs::VisibilityConfig cfg = parseVisibilityConfig(ctx, argv[2]);
    double now = 0; JS_ToFloat64(ctx, &now, argv[3]);
    auto t = obs::observe(*w, team, cfg, (float)now);
    return makeTeamObservation(ctx, t);
}

static JSValue js_mergeObservations(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "mergeObservations(prior, fresh, now)");
    auto prior = parseTeamObservation(ctx, argv[0]);
    auto fresh = parseTeamObservation(ctx, argv[1]);
    double now = 0; JS_ToFloat64(ctx, &now, argv[2]);
    auto merged = obs::merge(prior, fresh, (float)now);
    return makeTeamObservation(ctx, merged);
}

static JSValue js_createInfoSetMcts(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* d = new InfoSetMctsData();
    d->m = std::make_unique<mcts::InfoSetMcts>();
    return qjsbind::wrap<InfoSetMctsData>(ctx, d);
}

static JSValue js_createInfoSetTeamMcts(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* d = new InfoSetTeamMctsData();
    d->m = std::make_unique<mcts::InfoSetTeamMcts>();
    return qjsbind::wrap<InfoSetTeamMctsData>(ctx, d);
}

// ═══════════════════════════════════════════════════════════════════════════
// Install
// ═══════════════════════════════════════════════════════════════════════════

void installBeliefBindings(JSContext* ctx, JSValue gameObj) {
    registerClasses(ctx);

    JS_SetPropertyStr(ctx, gameObj, "createTeamBelief",
        JS_NewCFunction(ctx, js_createTeamBelief, "createTeamBelief", 1));
    JS_SetPropertyStr(ctx, gameObj, "observe",
        JS_NewCFunction(ctx, js_observe, "observe", 4));
    JS_SetPropertyStr(ctx, gameObj, "mergeObservations",
        JS_NewCFunction(ctx, js_mergeObservations, "mergeObservations", 3));
    JS_SetPropertyStr(ctx, gameObj, "createInfoSetMcts",
        JS_NewCFunction(ctx, js_createInfoSetMcts, "createInfoSetMcts", 0));
    JS_SetPropertyStr(ctx, gameObj, "createInfoSetTeamMcts",
        JS_NewCFunction(ctx, js_createInfoSetTeamMcts, "createInfoSetTeamMcts", 0));
}

} // namespace bro::js
