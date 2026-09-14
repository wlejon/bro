#include "bronze_host/eval.h"
#include "bronze_host/eval_jit.h"
#include "bronze_host/host_gc.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/app_module.h"
#include "bronze_host/host_headless.h"
#include "bronze_host/host_pins.h"

#include "cli/driver.h"
#include "embed/embed.h"
#include "modules/modules.h"
#include "engine/engine.h"
#include "util/asset_mounts.h"
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

} // namespace

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

// Module roots for a compile that runs against `engine`'s app. Every engine
// mount (`/app`, `/lib`, `/system`, `/std`) becomes a root, so the compiler
// resolves `import "/lib/x.js"` exactly as the asset loader resolves
// `<script src="/lib/x.js">`.
std::vector<bronze::modules::ModuleRoot> moduleRootsFor(const engine::Engine& engine) {
    std::vector<bronze::modules::ModuleRoot> roots;
    for (const auto& [prefix, target] : engine.assetMounts().mounts()) {
        roots.push_back({prefix, std::filesystem::path(target)});
    }
    return roots;
}

// Where script text handed to evalScript lives, as far as its own `./x.js`
// imports are concerned: the document it came from, or for `-e` text with no
// document, a file in the app dir.
std::string entryResolvesAsFor(const engine::Engine& engine, const std::string& filename) {
    std::error_code ec;
    if (!filename.empty()) return std::filesystem::absolute(filename, ec).string();
    if (!engine.appDir().empty()) {
        return (std::filesystem::absolute(engine.appDir(), ec) / "eval.js").string();
    }
    return {};
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

namespace {

// The `--host-globals` manifest an in-process `bronze build` compiles
// against: the registry, written out. runBuild takes the manifest as a PATH
// (cli/driver.h says why), so the enumeration goes through a file in the
// eval temp dir beside the script it compiles, and is removed with it. The
// caller has installed the host globals before asking, or the list is empty
// and every free read of `document` compiles to a runtime miss.
std::filesystem::path writeHostGlobalsManifest(const std::filesystem::path& dir,
                                               const std::string& stem) {
    const auto path = dir / (stem + ".globals");
    std::ofstream ofs(path, std::ios::binary);
    for (const auto& name : registeredHostGlobals()) ofs << name << '\n';
    return path;
}

} // namespace

#ifdef _WIN32
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
static bool safeRunEntry(void (*entry)()) {
    __try {
        bronze::embed::runEntry(entry);
        return true;
    } __except (
        [](LPEXCEPTION_POINTERS ep) -> int {
            DWORD code = ep->ExceptionRecord->ExceptionCode;
            void* addr = ep->ExceptionRecord->ExceptionAddress;
            printf("CRASH in runEntry! code: 0x%08X at %p\n", (unsigned int)code, addr);
            if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
                printf("Access violation %s address %p\n",
                    ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "reading" : "writing",
                    (void*)ep->ExceptionRecord->ExceptionInformation[1]);
            }
            HANDLE proc = GetCurrentProcess();
            SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
            std::string exeDir = getExecutableDirectory().string();
            SymInitialize(proc, exeDir.c_str(), TRUE);
            void* frames[62];
            USHORT n = CaptureStackBackTrace(0, 62, frames, nullptr);
            char symBuf[sizeof(SYMBOL_INFO) + 512];
            printf("Backtrace (%u frames):\n", (unsigned)n);
            for (USHORT i = 0; i < n; ++i) {
                DWORD64 frameAddr = reinterpret_cast<DWORD64>(frames[i]);
                auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 511;
                DWORD64 disp = 0;
                const char* name = "?";
                if (SymFromAddr(proc, frameAddr, &disp, sym)) name = sym->Name;
                char modName[MAX_PATH] = "?";
                HMODULE mod = nullptr;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCSTR>(frameAddr), &mod) && mod) {
                    GetModuleFileNameA(mod, modName, MAX_PATH);
                    const char* base = strrchr(modName, '\\');
                    if (base) memmove(modName, base + 1, strlen(base));
                }
                DWORD64 rva = mod ? (frameAddr - reinterpret_cast<DWORD64>(mod)) : 0;
                IMAGEHLP_LINE64 line;
                ZeroMemory(&line, sizeof(line));
                line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
                DWORD lineDisp = 0;
                if (SymGetLineFromAddr64(proc, frameAddr, &lineDisp, &line)) {
                    printf("  #%02u %p %s!%s+0x%llx (%s:%lu)\n", (unsigned)i, frames[i], modName,
                           name, (unsigned long long)disp, line.FileName, (unsigned long)line.LineNumber);
                } else {
                    printf("  #%02u %p %s (rva 0x%llx)!%s+0x%llx\n", (unsigned)i, frames[i], modName,
                           (unsigned long long)rva, name, (unsigned long long)disp);
                }
            }
            fflush(stdout);
            return EXCEPTION_EXECUTE_HANDLER;
        }(GetExceptionInformation())
    ) {
        return false;
    }
}
#else
static bool safeRunEntry(void (*entry)()) {
    bronze::embed::runEntry(entry);
    return true;
}
#endif

bool evalScript(engine::Engine& engine, const std::string& code,
                const std::string& filename) {
    HostEvalScope evalScope;
    if (!isJitDisabled()) {
        return evalScriptJit(engine, code, filename);
    }

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

    // Globals BEFORE the compile, not just before the run: the manifest the
    // compile admits is read off the registry, so the registry must be full.
    if (!isWebHostGlobalsInstalled()) {
        installWebHostGlobals(engine);
    }
    const auto manifest = writeHostGlobalsManifest(tempDir, stem);
    const std::string globalsPath = manifest.string();
    const auto roots = moduleRootsFor(engine);
    const std::string resolvesAs = entryResolvesAsFor(engine, filename);
    const std::string pinsPath = discoverPinsPath(engine, filename);
    const std::string censusOutPath = discoverCensusOutPath(engine, filename);
    std::string err;
    int status = bronze::cli::runBuild(
        tempJs.string(), outDll.string(), &err,
        /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
        /*hostGlobals=*/globalsPath, /*inferStats=*/false,
        /*statsOut=*/nullptr, /*moduleRoots=*/roots, /*entrySymbol=*/{},
        /*emitShared=*/true, /*retainFnSource=*/true,
        /*importMapPath=*/{}, /*assumeNoBigInt=*/false,
        /*pinsPath=*/pinsPath, /*censusOutPath=*/censusOutPath,
        /*pinsAllowObserved=*/false, /*no native FFI:*/{}, {},
        /*entryResolvesAs=*/resolvesAs);

    std::error_code ec;
    std::filesystem::remove(tempJs, ec);

    if ((status != 0 && err.find("unsupported construct: `await` outside an async function body") != std::string::npos) ||
        (err.find("unresolved name 'await'") != std::string::npos)) {
        std::string wrappedCode = "(async () => {\n" + std::string(code) + "\n})().catch(err => { console.error(err && err.stack ? err.stack : err); if (typeof assert === 'function') assert(false, 'Unhandled error: ' + (err && err.message ? err.message : err)); });\n";
        std::filesystem::path tempJsWrap = tempDir / (stem + "_wrap.js");
        {
            std::ofstream ofs(tempJsWrap, std::ios::binary);
            if (ofs.is_open()) {
                ofs.write(wrappedCode.data(), wrappedCode.size());
            }
        }
        err.clear();
        status = bronze::cli::runBuild(
            tempJsWrap.string(), outDll.string(), &err,
            /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
            /*hostGlobals=*/globalsPath, /*inferStats=*/false,
            /*statsOut=*/nullptr, /*moduleRoots=*/roots, /*entrySymbol=*/{},
            /*emitShared=*/true, /*retainFnSource=*/true,
            /*importMapPath=*/{}, /*assumeNoBigInt=*/false,
            /*pinsPath=*/pinsPath, /*censusOutPath=*/censusOutPath,
            /*pinsAllowObserved=*/false, /*no native FFI:*/{}, {},
            /*entryResolvesAs=*/resolvesAs);
        std::filesystem::remove(tempJsWrap, ec);
    }
    std::filesystem::remove(manifest, ec);

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

    auto entry = reinterpret_cast<void (*)()>(moduleSymbol(handle, "bronze_main"));
    if (!entry) {
        LOG_ERROR("eval module exports no bronze_main: %s", outDll.string().c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    bool entryOk = safeRunEntry(entry);
    if (!entryOk) {
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }
    if (ev::microtasksPending()) {
        ev::drainMicrotasks();
    }
    std::fflush(stdout);

    if (hasTestFailure() || engine.hasTestFailure()) {
        return false;
    }
    return true;
}

bool evalScriptFile(engine::Engine& engine, const std::string& filePath) {
    HostEvalScope evalScope;
    if (!isJitDisabled()) {
        return evalScriptFileJit(engine, filePath);
    }

    ensureSharedRuntimeEnv();

    std::error_code ec;
    const std::filesystem::path resolvedPath = filePath;
    if (!std::filesystem::exists(resolvedPath, ec)) {
        LOG_ERROR("evalScriptFile: file does not exist: %s", filePath.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    const auto absSource = std::filesystem::absolute(resolvedPath, ec);
    const auto tempDir = getEvalTempDir();
    const uint64_t id = s_evalCounter.fetch_add(1, std::memory_order_relaxed);
    const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string stem = "script_" + std::to_string(ts) + "_" + std::to_string(id);
    const auto outDll = tempDir / (stem + kModuleExt);

    if (!isWebHostGlobalsInstalled()) {
        installWebHostGlobals(engine);
    }
    const auto manifest = writeHostGlobalsManifest(tempDir, stem);
    const std::string globalsPath = manifest.string();
    const auto roots = moduleRootsFor(engine);
    const std::string pinsPath = discoverPinsPath(engine, absSource);
    const std::string censusOutPath = discoverCensusOutPath(engine, absSource);
    std::string err;

    std::ifstream ifs(absSource, std::ios::binary);
    std::string content;
    if (ifs) {
        content.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }

    auto hasAwaitStmt = [](const std::string& code) {
        size_t i = 0;
        while (i < code.size()) {
            while (i < code.size() && (code[i] == ' ' || code[i] == '\t')) i++;
            if (i + 5 <= code.size() && code.compare(i, 5, "await") == 0) {
                char next = (i + 5 < code.size()) ? code[i + 5] : '\0';
                if (next == ' ' || next == '\t' || next == '(') {
                    return true;
                }
            }
            while (i < code.size() && code[i] != '\n') i++;
            if (i < code.size() && code[i] == '\n') i++;
        }
        return false;
    };

    std::filesystem::path wrapFile;
    std::string buildSrc = absSource.string();
    if (hasAwaitStmt(content)) {
        std::string wrapped = "(async () => {\n" + content + "\n})().catch(err => { console.error(err && err.stack ? err.stack : err); if (typeof assert === 'function') assert(false, 'Unhandled error: ' + (err && err.message ? err.message : err)); });\n";
        wrapFile = tempDir / (stem + "_wrap.js");
        std::ofstream ofs(wrapFile, std::ios::binary);
        ofs.write(wrapped.data(), wrapped.size());
        ofs.close();
        buildSrc = wrapFile.string();
    }

    int status = bronze::cli::runBuild(
        buildSrc, outDll.string(), &err,
        /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
        /*hostGlobals=*/globalsPath, /*inferStats=*/false,
        /*statsOut=*/nullptr, /*moduleRoots=*/roots, /*entrySymbol=*/{},
        /*emitShared=*/true, /*retainFnSource=*/true,
        /*importMapPath=*/{}, /*assumeNoBigInt=*/false,
        /*pinsPath=*/pinsPath, /*censusOutPath=*/censusOutPath,
        /*pinsAllowObserved=*/false, /*no native FFI:*/{}, {});

    if (!wrapFile.empty()) {
        std::filesystem::remove(wrapFile, ec);
    }

    if (status != 0 && err.find("unsupported construct: `await` outside an async function body") != std::string::npos) {
        std::string wrapped = "(async () => {\n" + content + "\n})().catch(err => { console.error(err && err.stack ? err.stack : err); if (typeof assert === 'function') assert(false, 'Unhandled error: ' + (err && err.message ? err.message : err)); });\n";
        auto wrappedFile = tempDir / (stem + "_wrap2.js");
        std::ofstream ofs(wrappedFile, std::ios::binary);
        ofs.write(wrapped.data(), wrapped.size());
        ofs.close();

        err.clear();
        status = bronze::cli::runBuild(
            wrappedFile.string(), outDll.string(), &err,
            /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
            /*hostGlobals=*/globalsPath, /*inferStats=*/false,
            /*statsOut=*/nullptr, /*moduleRoots=*/roots, /*entrySymbol=*/{},
            /*emitShared=*/true, /*retainFnSource=*/true,
            /*importMapPath=*/{}, /*assumeNoBigInt=*/false,
            /*pinsPath=*/pinsPath, /*censusOutPath=*/censusOutPath,
            /*pinsAllowObserved=*/false, /*no native FFI:*/{}, {});
        std::filesystem::remove(wrappedFile, ec);
    }
    std::filesystem::remove(manifest, ec);

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

    auto entry = reinterpret_cast<void (*)()>(moduleSymbol(handle, "bronze_main"));
    if (!entry) {
        LOG_ERROR("evalScriptFile module exports no bronze_main: %s", outDll.string().c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    if (!safeRunEntry(entry)) {
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }
    if (ev::microtasksPending()) {
        ev::drainMicrotasks();
    }
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
