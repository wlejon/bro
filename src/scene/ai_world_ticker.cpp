#include "scene/ai_world_ticker.h"
#include "brogameagent/world.h"

namespace bro::scene {

AIWorldTicker::AIWorldTicker(brogameagent::World* world, float stepHz, int maxStepsPerFrame)
    : world_(world), stepHz_(stepHz > 0 ? stepHz : 60.0f),
      maxSteps_(maxStepsPerFrame > 0 ? maxStepsPerFrame : 8) {}

void AIWorldTicker::tick(float dt) {
    if (!world_ || dt <= 0) return;
    const double stepDt = 1.0 / static_cast<double>(stepHz_);
    accum_ += dt;
    int steps = 0;
    while (accum_ >= stepDt && steps < maxSteps_) {
        world_->tick(static_cast<float>(stepDt));
        accum_   -= stepDt;
        elapsed_ += static_cast<float>(stepDt);
        steps++;
    }
    // Clamp runaway accumulation after a long stall so we don't forever
    // try to catch up.
    if (accum_ > 0.25) accum_ = 0.25;
}

} // namespace bro::scene
