// bro-bronze-host — the smallest host that runs a bronze-compiled JS app
// inside a windowed bro Engine. The same shape as docs/embedding.md's host
// walkthrough and headless/main.cpp's __host: nothing but the public engine
// surface plus bronze's embed API.
//
// The compiled app itself is NOT in this file or this library: bronze emits
// an object file whose entry symbol is `bronze_main` (src/rt/rt.cpp's
// convention), and the build links that object into this executable
// (BRO_BRONZE_APP_OBJ — see CMakeLists.txt beside this file).
// bronze::embed::runMain() carries the one reference to the symbol.
//
// Order matters and is the whole of main():
//   1. Engine up (GL, document, frame loop machinery exist),
//   2. host globals registered (the program's top level reads them),
//   3. runMain() — the compiled top level runs: builds the scene, calls
//      requestAnimationFrame, returns,
//   4. engine.run() — frames fire the rAF queue until the window closes.

#include "bronze_host/bronze_host.h"

#include "engine/engine.h"
#include "engine/launcher.h"

#include "embed/embed.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

// --headless --frames N [--step MS] is the same host, driven by virtual time
// instead of a window. It exists so the compiled app can be CHECKED: a windowed
// run ends when a human closes the window and its frame count depends on how
// fast the machine is, neither of which a pinned expectation can be written
// against. Under headless the clock advances by exactly `step` per frame and
// the run ends after exactly `frames` of them, so the app's output is a
// function of the app.
//
// It is deliberately NOT bro-headless's driver (engine/headless_driver.h):
// that one's whole surface is JS — a script, a REPL, advanceTime() as a
// binding — and the app here has no JS realm to evaluate any of it in.
struct Options {
    const char* appDir = nullptr;
    bool headless = false;
    int frames = 0;
    double stepMs = 16.0;  // the 60 Hz frame the rest of the engine assumes
};

bool parseArgs(int argc, char* argv[], Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--headless") {
            out.headless = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            out.frames = std::atoi(argv[++i]);
        } else if (arg == "--step" && i + 1 < argc) {
            out.stepMs = std::atof(argv[++i]);
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "bro-bronze-host: unknown option %s\n", arg.c_str());
            return false;
        } else if (!out.appDir) {
            out.appDir = argv[i];
        } else {
            std::fprintf(stderr, "bro-bronze-host: unexpected argument %s\n", arg.c_str());
            return false;
        }
    }
    if (!out.appDir) return false;
    if (out.headless && out.frames <= 0) {
        std::fprintf(stderr, "bro-bronze-host: --headless needs --frames N\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    // A bro Engine still boots from an app directory (manifest, settings,
    // asset mounts) even when the app's JS was compiled away — point this at
    // a minimal app dir whose ui holds an empty page. app/appdir beside this
    // file is one, and README.md shows what is in it.
    Options options;
    if (!parseArgs(argc, argv, options)) {
        std::fprintf(stderr,
                     "usage: bro-bronze-host <appdir> [--headless --frames N [--step MS]]\n");
        return 1;
    }

    bro::engine::EngineConfig config;
    config.title = "bro-bronze-host";
    config.displayMode = options.headless ? bro::engine::DisplayMode::Headless
                                          : bro::engine::DisplayMode::Windowed;
    config.settingsPath = bro::engine::executableDir() + "/.bro_settings.json";
    // Same default bro-headless documents (docs/headless.md): the splash is
    // visual-only and its canvas animation would be the first thing a pinned
    // run captured. Windowed keeps it, so a compiled app starts like any other.
    config.showSplash = !options.headless;
    if (!bro::engine::resolveLaunchTarget(options.appDir, config)) {
        std::fprintf(stderr, "bro-bronze-host: %s is not an app directory\n", options.appDir);
        return 1;
    }
    bro::engine::publishLaunchEnv(config);

    bro::engine::Engine engine(config);

    // Globals BEFORE the program: its top level reads them as it runs.
    bro::bronze_host::installThreejsHostGlobals(engine);

    // runMain, not runProgram: the engine already owns stdio (its console
    // logging is text), so bronze's binary-stdout setup is skipped — a
    // compiled app hosted here talks through the log, not a byte stream.
    bronze::embed::runMain();

    // The compiled top level has returned; whatever it scheduled through
    // requestAnimationFrame now runs a frame at a time. Teardown order on
    // return: the Engine (and with it every GL context) dies at end of scope;
    // bronze's heap statics die at process exit WITHOUT running handle
    // finalizers (embed.h's contract), so no finalizer can chase the dead GL
    // context.
    //
    // run() is called in BOTH modes and is not a branch: windowed it is the
    // loop, headless it returns at once after rebasing virtual time onto the
    // wall clock — which advanceTime() below then steps from.
    engine.run();
    if (options.headless) {
        for (int i = 0; i < options.frames; ++i) engine.advanceTime(options.stepMs);
    }
    return 0;
}
