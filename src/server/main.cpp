#include "engine/engine.h"
#include "js/server_bindings.h"
#include "js/runtime.h"
#include "util/log.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

extern "C" {
#include "quickjs.h"
}

// Get the directory containing the current executable.
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

// Get parent directory of a file path.
static std::string parentDir(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos) return path.substr(0, slash);
    return ".";
}

/// Evaluate JS code, handling async/await.
static bool evalCode(JSContext* ctx, bro::js::Runtime* rt,
                     const std::string& code, const char* filename) {
    bool useAsync = code.find("await") != std::string::npos;
    int flags = JS_EVAL_TYPE_GLOBAL | (useAsync ? JS_EVAL_FLAG_ASYNC : 0);

    JSValue result = JS_Eval(ctx, code.c_str(), code.size(), filename, flags);

    if (JS_IsException(result)) {
        bro::js::Runtime::checkException(ctx, result);
        JS_FreeValue(ctx, result);
        return false;
    }

    if (useAsync && JS_IsPromise(result)) {
        while (JS_PromiseState(ctx, result) == JS_PROMISE_PENDING)
            rt->executePendingJobs();

        if (JS_PromiseState(ctx, result) == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(ctx, result);
            const char* str = JS_ToCString(ctx, reason);
            if (str) {
                LOG_ERROR("Unhandled rejection: %s", str);
                JS_FreeCString(ctx, str);
            }
            JS_FreeValue(ctx, reason);
            JS_FreeValue(ctx, result);
            return false;
        }
    } else {
        rt->executePendingJobs();
    }

    JS_FreeValue(ctx, result);
    return true;
}

int main(int argc, char* argv[]) {
    bool showHelp = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            showHelp = true;
    }

    if (argc < 2 || showHelp) {
        fprintf(stderr,
            "bro-server — dedicated game server for bro\n"
            "\n"
            "Usage: bro-server [options] <app-directory> <script.js>\n"
            "\n"
            "Options:\n"
            "  --tickrate N          Server tick rate in Hz (default: 60)\n"
            "\n"
            "Server JS globals:\n"
            "  bro.server.tickrate   Get/set tick rate (Hz)\n"
            "  bro.server.uptime     Seconds since server started\n"
            "  bro.server.stop()     Request graceful shutdown\n"
            "\n"
            "Also available: bro.net.*, bro.physics.*, bro.mesh.*,\n"
            "  bro.noise.*, setTimeout/setInterval, fetch, WebSocket,\n"
            "  localStorage, Workers\n");
        return showHelp ? 0 : 1;
    }

    // Parse args
    double tickrate = 60.0;
    std::string appDir;
    std::string scriptPath;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--tickrate") == 0 && i + 1 < argc) {
            tickrate = atof(argv[++i]);
            if (tickrate < 1.0) tickrate = 1.0;
            if (tickrate > 1000.0) tickrate = 1000.0;
        } else if (appDir.empty()) {
            appDir = argv[i];
        } else if (scriptPath.empty()) {
            scriptPath = argv[i];
        }
    }

    if (appDir.empty()) {
        fprintf(stderr, "Error: no app directory specified\n");
        return 1;
    }
    if (scriptPath.empty()) {
        fprintf(stderr, "Error: no script file specified\n");
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

        auto* rt = engine->jsRuntime();
        auto* ctx = rt->getContext();

        // Load and execute the server script
        std::ifstream ifs(scriptPath);
        if (!ifs.is_open()) {
            fprintf(stderr, "Error: cannot open script: %s\n", scriptPath.c_str());
            return 1;
        }
        std::ostringstream oss;
        oss << ifs.rdbuf();

        if (!evalCode(ctx, rt, oss.str(), scriptPath.c_str())) {
            fprintf(stderr, "Error: script execution failed\n");
            return 1;
        }

        // Enter the server tick loop (blocks until stop is requested)
        engine->run();

        // Cleanup
        engine.reset();
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: %s", e.what());
        exitCode = 1;
    }

    return exitCode;
}
