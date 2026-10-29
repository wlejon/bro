// perf.* for the 3D scene's measurement hygiene (docs/headless.md "Shader
// A/B" and "Settling a scene"): a second set of the scene's shaders measured
// against the first frame by frame in one process, and the scene's settle
// state with a helper that runs frames on a held clock until it is settled.

#include "bronze_host/host_headless_internal.h"
#include "bronze_host/host_access.h"
#include "bronze_host/host_brokit.h"
#include "engine/engine.h"
#if BRO_WITH_3D
#include "scene/scene_renderer.h"

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <vector>
#endif

namespace bro::bronze_host {

#if !BRO_WITH_3D
void installHeadlessScenePerf(engine::Engine&, const std::function<void(const char*, uint32_t, ev::NativeFn)>&) {}
#else
namespace {

constexpr double kFrameMs = 1000.0 / 30.0;

// One frame of the headless clock, as advanceTime(1000 / 30) runs it.
void stepFrame(engine::Engine& engine) {
    engine.advanceTime(kFrameMs);
    pumpBrokitTicks();
    drainMicrotasksAndLocalFetches();
}

// The scene's clock held (bro.time.scale 0) while it lives: the scene's
// work goes on frame by frame, the wind, sway and drift do not.
struct HeldClock {
    engine::Engine& engine;
    double scale;
    bool held;
    HeldClock(engine::Engine& e, bool hold) : engine(e), scale(e.timeScale()), held(hold) {
        if (held) engine.setTimeScale(0.0);
    }
    ~HeldClock() {
        if (held) engine.setTimeScale(scale);
    }
};

double optNumber(Value opts, const char* key, double fallback) {
    if (!ev::isObject(opts)) return fallback;
    const Value v = ev::getProperty(opts, key);
    return ev::isNumber(v) ? ev::toDouble(v) : fallback;
}

bool optBool(Value opts, const char* key, bool fallback) {
    if (!ev::isObject(opts)) return fallback;
    const Value v = ev::getProperty(opts, key);
    return ev::isUndefined(v) ? fallback : ev::toBool(v);
}

std::string optString(Value opts, const char* key) {
    if (!ev::isObject(opts)) return {};
    const Value v = ev::getProperty(opts, key);
    return ev::isString(v) ? ev::toUtf8(v) : std::string();
}

// A `defines` option: names split at spaces and commas ("KO_SUN KO_FORMS").
std::vector<std::string> optNames(Value opts, const char* key) {
    std::vector<std::string> names;
    std::string cur;
    for (char c : optString(opts, key) + " ") {
        if (c == ' ' || c == ',') {
            if (!cur.empty()) names.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    return names;
}

Value fromJson(const nlohmann::json& j) {
    ev::CallResult r = ev::parseJson(j.dump());
    return r.thrown ? ev::undefined() : r.value;
}

nlohmann::json summaryJson(const scene::SceneRenderer::ShaderSetSummary& s) {
    nlohmann::json j;
    j["dir"] = s.dir;
    j["shaders"] = s.shaders;
    j["differing"] = s.differing;
    j["pipelines"] = s.pipelines;
    j["twinned"] = s.twinned;
    j["failed"] = s.failed;
    return j;
}

std::string absoluteDir(const std::string& dir) {
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::absolute(dir, ec);
    std::string s = ec ? dir : p.generic_string();
    return s;
}

struct Summary {
    double mean = 0, median = 0, min = 0;
};

Summary summarize(std::vector<double> v) {
    Summary s;
    if (v.empty()) return s;
    double sum = 0;
    for (double x : v) sum += x;
    s.mean = sum / static_cast<double>(v.size());
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    s.median = n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    s.min = v.front();
    return s;
}

nlohmann::json summaryOf(const std::vector<double>& v) {
    const Summary s = summarize(v);
    return {{"mean", s.mean}, {"median", s.median}, {"min", s.min}};
}

// B minus A over the pairs: the mean, the median, the mean's standard error
// and the mean as a share of A's mean (%).
nlohmann::json diffOf(const std::vector<double>& a, const std::vector<double>& b) {
    std::vector<double> d;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) d.push_back(b[i] - a[i]);
    const Summary s = summarize(d);
    double var = 0;
    for (double x : d) var += (x - s.mean) * (x - s.mean);
    const double n = static_cast<double>(d.size());
    const double se = n > 1 ? std::sqrt(var / (n - 1) / n) : 0.0;
    const double aMean = summarize(a).mean;
    return {{"ms", s.mean}, {"median", s.median}, {"se", se}, {"pct", aMean > 0 ? 100.0 * s.mean / aMean : 0.0}};
}

// The frame's passes by name, the compute stream's prefixed "compute:".
std::map<std::string, double> framePasses(engine::Engine& engine) {
    std::vector<std::pair<std::string, double>> passes, compute;
    bool computed = false;
    std::map<std::string, double> out;
    if (!engine.gpuPasses(passes, compute, computed)) return out;
    for (const auto& [name, ms] : passes) out[name] += ms;
    if (computed)
        for (const auto& [name, ms] : compute)
            if (name != "start") out["compute:" + name] += ms;
    return out;
}

} // namespace

void installHeadlessScenePerf(engine::Engine& engine,
                              const std::function<void(const char*, uint32_t, ev::NativeFn)>& setPerfFn) {
    // perf.pendingWork(): the scene's settle state, [] once settled.
    setPerfFn("pendingWork", 0,
        [&engine](Value, std::span<const Value>) -> Value {
            return fromJson(nlohmann::json(engine.scenePendingWork()));
        });
    // perf.settle({ maxFrames, held, exposure, kinds }): frames until settled.
    setPerfFn("settle", 1,
        [&engine](Value, std::span<const Value> a) -> Value {
            const Value opts = a.empty() ? ev::undefined() : a[0];
            const int maxFrames = static_cast<int>(std::max(0.0, optNumber(opts, "maxFrames", 4000)));
            const bool held = optBool(opts, "held", true);
            const bool exposure = optBool(opts, "exposure", true);
            // Work whose reasons have not changed for this many frames is
            // stalled (not coming in): the wait ends there.
            const int stallFrames = static_cast<int>(std::max(1.0, optNumber(opts, "stallFrames", 600)));
            // { kinds: ['world service', ...] }: wait for those kinds only.
            std::vector<std::string> kinds;
            if (ev::isObject(opts)) {
                // (Rooted: the reads below may allocate.)
                const ev::Persistent k(ev::getProperty(opts, "kinds"));
                if (ev::isObject(k.get())) {
                    const uint32_t n = static_cast<uint32_t>(optNumber(k.get(), "length", 0));
                    for (uint32_t i = 0; i < n; ++i) {
                        const Value e = ev::getProperty(k.get(), std::to_string(i).c_str());
                        if (ev::isString(e)) kinds.push_back(ev::toUtf8(e));
                    }
                }
            }
            const auto pendingNow = [&engine, &kinds] {
                std::vector<std::string> all = engine.scenePendingWork();
                if (kinds.empty()) return all;
                std::vector<std::string> some;
                for (std::string& r : all)
                    if (std::find(kinds.begin(), kinds.end(), r.substr(0, r.find(':'))) != kinds.end())
                        some.push_back(std::move(r));
                return some;
            };
            // (SceneRenderer::pendingWork's reasons for them, whatever `kinds` says.)
            const auto building = [&engine] {
                for (const std::string& r : engine.scenePendingWork())
                    if (r.rfind("pipelines:", 0) == 0 && r.find("building") != std::string::npos) return true;
                return false;
            };
            HeldClock clock(engine, held);
            int frames = 0;
            // The frames each kind of work (a reason's text before its ':')
            // was pending: what the wait was for.
            std::map<std::string, int> waited;
            const auto note = [&waited](const std::vector<std::string>& pending) {
                std::map<std::string, bool> kinds;
                for (const std::string& r : pending) kinds[r.substr(0, r.find(':'))] = true;
                for (const auto& [k, _] : kinds) ++waited[k];
            };
            scene::SceneRenderer::settleAll(true);
            std::vector<std::string> pending = pendingNow();
            int same = 0;
            for (; !pending.empty() && frames < maxFrames && same < stallFrames; ++frames) {
                note(pending);
                // (Held: the exposure adapts as it would over the frame.)
                if (held && exposure) engine.advanceSceneExposure(static_cast<float>(kFrameMs / 1000.0));
                stepFrame(engine);
                std::vector<std::string> now = pendingNow();
                // (Pipelines building finish in wall time, not frames: while
                // they are under way an unchanged frame is no stall, and the
                // next waits a moment for them, whatever a kinds filter hides.)
                if (now == pending && building())
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                else
                    same = now == pending ? same + 1 : 0;
                pending = std::move(now);
            }
            scene::SceneRenderer::settleAll(false);
            nlohmann::json j;
            j["settled"] = pending.empty();
            j["stalled"] = !pending.empty() && same >= stallFrames;
            j["frames"] = frames;
            j["pending"] = pending;
            j["waited"] = waited;
            return fromJson(j);
        });
    // perf.asyncShaders([on]): whether frames skip what is still compiling
    // (a window's policy) or wait for it (headless's); `on` sets it for the
    // process from now on. Returns the policy in force.
    setPerfFn("asyncShaders", 1,
        [](Value, std::span<const Value> a) -> Value {
            if (!a.empty() && !ev::isUndefined(a[0])) scene::SceneRenderer::setAsyncShaders(ev::toBool(a[0]));
            return ev::fromBool(scene::SceneRenderer::asyncShaders());
        });
    // perf.loadShaderSet(dir, { defines }): set B from a shader tree, its
    // shaders compiled with `defines` ("A B": #define A 1 ...).
    setPerfFn("loadShaderSet", 2,
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.empty() || !ev::isString(a[0])) return ev::throwTypeError("loadShaderSet(dir) requires a directory");
            scene::SceneRenderer::ShaderSetSummary s;
            std::string err;
            const std::string dir = absoluteDir(ev::toUtf8(a[0]));
            requireHarnessPathAccess(engine, dir, false);  // the harness's, not the app's (host_access.h)
            if (!scene::SceneRenderer::loadShaderSet(dir, s, &err, optNames(a.size() > 1 ? a[1] : ev::undefined(), "defines")))
                return ev::throwTypeError(err.c_str());
            return fromJson(summaryJson(s));
        });
    // perf.useShaderSet('a' | 'b'): the set the scene binds.
    setPerfFn("useShaderSet", 1,
        [](Value, std::span<const Value> a) -> Value {
            const std::string which = a.empty() || !ev::isString(a[0]) ? std::string() : ev::toUtf8(a[0]);
            if (which != "a" && which != "b") return ev::throwTypeError("useShaderSet('a' | 'b')");
            if (!scene::SceneRenderer::useShaderSet(which == "b" ? 1 : 0))
                return ev::throwTypeError("useShaderSet('b'): no shader set B is loaded (perf.loadShaderSet)");
            return ev::undefined();
        });
    // perf.compareShaders({ b, frames, warm, held }): A and B on the same
    // view, a frame each in turn (A B B A ...), each frame's GPU ms waited on.
    setPerfFn("compareShaders", 1,
        [&engine](Value, std::span<const Value> a) -> Value {
            const Value opts = a.empty() ? ev::undefined() : a[0];
            const std::string dir = optString(opts, "b");
            const int frames = static_cast<int>(std::max(1.0, optNumber(opts, "frames", 60)));
            const int warm = static_cast<int>(std::max(0.0, optNumber(opts, "warm", 4)));
            const bool held = optBool(opts, "held", true);
            scene::SceneRenderer::ShaderSetSummary set = scene::SceneRenderer::shaderSetSummary();
            if (!dir.empty() && absoluteDir(dir) + "/" != set.dir && absoluteDir(dir) != set.dir) {
                std::string err;
                if (!scene::SceneRenderer::loadShaderSet(absoluteDir(dir), set, &err))
                    return ev::throwTypeError(err.c_str());
            }
            if (set.dir.empty()) return ev::throwTypeError("compareShaders: no set B (pass { b: dir })");
            HeldClock clock(engine, held);
            framePasses(engine);   // (the first call turns the stamps on)
            for (int s = 0; s < 2; ++s) {
                scene::SceneRenderer::useShaderSet(s);
                for (int i = 0; i < warm; ++i) stepFrame(engine);
            }
            engine.gpuFrameMs(true);
            // Per set: each frame's ms and passes, in pair order.
            std::vector<double> frameMs[2];
            std::vector<std::map<std::string, double>> passes[2];
            std::map<std::string, bool> names;
            // Each pair's order drawn at random (a fixed seed): work the scene
            // spreads over frames in a cycle (the clouds' cache slices, the
            // plants' plan) falls on both sets alike, where a fixed order
            // (A B B A ...) gives each set the same phases of it every time.
            uint32_t rng = 0x9e3779b9u;
            for (int k = 0; k < frames; ++k) {
                rng = rng * 1664525u + 1013904223u;
                const bool bFirst = (rng >> 31) != 0;
                for (int j = 0; j < 2; ++j) {
                    const int s = bFirst ? 1 - j : j;
                    scene::SceneRenderer::useShaderSet(s);
                    stepFrame(engine);
                    frameMs[s].push_back(engine.gpuFrameMs(true));
                    passes[s].push_back(framePasses(engine));
                    for (const auto& [n, ms] : passes[s].back()) names[n] = true;
                }
            }
            scene::SceneRenderer::useShaderSet(0);
            nlohmann::json j;
            j["frames"] = frames;
            j["set"] = summaryJson(scene::SceneRenderer::shaderSetSummary());
            const char* keys[2] = {"a", "b"};
            std::map<std::string, std::vector<double>> per[2];
            for (int s = 0; s < 2; ++s) {
                for (const auto& [n, _] : names) {
                    std::vector<double>& v = per[s][n];
                    for (const auto& f : passes[s]) {
                        auto it = f.find(n);
                        v.push_back(it == f.end() ? 0.0 : it->second);
                    }
                }
                nlohmann::json o;
                o["frame"] = summaryOf(frameMs[s]);
                for (const auto& [n, v] : per[s]) o["passes"][n] = summaryOf(v);
                j[keys[s]] = o;
            }
            nlohmann::json d;
            d["frame"] = diffOf(frameMs[0], frameMs[1]);
            for (const auto& [n, _] : names) d["passes"][n] = diffOf(per[0][n], per[1][n]);
            j["diff"] = d;
            return fromJson(j);
        });
}
#endif

} // namespace bro::bronze_host
