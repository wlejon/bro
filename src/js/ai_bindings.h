#pragma once

extern "C" {
#include "quickjs.h"
}

namespace brogameagent { class Agent; class World; }

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

} // namespace bro::js
