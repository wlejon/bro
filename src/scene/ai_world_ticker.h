#pragma once

namespace brogameagent { class World; }

namespace bro::scene {

/// Fixed-step driver for a brogameagent::World.
///
/// Scene binds a JS-owned World via SceneGraph::attachAIWorld; every frame
/// SceneGraph::syncAgents(dt) forwards the real frame delta to tick(), which
/// runs world->tick(1/stepHz) as many times as needed (up to maxStepsPerFrame)
/// to drain the accumulator. This lifts the manual accumulator pattern from
/// apps/ai-arena into C++ so games don't re-implement it per app.
class AIWorldTicker {
public:
    AIWorldTicker(brogameagent::World* world,
                  float stepHz = 60.0f,
                  int   maxStepsPerFrame = 8);

    void tick(float dt);

    brogameagent::World* world() const { return world_; }
    float nowSec() const { return elapsed_; }
    float stepHz() const { return stepHz_; }

    void setStepHz(float hz) { if (hz > 0) stepHz_ = hz; }
    void setMaxSteps(int n) { if (n > 0) maxSteps_ = n; }

private:
    brogameagent::World* world_ = nullptr;
    float  stepHz_   = 60.0f;
    int    maxSteps_ = 8;
    double accum_    = 0.0;
    float  elapsed_  = 0.0f;
};

} // namespace bro::scene
