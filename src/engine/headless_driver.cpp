#include "engine/headless_driver.h"

#include "engine/engine.h"
#include "engine/config_loader.h"

#include "bronze_host/bronze_host.h"
#include "bronze_host/eval.h"
#include "bronze_host/host_headless.h"

using bro::engine::parseConfig;
using bro::engine::findAncestorProjectRoot;
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

namespace {

static std::string exeDir() {
    std::string path;
#ifdef _WIN32
    char buf[260];
    DWORD len = GetModuleFileNameA(nullptr, buf, 260);
    if (len > 0 && len < 260) path = std::string(buf, len);
#elif defined(__APPLE__)
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) path = buf;
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) { buf[len] = '\0'; path = buf; }
#endif
    auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos) return path.substr(0, slash);
    return ".";
}

static std::string absolutize(const std::string& path) {
    if (path.empty()) return path;
#ifdef _WIN32
    char full[MAX_PATH];
    if (GetFullPathNameA(path.c_str(), MAX_PATH, full, nullptr)) {
        for (char* p = full; *p; ++p) if (*p == '\\') *p = '/';
        return full;
    }
#else
    char full[PATH_MAX];
    if (realpath(path.c_str(), full)) return full;
#endif
    return path;
}

} // namespace

namespace bro::engine {

int runHeadless(int argc, char* argv[], const HeadlessHooks& hooks) {
#ifdef _WIN32
    // Suppress the WER "bro-headless.exe has stopped working" dialog that
    // Windows shows after an unhandled crash/abort() — headless is driven
    // by scripts/CI that need the process to just die with a nonzero exit,
    // not block on a click. (Lived in src/headless/main.cpp until the
    // QuickJS removal dropped it; the driver is the one entry every
    // headless host shares, so it belongs here.)
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

    int width = 1920, height = 1080;
    float deviceScale = 1.0f;
    bool useGPU = true;
    bool realAudio = false;
    int cliSplash = -1;
    std::string appDir;
    std::string scriptPath;
    std::vector<std::string> inlineExprs;
    std::vector<std::string> scriptArgs;
    bool printHostGlobals = false;
    std::string nativeManifestOut;

    bool passThrough = false;
    for (int i = 1; i < argc; ++i) {
        if (passThrough) {
            scriptArgs.push_back(argv[i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "%s — %s\n\n"
                "Usage: %s [options] <app-directory> [script.js] [-- args...]\n\n"
                "Options:\n"
                "  --width N       Viewport width  (default: 1920)\n"
                "  --height N      Viewport height (default: 1080)\n"
                "  --device-scale-factor S\n"
                "                  Render at S device px per CSS px, as a HiDPI display\n"
                "                  would (devicePixelRatio, screenshots S times the\n"
                "                  viewport; default: 1)\n"
                "  --cpu           Use software renderer\n"
                "  --no-gpu        Use software renderer\n"
                "  --real-audio    Enable real audio output\n"
                "  --audio         Enable real audio output\n"
                "  --splash        Show splash screen during load\n"
                "  --no-splash     Skip splash screen\n"
                "  -e <expr>       Evaluate JavaScript expression\n"
                "  --print-host-globals\n"
                "                  Print the host globals this binary registers, one per\n"
                "                  line, and exit: the --host-globals manifest for\n"
                "                  `bronze build` of an app that will run on it\n"
                "  --print-native-manifest <path>\n"
                "                  Write the native manifest this binary registers (the\n"
                "                  __bro_native.* entry points and signatures) to <path>\n"
                "                  and exit: the --native-manifest for the same build.\n"
                "                  Combines with --print-host-globals in one run\n",
                hooks.programName.c_str(), hooks.tagline.c_str(), hooks.programName.c_str());
            return 0;
        } else if (strcmp(argv[i], "--") == 0) {
            passThrough = true;
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--device-scale-factor") == 0 && i + 1 < argc) {
            deviceScale = static_cast<float>(atof(argv[++i]));
            if (!(deviceScale > 0.0f)) deviceScale = 1.0f;
        } else if (strcmp(argv[i], "--cpu") == 0 || strcmp(argv[i], "--no-gpu") == 0) {
            useGPU = false;
        } else if (strcmp(argv[i], "--real-audio") == 0 || strcmp(argv[i], "--audio") == 0) {
            realAudio = true;
        } else if (strcmp(argv[i], "--splash") == 0) {
            cliSplash = 1;
        } else if (strcmp(argv[i], "--no-splash") == 0) {
            cliSplash = 0;
        } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            inlineExprs.push_back(argv[++i]);
        } else if (strcmp(argv[i], "--print-host-globals") == 0) {
            printHostGlobals = true;
        } else if (strcmp(argv[i], "--print-native-manifest") == 0 && i + 1 < argc) {
            nativeManifestOut = argv[++i];
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

    {
        auto fileExists = [](const std::string& p) {
            std::ifstream f(p);
            return f.good();
        };
        if (!fileExists(appDir) && !fileExists(appDir + "/bro.json") && !fileExists(appDir + "/index.html")) {
            std::string candidate = "src/bronze_host/" + appDir;
            if (fileExists(candidate) || fileExists(candidate + "/bro.json") || fileExists(candidate + "/index.html")) {
                appDir = candidate;
            }
        }
    }

    bro::bronze_host::setScriptArgs(scriptArgs);
    bro::bronze_host::clearTestFailure();

    int exitCode = 0;

    try {
        bro::engine::EngineConfig config;
        std::string settingsDir = exeDir();
        config.settingsPath = settingsDir + "/.bro_settings.json";

#ifdef _WIN32
        _putenv_s("BRO_EXE_DIR", settingsDir.c_str());
#else
        setenv("BRO_EXE_DIR", settingsDir.c_str(), 1);
#endif

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

            if (isJsonFile(appDir) && std::ifstream(appDir).good()) {
                std::string targetDir = dirOf(appDir);
                bool isProject = false;
                parseConfig(appDir, config, &isProject);
                if (isProject) {
                    config.projectRoot = targetDir;
                    if (config.appDir.empty()) config.appDir = targetDir;
                    else if (!isAbsolute(config.appDir)) config.appDir = targetDir + "/" + config.appDir;
                } else {
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

            if (config.projectRoot.empty() && !config.appDir.empty()) {
                config.projectRoot = findAncestorProjectRoot(config.appDir);
            }
        }

        config.appDir = absolutize(config.appDir);
        if (!config.projectRoot.empty()) config.projectRoot = absolutize(config.projectRoot);
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
        config.deviceScaleFactor = deviceScale;
        config.showSplash = (cliSplash == 1);
        config.hostProvidesCompiledApp =
            hooks.providesCompiledApp && hooks.providesCompiledApp(config.appDir);
        config.installHostBindings = hooks.installHostBindings;

        if (hooks.beforeEngine) hooks.beforeEngine();

        auto* engine = new bro::engine::Engine(config);
        engine->run();

        // The compile-time half of the host-globals contract, read off the
        // run-time half: install exactly what an app would find registered
        // and print the registry. Before afterEngine, so a module already in
        // the app dir is neither loaded nor run — the list is a property of
        // this BINARY, and the app dir is only what the Engine needs to
        // exist. The engine log is on stderr, so stdout is the manifest and
        // nothing else.
        //
        // The native manifest is the other half of the same contract — the
        // `__bro_native.*` entry points registered by the same install —
        // written to a file rather than stdout (it is JSON, and the two
        // manifests are asked for together by tests/bronze_host/lib.sh).
        if (printHostGlobals || !nativeManifestOut.empty()) {
            if (!bro::bronze_host::isWebHostGlobalsInstalled()) {
                bro::bronze_host::installWebHostGlobals(*engine);
            }
            int status = 0;
            if (!nativeManifestOut.empty()) {
                std::string err;
                if (!bro::bronze_host::writeNativeManifest(nativeManifestOut, &err)) {
                    fprintf(stderr, "--print-native-manifest: %s\n", err.c_str());
                    status = 1;
                }
            }
            if (printHostGlobals) {
                for (const auto& name : bro::bronze_host::registeredHostGlobals()) {
                    fputs(name.c_str(), stdout);
                    fputc('\n', stdout);
                }
                fflush(stdout);
            }
            delete engine;
            if (hooks.beforeExit) hooks.beforeExit();
            _exit(status);
        }

        if (hooks.afterEngine) hooks.afterEngine(*engine);

        auto drainAppReloads = [&]() {
            for (int i = 0; i < 8 && engine->processPendingAppReload(); ++i) {}
        };
        drainAppReloads();

        if (bro::bronze_host::hasTestFailure() || engine->hasTestFailure()) {
            exitCode = 1;
        }

        if (!inlineExprs.empty()) {
            std::ostringstream oss;
            for (size_t i = 0; i < inlineExprs.size(); ++i) {
                if (i > 0) oss << ";\n";
                oss << inlineExprs[i];
            }
            std::string err = engine->eval(oss.str());
            drainAppReloads();
            if (!err.empty() || bro::bronze_host::hasTestFailure() || engine->hasTestFailure()) {
                exitCode = 1;
            }
        }

        if (!scriptPath.empty()) {
            bool ok = bro::bronze_host::evalScriptFile(*engine, scriptPath);
            drainAppReloads();
            if (!ok || bro::bronze_host::hasTestFailure() || engine->hasTestFailure()) {
                exitCode = 1;
            }
        }

        delete engine;
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: %s", e.what());
        exitCode = 1;
    }

    if (hooks.beforeExit) hooks.beforeExit();
    _exit(exitCode);
}

} // namespace bro::engine
