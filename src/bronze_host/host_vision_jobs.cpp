// The async-job machine and the rasterizers behind `bro.vision`.
//
// THE THREAD SPLIT, and why it is drawn where it is. A bronze realm belongs
// to exactly one thread: its heap is a moving collector with no lock, so a
// Value touched from a second thread is a data race, not a slow path. The
// worker therefore runs `compute`, which is plain C++ over plain C++ state —
// a vector of bytes in, a model result struct out — and never sees a Value.
// Everything that builds or reads a Value (`build`, `release`, the onDone
// call) runs on the JS thread from tickVisionJobs(), which the engine's
// frame pump calls once a frame.
//
// WHAT OWNS WHAT, which is the other half of the same rule. `VisionWork` is
// the only object the two threads share, and it holds nothing but atomics
// and a string; the AsyncHandle the program gets back holds a shared_ptr to
// THAT and to nothing else, so dropping the handle — which happens inside a
// collection, where the embed API is off limits — can never destroy an
// ev::Persistent or join a thread. The thread and the rooted callbacks live
// in `VisionJob`, which only the per-thread registry owns and only
// tickVisionJobs()/shutdownVisionJobs() destroy.

#if BRO_WITH_VISION

#include "bronze_host/host_vision.h"

#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"  // ObjectBuilder
#include "engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <span>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// Job state
// ---------------------------------------------------------------------------

// Shared with the worker thread. Atomics and a string written before the
// release-store of `finished`, read after the acquiring load — no lock, and
// nothing here needs the embed API to destroy.
struct VisionWork {
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    std::string error;
};

// The JS thread's half. Only the registry below owns one.
struct VisionJob {
    std::shared_ptr<VisionWork> work;
    std::thread th;
    ev::Persistent onDone;
    ev::Persistent selfRef;
    VisionBuildFn build;
    VisionReleaseFn release;

    ~VisionJob() {
        // Belt and braces: every path that destroys a job has already
        // joined. A job that somehow reaches here running would otherwise
        // leave a thread reading freed state.
        if (th.joinable()) th.join();
    }
};

// Per thread, like the Persistents the jobs hold: the realm that launched a
// job is the one that settles it.
std::vector<std::unique_ptr<VisionJob>>& jobs() {
    static thread_local std::vector<std::unique_ptr<VisionJob>> v;
    return v;
}

// The models with an op in flight, keyed by the brovisionml wrapper pointer.
std::unordered_set<const void*>& busySet() {
    static thread_local std::unordered_set<const void*> s;
    return s;
}

// Run build/release and fire the callback. The job is already out of the
// registry, because building allocates and an onDone may launch the next op.
void settle(VisionJob& job) {
    if (job.th.joinable()) job.th.join();

    const bool cancelled = job.work->cancel.load(std::memory_order_acquire);
    const std::string error = job.work->error;

    ev::Persistent info(ev::createObject());
    {
        ObjectBuilder b(info.get());
        b.set("cancelled", ev::fromBool(cancelled));
        if (!error.empty()) b.set("error", ev::fromUtf8(error));
        info.set(b.get());
    }

    ev::Persistent result(ev::null());
    if (!cancelled && error.empty() && job.build) {
        result.set(job.build());
    }

    // BEFORE the callback: an onDone that synchronously starts the next op
    // on the same model has to find it free, which is the chained-step case
    // (step i kicking step i+1) the old binding called out.
    if (job.release) job.release();

    Value onDone = job.onDone.get();
    if (ev::isFunction(onDone)) {
        const Value args[2] = {result.get(), info.get()};
        ev::CallResult r = ev::call(onDone, ev::undefined(), std::span<const Value>(args, 2));
        if (r.thrown) reportBronzeError("bro.vision onDone", r.value);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// The runner
// ---------------------------------------------------------------------------

Value visionOnDone(Value opts) {
    if (!ev::isObject(opts)) return ev::undefined();
    return ev::getProperty(opts, "onDone");
}

bool visionMarkBusy(const void* model) {
    if (!model) return true;
    return busySet().insert(model).second;
}

void visionClearBusy(const void* model) {
    if (model) busySet().erase(model);
}

Value runVisionOp(Value onDone, Value selfRef, VisionComputeFn compute,
                  VisionBuildFn build, VisionReleaseFn release) {
    if (!ev::isFunction(onDone)) {
        // Synchronous: the same compute and the same builder, inline. A
        // throwing model is a thrown JS error, as it was before the job
        // machine existed.
        std::atomic<bool> noCancel{false};
        std::string error;
        try {
            if (compute) compute(noCancel);
        } catch (const std::exception& e) {
            error = e.what();
        } catch (...) {
            error = "unknown error";
        }
        if (!error.empty()) {
            if (release) release();
            return ev::throwError(error);
        }
        ev::Persistent result(build ? build() : ev::undefined());
        if (release) release();
        return result.get();
    }

    auto job = std::make_unique<VisionJob>();
    job->work = std::make_shared<VisionWork>();
    job->onDone = ev::Persistent(onDone);
    job->selfRef = ev::Persistent(selfRef);
    job->build = std::move(build);
    job->release = std::move(release);

    std::shared_ptr<VisionWork> work = job->work;
    job->th = std::thread([work, compute = std::move(compute)]() {
        try {
            if (compute) compute(work->cancel);
        } catch (const std::exception& e) {
            work->error = e.what();
        } catch (...) {
            work->error = "unknown error";
        }
        work->finished.store(true, std::memory_order_release);
    });

    jobs().push_back(std::move(job));

    // The handle: a plain object holding nothing but the shared work state.
    // Not a registered class — brolm already publishes a global named
    // AsyncHandle, and a second class of that name would shadow it for every
    // program in the realm.
    ObjectBuilder h;
    h.def("cancel", 0, [work](Value, std::span<const Value>) -> Value {
        work->cancel.store(true, std::memory_order_release);
        return ev::undefined();
    });
    h.accessor(
        "done",
        [work](Value, std::span<const Value>) -> Value {
            return ev::fromBool(work->finished.load(std::memory_order_acquire));
        },
        nullptr);
    return h.get();
}

void tickVisionJobs() {
    auto& v = jobs();
    if (v.empty()) return;
    // Take the finished jobs out first: settling allocates and runs user
    // code that may push another job onto this vector.
    std::vector<std::unique_ptr<VisionJob>> finished;
    for (std::size_t i = 0; i < v.size();) {
        if (v[i]->work->finished.load(std::memory_order_acquire)) {
            finished.push_back(std::move(v[i]));
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (auto& job : finished) settle(*job);
}

void shutdownVisionJobs() {
    auto& v = jobs();
    if (v.empty()) return;
    std::vector<std::unique_ptr<VisionJob>> all;
    all.swap(v);
    for (auto& job : all) {
        job->work->cancel.store(true, std::memory_order_release);
        if (job->th.joinable()) job->th.join();
        // settle() so each job's release() runs and the roots are dropped
        // while the runtime is still standing.
        settle(*job);
    }
    busySet().clear();
}

// ---------------------------------------------------------------------------
// Rasterizers
// ---------------------------------------------------------------------------

Value visionBitmapRGBA(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return ev::null();
    return wrapHostImageBitmap(rgba, w, h);
}

Value visionBitmapRGB(const uint8_t* rgb, int w, int h) {
    if (!rgb || w <= 0 || h <= 0) return ev::null();
    const std::size_t plane = static_cast<std::size_t>(w) * h;
    std::vector<uint8_t> rgba(plane * 4);
    for (std::size_t i = 0; i < plane; ++i) {
        rgba[4 * i + 0] = rgb[3 * i + 0];
        rgba[4 * i + 1] = rgb[3 * i + 1];
        rgba[4 * i + 2] = rgb[3 * i + 2];
        rgba[4 * i + 3] = 255;
    }
    return wrapHostImageBitmap(rgba.data(), w, h);
}

Value visionBitmapGrayNormalized(const std::vector<float>& map, int w, int h,
                                 bool invert, float& lo, float& hi) {
    lo = 0.0f;
    hi = 1.0f;
    if (w <= 0 || h <= 0 || map.empty()) return ev::null();
    lo = *std::min_element(map.begin(), map.end());
    hi = *std::max_element(map.begin(), map.end());
    const float range = (hi > lo) ? (hi - lo) : 1.0f;
    const std::size_t plane = static_cast<std::size_t>(w) * h;
    std::vector<uint8_t> rgba(plane * 4, 255);
    for (std::size_t i = 0; i < plane && i < map.size(); ++i) {
        float t = (map[i] - lo) / range;
        if (invert) t = 1.0f - t;
        const int g = std::clamp(static_cast<int>(std::lround(t * 255.0f)), 0, 255);
        rgba[4 * i + 0] = static_cast<uint8_t>(g);
        rgba[4 * i + 1] = static_cast<uint8_t>(g);
        rgba[4 * i + 2] = static_cast<uint8_t>(g);
        rgba[4 * i + 3] = 255;
    }
    return wrapHostImageBitmap(rgba.data(), w, h);
}

Value visionBitmapGrayUnit(const std::vector<float>& map, int w, int h, bool invert) {
    if (w <= 0 || h <= 0 || map.empty()) return ev::null();
    const std::size_t plane = static_cast<std::size_t>(w) * h;
    std::vector<uint8_t> rgba(plane * 4, 255);
    for (std::size_t i = 0; i < plane && i < map.size(); ++i) {
        float v = std::clamp(map[i], 0.0f, 1.0f);
        if (invert) v = 1.0f - v;
        const int g = std::clamp(static_cast<int>(std::lround(v * 255.0f)), 0, 255);
        rgba[4 * i + 0] = static_cast<uint8_t>(g);
        rgba[4 * i + 1] = static_cast<uint8_t>(g);
        rgba[4 * i + 2] = static_cast<uint8_t>(g);
        rgba[4 * i + 3] = 255;
    }
    return wrapHostImageBitmap(rgba.data(), w, h);
}

Value visionBitmapMask(const uint8_t* mask, int w, int h, uint8_t r, uint8_t g,
                       uint8_t b) {
    if (!mask || w <= 0 || h <= 0) return ev::null();
    const std::size_t plane = static_cast<std::size_t>(w) * h;
    std::vector<uint8_t> rgba(plane * 4, 0);
    for (std::size_t i = 0; i < plane; ++i) {
        if (!mask[i]) continue;
        rgba[4 * i + 0] = r;
        rgba[4 * i + 1] = g;
        rgba[4 * i + 2] = b;
        rgba[4 * i + 3] = 255;
    }
    return wrapHostImageBitmap(rgba.data(), w, h);
}

Value visionBitmapNormals(const std::vector<float>& nchw, int w, int h) {
    if (w <= 0 || h <= 0 || nchw.empty()) return ev::null();
    const std::size_t plane = static_cast<std::size_t>(w) * h;
    if (nchw.size() < plane * 3) return ev::null();
    std::vector<uint8_t> rgba(plane * 4, 255);
    for (std::size_t i = 0; i < plane; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float n = nchw[static_cast<std::size_t>(c) * plane + i];
            const int v =
                std::clamp(static_cast<int>(std::lround((n + 1.0f) * 0.5f * 255.0f)), 0, 255);
            rgba[4 * i + static_cast<std::size_t>(c)] = static_cast<uint8_t>(v);
        }
        rgba[4 * i + 3] = 255;
    }
    return wrapHostImageBitmap(rgba.data(), w, h);
}

namespace {

// Clipped Bresenham, one white opaque pixel per step.
void drawSegment(std::vector<uint8_t>& rgba, int w, int h, int x0, int y0, int x1,
                 int y1) {
    int dx = std::abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int e = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h) {
            const std::size_t i = (static_cast<std::size_t>(y0) * w + x0) * 4;
            rgba[i] = rgba[i + 1] = rgba[i + 2] = rgba[i + 3] = 255;
        }
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * e;
        if (e2 >= dy) {
            e += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            e += dx;
            y0 += sy;
        }
    }
}

}  // namespace

Value visionBitmapSegments(const std::vector<float>& x1, const std::vector<float>& y1,
                           const std::vector<float>& x2, const std::vector<float>& y2,
                           int w, int h) {
    if (w <= 0 || h <= 0) return ev::null();
    std::vector<uint8_t> rgba(static_cast<std::size_t>(w) * h * 4, 0);
    const std::size_t n =
        std::min(std::min(x1.size(), y1.size()), std::min(x2.size(), y2.size()));
    for (std::size_t i = 0; i < n; ++i) {
        drawSegment(rgba, w, h, static_cast<int>(std::lround(x1[i])),
                    static_cast<int>(std::lround(y1[i])),
                    static_cast<int>(std::lround(x2[i])),
                    static_cast<int>(std::lround(y2[i])));
    }
    return wrapHostImageBitmap(rgba.data(), w, h);
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installVisionHostOps(engine::Engine* engine) {
    installVisionDepthOps();
    installVisionSamOps();
    installVisionGenerativeOps();
    installVisionAnnotatorOps();

    // The pump and the hook belong to the process, not to a realm: a reload
    // re-runs the installers for the new realm and must not stack a second
    // drain onto the engine. (A Worker realm passes no engine; its drain is
    // tickWorkerSiblingApis.)
    if (!engine) return;
    static bool pumpInstalled = false;
    if (pumpInstalled) return;
    pumpInstalled = true;
    engine->addFramePump([] { tickVisionJobs(); });
    engine->addShutdownHook([] { shutdownVisionJobs(); });
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_VISION
