#include "engine/engine.h"
#include "util/log.h"
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        LOG_ERROR("Usage: bro <app-directory>");
        LOG_ERROR("Example: bro apps/hello");
        return 1;
    }

    std::string appDir = argv[1];

    try {
        bro::engine::Engine engine(appDir);
        engine.run();
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: %s", e.what());
        return 1;
    }

    return 0;
}
