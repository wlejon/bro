#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::util { class AssetMounts; class ImportMap; }

namespace bro::js {

/// Everything module specifier resolution consults, in one object so the
/// loader has a single opaque pointer. Both members are borrowed and may be
/// null; a null one simply removes that resolution step.
struct ModuleResolveContext {
    const util::AssetMounts* mounts = nullptr;
    const util::ImportMap* importMap = nullptr;
};

/// Where a JS invocation came from. Used purely for the log line — error
/// behavior (report-and-continue) does not depend on the origin.
enum class ErrorOriginKind : uint8_t {
    Eval,             // top-level script eval
    Module,           // ES module load/eval
    Timer,            // setTimeout/setInterval callback
    Raf,              // requestAnimationFrame callback
    Listener,         // addEventListener / on* property / on* attribute
    Microtask,        // promise job drained from JS_ExecutePendingJob
    PromiseRejection, // unhandled promise rejection (host tracker)
    Binding,          // generic native-binding callback into JS
};

struct ErrorOrigin {
    ErrorOriginKind kind = ErrorOriginKind::Binding;
    std::string label;  // free-form context — "click on #save", "setInterval#42", "myWorker.onmessage"

    static ErrorOrigin binding() { return {ErrorOriginKind::Binding, {}}; }
    static ErrorOrigin eval(std::string filename) { return {ErrorOriginKind::Eval, std::move(filename)}; }
    static ErrorOrigin module(std::string filename) { return {ErrorOriginKind::Module, std::move(filename)}; }
    static ErrorOrigin timer(int32_t id, bool repeating);
    static ErrorOrigin raf() { return {ErrorOriginKind::Raf, {}}; }
    static ErrorOrigin listener(std::string desc) { return {ErrorOriginKind::Listener, std::move(desc)}; }
    static ErrorOrigin microtask() { return {ErrorOriginKind::Microtask, {}}; }
    static ErrorOrigin promiseRejection() { return {ErrorOriginKind::PromiseRejection, {}}; }
};

class Runtime {
public:
    Runtime();
    ~Runtime();

    // Non-copyable, non-movable
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    /// Evaluate a script. Returns true on success.
    bool eval(const std::string& code, const std::string& filename = "<eval>");

    /// Evaluate code as an ES module. Returns true on success.
    bool evalModule(const std::string& code, const std::string& filename);

    /// Read a file from disk and evaluate it as a script. Returns true on success.
    bool loadFile(const std::string& path);

    /// Get the underlying JSContext (the primary/app context).
    JSContext* getContext() const { return ctx_; }

    /// Get the underlying JSRuntime.
    JSRuntime* getRuntime() const { return rt_; }

    /// Create an additional JSContext on the same runtime.
    /// The caller owns the returned context and must free it with JS_FreeContext.
    JSContext* createContext();

    /// Replace the primary/app context with a fresh one on the same runtime:
    /// frees the current getContext() and creates a new, empty context that
    /// getContext() returns from here on. The realm-swap primitive behind
    /// top-level location.reload(). The caller must have already run every
    /// per-context binding cleanup against the old context — this only does
    /// the brokit fetch uninstall (mirroring ~Runtime) and the raw free.
    JSContext* renewContext();

    /// Get the global object (caller must JS_FreeValue when done).
    JSValue getGlobalObject() const;

    /// True while JS code (eval, module, callback, microtask) is currently
    /// executing on this runtime. Used by the engine's event loop and modal
    /// hooks to avoid re-entrant timer/microtask dispatches.
    bool isExecuting() const { return execDepth_ > 0; }
    void enterExecution() { ++execDepth_; }
    void leaveExecution() { if (execDepth_ > 0) --execDepth_; }

    struct ExecutionGuard {
        Runtime* rt_ = nullptr;
        explicit ExecutionGuard(Runtime* rt) : rt_(rt) { if (rt_) rt_->enterExecution(); }
        ~ExecutionGuard() { if (rt_) rt_->leaveExecution(); }
        ExecutionGuard(const ExecutionGuard&) = delete;
        ExecutionGuard& operator=(const ExecutionGuard&) = delete;
    };

    /// Drain the microtask / promise job queue. Errors flow through the funnel.
    /// After the queue empties, rejected promises that never picked up a
    /// handler during the burst are reported (HTML-spec unhandledrejection
    /// timing — see the pending-rejection notes below).
    void executePendingJobs();

    /// Install a custom ES-module loader (file-based). When `mounts` is
    /// supplied, `/`-prefixed import specifiers (e.g. `import "/lib/x.js"`)
    /// resolve through the engine's asset mounts; relative specifiers always
    /// resolve against the importing module's path. When `importMap` is
    /// supplied, a *bare* specifier ("three") is resolved through the page's
    /// `<script type="importmap">` — the only thing that can give one a
    /// meaning, since a bare specifier names a package and not a file.
    ///
    /// Both pointers are held as the loader's opaque and must outlive any
    /// module evaluation. They are read at import time rather than copied, so
    /// an engine may hand over a map it has not filled in yet (the app's
    /// manifest is not loaded until later in init) and have it take effect.
    void setModuleLoader(const util::AssetMounts* mounts = nullptr,
                         const util::ImportMap* importMap = nullptr);

    // -----------------------------------------------------------------------
    // The error funnel.
    //
    // All JS invocations from C++ should go through callJs() (or pass their
    // result through checkException()). The funnel:
    //   1. Calls window.onerror / unhandledrejection on the global if defined.
    //   2. If not suppressed by the host hook, logs a single multi-line entry.
    //   3. Applies 5-second repeat suppression on (name, message, top-frame)
    //      so a callback that throws every frame logs once, plus a periodic
    //      "still failing" beat.
    //
    // Behavior is web-spec: report-and-continue. The funnel never modifies
    // timer/listener registration — callers may keep firing the same broken
    // callback; suppression makes that survivable.
    // -----------------------------------------------------------------------

    /// Invoke `fn` with `thisVal` and `argv`. On exception: routes through the
    /// funnel and returns JS_UNDEFINED (always safe to JS_FreeValue). On
    /// success: returns the function's return value (caller must JS_FreeValue).
    static JSValue callJs(JSContext* ctx, JSValueConst fn, JSValueConst thisVal,
                          int argc, JSValueConst* argv, const ErrorOrigin& origin);

    /// Check a JSValue for exceptions. Returns true if val IS an exception
    /// (in which case the exception has been routed through the funnel and
    /// freed; the value itself is also freed). Returns false otherwise.
    /// Existing call sites pass `Binding` origin implicitly.
    static bool checkException(JSContext* ctx, JSValue val,
                               const ErrorOrigin& origin = ErrorOrigin::binding());

    /// Periodic flush of the suppressor — emits "still failing" tails for
    /// fingerprints that have aged past the 5s window. Called automatically
    /// from the funnel; safe to invoke from the engine's frame loop too.
    static void flushSuppressor();

    // -----------------------------------------------------------------------
    // Pending promise rejections.
    //
    // QuickJS invokes the host rejection tracker the INSTANT a promise
    // rejects with no reactions attached — which is the normal shape of
    // `return Promise.reject(e)`: the caller's .then() lands one line later,
    // same job. Logging at that instant produces false "unhandled rejection"
    // errors for perfectly handled code. Instead the tracker parks the
    // rejection here (is_handled=false) or removes it (is_handled=true, a
    // handler attached), and executePendingJobs() reports whatever is still
    // parked once the job queue drains — the HTML spec's timing. Entries hold
    // a JS_DupContext ref so a realm torn down mid-burst can't dangle.
    // Internal — called only by the host tracker.
    // -----------------------------------------------------------------------
    void addPendingRejection(JSContext* ctx, JSValueConst promise, JSValueConst reason);
    void removePendingRejection(JSValueConst promise);

private:
    struct PendingRejection {
        JSContext* ctx = nullptr; // holds a JS_DupContext reference
        void* key = nullptr;      // promise heap pointer, for removal matching
        JSValue promise = JS_UNDEFINED;
        JSValue reason = JS_UNDEFINED;
    };

    /// Report every still-parked rejection through the funnel and clear the
    /// list. Handlers the funnel runs may park new rejections; those wait for
    /// the next flush.
    void flushPendingRejections();
    /// Free parked rejections without reporting (teardown path).
    void discardPendingRejections();

    JSRuntime* rt_ = nullptr;
    JSContext* ctx_ = nullptr;
    std::vector<PendingRejection> pendingRejections_;
    int execDepth_ = 0;
    bool drainingPendingJobs_ = false;

    /// What the module loader resolves against. Held by value so its address is
    /// stable for the runtime's lifetime — QuickJS keeps the opaque pointer, and
    /// setModuleLoader is called once, before the app manifest (and so the
    /// import map) exists.
    ModuleResolveContext moduleResolve_;
};

} // namespace bro::js
