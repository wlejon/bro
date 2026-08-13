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

    /// Name used in usage text and diagnostics.
    std::string programName = "bro-headless";

    /// One-line summary shown at the top of --help.
    std::string tagline = "headless mode for bro";
};

/// The headless driver: argument parsing, engine construction, script/REPL
/// evaluation and teardown. Returns the process exit code.
int runHeadless(int argc, char* argv[], const HeadlessHooks& hooks = {});

} // namespace bro::engine
