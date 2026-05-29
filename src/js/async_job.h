#pragma once

extern "C" {
#include "quickjs.h"
}

#include <atomic>
#include <functional>
#include <string>

namespace bro::js {

// ─── Per-context async-job runner ────────────────────────────────────────────
//
// Runs a blocking native operation on a background thread and delivers its
// results back into the (single-threaded) QuickJS context on that context's own
// event-loop tick. This is the engine-side machinery that lets heavy model
// inference (bro.lm/bro.stt/bro.tts) be non-blocking and cancellable without the
// caller hand-rolling a token loop or a JS Worker.
//
// It mirrors the proven bro.wake result-delivery convention: the worker thread
// touches only atomics / its own owned state; a per-frame tick on the JS thread
// drains that state and fires the JS callbacks. No mutexes — a background thread
// per request, joined on completion, plus a thread-local registry (one JS
// context runs on exactly one thread, so thread_local == per-context).
//
// A job is three lambdas:
//   work(cancel) — runs on the BACKGROUND thread. Does the blocking op. Reads
//                  `cancel` (an atomic<bool>) to stop early. May throw; the
//                  exception text is captured and handed to done() as `error`.
//   poll(ctx)    — runs on the JS thread once per tick WHILE the job runs
//                  (optional; used for streaming, e.g. draining LLM tokens to an
//                  onToken callback). Also called once more right after the work
//                  thread finishes, so nothing streamed is lost.
//   done(ctx,…)  — runs on the JS thread exactly once when the job completes
//                  (or is cancelled / throws). The binding builds its result
//                  JSValues here, invokes its onDone callback, and frees any
//                  JSValues it dup'd at launch.
//
// `work` and `poll`/`done` typically share a heap struct (held by shared_ptr,
// captured into both): the background thread is the sole writer of a committed
// prefix published via an atomic, the JS thread the sole reader — lock-free.

using AsyncWorkFn = std::function<void(const std::atomic<bool>& cancel)>;
using AsyncPollFn = std::function<void(JSContext* ctx)>;
using AsyncDoneFn =
    std::function<void(JSContext* ctx, bool cancelled, const std::string& error)>;

// Spawn `work` on a background thread and register the job on the calling
// thread's context. Returns an `AsyncHandle` JS object exposing `.cancel()`
// (non-blocking: sets the cancel flag; the work thread observes it between
// steps, or the result is dropped for a monolithic op). `poll` may be empty.
JSValue launchAsyncJob(JSContext* ctx, AsyncWorkFn work, AsyncPollFn poll,
                       AsyncDoneFn done);

// Drain streaming output and finish completed jobs for this context. Call once
// per event-loop iteration on the thread that owns `ctx` (engine frame loop and
// worker loop). Cheap no-op when no jobs are in flight.
void tickAsync(JSContext* ctx);

// Cancel and join every in-flight job, then run their done() so dup'd callbacks
// are freed. Call at context teardown on the owning thread, before the runtime
// is destroyed.
void shutdownAsyncJobs(JSContext* ctx);

}  // namespace bro::js
