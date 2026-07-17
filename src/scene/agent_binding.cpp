#include "scene/agent_binding.h"
#include "scene/scene_node.h"
#include "brogameagent/agent.h"
#include "brogameagent/unit.h"
#include "brogameagent/world.h"
#ifdef BROGAMEAGENT_HAS_NAVMESH
#include "brogameagent/nav_mesh.h"
#endif

#include <cmath>

namespace bro::scene {

AgentBinding::AgentBinding(SceneNode* node) : node_(node) {
    current_.done = true;
}

AgentBinding::~AgentBinding() {
    // Give the current capability a chance to clean up (e.g. JS-registered
    // caps that hold a JSValue in Action::jsState).
    if (!current_.done && current_.capId != brogameagent::kCapNone) {
        if (auto* cap = capSet_.get(current_.capId)) {
            brogameagent::CapContext ctx;
            ctx.self = agent_;
            ctx.unit = agent_ ? &agent_->unit() : nullptr;
            ctx.caps = &capSet_;
            cap->cancel(ctx, current_);
        }
    }
}

void AgentBinding::step(brogameagent::World* world, float dt, float nowSec) {
    if (!agent_) {
        syncToNode();
        return;
    }

    brogameagent::CapContext ctx;
    ctx.self  = agent_;
    ctx.unit  = &agent_->unit();
    ctx.world = world;
    ctx.caps  = &capSet_;
    ctx.now   = nowSec;

    // 1) Advance in-flight action.
    if (!current_.done && current_.capId != brogameagent::kCapNone) {
        if (auto* cap = capSet_.get(current_.capId)) {
            cap->advance(ctx, current_, dt);
        } else {
            current_.done = true;
        }
    }

    // 2) Think tick (rate-limited). Skip if dead; also skip if we're mid-action.
    thinkAccum_ += dt;
    const float gap = 1.0f / thinkHz_;
    const bool canThink = current_.done
                       && thinkAccum_ >= gap
                       && agent_->unit().alive();
    if (canThink) {
        thinkAccum_ -= gap;
        if (thinkAccum_ > gap) thinkAccum_ = gap; // clamp on frame-rate spikes

        brogameagent::Action next{};
        bool chosen = false;
        if (thinkHook_) {
            pending_ = brogameagent::Action{};
            pending_.capId = brogameagent::kCapNone;
            thinkHook_->think(ctx, capSet_, pending_);
            if (pending_.capId != brogameagent::kCapNone) {
                next = pending_;
                chosen = true;
            }
        } else if (policy_) {
            chosen = policy_->decide(ctx, capSet_, next);
        }
        if (!chosen || next.capId == brogameagent::kCapNone) {
            next.capId = brogameagent::kCapHold;
        }

        if (auto* cap = capSet_.get(next.capId)) {
            cap->start(ctx, next);
            current_ = next;
        } else {
            current_ = brogameagent::Action{};
            current_.done = true;
        }
    }

    // 3) Follow the active navmesh route (owns the agent's target while
    // active — runs after think so it wins over a same-tick moveTo).
    stepNavigation_(dt);

    // 4) Write transform.
    syncToNode();
}

bool AgentBinding::navigateTo(bromath::Vec3 target, bromath::Vec3 extents,
                              float repathInterval) {
#ifdef BROGAMEAGENT_HAS_NAVMESH
    stopNavigation();
    if (!navMesh_ || !agent_) return false;
    // Start height disambiguates stacked levels: prefer the tracked route/
    // ground height, falling back to the node's current Y.
    float startY = 0.0f;
    if (hasNavY_)            startY = navY_;
    else if (hasGround_)     startY = lastGroundY_;
    else if (node_)          startY = node_->position().y;
    bromath::Vec3 start{agent_->x(), startY, agent_->z()};

    auto path = navMesh_->findPath(start, target, extents);
    if (path.empty()) return false;

    navPath_ = std::move(path);
    navWaypoint_ = 0;
    navActive_ = true;
    navTarget_ = target;
    navExtents_ = extents;
    repathInterval_ = repathInterval;
    repathAccum_ = 0.0f;
    navGeneration_ = navMesh_->generation();
    navY_ = navPath_.front().y;
    hasNavY_ = true;
    return true;
#else
    (void)target; (void)extents; (void)repathInterval;
    return false;
#endif
}

void AgentBinding::stopNavigation() {
    if (navActive_ && agent_) agent_->clearTarget();
    navActive_ = false;
    navPath_.clear();
    navWaypoint_ = 0;
    repathAccum_ = 0.0f;
}

void AgentBinding::stepNavigation_(float dt) {
#ifdef BROGAMEAGENT_HAS_NAVMESH
    if (!navActive_ || !agent_) return;
    if (!agent_->unit().alive()) { stopNavigation(); return; }

    // Surface changed under the active path (a dynamic-obstacle batch was
    // applied, or the mesh was re-baked): the stored waypoints may now cut
    // through an obstacle, so re-plan toward the same goal. When the goal
    // has become unreachable, abandon the route — halting honestly beats
    // walking a stale path through the obstacle.
    if (navMesh_ && navMesh_->generation() != navGeneration_) {
        navGeneration_ = navMesh_->generation();
        bromath::Vec3 start{agent_->x(), navY_, agent_->z()};
        auto p = navMesh_->findPath(start, navTarget_, navExtents_);
        if (p.empty()) { stopNavigation(); return; }
        navPath_ = std::move(p);
        navWaypoint_ = 0;
        repathAccum_ = 0.0f;
    }

    // Optional periodic re-plan toward the same goal (moving obstacles are
    // ORCA's job; this covers a moved goal snapshot or a drifted agent).
    if (repathInterval_ > 0.0f && navMesh_) {
        repathAccum_ += dt;
        if (repathAccum_ >= repathInterval_) {
            repathAccum_ = 0.0f;
            bromath::Vec3 start{agent_->x(), navY_, agent_->z()};
            auto p = navMesh_->findPath(start, navTarget_, navExtents_);
            if (!p.empty()) {
                navPath_ = std::move(p);
                navWaypoint_ = 0;
            }
        }
    }

    // Advance waypoints on XZ proximity. The final waypoint uses the
    // agent's own arrive distance so it settles rather than orbiting.
    constexpr float kAdvanceRadius = 0.75f;
    constexpr float kArriveRadius  = 0.5f;   // matches Agent's ARRIVE_DIST
    while (navWaypoint_ < static_cast<int>(navPath_.size())) {
        const auto& wp = navPath_[static_cast<size_t>(navWaypoint_)];
        const float dx = wp.x - agent_->x();
        const float dz = wp.z - agent_->z();
        const bool last = navWaypoint_ == static_cast<int>(navPath_.size()) - 1;
        const float r = last ? kArriveRadius : kAdvanceRadius;
        if (dx * dx + dz * dz > r * r) break;
        navY_ = wp.y;
        navWaypoint_++;
    }
    if (navWaypoint_ >= static_cast<int>(navPath_.size())) {
        navActive_ = false;
        agent_->clearTarget();
        return;
    }

    const auto& wp = navPath_[static_cast<size_t>(navWaypoint_)];
    agent_->setTarget(wp.x, wp.z);  // World's ORCA pass filters this velocity

    // Route height: interpolate along the current segment by projecting the
    // agent's XZ progress, so ramps carry the node smoothly.
    const bromath::Vec3 from = navWaypoint_ > 0
        ? navPath_[static_cast<size_t>(navWaypoint_ - 1)] : navPath_.front();
    const float sx = wp.x - from.x, sz = wp.z - from.z;
    const float segLenSq = sx * sx + sz * sz;
    if (segLenSq > 1e-6f) {
        float t = ((agent_->x() - from.x) * sx + (agent_->z() - from.z) * sz) / segLenSq;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        navY_ = from.y + (wp.y - from.y) * t;
    } else {
        navY_ = wp.y;
    }
#else
    (void)dt;
#endif
}

void AgentBinding::syncToNode() {
    if (!node_ || !agent_) return;
    const float ax = agent_->x();
    const float az = agent_->z();
    float y = yOffset_;
    if (groundFn_) {
        float gy = 0.0f;
        if (groundFn_(ax, az, gy)) {
            lastGroundY_ = gy;
            hasGround_ = true;
        }
        // yOffset is clearance above the ground while following; until the
        // first successful probe, fall back to the absolute-Y behaviour.
        if (hasGround_) y = lastGroundY_ + yOffset_;
    } else if (hasNavY_) {
        // No ground probe: the navmesh route height (last known when the
        // route has finished) carries the node, with yOffset as clearance.
        y = navY_ + yOffset_;
    }
    node_->setPosition(ax, y, az);
    if (faceMovement_) {
        // brogameagent uses FPS yaw (0 = -Z, positive = clockwise from above).
        // OpenGL rotationY is counter-clockwise from above, so negate.
        node_->setRotationEuler(0.0f, -agent_->yaw(), 0.0f);
    }
}

} // namespace bro::scene
