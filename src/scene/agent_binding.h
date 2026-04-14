#pragma once

#include "brogameagent/capability.h"
#include "brogameagent/policy.h"

#include <memory>

namespace brogameagent { class Agent; class World; }

namespace bro::scene {

class SceneNode;

/// JS-neutral hook invoked when a binding needs to decide its next action.
///
/// scene/ must not depend on QuickJS, so the JS binding layer subclasses this
/// to call a stored JSValue think() function. C++-only consumers can also
/// implement it for bespoke behaviour without going through Policy.
///
/// Contract: on return, `out` is either a valid Action (out.capId != kCapNone)
/// or left untouched (the binding falls through to Hold).
class ThinkHook {
public:
    virtual ~ThinkHook() = default;
    virtual void think(const brogameagent::CapContext& ctx,
                       const brogameagent::CapabilitySet& caps,
                       brogameagent::Action& out) = 0;
};

/// An AI "tool belt" attached to a scene node.
///
/// Holds a non-owning pointer to a brogameagent::Agent (alive externally,
/// typically via a JS wrapper), a per-binding CapabilitySet (the available
/// tools), and either a ThinkHook (JS-primary path) or a Policy (C++ path).
///
/// Stepped once per frame by SceneGraph::syncAgents(dt):
///   1) Advance any in-flight action (advance() called per frame).
///   2) If the action has completed and enough time has passed since the last
///      think tick, invoke the hook/policy for a new action and start it.
///   3) Write agent.x / agent.z / agent.yaw into the associated node's
///      transform (with yOffset on Y and optional facing).
///
/// "One action at a time" semantics: while current.done == false the binding
/// ignores think ticks. Non-blocking capabilities (MoveTo, LaneWalk, Flee)
/// mark done=true in start() so the agent keeps executing background movement
/// while re-deciding every 1/thinkHz seconds. Blocking capabilities
/// (BasicAttack, CastAbility) hold done=false until their duration elapses.
class AgentBinding {
public:
    explicit AgentBinding(SceneNode* node);
    ~AgentBinding();

    AgentBinding(const AgentBinding&) = delete;
    AgentBinding& operator=(const AgentBinding&) = delete;

    SceneNode* node() const { return node_; }

    void setAgent(brogameagent::Agent* a) { agent_ = a; }
    brogameagent::Agent* agent() const { return agent_; }

    brogameagent::CapabilitySet& capabilities() { return capSet_; }
    const brogameagent::CapabilitySet& capabilities() const { return capSet_; }

    void setThinkHook(std::unique_ptr<ThinkHook> h) { thinkHook_ = std::move(h); }
    ThinkHook* thinkHook() const { return thinkHook_.get(); }

    void setPolicy(std::unique_ptr<brogameagent::Policy> p) { policy_ = std::move(p); }
    brogameagent::Policy* policy() const { return policy_.get(); }

    /// Configuration.
    void  setThinkHz(float hz) { thinkHz_ = (hz > 0) ? hz : 15.0f; }
    float thinkHz() const { return thinkHz_; }
    void  setYOffset(float y) { yOffset_ = y; }
    float yOffset() const { return yOffset_; }
    void  setFaceMovement(bool v) { faceMovement_ = v; }
    bool  faceMovement() const { return faceMovement_; }

    /// Current in-flight action (read-only; callers may need to peek for UI).
    const brogameagent::Action& currentAction() const { return current_; }

    /// Per-frame step. Safe to call with world==nullptr (becomes a no-op
    /// apart from transform sync).
    void step(brogameagent::World* world, float dt, float nowSec);

    /// Write agent.x/z/yaw → node transform.
    void syncToNode();

    /// Buffer for JS think() or ThinkHook to stash the chosen action before
    /// returning. Owned by the binding; lifetime = 1 think tick. Reset before
    /// each think() call.
    brogameagent::Action& pending() { return pending_; }

private:
    SceneNode* node_ = nullptr;
    brogameagent::Agent* agent_ = nullptr;

    brogameagent::CapabilitySet capSet_;
    std::unique_ptr<ThinkHook> thinkHook_;
    std::unique_ptr<brogameagent::Policy> policy_;

    brogameagent::Action current_{};
    brogameagent::Action pending_{};

    float thinkHz_    = 15.0f;
    float thinkAccum_ = 0.0f;
    float yOffset_    = 0.0f;
    bool  faceMovement_ = true;
};

} // namespace bro::scene
