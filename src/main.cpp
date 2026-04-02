#include "engine/engine.h"
#include "util/log.h"
#include <cstring>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        fprintf(stderr,
            "bro — lightweight HTML/CSS/JS app runtime\n"
            "\n"
            "Usage: bro <app-directory>\n"
            "\n"
            "Loads index.html from the given directory and runs it in a\n"
            "GPU-accelerated window (Skia + OpenGL via SDL3).\n"
            "\n"
            "Example:\n"
            "  bro apps/hello\n"
            "\n"
            "See also: bro-headless for scripted/headless mode.\n");
        return argc < 2 ? 1 : 0;
    }

    std::string appDir = argv[1];

    try {
        bro::engine::Engine engine(bro::engine::EngineConfig{appDir});
        engine.run();
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: %s", e.what());
        return 1;
    }

    return 0;
}
