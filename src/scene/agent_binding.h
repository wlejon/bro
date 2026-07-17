#pragma once

#include "brogameagent/capability.h"
#include "brogameagent/policy.h"
#include "brogameagent/types.h"

#include <functional>
#include <memory>
#include <vector>

namespace brogameagent { class Agent; class World; class NavMesh; }

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

    /// Ground-height probe for the agent's (x, z). Returns true and writes
    /// the ground Y on success; false = "no answer this frame" (the binding
    /// keeps the last known ground height). Installed by the JS layer for
    /// groundFollow (terrain height sample or physics down-raycast); the
    /// binding stays JS/physics/terrain-neutral. While set, yOffset becomes
    /// a clearance above the ground instead of an absolute Y.
    using GroundHeightFn = std::function<bool(float x, float z, float& outY)>;
    void setGroundFollow(GroundHeightFn fn) {
        groundFn_ = std::move(fn);
        hasGround_ = false;
    }
    bool groundFollow() const { return static_cast<bool>(groundFn_); }

    /// Polygon-navmesh routing. The binding shares ownership of the baked
    /// brogameagent::NavMesh with its JS wrapper (navMeshSharedFromJS), so
    /// the mesh stays alive while an agent routes on it even if the app drops
    /// every JS reference. navigateTo() plans a path with NavMesh::findPath
    /// and follows it by feeding successive XZ waypoints to the agent's
    /// setTarget steering, so the World's ORCA avoidance pass composes
    /// unchanged. While navigating, the binding owns the agent's movement
    /// target (a think-hook moveTo issued the same tick is overridden).
    /// Waypoint Y is tracked (interpolated along the current segment) and
    /// drives the node's height when no groundFollow probe is set —
    /// groundFollow, when set, wins.
    void setNavMesh(std::shared_ptr<const brogameagent::NavMesh> m) { navMesh_ = std::move(m); }
    const brogameagent::NavMesh* navMesh() const { return navMesh_.get(); }

    /// Opaque keep-alive tokens for externally-owned state this binding
    /// points at raw (the agent, the AI world, a groundFollow terrain). The
    /// JS layer pins the corresponding JS wrappers here (JSFnRef holders), so
    /// the raw pointers can't dangle if the app drops its own references.
    /// Released when the binding dies — detachAgent, node destroy, subtree
    /// destroy, or graph teardown, all of which run before JS runtime
    /// teardown (see engine_lifecycle.cpp). scene/ stays JS-neutral: the
    /// tokens are shared_ptr<void>.
    void clearKeepAlives() { keepAlive_.clear(); }
    void addKeepAlive(std::shared_ptr<void> p) {
        if (p) keepAlive_.push_back(std::move(p));
    }

    /// Plan a path on the bound navmesh from the agent's position to
    /// `target` and start following it. `extents` are the NavMesh snap
    /// half-extents; `repathInterval` > 0 re-plans toward the same target
    /// every that-many seconds (0 = plan once). Returns false (and does not
    /// start navigating) when there is no navmesh/agent or no COMPLETE path.
    /// Compiled without navmesh support (BROGAMEAGENT_HAS_NAVMESH unset)
    /// this always returns false.
    bool navigateTo(bromath::Vec3 target, bromath::Vec3 extents,
                    float repathInterval = 0.0f);

    /// Abandon the current route (agent halts via clearTarget).
    void stopNavigation();
    bool navigating() const { return navActive_; }

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

    GroundHeightFn groundFn_;
    float lastGroundY_ = 0.0f;
    bool  hasGround_   = false;

    // Externally-owned-state keep-alive pins (see addKeepAlive).
    std::vector<std::shared_ptr<void>> keepAlive_;

    // Navmesh route state (see navigateTo). navY_ is the height of the
    // route under the agent, interpolated along the active path segment.
    void stepNavigation_(float dt);
    std::shared_ptr<const brogameagent::NavMesh> navMesh_;
    std::vector<bromath::Vec3> navPath_;
    int   navWaypoint_ = 0;
    bool  navActive_ = false;
    bromath::Vec3 navTarget_{};
    bromath::Vec3 navExtents_{};
    float repathInterval_ = 0.0f;
    float repathAccum_ = 0.0f;
    float navY_ = 0.0f;
    bool  hasNavY_ = false;
};

} // namespace bro::scene
