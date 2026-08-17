// Finding, verifying and running the compiled module an app directory
// carries. app_module.h holds the reasoning; this file is the mechanism.

#include "bronze_host/app_module.h"

#include "bronze_host/bronze_host.h"

#include "util/log.h"

#include "embed/embed.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace bro::bronze_host {
namespace {

// The name a compiled app goes by inside its directory. Fixed rather than
// configurable: the point of the folder model is that `bro <folder>` needs to
// know nothing about the folder beyond where it is, and a name in bro.json
// would be one more thing that can disagree with what is on disk. The
// extension is the platform's own, so one folder can carry a module for each
// platform side by side and still be a single portable app directory.
#ifdef _WIN32
constexpr const char* kModuleName = "app.dll";
#elif defined(__APPLE__)
constexpr const char* kModuleName = "app.dylib";
#else
constexpr const char* kModuleName = "app.so";
#endif

// Symbols the module must export. bronze_abi.h's "loadable-module surface"
// names three, all derived from the module's entry symbol; bro compiles apps
// with the DEFAULT entry, whose stamp keeps the historical spelling
// `bronze_object_abi_fingerprint` rather than `bronze_main_abi_fingerprint`.
// The third, `<entry>_host_globals`, is the manifest the module was compiled
// against — read below only to report it, because bro registers its globals
// across a dozen call sites and has no single list to diff against yet.
constexpr const char* kFingerprintSymbol = "bronze_object_abi_fingerprint";
constexpr const char* kEntrySymbol = "bronze_main";
constexpr const char* kGlobalsSymbol = "bronze_main_host_globals";

using ModuleHandle = void*;

// --- The two platform primitives, and nothing else ------------------------

ModuleHandle openModule(const std::string& path, std::string& error) {
#ifdef _WIN32
    // The module sits beside its app's other files and may load DLLs from
    // there; LOAD_WITH_ALTERED_SEARCH_PATH makes the module's OWN directory
    // the first place the loader looks for its dependencies, which is what
    // lets an app ship a sidecar library without installing it system-wide.
    // It requires an absolute path — with a relative one the flag is ignored
    // and the failure is a confusing "module not found" naming a dependency
    // rather than the app.
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(path, ec);
    const std::wstring wide = (ec ? std::filesystem::path(path) : abs).wstring();
    HMODULE h = ::LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Windows error %lu",
                      static_cast<unsigned long>(::GetLastError()));
        error = buf;
        return nullptr;
    }
    return reinterpret_cast<ModuleHandle>(h);
#else
    // RTLD_LOCAL so the app's symbols do not join the global namespace and
    // shadow the host's: the runtime the module calls into must be the one
    // bro is already using, and a module that exported its own copy of a
    // runtime symbol could otherwise capture calls made by bro itself.
    void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* msg = ::dlerror();
        error = msg ? msg : "dlopen failed";
        return nullptr;
    }
    return h;
#endif
}

void* moduleSymbol(ModuleHandle handle, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(
        ::GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
    return ::dlsym(handle, name);
#endif
}

// The `<entry>_host_globals` manifest: a uint32 count, then that many
// NUL-terminated UTF-8 names back to back (bronze_abi.h, "the loadable-module
// surface"). A module compiled with no manifest still exports the symbol with
// count 0, so absence means "not a bronze module" and never "no globals".
//
// Reported, not yet enforced. Enforcing it means diffing against the names bro
// actually registered, and those go in from a dozen call sites in
// dom_globals.cpp and host_platform.cpp with no list to consult; recording
// them is a separate change. Until then this turns "the app died reading some
// global" into a log line naming what the app was promised.
std::string describeGlobals(ModuleHandle handle) {
    const auto* base = static_cast<const unsigned char*>(moduleSymbol(handle, kGlobalsSymbol));
    if (!base) return "none declared (module predates the manifest symbol)";

    uint32_t count = 0;
    std::memcpy(&count, base, sizeof(count));
    if (count == 0) return "none";

    std::string out;
    const char* cursor = reinterpret_cast<const char*>(base + sizeof(count));
    for (uint32_t i = 0; i < count; ++i) {
        if (i) out += ", ";
        out += cursor;
        cursor += std::strlen(cursor) + 1;
    }
    return out;
}

// --- Reporting ------------------------------------------------------------

// Every refusal path logs and returns the same sentence, so the log and the
// caller cannot describe the same failure two different ways.
AppModuleResult refuse(AppModuleStatus status, std::string detail) {
    LOG_ERROR("compiled app: %s", detail.c_str());
    return AppModuleResult{status, std::move(detail)};
}

}  // namespace

std::optional<std::string> findAppModule(const std::string& appDir) {
    if (appDir.empty()) return std::nullopt;

    std::error_code ec;
    std::filesystem::path candidate = std::filesystem::path(appDir) / kModuleName;
    // is_regular_file rather than exists: a DIRECTORY named app.dll would
    // otherwise be reported as a module and fail confusingly at load.
    if (!std::filesystem::is_regular_file(candidate, ec)) return std::nullopt;
    return candidate.string();
}

AppModuleResult runAppModule(engine::Engine& engine, const std::string& modulePath) {
    std::string error;
    ModuleHandle handle = openModule(modulePath, error);
    if (!handle) {
        return refuse(AppModuleStatus::Unloadable,
                      modulePath + " could not be loaded (" + error +
                          "). It is usually a missing sidecar library or a "
                          "module built for a different architecture.");
    }

    // The stamp FIRST, before the entry point is even looked up: the whole
    // point is that nothing from a module of unknown vintage runs, and a
    // module whose ABI disagrees is one whose `bronze_main` must not be
    // called even to fail.
    const auto* moduleAbi =
        static_cast<const uint32_t*>(moduleSymbol(handle, kFingerprintSymbol));
    if (!moduleAbi) {
        return refuse(AppModuleStatus::Unstamped,
                      modulePath + " exports no " + kFingerprintSymbol +
                          ", so it is not a bronze-compiled app. Build it with "
                          "the bronze CLI.");
    }

    // Asked of the runtime rather than read from BRONZE_ABI_FINGERPRINT: with
    // a shared runtime the macro is what BRO was compiled against, and the
    // library answering the module's calls is the one whose value has to
    // match. The two agree in a tree built together — but the whole purpose of
    // a stamp is the case where something has been swapped.
    const uint32_t kRuntimeAbi = bronze::embed::abiFingerprint();
    if (*moduleAbi != kRuntimeAbi) {
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "%s was compiled against bronze ABI %08x, but this bro "
                      "speaks %08x. Recompile the app with the bronze CLI built "
                      "from the same tree as this runtime.",
                      modulePath.c_str(), *moduleAbi, kRuntimeAbi);
        // Refused rather than fatal: bronze's own guard calls fatal() because
        // a LINKED object's mismatch means the process itself is malformed.
        // A LOADED module is data the folder supplied, and a bad app must not
        // take the runtime down with it — the page stays up, interpreted, and
        // says why.
        return refuse(AppModuleStatus::AbiMismatch, buf);
    }

    auto entry = reinterpret_cast<void (*)()>(moduleSymbol(handle, kEntrySymbol));
    if (!entry) {
        return refuse(AppModuleStatus::NoEntryPoint,
                      modulePath + " carries a bronze ABI stamp but exports no " +
                          kEntrySymbol + ". It was compiled as a library rather "
                          "than as an app entry point.");
    }

    // Read before the program runs, so a failure inside it has the list above
    // it in the log rather than below.
    const std::string globals = describeGlobals(handle);

    // Globals BEFORE the program: its top level reads them as it runs.
    installWebHostGlobals(engine);

    // The root frame around the top level and the microtask checkpoint after
    // it, which a program whose top level queued a job needs before it can be
    // called finished. Both belong to bronze — `runEntry` is `runMain` with
    // the entry passed in rather than linked — and calling it is not a style
    // choice: `ShadowStackFrame` and `rtDrainMicrotasks` are runtime-internal
    // C++, absent from the shared runtime's export list, so open-coding the
    // sequence does not link at all. The ABI check that opens runMain has
    // already happened above, against the module's exported stamp instead of a
    // linked constant.
    bronze::embed::runEntry(entry);

    LOG_INFO("compiled app: %s (bronze ABI %08x, host globals: %s)", modulePath.c_str(),
             kRuntimeAbi, globals.c_str());
    return AppModuleResult{AppModuleStatus::Ran, {}};
}

}  // namespace bro::bronze_host
