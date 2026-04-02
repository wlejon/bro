#include "engine/engine.h"
#include "headless/headless_controller.h"
#include "util/log.h"
#include <string>
#include <cstdlib>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: bro-headless <app-directory> [script.txt]\n");
        fprintf(stderr, "  No script = interactive mode (read commands from stdin)\n");
        fprintf(stderr, "  With script = run commands from file then exit\n");
        fprintf(stderr, "\nPipe commands:  echo \"dump\" | bro-headless apps/hello\n");
        return 1;
    }

    std::string appDir = argv[1];
    int exitCode = 0;

    try {
        auto* engine = new bro::engine::Engine(
            bro::engine::EngineConfig{appDir, 1024, 768, bro::engine::DisplayMode::Headless});
        engine->run();  // initial layout, returns immediately in headless mode

        bro::headless::HeadlessController controller(*engine);

        if (argc >= 3) {
            controller.runScript(argv[2]);
        } else {
            controller.runInteractive();
        }
        // Intentionally leak to avoid QuickJS GC assertion on shutdown.
        // The OS reclaims all memory on process exit.
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: %s", e.what());
        exitCode = 1;
    }

    _exit(exitCode);
}
