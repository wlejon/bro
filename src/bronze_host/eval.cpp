#include "bronze_host/eval.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/app_module.h"
#include "bronze_host/host_headless.h"

#include "cli/driver.h"
#include "embed/embed.h"
#include "engine/engine.h"
#include "engine/engine_init_cabi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include "util/log.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

namespace {

static std::atomic<uint64_t> s_evalCounter{0};

#ifdef _WIN32
constexpr const char* kModuleExt = ".dll";
#elif defined(__APPLE__)
constexpr const char* kModuleExt = ".dylib";
#else
constexpr const char* kModuleExt = ".so";
#endif

std::filesystem::path getExecutableDirectory() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(std::string(buf, len)).parent_path();
    }
#elif defined(__APPLE__)
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        return std::filesystem::path(buf).parent_path();
    }
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return std::filesystem::path(buf).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

std::filesystem::path getEvalTempDir() {
    std::filesystem::path p = std::filesystem::temp_directory_path() / "bro_eval";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
}

void ensureSharedRuntimeEnv() {
    if (std::getenv("BRONZE_SHARED_RT_LIB")) return;
    std::error_code ec;

#ifdef _WIN32
    const char* const name = "bronze_runtime_shared.lib";
#elif defined(__APPLE__)
    const char* const name = "libbronze_runtime_shared.dylib";
#else
    const char* const name = "libbronze_runtime_shared.so";
#endif

    const auto exeDir = getExecutableDirectory();
    std::vector<std::filesystem::path> bases = {
        exeDir,
        exeDir / "shared",
        exeDir / "shared/Release",
        exeDir / "shared/Debug",
        exeDir.parent_path() / "shared",
        exeDir.parent_path() / "shared/Release",
        exeDir.parent_path() / "shared/Debug",
        std::filesystem::current_path(),
        std::filesystem::current_path() / "build/shared",
        std::filesystem::current_path() / "build/shared/Release",
        std::filesystem::current_path() / "build-release/shared",
    };
    if (const char* env = std::getenv("BRO_PROJECT_ROOT")) {
        bases.push_back(std::filesystem::path(env) / "build/shared");
        bases.push_back(std::filesystem::path(env) / "build/shared/Release");
        bases.push_back(std::filesystem::path(env) / "build-release/shared");
    }

    for (const auto& base : bases) {
        auto cand = base / name;
        if (std::filesystem::exists(cand, ec)) {
            std::string abs = std::filesystem::absolute(cand, ec).string();
#ifdef _WIN32
            _putenv_s("BRONZE_SHARED_RT_LIB", abs.c_str());
#else
            setenv("BRONZE_SHARED_RT_LIB", abs.c_str(), 1);
#endif
            break;
        }
    }
}

} // namespace

std::string getWebHostGlobalsPath() {
    std::error_code ec;
    if (const char* env = std::getenv("BRO_PROJECT_ROOT")) {
        auto p = std::filesystem::path(env) / "src/bronze_host/web_host.globals";
        if (std::filesystem::exists(p, ec)) return std::filesystem::absolute(p, ec).string();
    }
    const auto exeDir = getExecutableDirectory();
    for (const auto& base : {
        exeDir,
        exeDir.parent_path(),
        exeDir.parent_path().parent_path(),
        std::filesystem::current_path(),
        std::filesystem::current_path().parent_path(),
    }) {
        for (const auto& rel : {
            "src/bronze_host/web_host.globals",
            "web_host.globals",
            "bronze/web_host.globals",
        }) {
            auto p = base / rel;
            if (std::filesystem::exists(p, ec)) return std::filesystem::absolute(p, ec).string();
        }
    }

    return "src/bronze_host/web_host.globals";
}

bool evalScript(engine::Engine& engine, const std::string& code,
                const std::string& filename) {
    (void)filename;
    ensureSharedRuntimeEnv();

    const auto tempDir = getEvalTempDir();
    const uint64_t id = s_evalCounter.fetch_add(1, std::memory_order_relaxed);
    const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string stem = "eval_" + std::to_string(ts) + "_" + std::to_string(id);

    const auto tempJs = tempDir / (stem + ".js");
    const auto outDll = tempDir / (stem + kModuleExt);

    {
        std::ofstream ofs(tempJs, std::ios::binary);
        if (!ofs.is_open()) {
            LOG_ERROR("eval: failed to write temporary file: %s", tempJs.string().c_str());
            setTestFailure(true);
            engine.setTestFailure(true);
            return false;
        }
        ofs.write(code.data(), code.size());
    }

    const std::string globalsPath = getWebHostGlobalsPath();
    std::string err;
    int status = bronze::cli::runBuild(
        tempJs.string(), outDll.string(), &err,
        /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
        /*hostGlobals=*/globalsPath, /*inferStats=*/false,
        /*statsOut=*/nullptr, /*moduleRoots=*/{}, /*entrySymbol=*/{},
        /*emitShared=*/true, /*retainFnSource=*/true,
        /*importMapPath=*/{}, /*assumeNoBigInt=*/false,
        /*pinsPath=*/{}, /*censusOutPath=*/{},
        /*pinsAllowObserved=*/false);

    std::error_code ec;
    std::filesystem::remove(tempJs, ec);

    if (status != 0) {
        LOG_ERROR("eval compilation failed:\n%s", err.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    std::string loadErr;
    ModuleHandle handle = openModule(outDll.string(), loadErr);
    if (!handle) {
        LOG_ERROR("eval module load failed: %s", loadErr.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    bro::engine::bro_engine_register_cabi_bridges(&engine);
    bro_c_abi_sync_bridges_to_module(handle);

    auto entry = reinterpret_cast<void (*)()>(moduleSymbol(handle, "bronze_main"));
    if (!entry) {
        LOG_ERROR("eval module exports no bronze_main: %s", outDll.string().c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    if (!isWebHostGlobalsInstalled()) {
        installWebHostGlobals(engine);
    }

    bronze::embed::runEntry(entry);
    std::fflush(stdout);

    if (hasTestFailure() || engine.hasTestFailure()) {
        return false;
    }
    return true;
}

bool evalScriptFile(engine::Engine& engine, const std::string& filePath) {
    ensureSharedRuntimeEnv();

    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec)) {
        LOG_ERROR("evalScriptFile: file does not exist: %s", filePath.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    const auto absSource = std::filesystem::absolute(filePath, ec);
    const auto tempDir = getEvalTempDir();
    const uint64_t id = s_evalCounter.fetch_add(1, std::memory_order_relaxed);
    const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string stem = "script_" + std::to_string(ts) + "_" + std::to_string(id);
    const auto outDll = tempDir / (stem + kModuleExt);

    const std::string globalsPath = getWebHostGlobalsPath();
    std::string err;
    int status = bronze::cli::runBuild(
        absSource.string(), outDll.string(), &err,
        /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
        /*hostGlobals=*/globalsPath, /*inferStats=*/false,
        /*statsOut=*/nullptr, /*moduleRoots=*/{}, /*entrySymbol=*/{},
        /*emitShared=*/true, /*retainFnSource=*/true,
        /*importMapPath=*/{}, /*assumeNoBigInt=*/false,
        /*pinsPath=*/{}, /*censusOutPath=*/{},
        /*pinsAllowObserved=*/false);

    if (status != 0) {
        LOG_ERROR("evalScriptFile compilation failed for %s:\n%s", filePath.c_str(), err.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    std::string loadErr;
    ModuleHandle handle = openModule(outDll.string(), loadErr);
    if (!handle) {
        LOG_ERROR("evalScriptFile module load failed: %s", loadErr.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    bro::engine::bro_engine_register_cabi_bridges(&engine);
    bro_c_abi_sync_bridges_to_module(handle);

    auto entry = reinterpret_cast<void (*)()>(moduleSymbol(handle, "bronze_main"));
    if (!entry) {
        LOG_ERROR("evalScriptFile module exports no bronze_main: %s", outDll.string().c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    if (!isWebHostGlobalsInstalled()) {
        installWebHostGlobals(engine);
    }

    bronze::embed::runEntry(entry);
    std::fflush(stdout);

    if (hasTestFailure() || engine.hasTestFailure()) {
        return false;
    }
    return true;
}

namespace {

static engine::Engine* s_activeEngine = nullptr;

const char* sourcePrefixFor(ev::DynamicFunctionKind kind) {
    switch (kind) {
        case ev::DynamicFunctionKind::Generator: return "(function* anonymous(";
        case ev::DynamicFunctionKind::Async: return "(async function anonymous(";
        case ev::DynamicFunctionKind::AsyncGenerator: return "(async function* anonymous(";
        default: return "(function anonymous(";
    }
}

bronze::Value dynamicFunction(ev::DynamicFunctionKind kind, std::span<const bronze::Value> args) {
    if (!s_activeEngine) {
        return ev::throwError("new Function: no active engine available");
    }

    std::string params;
    std::string body;
    for (size_t i = 0; i < args.size(); ++i) {
        const bool last = (i + 1 == args.size());
        std::string text =
            ev::isUndefined(args[i]) ? std::string("undefined") : ev::toUtf8(args[i]);
        if (last) {
            body = std::move(text);
        } else {
            if (!params.empty()) params += ",";
            params += text;
        }
    }

    const uint64_t fnId = s_evalCounter.fetch_add(1, std::memory_order_relaxed);
    const std::string globalName = "__bro_dyn_fn_" + std::to_string(fnId);
    const std::string code = "globalThis." + globalName + " = " + sourcePrefixFor(kind) + params + ") {\n" + body + "\n};";

    if (!evalScript(*s_activeEngine, code, "<new Function>")) {
        return ev::throwError("new Function: dynamic compilation failed");
    }

    ev::GlobalValue g = ev::globalValue(globalName);
    if (!g.found) {
        return ev::throwError("new Function: created function was not found");
    }
    return g.value;
}

bronze::Value dynamicEval(bronze::Value source) {
    if (!ev::isString(source)) return source;
    if (!s_activeEngine) {
        return ev::throwError("eval: no active engine available");
    }
    const std::string code = ev::toUtf8(source);
    const uint64_t evalId = s_evalCounter.fetch_add(1, std::memory_order_relaxed);
    const std::string resName = "__bro_eval_res_" + std::to_string(evalId);

    // First try evaluating as an expression assigning to global:
    std::string exprCode = "globalThis." + resName + " = (" + code + ");";
    if (evalScript(*s_activeEngine, exprCode, "<eval>")) {
        ev::GlobalValue g = ev::globalValue(resName);
        if (g.found) return g.value;
    }

    // Fallback: evaluate as statements:
    std::string stmtCode = "globalThis." + resName + " = undefined;\n" + code + ";";
    if (evalScript(*s_activeEngine, stmtCode, "<eval>")) {
        ev::GlobalValue g = ev::globalValue(resName);
        if (g.found) return g.value;
        return ev::undefined();
    }
    return ev::throwError("eval: evaluation failed");
}

} // namespace

void installDynamicHooks(engine::Engine& engine) {
    s_activeEngine = &engine;
    ev::setDynamicFunctionHook(dynamicFunction);
    ev::setDynamicEvalHook(dynamicEval);
}

} // namespace bro::bronze_host
