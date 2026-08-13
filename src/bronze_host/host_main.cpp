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
#include <string>

int main(int argc, char* argv[]) {
    // A bro Engine still boots from an app directory (manifest, settings,
    // asset mounts) even when the app's JS was compiled away — point this at
    // a minimal app dir whose ui holds an empty page. README.md beside this
    // file shows the two-line app dir that suffices.
    if (argc < 2) {
        std::fprintf(stderr, "usage: bro-bronze-host <appdir>\n");
        return 1;
    }

    bro::engine::EngineConfig config;
    config.title = "bro-bronze-host";
    config.displayMode = bro::engine::DisplayMode::Windowed;
    config.settingsPath = bro::engine::executableDir() + "/.bro_settings.json";
    if (!bro::engine::resolveLaunchTarget(argv[1], config)) {
        std::fprintf(stderr, "bro-bronze-host: %s is not an app directory\n", argv[1]);
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
    // requestAnimationFrame now runs a frame at a time until the window
    // closes. Teardown order on return: the Engine (and with it every GL
    // context) dies at end of scope; bronze's heap statics die at process
    // exit WITHOUT running handle finalizers (embed.h's contract), so no
    // finalizer can chase the dead GL context.
    engine.run();
    return 0;
}
