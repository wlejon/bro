#pragma once

#include <functional>
#include <string>

extern "C" { typedef struct JSContext JSContext; }

namespace bro::engine {

class Engine;

/// Everything a host application can inject into a headless run.
///
/// This exists because bro-headless is not just bro's own test driver — it is
/// the only way to script the engine, take screenshots and assert on a real
/// DOM without a human at a window. An application that links bro_engine and
/// adds its own capabilities (ffmpeg-bro links libav* and registers a media
/// backend) needs that same driver with its own pieces in place, and copying
/// 700 lines to get it would guarantee the copy drifts.
struct HeadlessHooks {
    /// Run before the Engine is constructed: register media backends, open
    /// devices, anything the first document might touch.
    std::function<void()> beforeEngine;

    /// Install extra `bro.*` bindings. Forwarded to
    /// EngineConfig::installHostBindings, so it runs for every realm.
    std::function<void(JSContext*)> installHostBindings;

    /// Run once the Engine is up and its first run() has returned — virtual
    /// time rebased, the app's document laid out — and before the headless
    /// globals go in or any script evaluates.
    ///
    /// This is the seam a host whose "app" is NATIVE code needs. bro-bronze-host
    /// registers its host globals and runs the compiled top level here, which
    /// puts that top level exactly where an interpreted app's own JS already
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
    /// FOLDER, not of the binary: one bro-headless opens an interpreted app
    /// and a compiled one, and which it got depends on whether that directory
    /// carries a module (bronze_host/app_module.h). A host with an app linked
    /// in answers yes unconditionally; a host that loads one answers by
    /// looking. Asked before construction because engine init's diagnostic
    /// needs it — after the fact the warning has already been emitted.
    std::function<bool(const std::string& appDir)> providesCompiledApp;

    /// Name used in usage text and diagnostics.
    std::string programName = "bro-headless";

    /// One-line summary shown at the top of --help.
    std::string tagline = "headless mode for bro";
};

/// The headless driver: argument parsing, engine construction, script/REPL
/// evaluation and teardown. Returns the process exit code.
int runHeadless(int argc, char* argv[], const HeadlessHooks& hooks = {});

} // namespace bro::engine
