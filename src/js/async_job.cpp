#include "js/async_job.h"

#include <qjsbind/qjsbind.h>

#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace bro::js {

// ─── Job + handle ────────────────────────────────────────────────────────────

struct AsyncJob {
    JSContext*        ctx = nullptr;
    AsyncWorkFn       work;
    AsyncPollFn       poll;
    AsyncDoneFn       done;
    std::atomic<bool> cancel{false};
    std::atomic<int>  state{0};   // 0 = running, 1 = finished (work returned)
    std::string       error;      // set on the work thread before state -> 1
    std::thread       th;

    ~AsyncJob() { if (th.joinable()) th.join(); }
};

// The JS-visible handle: an opaque box holding a strong ref to the job, with a
// cancel() method. Kept alive independently of the registry, so cancel() stays
// valid even after the job has finished and been removed (no-op then).
struct AsyncHandleBox {
    std::shared_ptr<AsyncJob> job;
};

// One JS context runs on exactly one thread (main context on the main thread,
// each Worker on its own), so a thread-local registry is naturally per-context
// — no global map, no mutex.
static thread_local std::vector<std::shared_ptr<AsyncJob>> g_jobs;

static JSValue js_async_cancel(JSContext* ctx, JSValueConst this_val,
                               int, JSValueConst*) {
    auto* box = qjsbind::unwrap<AsyncHandleBox>(ctx, this_val);
    if (box && box->job)
        box->job->cancel.store(true, std::memory_order_release);
    return JS_UNDEFINED;
}

// Register the AsyncHandle class once per context/runtime. class_id<T> is
// thread-local and JS_NewClassID is per-runtime, matching one-runtime-per-thread.
static void ensureHandleClass(JSContext* ctx) {
    static thread_local bool registered = false;
    if (registered) return;
    registered = true;
    qjsbind::Class<AsyncHandleBox>(ctx, "AsyncHandle", qjsbind::NoGlobal)
        .method_raw("cancel", js_async_cancel, 0);
}

// ─── API ─────────────────────────────────────────────────────────────────────

JSValue launchAsyncJob(JSContext* ctx, AsyncWorkFn work, AsyncPollFn poll,
                       AsyncDoneFn done) {
    ensureHandleClass(ctx);

    auto job  = std::make_shared<AsyncJob>();
    job->ctx  = ctx;
    job->work = std::move(work);
    job->poll = std::move(poll);
    job->done = std::move(done);

    // Capture a shared_ptr COPY into the thread so the job outlives the worker
    // even if the registry entry is removed first (it never is before join, but
    // this keeps the invariant local and obvious).
    job->th = std::thread([job]() {
        try {
            job->work(job->cancel);
        } catch (const std::exception& e) {
            job->error = e.what();
        } catch (...) {
            job->error = "unknown error";
        }
        job->state.store(1, std::memory_order_release);
    });

    g_jobs.push_back(job);
    return qjsbind::wrap<AsyncHandleBox>(ctx, new AsyncHandleBox{job});
}

void tickAsync(JSContext* ctx) {
    if (g_jobs.empty()) return;
    // Index walk: done() may launch a follow-up job (e.g. STT -> generate),
    // appending to g_jobs while we iterate.
    for (std::size_t i = 0; i < g_jobs.size();) {
        auto job = g_jobs[i];
        // poll()/done() invoke JS callbacks and handle their own exceptions
        // (each JS_Call site checks JS_IsException, like bro.wake's tick).
        if (job->poll) job->poll(ctx);
        if (job->state.load(std::memory_order_acquire) != 0) {
            if (job->th.joinable()) job->th.join();
            // Final drain so streaming output produced between the last poll and
            // the work thread returning isn't lost.
            if (job->poll) job->poll(ctx);
            const bool cancelled = job->cancel.load(std::memory_order_acquire);
            if (job->done) job->done(ctx, cancelled, job->error);
            g_jobs.erase(g_jobs.begin() + static_cast<std::ptrdiff_t>(i));
            continue;  // next job shifted into slot i
        }
        ++i;
    }
}

bool hasAsyncJobs() {
    return !g_jobs.empty();
}

void shutdownAsyncJobs(JSContext* ctx) {
    for (auto& job : g_jobs) {
        job->cancel.store(true, std::memory_order_release);
        if (job->th.joinable()) job->th.join();
        // Run done() so the binding frees its dup'd callbacks.
        if (job->done) job->done(ctx, true, job->error);
    }
    g_jobs.clear();
}

}  // namespace bro::js
