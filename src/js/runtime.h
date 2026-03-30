#pragma once

#include <string>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

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

    /// Drain the microtask / promise job queue.
    void executePendingJobs();

    /// Install a custom ES-module loader (file-based).
    void setModuleLoader();

    /// Check a JSValue for exceptions. Logs the error and frees the exception.
    /// Returns true if val *is* an exception.
    static bool checkException(JSContext* ctx, JSValue val);

private:
    JSRuntime* rt_ = nullptr;
    JSContext* ctx_ = nullptr;
};

} // namespace bro::js
