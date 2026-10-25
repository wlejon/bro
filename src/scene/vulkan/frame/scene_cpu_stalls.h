#pragma once
// A render's CPU stalls, named in the log: a render over kSlowRenderMs on
// the CPU (a frame the window waits on) logs what took the time, the steps
// of SceneVkBridge::render3D and the frame graph's passes over kStepMs, and
// the pipelines built and the shader sets compiled in it (first-use work
// that should have been made off the frame). Cheap: a clock read a step.

#include "util/log.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace bro::scene::vk::stalls {

constexpr double kSlowRenderMs = 100.0;
constexpr double kStepMs = 10.0;

inline double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

/// First-use work this thread did since the render began.
struct Counts {
    int pipelines = 0;
    double pipelineMs = 0.0;
    int shaderSets = 0;
    double shaderSetMs = 0.0;
};
inline Counts& counts() {
    thread_local Counts c;
    return c;
}
inline void notePipeline(double ms) {
    ++counts().pipelines;
    counts().pipelineMs += ms;
}
inline void noteShaderSet(double ms) {
    ++counts().shaderSets;
    counts().shaderSetMs += ms;
}

/// One render's clock: step(name) closes the step begun at the last call.
class RenderClock {
public:
    RenderClock() : start_(nowMs()), last_(start_) { counts() = Counts{}; }
    void step(const char* name) {
        const double t = nowMs();
        if (t - last_ > kStepMs) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s%s %.0f ms", steps_.empty() ? "" : ", ", name, t - last_);
            steps_ += buf;
        }
        last_ = t;
    }
    /// The render is over: logged when it was slow.
    void finish() const {
        const double total = nowMs() - start_;
        if (total < kSlowRenderMs) return;
        const Counts& c = counts();
        LOG_INFO("scene: a render took %.0f ms on the CPU (%s); %d pipelines built in %.0f ms, %d shader sets "
                 "compiled in %.0f ms",
                 total, steps_.empty() ? "no one step" : steps_.c_str(), c.pipelines, c.pipelineMs, c.shaderSets,
                 c.shaderSetMs);
    }

private:
    double start_, last_;
    std::string steps_;
};

/// The render being clocked on this thread (null: none), so the frame
/// graph's passes are steps of it.
inline RenderClock*& current() {
    thread_local RenderClock* c = nullptr;
    return c;
}

}  // namespace bro::scene::vk::stalls
