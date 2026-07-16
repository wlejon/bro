#pragma once

extern "C" {
#include "quickjs.h"
}

namespace brogameagent {
    class Agent;
    class World;
    class NavGrid;
    namespace nn { class SingleHeroNet; class PolicyValueNet; class SingleHeroNetTX; class WeightsHandle; }
    namespace mcts {
        class Mcts; class IPrior; class IEvaluator; class ITeamEvaluator; class IRolloutPolicy;
        struct MctsConfig; struct CombatAction;
    }
    namespace learn { class IInferenceBackend; }
}

namespace brotensor { struct Tensor; }

#include <memory>

namespace bro::js {

class AIBindings {
public:
    static void install(JSContext* ctx);
    static void cleanup(JSContext* ctx);
};

/// Unwrap a JSValue produced by bro.ai.game.createAgent into the underlying
/// brogameagent::Agent*. Returns nullptr if the value isn't an AI agent.
brogameagent::Agent* agentFromJS(JSContext* ctx, JSValueConst val);

/// Unwrap a JSValue produced by bro.ai.game.createWorld into the underlying
/// brogameagent::World*. Returns nullptr if the value isn't an AI world.
brogameagent::World* worldFromJS(JSContext* ctx, JSValueConst val);

/// Look up the JS wrapper (AgentData JSValue) for a given brogameagent::Agent
/// by walking the world's __agents array. Caller owns the returned JSValue
/// ref (must JS_FreeValue). Returns JS_NULL if not found.
JSValue findAgentJSRef(JSContext* ctx, JSValueConst worldJsRef, brogameagent::Agent* agent);

/// Apply an `avoidance` opts value (bool or {enabled?, radius?, maxSpeed?,
/// neighborDist?, maxNeighbors?, timeHorizon?, timeHorizonObst?}) onto an
/// agent's ORCA participation config. Shared by createAgent,
/// agent.setAvoidance and node.attachAgent's avoidance option.
void applyAgentAvoidanceOpts(JSContext* ctx, JSValueConst val, brogameagent::Agent& agent);

// ── Cross-binding integration (implemented in ai_binding_integration.cpp) ──

/// Install `bro.ai.game.registerCapability` onto the given namespace object.
/// Called from AIBindings::install once the gameObj exists.
void installRegisterCapability(JSContext* ctx, JSValue gameObj);

/// Free the JS gate/start/advance callbacks held by the process-global
/// capability registry and clear it. Called from AIBindings::cleanup during
/// engine teardown — the registry natively holds JSValue refs, which would
/// otherwise still be live at JS_FreeRuntime and trip its leak assert.
void clearRegisteredCapabilities(JSContext* ctx);

/// JS method bodies: node.attachAgent / node.detachAgent /
/// graph.attachAIWorld / graph.detachAIWorld. Exposed for the scene bindings
/// to register on NodeWrapper / GraphWrapper classes.
JSValue nodeAttachAgent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue nodeDetachAgent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue graphAttachAIWorld(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue graphDetachAIWorld(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

// ── NN bindings (implemented in ai_nn_bindings.cpp) ──

/// Install bro.ai.game.nn namespace (Tensor, circuits, heads, net, WeightsHandle, ops).
void installNNBindings(JSContext* ctx, JSValue gameObj);

/// Install bro.tensor namespace (GPU tensor + ops via brotensor).
/// Implemented in tensor_bindings.cpp; gated on BROTENSOR_HAS_GPU at build
/// time (real bindings when CUDA or Metal is enabled). When neither backend
/// is available, installs a stub `bro.tensor = { available: false }`.
void installTensorBindings(JSContext* ctx);

/// Unwrap an AIGpuTensor JS value to the underlying brotensor::Tensor*.
/// Returns nullptr if the value is not a GpuTensor wrapper or no GPU backend
/// is available.
::brotensor::Tensor* gpuTensorFromJS(JSContext* ctx, JSValueConst v);

/// Unwrap helpers for cross-binding use (learn/belief bindings).
brogameagent::nn::SingleHeroNet* nnSingleHeroNetFromJS(JSContext* ctx, JSValueConst v);
std::shared_ptr<brogameagent::nn::SingleHeroNet>
nnSingleHeroNetSharedFromJS(JSContext* ctx, JSValueConst v);
brogameagent::nn::WeightsHandle* nnWeightsHandleFromJS(JSContext* ctx, JSValueConst v);
brogameagent::nn::PolicyValueNet* nnPolicyValueNetFromJS(JSContext* ctx, JSValueConst v);
std::shared_ptr<brogameagent::nn::PolicyValueNet>
nnPolicyValueNetSharedFromJS(JSContext* ctx, JSValueConst v);
brogameagent::nn::SingleHeroNetTX* nnSingleHeroNetTXFromJS(JSContext* ctx, JSValueConst v);
std::shared_ptr<brogameagent::nn::SingleHeroNetTX>
nnSingleHeroNetTXSharedFromJS(JSContext* ctx, JSValueConst v);
brogameagent::mcts::Mcts*        mctsFromJS(JSContext* ctx, JSValueConst v);

// ── Learn bindings (implemented in ai_learn_bindings.cpp) ──

void installLearnBindings(JSContext* ctx, JSValue gameObj);

// ── Belief / observability / InfoSetMcts (ai_belief_bindings.cpp) ──
void installBeliefBindings(JSContext* ctx, JSValue gameObj);

// ── Extras (ai_extras_bindings.cpp): snapshots, projectiles, VecSimulation,
//    classic MCTS evaluators/priors/rollouts as first-class JS objects. ──
void installExtrasBindings(JSContext* ctx, JSValue gameObj);

// ── Env-agnostic GenericMcts (ai_generic_mcts_bindings.cpp) ──
void installGenericMctsBindings(JSContext* ctx, JSValue gameObj);

// ── Grid-world / platformer training kit (ai_grid_bindings.cpp) ──
void installGridBindings(JSContext* ctx, JSValue gameObj);

/// Classic-MCTS wrapper extractors. Return empty shared_ptr if not a match.
std::shared_ptr<brogameagent::mcts::IEvaluator>
extractHeroEvaluatorClassic(JSContext* ctx, JSValueConst v);
std::shared_ptr<brogameagent::mcts::ITeamEvaluator>
extractTeamEvaluatorClassic(JSContext* ctx, JSValueConst v);
std::shared_ptr<brogameagent::mcts::IPrior>
extractPriorClassic(JSContext* ctx, JSValueConst v);
std::shared_ptr<brogameagent::mcts::IRolloutPolicy>
extractRolloutClassic(JSContext* ctx, JSValueConst v);

/// Unwrap a NavGridData JSValue into a brogameagent::NavGrid*. Returns nullptr
/// if the value isn't a NavGrid wrapper.
brogameagent::NavGrid* navGridFromJS(JSContext* ctx, JSValueConst v);

/// Construct a wrapped AINavGrid (a bro.ai.game NavGrid JS value) covering the
/// given world-space XZ bounds at `cellSize`. Lets other bindings (e.g.
/// TileWorld::syncNavGrid / toNavGrid) hand back a ready-to-use nav grid that
/// shares the AINavGrid prototype. Caller owns the returned JSValue.
JSValue createNavGridJS(JSContext* ctx, float minX, float minZ,
                        float maxX, float maxZ, float cellSize);

/// Unwrap a wrapped Neural/Gumbel prior as a shared_ptr<IPrior>. Returns empty
/// shared_ptr if the value isn't a bound prior wrapper.
std::shared_ptr<brogameagent::mcts::IPrior>
extractPriorShared(JSContext* ctx, JSValueConst v);

/// Unwrap a wrapped NeuralEvaluator as a shared_ptr<IEvaluator>. Empty if not.
std::shared_ptr<brogameagent::mcts::IEvaluator>
extractHeroEvaluatorShared(JSContext* ctx, JSValueConst v);

/// Parse the common MctsConfig fields (iterations, budgetMs, rolloutHorizon,
/// simDt, actionRepeat, uctC, seed, tacticWindowDecisions, pwAlpha, priorC,
/// optionMaxWindows, useLeafValue) off a flat JS options object. Shared by
/// every MCTS-variant creator and by the root-parallel bindings.
brogameagent::mcts::MctsConfig parseMctsConfig(JSContext* ctx, JSValueConst opts);

/// Build a plain {moveDir, attackSlot, abilitySlot} JS object from a
/// CombatAction. Shared by every MCTS-variant creator and by the
/// root-parallel bindings.
JSValue makeCombatAction(JSContext* ctx, const brogameagent::mcts::CombatAction& a);

// ── Root-parallel search (implemented in ai_parallel_bindings.cpp) ──
void installParallelBindings(JSContext* ctx, JSValue gameObj);

/// Unwrap a wrapped DirectBackend/ServerBackend (ai_learn_bindings.cpp) into
/// the underlying brogameagent::learn::IInferenceBackend*. Returns nullptr
/// if the value isn't a bound backend wrapper. Used by ai_generic_mcts_
/// bindings.cpp's `backend` option to install a native prior/value fast
/// path (no JS-callback round trip per MCTS node).
brogameagent::learn::IInferenceBackend*
inferenceBackendFromJS(JSContext* ctx, JSValueConst v);

} // namespace bro::js
