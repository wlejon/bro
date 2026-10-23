// Finding, verifying and running the compiled module an app directory
// carries. app_module.h holds the reasoning; this file is the mechanism.

#include "bronze_host/app_module.h"

#include "bronze_host/bronze_host.h"

#include "util/log.h"

#include "embed/embed.h"

#include "engine/engine.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <vector>

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
// against — verified against the live runtime before `bronze_main` runs.
constexpr const char* kFingerprintSymbol = "bronze_object_abi_fingerprint";
constexpr const char* kEntrySymbol = "bronze_main";
constexpr const char* kGlobalsSymbol = "bronze_main_host_globals";

}  // namespace

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
// count 0, so absence means "module predates the manifest symbol" and never
// "no globals".
//
// Enforced after `installWebHostGlobals` and before `entry()` runs: each
// required global name is checked against the runtime via
// `bronze::embed::globalValue(name).found` (which checks the builtin ladder,
// the host-global registry, and own properties of `globalThis`). Any missing
// names refuse the module before its top level executes.
bool verifyHostGlobals(ModuleHandle handle, std::string& errorOut, std::string* describedOut) {
    errorOut.clear();
    const auto* base = static_cast<const unsigned char*>(moduleSymbol(handle, kGlobalsSymbol));
    if (!base) {
        if (describedOut) *describedOut = "none declared (module predates the manifest symbol)";
        return true;
    }

    uint32_t count = 0;
    std::memcpy(&count, base, sizeof(count));
    if (count == 0) {
        if (describedOut) *describedOut = "none";
        return true;
    }

    std::string described;
    std::string missing;
    const char* cursor = reinterpret_cast<const char*>(base + sizeof(count));
    for (uint32_t i = 0; i < count; ++i) {
        const std::string_view name(cursor);
        if (describedOut) {
            if (i) described += ", ";
            described.append(name.data(), name.size());
        }
        if (name != "globalThis" && !bronze::embed::globalValue(name).found) {
            if (!missing.empty()) missing += ", ";
            missing.append(name.data(), name.size());
        }
        cursor += name.size() + 1;
    }
    if (describedOut) *describedOut = std::move(described);
    if (!missing.empty()) {
        errorOut = "host globals missing from runtime: " + missing;
        return false;
    }
    return true;
}

namespace {

bool readTextFile(const std::filesystem::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::string extractJsonStringField(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return {};
    size_t colon = json.find(':', pos + pat.size());
    if (colon == std::string::npos) return {};
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}

bool extractJsonStringArray(const std::string& json, const std::string& key,
                            std::vector<std::string>& out) {
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return false;
    size_t colon = json.find(':', pos + pat.size());
    if (colon == std::string::npos) return false;
    size_t openBr = json.find('[', colon + 1);
    if (openBr == std::string::npos) return false;
    size_t closeBr = json.find(']', openBr + 1);
    if (closeBr == std::string::npos) return false;

    size_t cur = openBr + 1;
    while (cur < closeBr) {
        size_t q1 = json.find('"', cur);
        if (q1 == std::string::npos || q1 >= closeBr) break;
        size_t q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 >= closeBr) break;
        out.push_back(json.substr(q1 + 1, q2 - q1 - 1));
        cur = q2 + 1;
    }
    return true;
}

bool jsonHasLiteralField(const std::string& json, const std::string& key, const std::string& lit) {
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return false;
    size_t colon = json.find(':', pos + pat.size());
    if (colon == std::string::npos) return false;
    size_t cur = colon + 1;
    while (cur < json.size() && (json[cur] == ' ' || json[cur] == '\t' || json[cur] == '\r' || json[cur] == '\n')) {
        ++cur;
    }
    return json.compare(cur, lit.size(), lit) == 0;
}

bool isPrivilegedHostGlobal(std::string_view cap) {
    return cap == "child_process" || cap == "fs" || cap == "process" ||
           cap == "require" || cap == "eval" || cap == "Function" ||
           cap == "bro.sys" || cap == "bro.cred" || cap == "bro.seat" ||
           cap == "bro.portal" || cap == "bro.displays" ||
           cap == "bro.compositor" || cap == "bro.wl" ||
           cap == "bro.fs" || cap == "bro.process" ||
           cap.rfind("__bro_native", 0) == 0;
}

bool isCapabilityAllowed(const std::string& cap, const std::set<std::string>& allowedCaps) {
    if (allowedCaps.count(cap) != 0) return true;
    for (const auto& ac : allowedCaps) {
        if (cap.size() > ac.size() && cap.compare(0, ac.size(), ac) == 0 && cap[ac.size()] == '.') {
            return true;
        }
    }
    return false;
}

bool shouldRequireAppletCertificate(const std::filesystem::path& appDir,
                                    const std::filesystem::path& moduleFile) {
    if (const char* env = std::getenv("BRO_REQUIRE_VERIFIED_APPLET"); env && std::strcmp(env, "1") == 0) {
        return true;
    }
    std::error_code ec;
    if (std::filesystem::is_regular_file(appDir / "app.cert.json", ec) ||
        std::filesystem::is_regular_file(appDir / (moduleFile.stem().string() + ".cert.json"), ec)) {
        return true;
    }
    std::string broJson;
    if (readTextFile(appDir / "bro.json", broJson) &&
        jsonHasLiteralField(broJson, "verifiedApplet", "true")) {
        return true;
    }
    return false;
}

}  // namespace

std::string computeModuleFileHash(const std::string& filePath, bool* okOut) {
    std::string bytes;
    if (!readTextFile(filePath, bytes)) {
        if (okOut) *okOut = false;
        return {};
    }
    if (okOut) *okOut = true;
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < bytes.size(); ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= 0x100000001b3ULL;
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return oss.str();
}

bool verifyAppletCertificate(const std::string& modulePath, ModuleHandle handle,
                             std::string& errorOut) {
    errorOut.clear();
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path modPath(modulePath);
    fs::path appDir = modPath.parent_path();
    if (appDir.empty()) appDir = ".";

    fs::path certPath = appDir / "app.cert.json";
    if (!fs::is_regular_file(certPath, ec)) {
        fs::path stemCert = appDir / (modPath.stem().string() + ".cert.json");
        if (fs::is_regular_file(stemCert, ec)) certPath = stemCert;
    }

    std::string certJson;
    if (!readTextFile(certPath, certJson)) {
        errorOut = "missing applet verification certificate (" + certPath.string() + ")";
        return false;
    }

    if (!jsonHasLiteralField(certJson, "verified", "true")) {
        errorOut = "applet certificate '" + certPath.string() + "' does not have \"verified\": true";
        return false;
    }
    if (!jsonHasLiteralField(certJson, "hazards", "0")) {
        errorOut = "applet certificate '" + certPath.string() + "' reports non-zero static hazards";
        return false;
    }
    if (!jsonHasLiteralField(certJson, "proofsValid", "true")) {
        errorOut = "applet certificate '" + certPath.string() + "' reports invalid IL proofs";
        return false;
    }

    std::string expectedHash = extractJsonStringField(certJson, "moduleHash");
    bool hashOk = false;
    std::string actualHash = computeModuleFileHash(modulePath, &hashOk);
    if (!hashOk || expectedHash.empty() || expectedHash != actualHash) {
        errorOut = "applet certificate hash mismatch for " + modulePath +
                   " (cert has '" + expectedHash + "', actual is '" + actualHash + "')";
        return false;
    }

    std::vector<std::string> certCaps;
    extractJsonStringArray(certJson, "capabilities", certCaps);
    std::set<std::string> certCapSet(certCaps.begin(), certCaps.end());

    // Enforce bro.json capability manifest if present
    bool hasManifestCaps = false;
    std::set<std::string> allowedCaps;
    std::string broJson;
    if (readTextFile(appDir / "bro.json", broJson)) {
        std::vector<std::string> caps;
        if (extractJsonStringArray(broJson, "capabilities", caps)) {
            hasManifestCaps = true;
            allowedCaps.insert(caps.begin(), caps.end());
        }
        std::vector<std::string> perms;
        if (extractJsonStringArray(broJson, "permissions", perms)) {
            hasManifestCaps = true;
            allowedCaps.insert(perms.begin(), perms.end());
        }
    }

    for (const auto& cap : certCaps) {
        if (!isCapabilityAllowed(cap, allowedCaps) &&
            (isPrivilegedHostGlobal(cap) || hasManifestCaps)) {
            errorOut = "applet certificate requests unauthorized capability '" + cap + "'";
            return false;
        }
    }

    // Also inspect the compiled module's exported `bronze_main_host_globals` symbol
    if (handle) {
        const auto* base = static_cast<const unsigned char*>(moduleSymbol(handle, kGlobalsSymbol));
        if (base) {
            uint32_t count = 0;
            std::memcpy(&count, base, sizeof(count));
            const char* cursor = reinterpret_cast<const char*>(base + sizeof(count));
            for (uint32_t i = 0; i < count; ++i) {
                std::string gname(cursor);
                cursor += gname.size() + 1;
                if (isPrivilegedHostGlobal(gname) &&
                    (!isCapabilityAllowed(gname, certCapSet) ||
                     (!isCapabilityAllowed(gname, allowedCaps) && hasManifestCaps))) {
                    errorOut = "compiled module imports unauthorized host global '" + gname + "'";
                    return false;
                }
            }
        }
    }

    return true;
}

namespace {

// --- Reporting ------------------------------------------------------------

// Every refusal path logs and returns the same sentence, so the log and the
// caller cannot describe the same failure two different ways.
AppModuleResult refuse(engine::Engine* engine, AppModuleStatus status, std::string detail) {
    LOG_ERROR("compiled app: %s", detail.c_str());
    if (engine) engine->dispatchDocumentReadyEvents();
    return AppModuleResult{status, std::move(detail), 0};
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
        return refuse(&engine, AppModuleStatus::Unloadable,
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
        return refuse(&engine, AppModuleStatus::Unstamped,
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
        // take the runtime down with it — the page stays up, running its scripts,
        // and says why.
        return refuse(&engine, AppModuleStatus::AbiMismatch, buf);
    }

    auto entry = reinterpret_cast<void (*)()>(moduleSymbol(handle, kEntrySymbol));
    if (!entry) {
        return refuse(&engine, AppModuleStatus::NoEntryPoint,
                      modulePath + " carries a bronze ABI stamp but exports no " +
                          kEntrySymbol + ". It was compiled as a library rather "
                          "than as an app entry point.");
    }

    // eBPF-style verified applet gate: when `app.cert.json` is present,
    // `bro.json` sets `"verifiedApplet": true`, or `BRO_REQUIRE_VERIFIED_APPLET=1`,
    // verify the proof certificate and capability manifest BEFORE any host
    // globals are installed or `bronze_main` executes.
    {
        std::filesystem::path modFile(modulePath);
        std::filesystem::path appDir = modFile.parent_path();
        if (appDir.empty()) appDir = ".";
        if (shouldRequireAppletCertificate(appDir, modFile)) {
            std::string certError;
            if (!verifyAppletCertificate(modulePath, handle, certError)) {
                return refuse(&engine, AppModuleStatus::UntrustedApplet,
                              modulePath + ": " + certError);
            }
        }
    }

    // Globals BEFORE the manifest check and the program: its top level reads
    // them as it runs, and verifyHostGlobals checks what was actually installed.
    installWebHostGlobals(engine);

    std::string globals;
    std::string globalsError;
    if (!verifyHostGlobals(handle, globalsError, &globals)) {
        return refuse(&engine, AppModuleStatus::MissingHostGlobals,
                      modulePath + ": " + globalsError);
    }

    // The root frame around the top level and the microtask checkpoint after
    // it, which a program whose top level queued a job needs before it can be
    // called finished. Both belong to bronze — `runEntry` is `runMain` with
    // the entry passed in rather than linked — and calling it is not a style
    // choice: `ShadowStackFrame` and `rtDrainMicrotasks` are runtime-internal
    // C++, absent from the shared runtime's export list, so open-coding the
    // sequence does not link at all. The ABI check that opens runMain has
    // already happened above, against the module's exported stamp instead of a
    // linked constant.
    // Closed right after the entry: left current, the handle would tag every
    // span a LATER program registers on this thread — a headless driver
    // script's, say — and reloading the app would unroot that program while it
    // is still on the stack driving the reload.
    const bronze::embed::ModuleHandle bronzeHandle = bronze::embed::beginModuleLoad();
    bronze::embed::runEntry(entry);
    bronze::embed::endModuleLoad(bronzeHandle);
    std::fflush(stdout);

    engine.addAppModuleHandle(bronzeHandle);
    engine.dispatchDocumentReadyEvents();

    LOG_INFO("compiled app: %s (bronze ABI %08x, host globals: %s)", modulePath.c_str(),
             kRuntimeAbi, globals.c_str());
    return AppModuleResult{AppModuleStatus::Ran, {}, bronzeHandle};
}

void unloadAppModule(bronze::embed::ModuleHandle handle) {
    if (handle != 0) {
        bronze::embed::unloadModule(handle);
    }
}

void clearAppModuleRegistry() { bronze::embed::clearModuleRegistry(); }

}  // namespace bro::bronze_host
