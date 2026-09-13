#pragma once

#include <functional>
#include <string>

namespace bro::engine {

class Engine;

struct HeadlessHooks {
    std::function<void()> beforeEngine;

    /// Run once the Engine is up and its first run() has returned — virtual
    /// time rebased, the app's document laid out — and before the headless
    /// globals go in or any script evaluates.
    ///
    /// This is the seam a host whose "app" is NATIVE code needs. bro-bronze-host
    /// registers its host globals and runs the compiled top level here, which
    /// puts that top level exactly where a script-based app's own JS already
    /// is by this point: finished, with its first frame scheduled. The driver
    /// script that follows then steps a running app rather than starting one.
    std::function<void(Engine&)> afterEngine;

    /// Asked once the app directory is resolved and before the Engine is
    /// constructed: is `afterEngine` going to run compiled logic for THIS app?
    /// Forwarded to EngineConfig::hostProvidesCompiledApp, which is what lets
    /// engine init report an app dir and a binary that disagree about which of
    /// them owns the app's logic. Unset means no, which is bro-headless's own
    /// answer.
    ///
    /// A predicate rather than a bool because the answer is a property of the
    /// FOLDER, not of the binary: one bro-headless opens a script-based app
    /// and a compiled one, and which it got depends on whether that directory
    /// carries a module (bronze_host/app_module.h). A host with an app linked
    /// in answers yes unconditionally; a host that loads one answers by
    /// looking. Asked before construction because engine init's diagnostic
    /// needs it — after the fact the warning has already been emitted.
    std::function<bool(const std::string& appDir)> providesCompiledApp;

    /// Run after the Engine is destroyed and immediately before `_exit()`.
    ///
    /// The driver leaves through `_exit()`, which skips `atexit` handlers and
    /// static destruction on purpose (see the teardown comment in
    /// headless_driver.cpp). Anything a host wants reported at the end of a
    /// run — a profile table, a leak census — has to be called here rather
    /// than registered with the CRT, because the CRT never gets the chance.
    std::function<void()> beforeExit;

    /// Name used in usage text and diagnostics.
    std::string programName = "bro-headless";

    /// One-line summary shown at the top of --help.
    std::string tagline = "headless mode for bro";
};

/// The headless driver: argument parsing, engine construction, script/REPL
/// evaluation and teardown. Returns the process exit code.
int runHeadless(int argc, char* argv[], const HeadlessHooks& hooks = {});

} // namespace bro::engine
