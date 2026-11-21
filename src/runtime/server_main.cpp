// bro-server (runtime_mains.h): compiled into libbro, entered through
// bro_run_server.

#include "runtime/runtime_mains.h"

#include "engine/capabilities.h"
#include "engine/engine.h"
#include "engine/launcher.h"
#include "bronze_host/app_aot.h"
#include "bronze_host/eval.h"
#include "bronze_host/host_headless.h"
#include "util/exe_dir.h"
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

int bro::runtime::runServerMain(int argc, char* argv[]) {
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
            "  --tickrate N          Fixed server tick rate in Hz (default: 0, event-driven)\n"
            "  --grant <caps>        Grant capabilities for this run (comma-separated;\n"
            "                        'declared' is everything bro.json declares), as on bro\n"
            "  --aot <dir>           Run the script from its module built ahead of time in\n"
            "                        <dir>; with none yet, compile it in process and harvest\n"
            "                        the run, and build the module on that when it ends\n"
            "  --aot-build <dir>     Build the script's module in <dir> on its harvested\n"
            "                        run, then exit (what --aot starts in the background)\n"
            "  -h, --help            Show this help text\n"
            "\n"
            "Server JS globals:\n"
            "  bro.server.tickrate   Get/set tick rate in Hz (0 = event-driven)\n"
            "  bro.server.uptime     Seconds since server started\n"
            "  bro.server.stop()     Request graceful shutdown\n"
            "\n"
            "Also available: bro.net.*, brokit.*, setTimeout/setInterval, fetch,\n"
            "  WebSocket, localStorage, Workers\n");
        return 0;
    }

    double tickrate = 0.0;
    std::string aotDir;
    std::string aotBuildDir;
    std::vector<std::string> hostGrants;
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
            if (tickrate > 1000.0) tickrate = 1000.0;
        } else if (strcmp(argv[i], "--grant") == 0 && i + 1 < argc) {
            bro::engine::appendGrantList(argv[++i], hostGrants);
        } else if (strcmp(argv[i], "--aot") == 0 && i + 1 < argc) {
            aotDir = argv[++i];
        } else if (strcmp(argv[i], "--aot-build") == 0 && i + 1 < argc) {
            aotBuildDir = argv[++i];
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
    bool standaloneScript = false;

    try {
        bro::engine::EngineConfig config;
        config.settingsPath = bro::util::defaultSettingsPath();
        config.displayMode = bro::engine::DisplayMode::Server;
        config.hostGrants = hostGrants;

        if (!bro::engine::resolveLaunchTarget(target, config)) {
            // If target is directly a script file (.js/.mjs/.cjs/.ts/.mts):
            std::error_code ec;
            const std::string ext = std::filesystem::path(target).extension().string();
            if (std::filesystem::is_regular_file(target, ec) &&
                (ext == ".js" || ext == ".mjs" || ext == ".cjs" || ext == ".ts" || ext == ".mts")) {
                scriptPath = target;
                config.appDir = bro::util::executableDir();
                standaloneScript = true;
            } else {
                fprintf(stderr, "Error: no app found at '%s'\n", target.c_str());
                return 1;
            }
        }

        bro::engine::publishLaunchEnv(config);

        // A page app (one with an index.html) is not run here: its scripts
        // are written for a renderer. The server runs the script named on the
        // command line, else the app's own server.js / server.ts, as the
        // app's one script unit (Engine::loadPageManifest). An app without
        // index.html is a script-entry app: its server / main script is
        // that unit already.
        std::error_code fsEc;
        const std::filesystem::path appPath(config.appDir);
        const bool pageApp = std::filesystem::is_regular_file(appPath / "index.html", fsEc);
        if (pageApp && scriptPath.empty()) {
            if (std::filesystem::is_regular_file(appPath / "server.js", fsEc)) {
                scriptPath = (appPath / "server.js").string();
            } else if (std::filesystem::is_regular_file(appPath / "server.ts", fsEc)) {
                scriptPath = (appPath / "server.ts").string();
            }
        }
        if (!scriptPath.empty()) {
            if (!std::filesystem::is_regular_file(scriptPath, fsEc)) {
                fprintf(stderr, "Error: no script at '%s'\n", scriptPath.c_str());
                return 1;
            }
            // Resolved as the app directory is (publishLaunchEnv), so a script
            // named on the command line and the app's own server.js are one
            // path, and one AOT unit, when the app is reached through a
            // symlink (macOS's /var is /private/var).
            config.serverScript = bro::engine::absolutePath(scriptPath);
        }

        // The script's module built ahead of time (bronze_host/app_aot.h):
        // run from it, or harvest the run for it and build it on that at the
        // end, in the background, with this same command line.
        if (!aotBuildDir.empty()) {
            config.aotBuildDir = std::filesystem::absolute(aotBuildDir, fsEc).string();
        } else if (!aotDir.empty()) {
            config.aotDir = std::filesystem::absolute(aotDir, fsEc).string();
            std::filesystem::create_directories(config.aotDir, fsEc);
            if (fsEc) {
                fprintf(stderr, "Error: cannot make the AOT directory '%s': %s\n", config.aotDir.c_str(),
                        fsEc.message().c_str());
                return 1;
            }
#ifdef _WIN32
            const char* exeName = "bro-server.exe";
#else
            const char* exeName = "bro-server";
#endif
            const std::string self = (std::filesystem::path(bro::util::executableDir()) / exeName).string();
            std::vector<std::string> rebuild = {self};
            if (!standaloneScript) rebuild.push_back(config.appDir);
            if (!config.serverScript.empty()) rebuild.push_back(config.serverScript);
            rebuild.push_back("--aot-build");
            rebuild.push_back(config.aotDir);
            bro::bronze_host::setAotRebuildCommand(std::move(rebuild));
        }

        auto engine = std::make_unique<bro::engine::Engine>(config);
        engine->setServerTickRate(tickrate);

        // --aot-build: the engine's load built the unit instead of running it.
        if (!aotBuildDir.empty()) {
            const bool failed = bro::bronze_host::hasTestFailure() || engine->hasTestFailure();
            engine.reset();
            return failed ? 1 : 0;
        }
        if (engine->hasTestFailure()) {
            fprintf(stderr, "Error: the server script failed: see the log\n");
            engine.reset();
            return 1;
        }

        engine->run();
        engine.reset();
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: %s", e.what());
        exitCode = 1;
    }

    return exitCode;
}
