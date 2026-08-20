#include "engine/headless_driver.h"

#include "engine/engine.h"
#include "engine/config_loader.h"

using bro::engine::parseConfig;
using bro::engine::findAncestorProjectRoot;
#include "js/async_job.h"
#include "js/headless_bindings.h"
#include "js/runtime.h"
#include "util/interrupt.h"
#include "util/log.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <crtdbg.h>
#include <tlhelp32.h>
#include <psapi.h>
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

#ifdef _WIN32
// BRO_THREAD_CENSUS=1: right before _exit(), tally the threads still alive in
// this process by owning module. Every live thread here gets force-terminated
// by the kernel's process rundown, which is the race window behind the
// recurring 0x139 CORRUPT_LIST_ENTRY bugchecks (see commit 1ab30390) — this
// census is how we measure whether teardown actually retired them.
typedef LONG(NTAPI* NtQueryInformationThreadFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);

static void printThreadCensus() {
    auto* ntdll = GetModuleHandleA("ntdll.dll");
    auto queryThread = ntdll ? reinterpret_cast<NtQueryInformationThreadFn>(
                                   GetProcAddress(ntdll, "NtQueryInformationThread"))
                             : nullptr;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    std::vector<std::pair<std::string, int>> tally;  // module -> count
    int total = 0;
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        total++;
        std::string mod = "<unknown>";
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (h) {
            PVOID start = nullptr;
            ULONG got = 0;
            // 9 = ThreadQuerySetWin32StartAddress
            if (queryThread && queryThread(h, 9, &start, sizeof(start), &got) == 0 && start) {
                HMODULE hm = nullptr;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCSTR>(start), &hm) && hm) {
                    char path[MAX_PATH];
                    if (GetModuleFileNameA(hm, path, MAX_PATH)) {
                        const char* base = strrchr(path, '\\');
                        mod = base ? base + 1 : path;
                    }
                }
            }
            CloseHandle(h);
        }
        // Thread name (if the owner labeled it) pins down which driver
        // subsystem owns it — that decides which knob can prevent it.
        HANDLE hd = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (hd) {
            PWSTR desc = nullptr;
            if (SUCCEEDED(GetThreadDescription(hd, &desc)) && desc) {
                if (desc[0]) {
                    char nbuf[128];
                    int len = WideCharToMultiByte(CP_UTF8, 0, desc, -1, nbuf, sizeof(nbuf),
                                                  nullptr, nullptr);
                    if (len > 0) mod += std::string(" \"") + nbuf + "\"";
                }
                LocalFree(desc);
            }
            CloseHandle(hd);
        }
        bool found = false;
        for (auto& [m, n] : tally)
            if (m == mod) { n++; found = true; break; }
        if (!found) tally.emplace_back(mod, 1);
    }
    CloseHandle(snap);
    fprintf(stderr, "[thread-census] %d live threads at _exit (excluding main):\n", total);
    for (auto& [m, n] : tally) fprintf(stderr, "[thread-census]   %3d  %s\n", n, m.c_str());
    fflush(stderr);
}
#endif

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

// Resolve a possibly-relative path to absolute, matching windowed bro's
// main.cpp (src/main.cpp) so BRO_APP_DIR/BRO_PROJECT_ROOT mean the same
// thing regardless of which executable set them.
static std::string absolutize(const std::string& p) {
    if (p.empty()) return p;
#ifdef _WIN32
    char abs[MAX_PATH];
    return _fullpath(abs, p.c_str(), MAX_PATH) ? std::string(abs) : p;
#else
    char abs[PATH_MAX];
    return realpath(p.c_str(), abs) ? std::string(abs) : p;
#endif
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Drain until a top-level promise settles, without busy-spinning forever.
///
/// Microtasks alone can't always settle a top-level await: timers, async
/// model jobs (bro.lm/tts/...), workers, and net/fetch completions are all
/// pumped by Engine::advanceTime(), which the old bare
/// `while (pending) executePendingJobs()` loop never called — a promise
/// waiting on any of those spun one core at 100% forever. So once the
/// microtask queue is drained and the promise is still pending, pump the
/// engine one 16 ms virtual-time step per pass (identical to one frame of
/// advanceTime), which fires due timers and delivers async-job results.
///
/// Deadline: "no forward progress for N seconds", not total elapsed — a
/// multi-minute model load keeps an async job in flight the whole time and
/// never trips it. Progress = a microtask ran or an async job is running.
/// When neither holds, external events (net peers, workers, fetch) may still
/// arrive, so we keep pumping on a wall-clock deadline (default 60 s,
/// BRO_PROMISE_TIMEOUT_MS overrides) instead of failing immediately.
/// Returns false (with an error logged) when the deadline trips.
static bool drainPromise(JSContext* ctx, bro::js::Runtime* rt,
                         bro::engine::Engine* engine, JSValue promise,
                         const char* what) {
    using clock = std::chrono::steady_clock;
    int timeoutMs = 60000;
    if (const char* env = std::getenv("BRO_PROMISE_TIMEOUT_MS")) {
        int v = atoi(env);
        if (v > 0) timeoutMs = v;
    }
    auto lastProgress = clock::now();
    while (JS_PromiseState(ctx, promise) == JS_PROMISE_PENDING) {
        bool hadJobs = JS_IsJobPending(JS_GetRuntime(ctx));
        rt->executePendingJobs();
        if (JS_PromiseState(ctx, promise) != JS_PROMISE_PENDING) break;

        // Microtask queue drained, promise still pending: pump the engine so
        // timers / async jobs / workers / net / fetch can resolve it.
        engine->advanceTime(16.0);

        if (hadJobs || bro::js::hasAsyncJobs()) {
            lastProgress = clock::now();
        } else if (clock::now() - lastProgress >
                   std::chrono::milliseconds(timeoutMs)) {
            LOG_ERROR("%s: top-level await did not settle within %d ms with no "
                      "pending jobs, timers, or async work remaining — a promise "
                      "is likely never resolved. Set BRO_PROMISE_TIMEOUT_MS to "
                      "override the deadline.", what, timeoutMs);
            return false;
        } else {
            // Nothing visibly in flight — wait for external events without
            // burning a core.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return true;
}

/// Evaluate JS code. Uses async mode only when code contains 'await'.
/// Returns true on success. If printResult is true, prints the final value
/// to stdout (for -e mode).
static bool evalCode(JSContext* ctx, bro::js::Runtime* rt,
                     bro::engine::Engine* engine,
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
        // Drain until the promise settles (deadline on no-forward-progress)
        if (!drainPromise(ctx, rt, engine, result, filename)) {
            JS_FreeValue(ctx, result);
            return false;
        }

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
                           bro::engine::Engine* engine,
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
        if (!drainPromise(ctx, rt, engine, result, filename)) {
            JS_FreeValue(ctx, result);
            return false;
        }
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

        // A location.reload() this line queued swaps the primary JSContext;
        // between lines is the REPL's no-JS-on-stack point. Re-fetch ctx so
        // the next line evaluates in the fresh realm.
        if (engine->processPendingAppReload())
            ctx = rt->getContext();

        if (tty) fprintf(stderr, "bro> ");
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int bro::engine::runHeadless(int argc, char* argv[], const HeadlessHooks& hooks) {
#ifdef _WIN32
    // Suppress the WER "bro-headless.exe has stopped working" dialog that
    // Windows shows after an unhandled crash/abort() — headless is driven
    // by scripts/CI that need the process to just die with a nonzero exit,
    // not block on a click.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#ifdef _DEBUG
    // Debug CRT's assert() otherwise ALSO pops its own blocking "Debug
    // Error!" Abort/Retry/Ignore dialog before the abort() above even
    // fires. Route it to stderr instead so the assertion text still shows
    // up in the log, just without the modal.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
#endif
    bro::util::installSignalHandler();

    // `--` ends bro's own options; everything after it belongs to the script,
    // so a script flag named --help must not print bro's help and exit.
    bool showHelp = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--") == 0) break;
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            showHelp = true;
    }

    if (argc < 2 || showHelp) {
        const char* prog = hooks.programName.empty() ? "bro-headless"
                                                     : hooks.programName.c_str();
        const char* tag = hooks.tagline.empty() ? "headless mode for bro"
                                                : hooks.tagline.c_str();
        fprintf(stderr,
            "%s — %s\n"
            "\n"
            "Usage: %s [options] <app-directory> [script.js | -e \"expr\"] [-- script-args...]\n"
            "\n"
            "Modes:\n"
            "  %s app/              Interactive JS REPL\n"
            "  %s app/ test.js      Run script file\n"
            "  %s app/ -e \"expr\"     Evaluate inline expression(s)\n"
            "\n",
            prog, tag, prog, prog, prog, prog);
        fprintf(stderr,
            "Script arguments:\n"
            "  Anything after the script path that bro does not recognise is passed\n"
            "  through as globalThis.scriptArgs (an array of strings). Use -- to pass\n"
            "  through arguments that would otherwise be read as bro options:\n"
            "    %s app/ etl/build.js --force\n"
            "    %s app/ etl/build.js -- --width 40\n"
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
            "  assert(cond [, msg])           Throw on failure (exit code 1)\n",
            prog, prog);
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
    std::vector<std::string> scriptArgs;

    // Everything past the app directory and the script path is the script's,
    // not ours. Without this a script had no way to take an argument at all,
    // which pushed ETL and test drivers into environment variables for what is
    // plainly a command-line flag.
    bool passThrough = false;
    for (int i = 1; i < argc; ++i) {
        if (passThrough) {
            scriptArgs.push_back(argv[i]);
        } else if (strcmp(argv[i], "--") == 0) {
            passThrough = true;
        } else if (strcmp(argv[i], "--no-gpu") == 0) {
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
        } else if (scriptPath.empty() && argv[i][0] != '-') {
            scriptPath = argv[i];
        } else {
            scriptArgs.push_back(argv[i]);
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

            // Still nothing: same fallback as windowed bro's main.cpp — walk
            // up looking for an ancestor project manifest so /lib, /system,
            // /std, /app mounts still resolve when an app is launched
            // standalone by passing its own directory directly.
            if (config.projectRoot.empty() && !config.appDir.empty()) {
                config.projectRoot = findAncestorProjectRoot(config.appDir);
            }
        }

        // Expose the resolved app directory and project root to JS
        // (process.env.BRO_APP_DIR, BRO_PROJECT_ROOT), matching windowed
        // bro's main.cpp — apps use this to locate their own directory on
        // disk for real filesystem writes (e.g. ai-arena's replay recorder),
        // since a raw relative path resolves against this process's actual
        // working directory, not the app directory.
        config.appDir = absolutize(config.appDir);
        if (!config.projectRoot.empty()) config.projectRoot = absolutize(config.projectRoot);
        // BRO_EXE_DIR matches windowed bro's main.cpp: scripts locate sibling
        // executables (bro, bro-headless, bro-server) through it — e.g. a test
        // spawning a child bro-headless.
        std::string exeDirPath = exeDir();
#ifdef _WIN32
        _putenv_s("BRO_APP_DIR", config.appDir.c_str());
        _putenv_s("BRO_PROJECT_ROOT", config.projectRoot.c_str());
        _putenv_s("BRO_EXE_DIR", exeDirPath.c_str());
#else
        setenv("BRO_APP_DIR", config.appDir.c_str(), 1);
        if (!config.projectRoot.empty()) setenv("BRO_PROJECT_ROOT", config.projectRoot.c_str(), 1);
        else unsetenv("BRO_PROJECT_ROOT");
        setenv("BRO_EXE_DIR", exeDirPath.c_str(), 1);
#endif

        config.displayMode = bro::engine::DisplayMode::Headless;
        config.realAudio = realAudio;
        config.graphics.width = width;
        config.graphics.height = height;
        config.graphics.useGPU = useGPU;
        // Headless: splash defaults off; --splash opts in, --no-splash is
        // explicit-off (kept for symmetry with windowed bro).
        config.showSplash = (cliSplash == 1);
        config.installHostBindings = hooks.installHostBindings;
        // Asked here and not earlier: the predicate is answered per app dir,
        // and config.appDir only became final (resolved and absolutised) a few
        // lines above.
        config.hostProvidesCompiledApp =
            hooks.providesCompiledApp && hooks.providesCompiledApp(config.appDir);

        // Host setup that the first document may depend on — a media backend
        // registration has to be in place before any <video> is parsed.
        if (hooks.beforeEngine) hooks.beforeEngine();

        auto* engine = new bro::engine::Engine(config);
        engine->run();  // initial layout, returns immediately in headless

        // A host whose app is native code runs it here (see the hook's
        // comment): before the headless globals, because the globals are the
        // driver's surface and the app is the subject being driven.
        if (hooks.afterEngine) hooks.afterEngine(*engine);

        auto* rt = engine->jsRuntime();
        auto* ctx = rt->getContext();
        bro::js::installHeadlessBindings(ctx, engine);
        bro::js::installScriptArgs(ctx, scriptArgs);

        // Drain any top-level location.reload() the app queued. In headless
        // the driving script runs INSIDE the app realm, so a reload can only
        // be performed between evaluation units — here (a reload requested
        // during app construction), after the -e block, after the script
        // file, and after each REPL line. Each reload swaps the primary
        // JSContext (initAppRealm re-installs the headless globals), so
        // re-fetch ctx after draining. Bounded: an app that unconditionally
        // reloads itself would otherwise never yield.
        auto drainAppReloads = [&]() {
            bool reloaded = false;
            for (int i = 0; i < 8 && engine->processPendingAppReload(); ++i) reloaded = true;
            ctx = rt->getContext();
            // A reload builds a fresh realm and re-installs the headless
            // globals into it; the process's arguments have not changed, so
            // the new realm gets them back too.
            if (reloaded) bro::js::installScriptArgs(ctx, scriptArgs);
        };
        drainAppReloads();

        bool ok = true;
        if (!inlineExprs.empty()) {
            // -e mode: concatenate and eval. When a script path also follows
            // (documented by fast_eval.js/headless_eval.js's own usage
            // comments as a way to set override globals before the script
            // runs — e.g. `-e "EVAL_MATCHES=30;" fast_eval.js`), only print
            // the final value when there's no script to follow; otherwise
            // this is just setting up globals for it.
            std::ostringstream oss;
            for (size_t i = 0; i < inlineExprs.size(); ++i) {
                if (i > 0) oss << ";\n";
                oss << inlineExprs[i];
            }
            ok = evalCode(ctx, rt, engine, oss.str(), "<inline>", scriptPath.empty());
            drainAppReloads();
        }
        if (ok && !scriptPath.empty()) {
            // Script file mode
            std::ifstream ifs(scriptPath);
            if (!ifs.is_open()) {
                fprintf(stderr, "Error: cannot open script: %s\n", scriptPath.c_str());
                ok = false;
            } else {
                std::ostringstream oss;
                oss << ifs.rdbuf();
                std::string src = oss.str();
                ok = looksLikeModule(src)
                         ? evalModuleFile(ctx, rt, engine, src, scriptPath.c_str())
                         : evalCode(ctx, rt, engine, src, scriptPath.c_str());
                drainAppReloads();
            }
        } else if (inlineExprs.empty() && scriptPath.empty()) {
            // Interactive REPL
            runRepl(ctx, rt, engine);
        }
        if (!ok) exitCode = 1;

        // Real teardown. This used to be an intentional leak ("to avoid a
        // QuickJS GC assertion on shutdown") plus a bare stopBackgroundServices()
        // call — but that call destroyed NetService before ~Engine()'s binding
        // cleanup dereferenced it, and the _exit() below hid the resulting
        // fault. The leak also meant ~Engine() was exercised by nothing except
        // a human closing the windowed app, so teardown bugs (and leaks the GC
        // assertion would have caught) accumulated unseen. Every test now exits
        // through here.
        //
        // _exit() below still skips the process's STATIC destruction phase,
        // which is a separate matter — see ~Engine()'s brotensor::shutdown()
        // comment for why that phase is hazardous.
        delete engine;
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: %s", e.what());
        exitCode = 1;
    }

#ifdef _WIN32
    if (getenv("BRO_THREAD_CENSUS")) printThreadCensus();
#endif
    // Last call before the CRT is skipped entirely: a host's end-of-run
    // reporting goes here or nowhere (HeadlessHooks::beforeExit).
    if (hooks.beforeExit) hooks.beforeExit();
    _exit(exitCode);
}
