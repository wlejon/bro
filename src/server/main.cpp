#include "engine/engine.h"
#include "engine/launcher.h"
#include "bronze_host/app_module.h"
#include "bronze_host/eval.h"
#include "bronze_host/host_headless.h"
#include "util/interrupt.h"
#include "util/log.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    bro::util::installSignalHandler();

    bool showHelp = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--") == 0) break;
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            showHelp = true;
    }

    if (showHelp) {
        fprintf(stderr,
            "bro-server — dedicated game and agent server for bro\n"
            "\n"
            "Usage: bro-server [options] [app-directory] [script.js] [-- script-args...]\n"
            "\n"
            "Runs script.js (default: the app's server.js) against the app directory.\n"
            "An app's page (index.html and its scripts) is not run.\n"
            "\n"
            "Options:\n"
            "  --tickrate N          Server tick rate in Hz (default: 60)\n"
            "  -h, --help            Show this help text\n"
            "\n"
            "Server JS globals:\n"
            "  bro.server.tickrate   Get/set tick rate (Hz)\n"
            "  bro.server.uptime     Seconds since server started\n"
            "  bro.server.stop()     Request graceful shutdown\n"
            "\n"
            "Also available: bro.net.*, brokit.*, setTimeout/setInterval, fetch,\n"
            "  WebSocket, localStorage, Workers\n");
        return 0;
    }

    double tickrate = 60.0;
    std::string target;
    std::string scriptPath;
    std::vector<std::string> scriptArgs;

    bool passThrough = false;
    for (int i = 1; i < argc; ++i) {
        if (passThrough) {
            scriptArgs.push_back(argv[i]);
        } else if (strcmp(argv[i], "--") == 0) {
            passThrough = true;
        } else if (strcmp(argv[i], "--tickrate") == 0 && i + 1 < argc) {
            tickrate = atof(argv[++i]);
            if (tickrate < 1.0) tickrate = 1.0;
            if (tickrate > 1000.0) tickrate = 1000.0;
        } else if (target.empty() && argv[i][0] != '-') {
            target = argv[i];
        } else if (scriptPath.empty() && argv[i][0] != '-') {
            scriptPath = argv[i];
        } else {
            scriptArgs.push_back(argv[i]);
        }
    }

    if (target.empty()) {
        target = ".";
    }

    bro::bronze_host::setScriptArgs(scriptArgs);

    int exitCode = 0;

    try {
        bro::engine::EngineConfig config;
        config.settingsPath = bro::engine::executableDir() + "/.bro_settings.json";
        config.displayMode = bro::engine::DisplayMode::Server;

        if (!bro::engine::resolveLaunchTarget(target, config)) {
            // If target is directly a .js script file:
            std::error_code ec;
            if (std::filesystem::is_regular_file(target, ec) && target.size() >= 3 &&
                target.rfind(".js") == target.size() - 3) {
                scriptPath = target;
                config.appDir = bro::engine::executableDir();
            } else {
                fprintf(stderr, "Error: no app found at '%s'\n", target.c_str());
                return 1;
            }
        }

        bro::engine::publishLaunchEnv(config);

        // A page app (one with an index.html) is not run here: its scripts,
        // and the app.dll they compile to, are written for a renderer. The
        // server runs the script named on the command line, else the app's
        // own server.js. An app without index.html is a script-entry app and
        // its server.js / main.js (or compiled module) is the entry as before.
        std::error_code fsEc;
        const std::filesystem::path appPath(config.appDir);
        const bool pageApp = std::filesystem::is_regular_file(appPath / "index.html", fsEc);
        if (pageApp && scriptPath.empty() &&
            std::filesystem::is_regular_file(appPath / "server.js", fsEc)) {
            scriptPath = (appPath / "server.js").string();
        }

        std::optional<std::string> appModule = bro::bronze_host::findAppModule(config.appDir);
        config.hostProvidesCompiledApp = appModule.has_value();

        auto engine = std::make_unique<bro::engine::Engine>(config);
        engine->setServerTickRate(tickrate);

        if (appModule && !pageApp) {
            bro::bronze_host::runAppModule(*engine, *appModule);
        }

        if (!scriptPath.empty()) {
            if (!bro::bronze_host::evalScriptFile(*engine, scriptPath)) {
                fprintf(stderr, "Error: failed to evaluate script '%s'\n", scriptPath.c_str());
                return 1;
            }
        }

        engine->run();
        engine.reset();
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: %s", e.what());
        exitCode = 1;
    }

    return exitCode;
}
