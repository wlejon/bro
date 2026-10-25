#pragma once
// A render's CPU stalls, named in the log: a render over kSlowRenderMs on
// the CPU (a frame the window waits on) logs what took the time, the steps
// of SceneVkBridge::render3D and the frame graph's passes over kStepMs, and
// the pipelines built and the shader sets compiled in it (first-use work
// that should have been made off the frame). Cheap: a clock read a step.

#include "util/log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace bro::scene::vk::stalls {

constexpr double kSlowRenderMs = 100.0;
constexpr double kStepMs = 10.0;

/// kSlowRenderMs, or BRO_SLOW_RENDER_MS (read once) to name shorter stalls.
inline double slowRenderMs() {
    static const double ms = [] {
        const char* e = std::getenv("BRO_SLOW_RENDER_MS");
        const double v = e ? std::atof(e) : 0.0;
        return v > 0.0 ? v : kSlowRenderMs;
    }();
    return ms;
}

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
    /// Time inside a step spent on one named part (a producer's offer, a
    /// node's draw preparation), summed over the render by name.
    void part(const char* name, double ms) {
        for (Part& p : parts_)
            if (std::strcmp(p.name, name) == 0) {
                p.ms += ms;
                return;
            }
        parts_.push_back({name, ms});
    }
    /// The render is over: logged when it was slow.
    void finish() const {
        const double total = nowMs() - start_;
        if (total < slowRenderMs()) return;
        const Counts& c = counts();
        std::string parts;
        for (const Part& p : parts_) {
            if (p.ms < kStepMs) continue;
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s%s %.0f ms", parts.empty() ? "; within them " : ", ", p.name, p.ms);
            parts += buf;
        }
        LOG_INFO("scene: a render took %.0f ms on the CPU (%s%s); %d pipelines built in %.0f ms, %d shader sets "
                 "compiled in %.0f ms",
                 total, steps_.empty() ? "no one step" : steps_.c_str(), parts.c_str(), c.pipelines, c.pipelineMs,
                 c.shaderSets, c.shaderSetMs);
    }

private:
    struct Part {
        const char* name;   // (outlives the render: a literal, a kind's name)
        double ms;
    };
    double start_, last_;
    std::string steps_;
    std::vector<Part> parts_;
};

/// The render being clocked on this thread (null: none), so the frame
/// graph's passes are steps of it.
inline RenderClock*& current() {
    thread_local RenderClock* c = nullptr;
    return c;
}

/// A scope timed as a part of the render being clocked (none: free).
class Part {
public:
    explicit Part(const char* name) : name_(name), t0_(current() ? nowMs() : 0.0) {}
    ~Part() {
        if (RenderClock* c = current()) c->part(name_, nowMs() - t0_);
    }
    Part(const Part&) = delete;
    Part& operator=(const Part&) = delete;

private:
    const char* name_;
    double t0_;
};

}  // namespace bro::scene::vk::stalls
