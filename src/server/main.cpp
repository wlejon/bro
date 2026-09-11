#include "engine/engine.h"
#include "util/interrupt.h"
#include "util/log.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static std::string exeDir() {
    std::string path;
#ifdef _WIN32
    char buf[260];
    DWORD len = GetModuleFileNameA(nullptr, buf, 260);
    if (len > 0 && len < 260) path = std::string(buf, len);
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) { buf[len] = '\0'; path = buf; }
#endif
    auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos) return path.substr(0, slash);
    return ".";
}

int main(int argc, char* argv[]) {
    bro::util::installSignalHandler();

    bool showHelp = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--") == 0) break;
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            showHelp = true;
    }

    if (argc < 2 || showHelp) {
        fprintf(stderr,
            "bro-server — dedicated game server for bro\n"
            "\n"
            "Usage: bro-server [options] <app-directory>\n"
            "\n"
            "Options:\n"
            "  --tickrate N          Server tick rate in Hz (default: 60)\n");
        return showHelp ? 0 : 1;
    }

    double tickrate = 60.0;
    std::string appDir;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--") == 0) {
            break;
        } else if (strcmp(argv[i], "--tickrate") == 0 && i + 1 < argc) {
            tickrate = atof(argv[++i]);
            if (tickrate < 1.0) tickrate = 1.0;
            if (tickrate > 1000.0) tickrate = 1000.0;
        } else if (appDir.empty()) {
            appDir = argv[i];
        }
    }

    if (appDir.empty()) {
        fprintf(stderr, "Error: no app directory specified\n");
        return 1;
    }

    int exitCode = 0;

    try {
        bro::engine::EngineConfig config;
        config.appDir = appDir;
        config.settingsPath = exeDir() + "/.bro_settings.json";
        config.displayMode = bro::engine::DisplayMode::Server;

        auto engine = std::make_unique<bro::engine::Engine>(config);
        engine->setServerTickRate(tickrate);

        engine->run();
        engine.reset();
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: %s", e.what());
        exitCode = 1;
    }

    return exitCode;
}
