// JS bindings for brogameagent::mcts::root_parallel_search /
// root_parallel_search_decoupled — multi-threaded root-parallel MCTS over N
// pre-built Worlds, merged into one action after all threads join.
//
// Both C++ entry points fully join their worker threads before returning, so
// this binding never exposes concurrency to QuickJS (which is single-
// threaded) and needs no JS-side locking. For the same reason, JS-function
// evaluators/rollout policies are rejected outright here: N native threads
// calling back into QuickJS concurrently would be unsafe. Only string
// presets and native/neural (shared_ptr) wrappers are accepted — see
// docs/ai-game-api.js for the full contract.
//
// Installed onto bro.ai.game.

#include "js/ai_bindings.h"
#if BRO_WITH_GAMEAI  // modular-build feature gate

#include <qjsbind/qjsbind.h>
#include <brogameagent/brogameagent.h>

#include <string>
#include <vector>

namespace bro::js {

namespace {

double getDouble(JSContext* ctx, JSValueConst obj, const char* key, double def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    double out = def;
    if (JS_IsNumber(v)) JS_ToFloat64(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}

int32_t getInt(JSContext* ctx, JSValueConst obj, const char* key, int32_t def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    int32_t out = def;
    if (JS_IsNumber(v)) JS_ToInt32(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}

std::string getString(JSContext* ctx, JSValueConst obj, const char* key) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    std::string out;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { out = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    return out;
}

std::vector<brogameagent::World*> parseWorldsArray(JSContext* ctx, JSValueConst opts,
                                                    bool* hadBadEntry) {
    std::vector<brogameagent::World*> out;
    JSValue arr = JS_GetPropertyStr(ctx, opts, "worlds");
    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); return out; }
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);
    out.reserve(len);
    for (int32_t i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        brogameagent::World* w = worldFromJS(ctx, e);
        JS_FreeValue(ctx, e);
        if (!w) { *hadBadEntry = true; break; }
        out.push_back(w);
    }
    JS_FreeValue(ctx, arr);
    return out;
}

// World::registerAbility({fn: ...}) (the only way an app author registers
// abilities from JS — see ai_bindings.cpp's registerAbility binding) wraps
// the JS closure in a std::function that calls JS_Call directly, with no
// thread affinity check. World::resolveAbility invokes that fn on every
// ability cast, including ones mcts::apply performs internally while
// expanding/rolling out search tree nodes — which for root_parallel_search
// happens on N worker threads concurrently. QuickJS is single-threaded;
// concurrent JS_Call from multiple OS threads corrupts its heap (observed:
// an intermittent "Assertion failed: p->ref_count == 0" abort a few
// seconds into a real match, once two worker threads' timing overlapped).
// Detect this before spawning any threads and fail with a clear exception
// instead of undefined behavior — mirrors the evaluator/rolloutPolicy
// JS-function rejection above, extended to cover ability handlers, which
// callers don't pass explicitly but which are reachable from any World.
bool worldHasJsBackedAbility(const brogameagent::World& world) {
    for (const brogameagent::Agent* agent : world.agents()) {
        if (!agent) continue;
        const auto& slots = agent->unit().abilitySlot;
        for (int i = 0; i < brogameagent::Unit::MAX_ABILITIES; i++) {
            int abilityId = slots[i];
            if (abilityId < 0) continue;
            const brogameagent::AbilitySpec* spec = world.abilitySpec(abilityId);
            if (spec && spec->fn) return true;
        }
    }
    return false;
}

bool anyWorldHasJsBackedAbility(const std::vector<brogameagent::World*>& worlds) {
    for (const brogameagent::World* w : worlds) {
        if (w && worldHasJsBackedAbility(*w)) return true;
    }
    return false;
}

// Native-only hero evaluator: a string preset, or a shared/classic native
// wrapper (e.g. a NeuralEvaluator, or a classic-evaluator object). Returns
// nullptr with a pending JS exception if the JS side handed a plain
// function — those can't safely be called from N worker threads.
std::shared_ptr<brogameagent::mcts::IEvaluator>
parseNativeHeroEvaluator(JSContext* ctx, JSValueConst opts, bool* ok) {
    JSValue v = JS_GetPropertyStr(ctx, opts, "evaluator");
    if (auto sp = extractHeroEvaluatorShared(ctx, v))  { JS_FreeValue(ctx, v); return sp; }
    if (auto sp = extractHeroEvaluatorClassic(ctx, v)) { JS_FreeValue(ctx, v); return sp; }
    if (JS_IsFunction(ctx, v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx,
            "rootParallelSearch: opts.evaluator must be a native/string preset, not a JS "
            "function — worker threads cannot safely call back into QuickJS");
        *ok = false;
        return nullptr;
    }
    std::string kind;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { kind = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    if (kind == "hpDelta") return std::make_shared<brogameagent::mcts::HpDeltaEvaluator>();
    return std::make_shared<brogameagent::mcts::HpDeltaEvaluator>();  // default
}

// Native-only rollout policy — same constraint as the evaluator above.
std::shared_ptr<brogameagent::mcts::IRolloutPolicy>
parseNativeRolloutPolicy(JSContext* ctx, JSValueConst opts, bool* ok) {
    JSValue v = JS_GetPropertyStr(ctx, opts, "rolloutPolicy");
    if (auto sp = extractRolloutClassic(ctx, v)) { JS_FreeValue(ctx, v); return sp; }
    if (JS_IsFunction(ctx, v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx,
            "rootParallelSearch: opts.rolloutPolicy must be a native/string preset, not a JS "
            "function — worker threads cannot safely call back into QuickJS");
        *ok = false;
        return nullptr;
    }
    std::string kind;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { kind = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    if (kind == "scripted") return std::make_shared<brogameagent::mcts::ScriptedRollout>();
    if (kind == "random")   return std::make_shared<brogameagent::mcts::RandomRollout>();
    return std::make_shared<brogameagent::mcts::AggressiveRollout>();  // default (incl. "aggressive")
}

brogameagent::mcts::OpponentPolicy parseOpponentPolicyLocal(JSContext* ctx, JSValueConst opts) {
    std::string kind = getString(ctx, opts, "opponentPolicy");
    if (kind == "idle")     return brogameagent::mcts::policy_idle;
    if (kind == "scripted") return brogameagent::mcts::policy_scripted;
    return brogameagent::mcts::policy_aggressive;  // default (incl. "aggressive")
}

JSValue makeParallelStats(JSContext* ctx, const brogameagent::mcts::ParallelSearchStats& s) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "numThreads",       JS_NewInt32(ctx, s.num_threads));
    JS_SetPropertyStr(ctx, o, "totalIterations",  JS_NewInt32(ctx, s.total_iterations));
    JS_SetPropertyStr(ctx, o, "elapsedMs",        JS_NewInt32(ctx, s.elapsed_ms));
    JS_SetPropertyStr(ctx, o, "mergedBestVisits", JS_NewInt32(ctx, s.merged_best_visits));
    return o;
}

// bro.ai.game.rootParallelSearch(opts)
// opts: { worlds, heroId, ...MctsConfig fields, evaluator, rolloutPolicy, opponentPolicy }
// -> { action: CombatAction, stats: ParallelSearchStats }
JSValue js_rootParallelSearch(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "rootParallelSearch(opts): opts object required");
    JSValueConst opts = argv[0];

    bool badWorld = false;
    auto worlds = parseWorldsArray(ctx, opts, &badWorld);
    if (badWorld)
        return JS_ThrowTypeError(ctx, "rootParallelSearch: opts.worlds must contain only AIWorld values");
    if (worlds.empty())
        return JS_ThrowTypeError(ctx, "rootParallelSearch: opts.worlds must be a non-empty array");
    if (anyWorldHasJsBackedAbility(worlds))
        return JS_ThrowTypeError(ctx,
            "rootParallelSearch: opts.worlds contains an ability registered with a JS `fn` "
            "handler — worker threads would call back into QuickJS concurrently and corrupt "
            "it. Register search-clone worlds' abilities as native-only (no `fn`), or don't "
            "use rootParallelSearch with worlds carrying JS-authored abilities.");

    int heroId = getInt(ctx, opts, "heroId", -1);
    if (heroId < 0)
        return JS_ThrowTypeError(ctx, "rootParallelSearch: opts.heroId required");

    auto cfg = parseMctsConfig(ctx, opts);

    bool ok = true;
    auto evaluator = parseNativeHeroEvaluator(ctx, opts, &ok);
    if (!ok) return JS_EXCEPTION;
    auto rollout = parseNativeRolloutPolicy(ctx, opts, &ok);
    if (!ok) return JS_EXCEPTION;
    auto opp = parseOpponentPolicyLocal(ctx, opts);

    brogameagent::mcts::ParallelSearchStats stats{};
    auto action = brogameagent::mcts::root_parallel_search(
        worlds, heroId, cfg, evaluator, rollout, opp, &stats);

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "action", makeCombatAction(ctx, action));
    JS_SetPropertyStr(ctx, result, "stats", makeParallelStats(ctx, stats));
    return result;
}

// bro.ai.game.rootParallelSearchDecoupled(opts)
// opts: { worlds, heroId, oppId, ...MctsConfig fields, evaluator, rolloutPolicy }
// -> { hero: CombatAction, opp: CombatAction, stats: ParallelSearchStats }
JSValue js_rootParallelSearchDecoupled(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "rootParallelSearchDecoupled(opts): opts object required");
    JSValueConst opts = argv[0];

    bool badWorld = false;
    auto worlds = parseWorldsArray(ctx, opts, &badWorld);
    if (badWorld)
        return JS_ThrowTypeError(ctx, "rootParallelSearchDecoupled: opts.worlds must contain only AIWorld values");
    if (worlds.empty())
        return JS_ThrowTypeError(ctx, "rootParallelSearchDecoupled: opts.worlds must be a non-empty array");
    if (anyWorldHasJsBackedAbility(worlds))
        return JS_ThrowTypeError(ctx,
            "rootParallelSearchDecoupled: opts.worlds contains an ability registered with a "
            "JS `fn` handler — worker threads would call back into QuickJS concurrently and "
            "corrupt it. Register search-clone worlds' abilities as native-only (no `fn`), or "
            "don't use rootParallelSearchDecoupled with worlds carrying JS-authored abilities.");

    int heroId = getInt(ctx, opts, "heroId", -1);
    int oppId  = getInt(ctx, opts, "oppId", -1);
    if (heroId < 0 || oppId < 0)
        return JS_ThrowTypeError(ctx, "rootParallelSearchDecoupled: opts.heroId and opts.oppId required");

    auto cfg = parseMctsConfig(ctx, opts);

    bool ok = true;
    auto evaluator = parseNativeHeroEvaluator(ctx, opts, &ok);
    if (!ok) return JS_EXCEPTION;
    auto rollout = parseNativeRolloutPolicy(ctx, opts, &ok);
    if (!ok) return JS_EXCEPTION;

    brogameagent::mcts::ParallelSearchStats stats{};
    auto joint = brogameagent::mcts::root_parallel_search_decoupled(
        worlds, heroId, oppId, cfg, evaluator, rollout, &stats);

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "hero", makeCombatAction(ctx, joint.hero));
    JS_SetPropertyStr(ctx, result, "opp", makeCombatAction(ctx, joint.opp));
    JS_SetPropertyStr(ctx, result, "stats", makeParallelStats(ctx, stats));
    return result;
}

} // namespace

void installParallelBindings(JSContext* ctx, JSValue gameObj) {
    JS_SetPropertyStr(ctx, gameObj, "rootParallelSearch",
        JS_NewCFunction(ctx, js_rootParallelSearch, "rootParallelSearch", 1));
    JS_SetPropertyStr(ctx, gameObj, "rootParallelSearchDecoupled",
        JS_NewCFunction(ctx, js_rootParallelSearchDecoupled, "rootParallelSearchDecoupled", 1));
}

} // namespace bro::js

#endif  // BRO_WITH_GAMEAI
