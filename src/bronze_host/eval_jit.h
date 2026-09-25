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

/// Pins the tier every in-process compile runs at when BRO_JIT_TIER names one
/// (bronze's parseExecutionTier: 0/interpreter, 1/baseline, 2/optimized,
/// auto). Unset, JS runs on bronze's tiered default. A debugging switch,
/// applied once when an Engine starts.
void applyJitTierOverride();

/// Drops every in-process program's queued background compiles (tier-ups
/// and OSR entries) and waits out the running ones, so none is still
/// compiling while the engine tears down.
void stopBackgroundCompiles();

/// The compile-and-run behind evalScriptJit: what the program produced — its
/// completion value, or what it threw — with nothing logged and no failure
/// latched. This is what a caller that has its own use for the outcome wants:
/// the `eval()` hook returns the value or rethrows, and must not fail the run
/// on a throw the script is about to catch. `moduleHandleOut`, when set,
/// brackets the run as a module load the host can later unload (bronze
/// EvalOptions::moduleHandleOut). `moduleFile` says `code` is the text of the
/// module FILE `filename` (a `<script type="module" src>`): that instance is
/// published under the file's path, so a driver script's `import` of it binds
/// the page's instance instead of running the file again (bronze
/// EvalOptions::publishEntry).
bronze::embed::CallResult evalScriptJitResult(engine::Engine& engine, const std::string& code,
                                              const std::string& filename = {},
                                              bronze::embed::ModuleHandle* moduleHandleOut = nullptr,
                                              bool moduleFile = false);

/// Evaluates JavaScript code in-memory using the Brass JIT engine without disk files.
/// Returns true on success, false on failure (and logs error / sets test failure).
bool evalScriptJit(engine::Engine& engine, const std::string& code, const std::string& filename = {},
                   bronze::embed::ModuleHandle* moduleHandleOut = nullptr,
                   bool moduleFile = false);

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
