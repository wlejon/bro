#include "scene/agent_binding.h"
#include "scene/scene_node.h"
#include "brogameagent/agent.h"
#include "brogameagent/unit.h"
#include "brogameagent/world.h"

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

    // 3) Write transform.
    syncToNode();
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
    }
    node_->setPosition(ax, y, az);
    if (faceMovement_) {
        // brogameagent uses FPS yaw (0 = -Z, positive = clockwise from above).
        // OpenGL rotationY is counter-clockwise from above, so negate.
        node_->setRotationEuler(0.0f, -agent_->yaw(), 0.0f);
    }
}

} // namespace bro::scene
