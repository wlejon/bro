#include "engine/engine.h"
#include "engine/config_loader.h"

using bro::engine::parseConfig;
#include "js/headless_bindings.h"
#include "js/runtime.h"
#include "util/interrupt.h"
#include "util/log.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#include <climits>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
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
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char resolved[PATH_MAX];
        if (realpath(buf, resolved)) path = resolved;
        else path = buf;
    }
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) { buf[len] = '\0'; path = buf; }
#endif
    auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos) return path.substr(0, slash);
    return ".";
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Evaluate JS code. Uses async mode only when code contains 'await'.
/// Returns true on success. If printResult is true, prints the final value
/// to stdout (for -e mode).
static bool evalCode(JSContext* ctx, bro::js::Runtime* rt,
                     const std::string& code, const char* filename,
                     bool printResult = false) {
    bool useAsync = code.find("await") != std::string::npos;
    int flags = JS_EVAL_TYPE_GLOBAL | (useAsync ? JS_EVAL_FLAG_ASYNC : 0);

    JSValue result = JS_Eval(ctx, code.c_str(), code.size(), filename, flags);

    if (JS_IsException(result)) {
        bro::js::Runtime::checkException(ctx, result);
        JS_FreeValue(ctx, result);
        return false;
    }

    if (useAsync && JS_IsPromise(result)) {
        // Drain microtasks until promise settles
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

        if (printResult) {
            JSValue resolved = JS_PromiseResult(ctx, result);
            if (!JS_IsUndefined(resolved)) {
                const char* str = JS_ToCString(ctx, resolved);
                if (str) { printf("%s\n", str); JS_FreeCString(ctx, str); }
            }
            JS_FreeValue(ctx, resolved);
        }
    } else {
        rt->executePendingJobs();
        if (printResult && !JS_IsUndefined(result)) {
            const char* str = JS_ToCString(ctx, result);
            if (str) { printf("%s\n", str); JS_FreeCString(ctx, str); }
        }
    }

    JS_FreeValue(ctx, result);
    return true;
}

/// Heuristic: does this top-level script use ES-module syntax? Scans for a line
/// whose first non-space token is a static `import`/`export` (a bare `import(`
/// dynamic import is valid in a classic script and is intentionally excluded).
static bool looksLikeModule(const std::string& code) {
    const size_t n = code.size();
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && (code[j] == ' ' || code[j] == '\t')) j++;
        auto starts = [&](const char* kw) {
            size_t k = 0;
            while (kw[k]) {
                if (j + k >= n || code[j + k] != kw[k]) return false;
                ++k;
            }
            char c = (j + k < n) ? code[j + k] : '\0';
            // Static import/export is followed by a space, brace, star, or quote.
            return c == ' ' || c == '\t' || c == '{' || c == '*' ||
                   c == '"' || c == '\'';
        };
        if (starts("import") || starts("export")) return true;
        while (i < n && code[i] != '\n') i++;
        if (i < n) i++;
    }
    return false;
}

/// Evaluate a script file as an ES module (so it can `import` the app's
/// already-loaded modules — e.g. test harnesses importing from /app/*). Module
/// evaluation is async, so drain microtasks until the evaluation promise settles
/// and surface a rejected body (a failed assert / ReferenceError) as an error —
/// otherwise it would be silently swallowed.
static bool evalModuleFile(JSContext* ctx, bro::js::Runtime* rt,
                           const std::string& code, const char* filename) {
    JSValue func = JS_Eval(ctx, code.c_str(), code.size(), filename,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(func)) {
        bro::js::Runtime::checkException(ctx, func);
        JS_FreeValue(ctx, func);
        return false;
    }
    JSValue result = JS_EvalFunction(ctx, func);
    if (JS_IsException(result)) {
        bro::js::Runtime::checkException(ctx, result);
        JS_FreeValue(ctx, result);
        return false;
    }
    bool ok = true;
    if (JS_IsPromise(result)) {
        while (JS_PromiseState(ctx, result) == JS_PROMISE_PENDING)
            rt->executePendingJobs();
        if (JS_PromiseState(ctx, result) == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(ctx, result);
            const char* msg = JS_ToCString(ctx, reason);
            JSValue stack = JS_GetPropertyStr(ctx, reason, "stack");
            const char* st = JS_IsUndefined(stack) ? nullptr
                                                   : JS_ToCString(ctx, stack);
            LOG_ERROR("Module error: %s%s%s", msg ? msg : "(unknown)",
                      st ? "\n" : "", st ? st : "");
            if (msg) JS_FreeCString(ctx, msg);
            if (st) JS_FreeCString(ctx, st);
            JS_FreeValue(ctx, stack);
            JS_FreeValue(ctx, reason);
            ok = false;
        }
    } else {
        rt->executePendingJobs();
    }
    JS_FreeValue(ctx, result);
    return ok;
}

/// Run a JS REPL on stdin.
static void runRepl(JSContext* ctx, bro::js::Runtime* rt,
                    bro::engine::Engine* engine) {
    bool tty = isatty(fileno(stdin));

    if (tty)
        fprintf(stderr, "bro> ");

    std::string line;
    while (std::getline(std::cin, line)) {
        // Trim
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty()) {
            if (tty) fprintf(stderr, "bro> ");
            continue;
        }
        if (line == "quit" || line == "exit") break;

        JSValue result = JS_Eval(ctx, line.c_str(), line.size(), "<repl>",
                                 JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result)) {
            bro::js::Runtime::checkException(ctx, result);
        } else if (!JS_IsUndefined(result)) {
            const char* str = JS_ToCString(ctx, result);
            if (str) {
                printf("%s\n", str);
                JS_FreeCString(ctx, str);
            }
        }
        JS_FreeValue(ctx, result);
        engine->flush();

        if (tty) fprintf(stderr, "bro> ");
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    bro::util::installSignalHandler();

    bool showHelp = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            showHelp = true;
    }

    if (argc < 2 || showHelp) {
        fprintf(stderr,
            "bro-headless — headless mode for bro\n"
            "\n"
            "Usage: bro-headless [--no-gpu] <app-directory> [script.js | -e \"expr\" ...]\n"
            "\n"
            "Modes:\n"
            "  bro-headless app/              Interactive JS REPL\n"
            "  bro-headless app/ test.js      Run script file\n"
            "  bro-headless app/ -e \"expr\"     Evaluate inline expression(s)\n"
            "\n"
            "Options:\n"
            "  --no-gpu              Disable GPU rendering (CPU-only, no WebGL)\n"
            "  --audio               Open the real SDL audio device + mic (default: no device)\n"
            "  --width N             Viewport width (default: 1920)\n"
            "  --height N            Viewport height (default: 1080)\n"
            "  --splash              Show the startup splash (off by default in headless)\n"
            "  --no-splash           Explicitly disable the splash\n"
            "\n"
            "Headless globals:\n"
            "  screenshot(path [, selector])  Render to PNG (optionally cropped to element)\n"
            "  advanceTime(ms) / sleep(ms)    Advance virtual time\n"
            "  flush()                        Force layout recalculation\n"
            "  assert(cond [, msg])           Throw on failure (exit code 1)\n");
        return showHelp ? 0 : 1;
    }

    // Parse args
    bool useGPU = true;
    bool realAudio = false;
    int width = 1920;
    int height = 1080;
    // Splash defaults off in headless — the splash canvas animation leaks
    // into early screenshots (matrix glyphs at top of frame). Opt-in via
    // --splash if you specifically want to exercise the splash lifecycle.
    int cliSplash = -1;   // -1 unset, 0 off, 1 on
    std::string appDir;
    std::string scriptPath;
    std::vector<std::string> inlineExprs;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-gpu") == 0) {
            useGPU = false;
        } else if (strcmp(argv[i], "--audio") == 0) {
            realAudio = true;
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--splash") == 0) {
            cliSplash = 1;
        } else if (strcmp(argv[i], "--no-splash") == 0) {
            cliSplash = 0;
        } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            inlineExprs.push_back(argv[++i]);
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

    int exitCode = 0;

    try {
        bro::engine::EngineConfig config;
        std::string settingsDir = exeDir();
        config.settingsPath = settingsDir + "/.bro_settings.json";

        // Expose exe directory to JS (process.env.BRO_EXE_DIR).
#ifdef _WIN32
        _putenv_s("BRO_EXE_DIR", settingsDir.c_str());
#else
        setenv("BRO_EXE_DIR", settingsDir.c_str(), 1);
#endif

        // Project root resolution: same model as bro main.cpp. If appDir is a
        // .json, treat as project bro.json; if it's a directory containing a
        // bro.json with project keys, treat that as project. Else inherit
        // from BRO_PROJECT_ROOT env if set.
        {
            auto isJsonFile = [](const std::string& s) {
                return s.size() >= 5 && s.substr(s.size() - 5) == ".json";
            };
            auto dirOf = [](const std::string& p) -> std::string {
                size_t i = p.find_last_of("/\\");
                return (i == std::string::npos) ? std::string(".") : p.substr(0, i);
            };
            auto isAbsolute = [](const std::string& p) {
                return !p.empty() && (p[0] == '/' || p[0] == '\\' ||
                                      (p.size() >= 2 && p[1] == ':'));
            };

            // Don't preset config.appDir — parseConfig fills it from
            // "app"/"default_app". Presetting would make the empty-check at
            // config_loader skip default_app, then concat the original argv
            // path back onto its own dir (../broworkshop/../broworkshop/bro.json).
            if (isJsonFile(appDir) && std::ifstream(appDir).good()) {
                std::string targetDir = dirOf(appDir);
                bool isProject = false;
                parseConfig(appDir, config, &isProject);
                if (isProject) {
                    config.projectRoot = targetDir;
                    if (config.appDir.empty()) config.appDir = targetDir;
                    else if (!isAbsolute(config.appDir)) config.appDir = targetDir + "/" + config.appDir;
                } else {
                    // App manifest pointed to directly — its dir is the appDir.
                    config.appDir = targetDir;
                }
            } else {
                std::string broJson = appDir + "/bro.json";
                if (std::ifstream(broJson).good()) {
                    bool isProject = false;
                    parseConfig(broJson, config, &isProject);
                    if (isProject) {
                        config.projectRoot = appDir;
                        if (config.appDir.empty()) config.appDir = appDir;
                        else if (!isAbsolute(config.appDir)) config.appDir = appDir + "/" + config.appDir;
                    } else {
                        config.appDir = appDir;
                    }
                } else {
                    config.appDir = appDir;
                }
            }

            if (config.projectRoot.empty()) {
                if (const char* env = std::getenv("BRO_PROJECT_ROOT")) {
                    if (*env) config.projectRoot = env;
                }
            }
        }
        config.displayMode = bro::engine::DisplayMode::Headless;
        config.realAudio = realAudio;
        config.graphics.width = width;
        config.graphics.height = height;
        config.graphics.useGPU = useGPU;
        // Headless: splash defaults off; --splash opts in, --no-splash is
        // explicit-off (kept for symmetry with windowed bro).
        config.showSplash = (cliSplash == 1);

        auto* engine = new bro::engine::Engine(config);
        engine->run();  // initial layout, returns immediately in headless

        auto* rt = engine->jsRuntime();
        auto* ctx = rt->getContext();
        bro::js::installHeadlessBindings(ctx, engine);

        if (!inlineExprs.empty()) {
            // -e mode: concatenate and eval
            std::ostringstream oss;
            for (size_t i = 0; i < inlineExprs.size(); ++i) {
                if (i > 0) oss << ";\n";
                oss << inlineExprs[i];
            }
            if (!evalCode(ctx, rt, oss.str(), "<inline>", true))
                exitCode = 1;
        } else if (!scriptPath.empty()) {
            // Script file mode
            std::ifstream ifs(scriptPath);
            if (!ifs.is_open()) {
                fprintf(stderr, "Error: cannot open script: %s\n", scriptPath.c_str());
                exitCode = 1;
            } else {
                std::ostringstream oss;
                oss << ifs.rdbuf();
                std::string src = oss.str();
                bool ok = looksLikeModule(src)
                              ? evalModuleFile(ctx, rt, src, scriptPath.c_str())
                              : evalCode(ctx, rt, src, scriptPath.c_str());
                if (!ok)
                    exitCode = 1;
            }
        } else {
            // Interactive REPL
            runRepl(ctx, rt, engine);
        }

        // Intentionally leak to avoid QuickJS GC assertion on shutdown.
        // The OS reclaims all memory on process exit.
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: %s", e.what());
        exitCode = 1;
    }

    _exit(exitCode);
}
