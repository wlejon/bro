#pragma once

#include "embed/embed.h"

#include <string>

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

/// Checks whether in-memory JIT evaluation is disabled via environment variables
/// (e.g. BRO_DISABLE_JIT=1, BRO_EVAL_AOT=1, BRO_NO_JIT=1).
bool isJitDisabled();

/// The compile-and-run behind evalScriptJit: what the program produced — its
/// completion value, or what it threw — with nothing logged and no failure
/// latched. This is what a caller that has its own use for the outcome wants:
/// the `eval()` hook returns the value or rethrows, and must not fail the run
/// on a throw the script is about to catch. `moduleHandleOut`, when set,
/// brackets the run as a module load the host can later unload (bronze
/// EvalOptions::moduleHandleOut).
bronze::embed::CallResult evalScriptJitResult(engine::Engine& engine, const std::string& code,
                                              const std::string& filename = {},
                                              bronze::embed::ModuleHandle* moduleHandleOut = nullptr);

/// Evaluates JavaScript code in-memory using the Brass JIT engine without disk files.
/// Returns true on success, false on failure (and logs error / sets test failure).
bool evalScriptJit(engine::Engine& engine, const std::string& code, const std::string& filename = {},
                   bronze::embed::ModuleHandle* moduleHandleOut = nullptr);

/// Evaluates a JavaScript file in-memory using the Brass JIT engine without disk files.
/// Returns true on success, false on failure (and logs error / sets test failure).
bool evalScriptFileJit(engine::Engine& engine, const std::string& filePath);

/// A script with top-level `await`, rewritten as an async IIFE bronze can
/// compile. Its `.catch` reports the rejection as TEXT — `stack` when set,
/// else `Name: message` — and fails the run through `assert`: an error
/// object handed to `console.error` prints as "[object]" and names nothing.
/// Shared by the JIT and AOT paths so the two report a failure identically.
/// `filename` names the script in that report; bronze records no source
/// position on an Error, so the file is the location the report can give.
std::string wrapAsyncIife(const std::string& code, const std::string& filename = {});

} // namespace bro::bronze_host
