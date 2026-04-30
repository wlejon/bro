// JS bindings for the remaining brogameagent surface:
//   - Snapshot / restore (AgentSnapshot, WorldSnapshot as opaque wrappers)
//   - Projectile construction + World::spawnProjectile / projectiles()
//   - VecSimulation (batched 1v1 self-play)
//   - MCTS evaluators / priors / rollouts as first-class JS classes
//   - patch_snapshot_with_particles helper
//
// Installed onto bro.ai.game by installExtrasBindings().

#include "js/ai_bindings.h"

#include <qjsbind/qjsbind.h>
#include <brogameagent/brogameagent.h>
#include <brogameagent/snapshot.h>
#include <brogameagent/projectile.h>
#include <brogameagent/vec_simulation.h>
#include <brogameagent/mcts.h>
#include <brogameagent/info_set_mcts.h>
#include <brogameagent/belief.h>

#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace bro::js {

namespace mcts   = brogameagent::mcts;
namespace belief = brogameagent::belief;

// Forward-decl helpers implemented in other files.
brogameagent::NavGrid* navGridFromJS(JSContext* ctx, JSValueConst v);

// ─── Wrapper structs ───────────────────────────────────────────────────────
struct AgentSnapshotData { brogameagent::AgentSnapshot s; };
struct WorldSnapshotData { brogameagent::WorldSnapshot s; };

struct VecSimulationData {
    std::unique_ptr<brogameagent::VecSimulation> sim;
};

// MCTS primitive wrappers — each holds a shared_ptr<IPrior/IEvaluator/
// ITeamEvaluator/IRolloutPolicy> so it can plug into Mcts/TeamMcts.set_*().
struct HpDeltaEvaluatorData          { std::shared_ptr<mcts::HpDeltaEvaluator> p; };
struct TeamHpDeltaEvaluatorData      { std::shared_ptr<mcts::TeamHpDeltaEvaluator> p; };
struct TeamAdvantageEvaluatorData    { std::shared_ptr<mcts::TeamAdvantageEvaluator> p; };
struct TeamPositionEvaluatorData     { std::shared_ptr<mcts::TeamPositionEvaluator> p; };
struct RandomRolloutData             { std::shared_ptr<mcts::RandomRollout> p; };
struct AggressiveRolloutData         { std::shared_ptr<mcts::AggressiveRollout> p; };
struct ScriptedRolloutData           { std::shared_ptr<mcts::ScriptedRollout> p; };
struct UniformPriorData              { std::shared_ptr<mcts::UniformPrior> p; };
struct AttackBiasPriorData           { std::shared_ptr<mcts::AttackBiasPrior> p; };
struct TacticPriorData               { std::shared_ptr<mcts::TacticPrior> p; };

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

static const char* damageKindStr(brogameagent::DamageKind k) {
    switch (k) {
        case brogameagent::DamageKind::Magical: return "magical";
        case brogameagent::DamageKind::True:    return "true";
        default:                                 return "physical";
    }
}
static brogameagent::DamageKind parseDamageKind(const char* s) {
    if (s && std::strcmp(s, "magical") == 0) return brogameagent::DamageKind::Magical;
    if (s && std::strcmp(s, "true") == 0)    return brogameagent::DamageKind::True;
    return brogameagent::DamageKind::Physical;
}
static brogameagent::ProjectileMode parseProjectileMode(const char* s) {
    if (s && std::strcmp(s, "pierce") == 0) return brogameagent::ProjectileMode::Pierce;
    if (s && std::strcmp(s, "aoe") == 0)    return brogameagent::ProjectileMode::AoE;
    return brogameagent::ProjectileMode::Single;
}
static const char* projectileModeStr(brogameagent::ProjectileMode m) {
    switch (m) {
        case brogameagent::ProjectileMode::Pierce: return "pierce";
        case brogameagent::ProjectileMode::AoE:    return "aoe";
        default:                                    return "single";
    }
}

static JSValue makeProjectile(JSContext* ctx, const brogameagent::Projectile& p) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "id",            JS_NewInt32(ctx, p.id));
    JS_SetPropertyStr(ctx, o, "ownerId",       JS_NewInt32(ctx, p.ownerId));
    JS_SetPropertyStr(ctx, o, "teamId",        JS_NewInt32(ctx, p.teamId));
    JS_SetPropertyStr(ctx, o, "targetId",      JS_NewInt32(ctx, p.targetId));
    JS_SetPropertyStr(ctx, o, "x",             JS_NewFloat64(ctx, p.x));
    JS_SetPropertyStr(ctx, o, "z",             JS_NewFloat64(ctx, p.z));
    JS_SetPropertyStr(ctx, o, "vx",            JS_NewFloat64(ctx, p.vx));
    JS_SetPropertyStr(ctx, o, "vz",            JS_NewFloat64(ctx, p.vz));
    JS_SetPropertyStr(ctx, o, "speed",         JS_NewFloat64(ctx, p.speed));
    JS_SetPropertyStr(ctx, o, "radius",        JS_NewFloat64(ctx, p.radius));
    JS_SetPropertyStr(ctx, o, "damage",        JS_NewFloat64(ctx, p.damage));
    JS_SetPropertyStr(ctx, o, "kind",          JS_NewString(ctx, damageKindStr(p.kind)));
    JS_SetPropertyStr(ctx, o, "remainingLife", JS_NewFloat64(ctx, p.remainingLife));
    JS_SetPropertyStr(ctx, o, "mode",          JS_NewString(ctx, projectileModeStr(p.mode)));
    JS_SetPropertyStr(ctx, o, "splashRadius",  JS_NewFloat64(ctx, p.splashRadius));
    JS_SetPropertyStr(ctx, o, "maxHits",       JS_NewInt32(ctx, p.maxHits));
    JS_SetPropertyStr(ctx, o, "alive",         JS_NewBool(ctx, p.alive));
    return o;
}

static brogameagent::Projectile parseProjectile(JSContext* ctx, JSValueConst o) {
    brogameagent::Projectile p{};
    if (!JS_IsObject(o)) return p;
    p.ownerId       = getInt(ctx, o, "ownerId", -1);
    p.teamId        = getInt(ctx, o, "teamId", 0);
    p.targetId      = getInt(ctx, o, "targetId", -1);
    p.x             = (float)getDouble(ctx, o, "x", 0);
    p.z             = (float)getDouble(ctx, o, "z", 0);
    p.vx            = (float)getDouble(ctx, o, "vx", 0);
    p.vz            = (float)getDouble(ctx, o, "vz", 0);
    p.speed         = (float)getDouble(ctx, o, "speed", 20.0);
    p.radius        = (float)getDouble(ctx, o, "radius", 0.3);
    p.damage        = (float)getDouble(ctx, o, "damage", 0);
    p.remainingLife = (float)getDouble(ctx, o, "remainingLife", 2.0);
    p.splashRadius  = (float)getDouble(ctx, o, "splashRadius", 0);
    p.maxHits       = getInt(ctx, o, "maxHits", 0);
    {
        JSValue kv = JS_GetPropertyStr(ctx, o, "kind");
        if (JS_IsString(kv)) {
            const char* s = JS_ToCString(ctx, kv);
            p.kind = parseDamageKind(s);
            if (s) JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, kv);
    }
    {
        JSValue mv = JS_GetPropertyStr(ctx, o, "mode");
        if (JS_IsString(mv)) {
            const char* s = JS_ToCString(ctx, mv);
            p.mode = parseProjectileMode(s);
            if (s) JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, mv);
    }
    return p;
}

static brogameagent::AgentAction parseAgentAction(JSContext* ctx, JSValueConst obj) {
    brogameagent::AgentAction a;
    a.moveX    = (float)getDouble(ctx, obj, "moveX", 0);
    a.moveZ    = (float)getDouble(ctx, obj, "moveZ", 0);
    a.aimYaw   = (float)getDouble(ctx, obj, "aimYaw", 0);
    a.aimPitch = (float)getDouble(ctx, obj, "aimPitch", 0);
    a.attackTargetId = getInt(ctx, obj, "attackTargetId", -1);
    a.useAbilityId   = getInt(ctx, obj, "useAbilityId", -1);
    return a;
}

using qjsbind::make_float32_array;
static inline JSValue make_int32_array_from_ints(JSContext* ctx, const int* data, size_t count) {
    return qjsbind::make_int32_array(ctx, reinterpret_cast<const int32_t*>(data), count);
}

// ─── Extractors for MCTS shared objects ───────────────────────────────────

std::shared_ptr<mcts::IEvaluator> extractHeroEvaluatorClassic(JSContext* ctx, JSValueConst v) {
    if (auto* d = qjsbind::unwrap<HpDeltaEvaluatorData>(ctx, v)) return d->p;
    return {};
}

std::shared_ptr<mcts::ITeamEvaluator> extractTeamEvaluatorClassic(JSContext* ctx, JSValueConst v) {
    if (auto* d = qjsbind::unwrap<TeamHpDeltaEvaluatorData>(ctx, v))   return d->p;
    if (auto* d = qjsbind::unwrap<TeamAdvantageEvaluatorData>(ctx, v)) return d->p;
    if (auto* d = qjsbind::unwrap<TeamPositionEvaluatorData>(ctx, v))  return d->p;
    return {};
}

std::shared_ptr<mcts::IPrior> extractPriorClassic(JSContext* ctx, JSValueConst v) {
    if (auto* d = qjsbind::unwrap<UniformPriorData>(ctx, v))    return d->p;
    if (auto* d = qjsbind::unwrap<AttackBiasPriorData>(ctx, v)) return d->p;
    if (auto* d = qjsbind::unwrap<TacticPriorData>(ctx, v))     return d->p;
    return {};
}

std::shared_ptr<mcts::IRolloutPolicy> extractRolloutClassic(JSContext* ctx, JSValueConst v) {
    if (auto* d = qjsbind::unwrap<RandomRolloutData>(ctx, v))     return d->p;
    if (auto* d = qjsbind::unwrap<AggressiveRolloutData>(ctx, v)) return d->p;
    if (auto* d = qjsbind::unwrap<ScriptedRolloutData>(ctx, v))   return d->p;
    return {};
}

// ═══════════════════════════════════════════════════════════════════════════
// Class registration
// ═══════════════════════════════════════════════════════════════════════════

static void registerClasses(JSContext* ctx) {
    // Opaque snapshot wrappers.
    qjsbind::Class<AgentSnapshotData>(ctx, "AIAgentSnapshot", qjsbind::NoGlobal)
        .get("id",    [](AgentSnapshotData* d) -> int { return d->s.id; })
        .get("x",     [](AgentSnapshotData* d) -> double { return d->s.x; })
        .get("z",     [](AgentSnapshotData* d) -> double { return d->s.z; })
        .get("yaw",   [](AgentSnapshotData* d) -> double { return d->s.yaw; })
        .get("hp",    [](AgentSnapshotData* d) -> double { return d->s.unit.hp; })
        .get("alive", [](AgentSnapshotData* d) -> bool { return d->s.unit.alive(); });

    qjsbind::Class<WorldSnapshotData>(ctx, "AIWorldSnapshot", qjsbind::NoGlobal)
        .get("agentCount",
            [](WorldSnapshotData* d) -> int { return (int)d->s.agents.size(); })
        .get("projectileCount",
            [](WorldSnapshotData* d) -> int { return (int)d->s.projectiles.size(); })
        .get("eventCount",
            [](WorldSnapshotData* d) -> int { return (int)d->s.events.size(); })
        .get("nextProjectileId",
            [](WorldSnapshotData* d) -> int { return d->s.nextProjectileId; })
        .method("projectiles",
            [](WorldSnapshotData* d, JSContext* ctx) -> JSValue {
                JSValue arr = JS_NewArray(ctx);
                for (size_t i = 0; i < d->s.projectiles.size(); ++i)
                    JS_SetPropertyUint32(ctx, arr, (uint32_t)i, makeProjectile(ctx, d->s.projectiles[i]));
                return arr;
            });

    // ── VecSimulation ─────────────────────────────────────────────────────
    qjsbind::Class<VecSimulationData>(ctx, "AIVecSimulation", qjsbind::NoGlobal)
        .get("numEnvs", [](VecSimulationData* d) -> int { return d->sim ? d->sim->numEnvs() : 0; })
        .method("seedAndReset",
            [](VecSimulationData* d, JSContext* ctx, JSValueConst seedV) {
                if (!d->sim) return;
                int64_t s = 0;
                if (JS_ToBigInt64(ctx, &s, seedV) != 0) {
                    double ds = 0; JS_ToFloat64(ctx, &ds, seedV); s = (int64_t)ds;
                }
                d->sim->seedAndReset((uint64_t)s);
            })
        .method("resetDone", [](VecSimulationData* d) { if (d->sim) d->sim->resetDone(); })
        .method("resetEnv",  [](VecSimulationData* d, int i) { if (d->sim) d->sim->resetEnv(i); })
        .method("observe",
            [](VecSimulationData* d, JSContext* ctx, int agentId) -> JSValue {
                if (!d->sim) return JS_NULL;
                int N = d->sim->numEnvs();
                int total = N * brogameagent::observation::TOTAL;
                std::vector<float> buf(total);
                d->sim->observe(agentId, buf.data());
                return make_float32_array(ctx, buf.data(), total);
            })
        .method("actionMask",
            [](VecSimulationData* d, JSContext* ctx, int agentId) -> JSValue {
                if (!d->sim) return JS_NULL;
                int N = d->sim->numEnvs();
                std::vector<float> mask((size_t)N * brogameagent::action_mask::TOTAL);
                std::vector<int>   ids((size_t)N * brogameagent::action_mask::N_ENEMY_SLOTS);
                d->sim->actionMask(agentId, mask.data(), ids.data());
                JSValue obj = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, obj, "mask",     make_float32_array(ctx, mask.data(), (int)mask.size()));
                JS_SetPropertyStr(ctx, obj, "enemyIds", make_int32_array_from_ints(ctx, ids.data(), (int)ids.size()));
                return obj;
            })
        .method_raw("applyActions",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<VecSimulationData>(ctx, this_val);
                if (!d || !d->sim || argc < 2) return JS_UNDEFINED;
                int32_t agentId = 0; JS_ToInt32(ctx, &agentId, argv[0]);
                if (!JS_IsArray(argv[1])) return JS_ThrowTypeError(ctx, "actions must be array");
                int N = d->sim->numEnvs();
                JSValue lv = JS_GetPropertyStr(ctx, argv[1], "length");
                int32_t len = 0; JS_ToInt32(ctx, &len, lv); JS_FreeValue(ctx, lv);
                std::vector<brogameagent::AgentAction> acts((size_t)N);
                int n = len < N ? len : N;
                for (int i = 0; i < n; i++) {
                    JSValue e = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
                    acts[i] = parseAgentAction(ctx, e);
                    JS_FreeValue(ctx, e);
                }
                d->sim->applyActions(agentId, acts.data());
                return JS_UNDEFINED;
            }, 2)
        .method("step", [](VecSimulationData* d) { if (d->sim) d->sim->step(); })
        .method("dones",
            [](VecSimulationData* d, JSContext* ctx) -> JSValue {
                if (!d->sim) return JS_NULL;
                int N = d->sim->numEnvs();
                std::vector<int> done(N), win(N);
                d->sim->dones(done.data(), win.data());
                JSValue obj = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, obj, "done",   make_int32_array_from_ints(ctx, done.data(), N));
                JS_SetPropertyStr(ctx, obj, "winner", make_int32_array_from_ints(ctx, win.data(),  N));
                return obj;
            })
        .method("rewards",
            [](VecSimulationData* d, JSContext* ctx) -> JSValue {
                if (!d->sim) return JS_NULL;
                int N = d->sim->numEnvs();
                std::vector<float> rh(N), ro(N);
                d->sim->rewards(rh.data(), ro.data());
                JSValue obj = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, obj, "hero",     make_float32_array(ctx, rh.data(), N));
                JS_SetPropertyStr(ctx, obj, "opponent", make_float32_array(ctx, ro.data(), N));
                return obj;
            })
        .method("stepCounts",
            [](VecSimulationData* d, JSContext* ctx) -> JSValue {
                if (!d->sim) return JS_NULL;
                int N = d->sim->numEnvs();
                std::vector<int> v(N);
                d->sim->stepCounts(v.data());
                return make_int32_array_from_ints(ctx, v.data(), N);
            })
        .method("episodeCounts",
            [](VecSimulationData* d, JSContext* ctx) -> JSValue {
                if (!d->sim) return JS_NULL;
                int N = d->sim->numEnvs();
                std::vector<int> v(N);
                d->sim->episodeCounts(v.data());
                return make_int32_array_from_ints(ctx, v.data(), N);
            });

    // ── MCTS primitive wrappers — no methods, constructed via factories.
    // They carry a shared_ptr and are recognised by the extractors above.
    qjsbind::Class<HpDeltaEvaluatorData>      (ctx, "AIHpDeltaEvaluator",        qjsbind::NoGlobal);
    qjsbind::Class<TeamHpDeltaEvaluatorData>  (ctx, "AITeamHpDeltaEvaluator",    qjsbind::NoGlobal);
    qjsbind::Class<TeamAdvantageEvaluatorData>(ctx, "AITeamAdvantageEvaluator",  qjsbind::NoGlobal);
    qjsbind::Class<TeamPositionEvaluatorData> (ctx, "AITeamPositionEvaluator",   qjsbind::NoGlobal);
    qjsbind::Class<RandomRolloutData>         (ctx, "AIRandomRollout",           qjsbind::NoGlobal);
    qjsbind::Class<AggressiveRolloutData>     (ctx, "AIAggressiveRollout",       qjsbind::NoGlobal);
    qjsbind::Class<ScriptedRolloutData>       (ctx, "AIScriptedRollout",         qjsbind::NoGlobal);
    qjsbind::Class<UniformPriorData>          (ctx, "AIUniformPrior",            qjsbind::NoGlobal);
    qjsbind::Class<AttackBiasPriorData>       (ctx, "AIAttackBiasPrior",         qjsbind::NoGlobal);
    qjsbind::Class<TacticPriorData>           (ctx, "AITacticPrior",             qjsbind::NoGlobal)
        .method("setMatchWeight",
            [](TacticPriorData* d, double w) { if (d->p) d->p->set_match_weight((float)w); })
        .method("setOtherWeight",
            [](TacticPriorData* d, double w) { if (d->p) d->p->set_other_weight((float)w); });
}

// ═══════════════════════════════════════════════════════════════════════════
// Factory + free functions
// ═══════════════════════════════════════════════════════════════════════════

static JSValue js_createVecSimulation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    brogameagent::VecSimulation::Config cfg{};
    if (argc >= 1 && JS_IsObject(argv[0])) {
        auto o = argv[0];
        cfg.numEnvs           = getInt(ctx, o, "numEnvs", cfg.numEnvs);
        cfg.arenaHalfSize     = (float)getDouble(ctx, o, "arenaHalfSize", cfg.arenaHalfSize);
        cfg.minSpawnDist      = (float)getDouble(ctx, o, "minSpawnDist", cfg.minSpawnDist);
        cfg.maxSpawnDist      = (float)getDouble(ctx, o, "maxSpawnDist", cfg.maxSpawnDist);
        cfg.dt                = (float)getDouble(ctx, o, "dt", cfg.dt);
        cfg.maxStepsPerEpisode= getInt(ctx, o, "maxStepsPerEpisode", cfg.maxStepsPerEpisode);
        cfg.hp                = (float)getDouble(ctx, o, "hp", cfg.hp);
        cfg.maxMana           = (float)getDouble(ctx, o, "maxMana", cfg.maxMana);
        cfg.manaRegenPerSec   = (float)getDouble(ctx, o, "manaRegenPerSec", cfg.manaRegenPerSec);
        cfg.damage            = (float)getDouble(ctx, o, "damage", cfg.damage);
        cfg.attackRange       = (float)getDouble(ctx, o, "attackRange", cfg.attackRange);
        cfg.attacksPerSec     = (float)getDouble(ctx, o, "attacksPerSec", cfg.attacksPerSec);
        cfg.moveSpeed         = (float)getDouble(ctx, o, "moveSpeed", cfg.moveSpeed);
        cfg.maxAccel          = (float)getDouble(ctx, o, "maxAccel", cfg.maxAccel);
        cfg.maxTurnRate       = (float)getDouble(ctx, o, "maxTurnRate", cfg.maxTurnRate);
        cfg.radius            = (float)getDouble(ctx, o, "radius", cfg.radius);
        cfg.rewardDamageDealt    = (float)getDouble(ctx, o, "rewardDamageDealt", cfg.rewardDamageDealt);
        cfg.rewardDamageTakenMul = (float)getDouble(ctx, o, "rewardDamageTakenMul", cfg.rewardDamageTakenMul);
        cfg.rewardKill           = (float)getDouble(ctx, o, "rewardKill", cfg.rewardKill);
        cfg.rewardDeath          = (float)getDouble(ctx, o, "rewardDeath", cfg.rewardDeath);
        cfg.rewardStep           = (float)getDouble(ctx, o, "rewardStep", cfg.rewardStep);
        cfg.rewardTimeout        = (float)getDouble(ctx, o, "rewardTimeout", cfg.rewardTimeout);
    }
    auto* d = new VecSimulationData();
    d->sim = std::make_unique<brogameagent::VecSimulation>(cfg);
    return qjsbind::wrap<VecSimulationData>(ctx, d);
}

// Agent snapshot / restore
static JSValue js_captureAgentSnapshot(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "captureAgentSnapshot(agent)");
    auto* a = agentFromJS(ctx, argv[0]);
    if (!a) return JS_ThrowTypeError(ctx, "expected agent");
    return qjsbind::wrap<AgentSnapshotData>(ctx, new AgentSnapshotData{ a->captureSnapshot() });
}
static JSValue js_applyAgentSnapshot(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "applyAgentSnapshot(agent, snap)");
    auto* a = agentFromJS(ctx, argv[0]);
    auto* s = qjsbind::unwrap<AgentSnapshotData>(ctx, argv[1]);
    if (!a || !s) return JS_ThrowTypeError(ctx, "bad args");
    a->applySnapshot(s->s);
    return JS_UNDEFINED;
}

// World snapshot / restore
static JSValue js_captureWorldSnapshot(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "captureWorldSnapshot(world)");
    auto* w = worldFromJS(ctx, argv[0]);
    if (!w) return JS_ThrowTypeError(ctx, "expected world");
    return qjsbind::wrap<WorldSnapshotData>(ctx, new WorldSnapshotData{ w->snapshot() });
}
static JSValue js_applyWorldSnapshot(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "applyWorldSnapshot(world, snap)");
    auto* w = worldFromJS(ctx, argv[0]);
    auto* s = qjsbind::unwrap<WorldSnapshotData>(ctx, argv[1]);
    if (!w || !s) return JS_ThrowTypeError(ctx, "bad args");
    w->restore(s->s);
    return JS_UNDEFINED;
}

// Projectile ops via World
static JSValue js_spawnProjectile(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "spawnProjectile(world, projectile)");
    auto* w = worldFromJS(ctx, argv[0]);
    if (!w) return JS_ThrowTypeError(ctx, "expected world");
    auto p = parseProjectile(ctx, argv[1]);
    int id = w->spawnProjectile(p);
    return JS_NewInt32(ctx, id);
}
static JSValue js_worldProjectiles(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "worldProjectiles(world)");
    auto* w = worldFromJS(ctx, argv[0]);
    if (!w) return JS_ThrowTypeError(ctx, "expected world");
    JSValue arr = JS_NewArray(ctx);
    const auto& v = w->projectiles();
    for (size_t i = 0; i < v.size(); ++i)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, makeProjectile(ctx, v[i]));
    return arr;
}

// patch_snapshot_with_particles(snap, particleMap)
static JSValue js_patchSnapshotWithParticles(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "patchSnapshotWithParticles(snap, particleMap)");
    auto* s = qjsbind::unwrap<WorldSnapshotData>(ctx, argv[0]);
    if (!s) return JS_ThrowTypeError(ctx, "expected WorldSnapshot");

    std::unordered_map<int, belief::EnemyParticle> particles;
    if (JS_IsObject(argv[1])) {
        JSPropertyEnum* props = nullptr;
        uint32_t plen = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &plen, argv[1],
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) >= 0) {
            for (uint32_t i = 0; i < plen; i++) {
                JSValue key = JS_AtomToValue(ctx, props[i].atom);
                const char* ks = JS_ToCString(ctx, key);
                int id = ks ? std::atoi(ks) : 0;
                if (ks) JS_FreeCString(ctx, ks);
                JS_FreeValue(ctx, key);
                JSValue val = JS_GetProperty(ctx, argv[1], props[i].atom);
                belief::EnemyParticle p{};
                p.pos.x   = (float)getDouble(ctx, val, "x", 0);
                p.pos.z   = (float)getDouble(ctx, val, "z", 0);
                p.vel.x   = (float)getDouble(ctx, val, "vx", 0);
                p.vel.z   = (float)getDouble(ctx, val, "vz", 0);
                p.hp      = (float)getDouble(ctx, val, "hp", 0);
                p.heading = (float)getDouble(ctx, val, "heading", 0);
                p.weight  = (float)getDouble(ctx, val, "weight", 1.0);
                particles[id] = p;
                JS_FreeValue(ctx, val);
                JS_FreeAtom(ctx, props[i].atom);
            }
            js_free(ctx, props);
        }
    }
    mcts::patch_snapshot_with_particles(s->s, particles);
    return JS_UNDEFINED;
}

// MCTS primitive factories.
static JSValue js_createHpDeltaEvaluator(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return qjsbind::wrap<HpDeltaEvaluatorData>(ctx,
        new HpDeltaEvaluatorData{ std::make_shared<mcts::HpDeltaEvaluator>() });
}
static JSValue js_createTeamHpDeltaEvaluator(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return qjsbind::wrap<TeamHpDeltaEvaluatorData>(ctx,
        new TeamHpDeltaEvaluatorData{ std::make_shared<mcts::TeamHpDeltaEvaluator>() });
}
static JSValue js_createTeamAdvantageEvaluator(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return qjsbind::wrap<TeamAdvantageEvaluatorData>(ctx,
        new TeamAdvantageEvaluatorData{ std::make_shared<mcts::TeamAdvantageEvaluator>() });
}
static JSValue js_createTeamPositionEvaluator(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return qjsbind::wrap<TeamPositionEvaluatorData>(ctx,
        new TeamPositionEvaluatorData{ std::make_shared<mcts::TeamPositionEvaluator>() });
}
static JSValue js_createRandomRollout(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return qjsbind::wrap<RandomRolloutData>(ctx,
        new RandomRolloutData{ std::make_shared<mcts::RandomRollout>() });
}
static JSValue js_createAggressiveRollout(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return qjsbind::wrap<AggressiveRolloutData>(ctx,
        new AggressiveRolloutData{ std::make_shared<mcts::AggressiveRollout>() });
}
static JSValue js_createScriptedRollout(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return qjsbind::wrap<ScriptedRolloutData>(ctx,
        new ScriptedRolloutData{ std::make_shared<mcts::ScriptedRollout>() });
}
static JSValue js_createUniformPrior(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return qjsbind::wrap<UniformPriorData>(ctx,
        new UniformPriorData{ std::make_shared<mcts::UniformPrior>() });
}
static JSValue js_createAttackBiasPrior(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return qjsbind::wrap<AttackBiasPriorData>(ctx,
        new AttackBiasPriorData{ std::make_shared<mcts::AttackBiasPrior>() });
}
static JSValue js_createTacticPrior(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return qjsbind::wrap<TacticPriorData>(ctx,
        new TacticPriorData{ std::make_shared<mcts::TacticPrior>() });
}

// ═══════════════════════════════════════════════════════════════════════════
// Install
// ═══════════════════════════════════════════════════════════════════════════

void installExtrasBindings(JSContext* ctx, JSValue gameObj) {
    registerClasses(ctx);

    // Snapshots
    JS_SetPropertyStr(ctx, gameObj, "captureAgentSnapshot",
        JS_NewCFunction(ctx, js_captureAgentSnapshot, "captureAgentSnapshot", 1));
    JS_SetPropertyStr(ctx, gameObj, "applyAgentSnapshot",
        JS_NewCFunction(ctx, js_applyAgentSnapshot, "applyAgentSnapshot", 2));
    JS_SetPropertyStr(ctx, gameObj, "captureWorldSnapshot",
        JS_NewCFunction(ctx, js_captureWorldSnapshot, "captureWorldSnapshot", 1));
    JS_SetPropertyStr(ctx, gameObj, "applyWorldSnapshot",
        JS_NewCFunction(ctx, js_applyWorldSnapshot, "applyWorldSnapshot", 2));

    // Projectiles
    JS_SetPropertyStr(ctx, gameObj, "spawnProjectile",
        JS_NewCFunction(ctx, js_spawnProjectile, "spawnProjectile", 2));
    JS_SetPropertyStr(ctx, gameObj, "worldProjectiles",
        JS_NewCFunction(ctx, js_worldProjectiles, "worldProjectiles", 1));

    // IS-MCTS helper
    JS_SetPropertyStr(ctx, gameObj, "patchSnapshotWithParticles",
        JS_NewCFunction(ctx, js_patchSnapshotWithParticles, "patchSnapshotWithParticles", 2));

    // VecSimulation
    JS_SetPropertyStr(ctx, gameObj, "createVecSimulation",
        JS_NewCFunction(ctx, js_createVecSimulation, "createVecSimulation", 1));

    // MCTS primitives as first-class objects
    JS_SetPropertyStr(ctx, gameObj, "createHpDeltaEvaluator",
        JS_NewCFunction(ctx, js_createHpDeltaEvaluator, "createHpDeltaEvaluator", 0));
    JS_SetPropertyStr(ctx, gameObj, "createTeamHpDeltaEvaluator",
        JS_NewCFunction(ctx, js_createTeamHpDeltaEvaluator, "createTeamHpDeltaEvaluator", 0));
    JS_SetPropertyStr(ctx, gameObj, "createTeamAdvantageEvaluator",
        JS_NewCFunction(ctx, js_createTeamAdvantageEvaluator, "createTeamAdvantageEvaluator", 0));
    JS_SetPropertyStr(ctx, gameObj, "createTeamPositionEvaluator",
        JS_NewCFunction(ctx, js_createTeamPositionEvaluator, "createTeamPositionEvaluator", 0));
    JS_SetPropertyStr(ctx, gameObj, "createRandomRollout",
        JS_NewCFunction(ctx, js_createRandomRollout, "createRandomRollout", 0));
    JS_SetPropertyStr(ctx, gameObj, "createAggressiveRollout",
        JS_NewCFunction(ctx, js_createAggressiveRollout, "createAggressiveRollout", 0));
    JS_SetPropertyStr(ctx, gameObj, "createScriptedRollout",
        JS_NewCFunction(ctx, js_createScriptedRollout, "createScriptedRollout", 0));
    JS_SetPropertyStr(ctx, gameObj, "createUniformPrior",
        JS_NewCFunction(ctx, js_createUniformPrior, "createUniformPrior", 0));
    JS_SetPropertyStr(ctx, gameObj, "createAttackBiasPrior",
        JS_NewCFunction(ctx, js_createAttackBiasPrior, "createAttackBiasPrior", 0));
    JS_SetPropertyStr(ctx, gameObj, "createTacticPrior",
        JS_NewCFunction(ctx, js_createTacticPrior, "createTacticPrior", 0));

    // Projectile mode / damage kind constants
    {
        JSValue m = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, m, "Single", JS_NewString(ctx, "single"));
        JS_SetPropertyStr(ctx, m, "Pierce", JS_NewString(ctx, "pierce"));
        JS_SetPropertyStr(ctx, m, "AoE",    JS_NewString(ctx, "aoe"));
        JS_SetPropertyStr(ctx, gameObj, "PROJECTILE_MODE", m);
    }
    {
        JSValue m = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, m, "Physical", JS_NewString(ctx, "physical"));
        JS_SetPropertyStr(ctx, m, "Magical",  JS_NewString(ctx, "magical"));
        JS_SetPropertyStr(ctx, m, "True",     JS_NewString(ctx, "true"));
        JS_SetPropertyStr(ctx, gameObj, "DAMAGE_KIND", m);
    }
}

} // namespace bro::js
