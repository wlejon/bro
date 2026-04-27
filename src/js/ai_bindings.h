#pragma once

extern "C" {
#include "quickjs.h"
}

namespace brogameagent {
    class Agent;
    class World;
    class NavGrid;
    namespace nn { class SingleHeroNet; class PolicyValueNet; class WeightsHandle; }
    namespace mcts { class Mcts; class IPrior; class IEvaluator; class ITeamEvaluator; class IRolloutPolicy; }
}

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

// ── Cross-binding integration (implemented in ai_binding_integration.cpp) ──

/// Install `bro.ai.game.registerCapability` onto the given namespace object.
/// Called from AIBindings::install once the gameObj exists.
void installRegisterCapability(JSContext* ctx, JSValue gameObj);

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

/// Unwrap helpers for cross-binding use (learn/belief bindings).
brogameagent::nn::SingleHeroNet* nnSingleHeroNetFromJS(JSContext* ctx, JSValueConst v);
std::shared_ptr<brogameagent::nn::SingleHeroNet>
nnSingleHeroNetSharedFromJS(JSContext* ctx, JSValueConst v);
brogameagent::nn::WeightsHandle* nnWeightsHandleFromJS(JSContext* ctx, JSValueConst v);
brogameagent::nn::PolicyValueNet* nnPolicyValueNetFromJS(JSContext* ctx, JSValueConst v);
std::shared_ptr<brogameagent::nn::PolicyValueNet>
nnPolicyValueNetSharedFromJS(JSContext* ctx, JSValueConst v);
brogameagent::mcts::Mcts*        mctsFromJS(JSContext* ctx, JSValueConst v);

// ── Learn bindings (implemented in ai_learn_bindings.cpp) ──

void installLearnBindings(JSContext* ctx, JSValue gameObj);

// ── Belief / observability / InfoSetMcts (ai_belief_bindings.cpp) ──
void installBeliefBindings(JSContext* ctx, JSValue gameObj);

// ── Extras (ai_extras_bindings.cpp): snapshots, projectiles, VecSimulation,
//    classic MCTS evaluators/priors/rollouts as first-class JS objects. ──
void installExtrasBindings(JSContext* ctx, JSValue gameObj);

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

/// Unwrap a wrapped Neural/Gumbel prior as a shared_ptr<IPrior>. Returns empty
/// shared_ptr if the value isn't a bound prior wrapper.
std::shared_ptr<brogameagent::mcts::IPrior>
extractPriorShared(JSContext* ctx, JSValueConst v);

/// Unwrap a wrapped NeuralEvaluator as a shared_ptr<IEvaluator>. Empty if not.
std::shared_ptr<brogameagent::mcts::IEvaluator>
extractHeroEvaluatorShared(JSContext* ctx, JSValueConst v);

} // namespace bro::js
