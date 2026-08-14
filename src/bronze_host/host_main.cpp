// bro-bronze-host — the smallest host that runs a bronze-compiled JS app
// inside a bro Engine. The same shape as docs/embedding.md's host walkthrough
// and headless/main.cpp's __host: nothing but the public engine surface plus
// bronze's embed API.
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
//   4. frames — windowed, a fixed count, or one driver script's advanceTime()
//      calls; every one of them fires the same Engine::onFrame seam.
//
// Three modes, and step 4 is the only thing that differs between them:
//
//   bro-bronze-host <appdir>                        windowed, until closed
//   bro-bronze-host <appdir> --headless --frames N  exactly N virtual frames
//   bro-bronze-host <appdir> --headless script.js   bro-headless's driver
//
// The third one is bro-headless (engine/headless_driver.h) with this host's
// globals and compiled top level dropped into it, not a reimplementation:
// the driver's script/-e/REPL handling, its options and its whole headless
// vocabulary — advanceTime, screenshot, getPixel, assert, click — are the
// ones docs/headless.md documents, evaluated in the QuickJS realm the Engine
// boots for the app dir's page. The compiled app has no JS realm and needs
// none: driver and app share the Engine and the clock, so advanceTime() steps
// the app through the same Engine::onFrame seam its rAF and microtask
// checkpoint already hang from (README.md, "The frame seam").

#include "bronze_host/bronze_host.h"

#include "engine/engine.h"
#include "engine/headless_driver.h"
#include "engine/launcher.h"

#include "embed/embed.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// --headless --frames N [--step MS] exists so the compiled app can be CHECKED
// with no script at all: a windowed run ends when a human closes the window and
// its frame count depends on how fast the machine is, neither of which a pinned
// expectation can be written against. Here the clock advances by exactly `step`
// per frame and the run ends after exactly `frames` of them, so the app's
// output is a function of the app. tests/bronze_host/ is that expectation.
struct Options {
    bool headless = false;
    int frames = 0;        // >0 selects the fixed-frame mode
    double stepMs = 16.0;  // the 60 Hz frame the rest of the engine assumes
    std::vector<std::string> rest;  // everything that is not ours, in order
};

// Our three flags are consumed; everything else is forwarded verbatim and in
// order, because in driver mode `rest` IS bro-headless's command line (app
// dir, script path, -e, --width, and whatever follows a `--`). Unknown options
// are therefore not rejected here — the driver owns that vocabulary and its
// diagnostics, and a second opinion could only disagree with it.
void parseArgs(int argc, char* argv[], Options& out) {
    bool passThrough = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (passThrough) {
            out.rest.push_back(arg);
        } else if (arg == "--") {
            // Kept, not dropped: it is the driver's own end-of-options marker
            // and the script behind it may want a literal `--headless`.
            passThrough = true;
            out.rest.push_back(arg);
        } else if (arg == "--headless") {
            out.headless = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            out.frames = std::atoi(argv[++i]);
        } else if (arg == "--step" && i + 1 < argc) {
            out.stepMs = std::atof(argv[++i]);
        } else {
            out.rest.push_back(arg);
        }
    }
}

// The two modes this file drives itself take exactly one argument — the app
// directory — so anything else is a typo worth naming rather than ignoring.
const char* soleAppDir(const Options& options) {
    if (options.rest.size() != 1) return nullptr;
    const std::string& arg = options.rest.front();
    if (arg.empty() || arg[0] == '-') return nullptr;
    return arg.c_str();
}

void usage() {
    std::fprintf(stderr,
                 "usage: bro-bronze-host <appdir>\n"
                 "       bro-bronze-host <appdir> --headless --frames N [--step MS]\n"
                 "       bro-bronze-host <appdir> --headless [script.js | -e \"expr\" ...]\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    // A bro Engine still boots from an app directory (manifest, settings,
    // asset mounts) even when the app's JS was compiled away — point this at
    // a minimal app dir whose ui holds an empty page. app/appdir beside this
    // file is one, and README.md shows what is in it.
    Options options;
    parseArgs(argc, argv, options);

    // Driver mode: --headless with no --frames, i.e. a frame count that comes
    // from a script instead of the command line. runHeadless does everything
    // below (config, app-dir resolution, Engine, run()) and then evaluates the
    // script; the two calls main() makes by hand for the other modes are the
    // afterEngine hook, at the point in the driver where an interpreted app's
    // own JS has just finished running. It never returns — the driver exits
    // the process with the script's status, which is how a failed assert()
    // becomes a nonzero exit.
    if (options.headless && options.frames <= 0) {
        bro::engine::HeadlessHooks hooks;
        hooks.programName = "bro-bronze-host";
        hooks.tagline = "headless driver for a bronze-compiled app";
        // This binary IS the compiled app's host, which is what lets engine
        // init tell an app dir that forgot `"compiled": true` from one that
        // declared it and got opened by a binary with nothing linked in.
        hooks.providesCompiledApp = true;
        hooks.afterEngine = [](bro::engine::Engine& engine) {
            bro::bronze_host::installWebHostGlobals(engine);
            bronze::embed::runMain();
        };
        std::vector<char*> forwarded;
        forwarded.push_back(argv[0]);
        for (std::string& arg : options.rest) forwarded.push_back(arg.data());
        return bro::engine::runHeadless(static_cast<int>(forwarded.size()),
                                        forwarded.data(), hooks);
    }

    const char* appDir = soleAppDir(options);
    if (!appDir) {
        usage();
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
    config.hostProvidesCompiledApp = true;  // see the driver-mode hook above
    if (!bro::engine::resolveLaunchTarget(appDir, config)) {
        std::fprintf(stderr, "bro-bronze-host: %s is not an app directory\n", appDir);
        return 1;
    }
    bro::engine::publishLaunchEnv(config);

    bro::engine::Engine engine(config);

    // Globals BEFORE the program: its top level reads them as it runs.
    bro::bronze_host::installWebHostGlobals(engine);

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
