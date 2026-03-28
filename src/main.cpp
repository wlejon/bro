#include "engine/engine.h"
#include "render/demo_scene.h"
#include "util/log.h"
#include <cstring>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        LOG_ERROR("Usage: bro [--scene] <app-directory>");
        LOG_ERROR("Example: bro apps/hello");
        LOG_ERROR("         bro --scene apps/hello");
        return 1;
    }

    bool enableScene = false;
    std::string appDir;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--scene") == 0) {
            enableScene = true;
        } else {
            appDir = argv[i];
        }
    }

    if (appDir.empty()) {
        LOG_ERROR("No app directory specified");
        return 1;
    }

    try {
        bro::engine::Engine engine(appDir);
        if (enableScene) {
            engine.setSceneLayer(std::make_unique<bro::render::DemoScene>());
        }
        engine.run();
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: %s", e.what());
        return 1;
    }

    return 0;
}
