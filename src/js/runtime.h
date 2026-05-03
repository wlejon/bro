#pragma once

#include <cstdint>
#include <deque>
#include <string>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

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

    /// Get the global object (caller must JS_FreeValue when done).
    JSValue getGlobalObject() const;

    /// Drain the microtask / promise job queue. Errors flow through the funnel.
    void executePendingJobs();

    /// Install a custom ES-module loader (file-based).
    void setModuleLoader();

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

private:
    JSRuntime* rt_ = nullptr;
    JSContext* ctx_ = nullptr;
};

} // namespace bro::js
