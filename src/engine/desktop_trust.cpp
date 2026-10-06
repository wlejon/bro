#include "engine/desktop_trust.h"
#include "util/exe_dir.h"
#include "util/log.h"
#include "util/platform.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace bro::engine {

namespace {

bool isSubpath(const fs::path& base, const fs::path& sub) {
    std::error_code ec;
    auto baseCanon = fs::weakly_canonical(base, ec);
    if (ec) return false;
    auto subCanon = fs::weakly_canonical(sub, ec);
    if (ec) return false;

    auto baseStr = baseCanon.string();
    auto subStr = subCanon.string();
    if (baseStr.empty() || subStr.empty()) return false;
    if (subStr == baseStr) return true;
    if (subStr.size() > baseStr.size() &&
        subStr.compare(0, baseStr.size(), baseStr) == 0 &&
        (subStr[baseStr.size()] == '/' || subStr[baseStr.size()] == '\\')) {
        return true;
    }
    return false;
}

} // namespace

bool isTrustedAppLocation(const std::string& appDir, const std::string& projectRoot) {
    if (appDir.empty()) return false;

    // 1. Explicit developer / test opt-in via environment variable
    const char* envTrusted = std::getenv("BRO_TRUSTED");
    if (envTrusted && (std::string(envTrusted) == "1" || std::string(envTrusted) == "true")) {
        return true;
    }

    fs::path appPath(appDir);
    std::error_code ec;
    if (!fs::exists(appPath, ec)) return false;

    // 2. Extra trusted directories via BRO_TRUSTED_APP_DIR (colon-separated on POSIX, semicolon on Windows)
    const char* envDirs = std::getenv("BRO_TRUSTED_APP_DIR");
    if (envDirs && *envDirs) {
        std::stringstream ss(envDirs);
        std::string item;
#ifdef _WIN32
        char delim = ';';
#else
        char delim = ':';
#endif
        while (std::getline(ss, item, delim)) {
            if (!item.empty() && isSubpath(fs::path(item), appPath)) {
                return true;
            }
        }
    }

    // 3. System and desktop directories relative to executable or resources
    std::vector<fs::path> trustedPrefixes = {
        fs::path(util::executableDir()) / "apps",
        fs::path(util::executableDir()) / "system",
        fs::path(util::resourceDir()) / "apps",
        fs::path(util::resourceDir()) / "system",
        fs::current_path() / "system",
        fs::current_path() / "apps",
    };

    // Standard OS install prefixes per platform
#ifdef _WIN32
    if (const char* pf = std::getenv("ProgramFiles")) {
        trustedPrefixes.emplace_back(fs::path(pf) / "bro" / "apps");
        trustedPrefixes.emplace_back(fs::path(pf) / "bro" / "system");
    }
    if (const char* pd = std::getenv("ProgramData")) {
        trustedPrefixes.emplace_back(fs::path(pd) / "bro" / "apps");
        trustedPrefixes.emplace_back(fs::path(pd) / "bro" / "system");
    }
    if (const char* la = std::getenv("LOCALAPPDATA")) {
        trustedPrefixes.emplace_back(fs::path(la) / "bro" / "system");
    }
#elif defined(__APPLE__)
    trustedPrefixes.emplace_back("/Library/Application Support/bro/apps");
    trustedPrefixes.emplace_back("/Library/Application Support/bro/system");
    trustedPrefixes.emplace_back("/Applications/bro.app/Contents/Resources/apps");
    trustedPrefixes.emplace_back("/usr/local/share/bro");
    trustedPrefixes.emplace_back("/opt/bro/apps");
#else
    trustedPrefixes.emplace_back("/usr/share/bro");
    trustedPrefixes.emplace_back("/usr/local/share/bro");
    trustedPrefixes.emplace_back("/opt/bro/apps");
#endif

    // 4. Project-relative trusted paths (system apps in project root)
    if (!projectRoot.empty()) {
        trustedPrefixes.push_back(fs::path(projectRoot) / "system");
        trustedPrefixes.push_back(fs::path(projectRoot) / "desktop");
        trustedPrefixes.push_back(fs::path(projectRoot) / "apps");
    }

    for (const auto& prefix : trustedPrefixes) {
        if (fs::exists(prefix, ec) && isSubpath(prefix, appPath)) {
            return true;
        }
    }

    return false;
}

DesktopTrustInfo evaluateDesktopTrust(const std::string& appDir,
                                      const std::string& projectRoot,
                                      bool requestedShell,
                                      const std::vector<std::string>& requestedPrivileges) {
    DesktopTrustInfo info;
    bool wantsPrivileges = requestedShell || !requestedPrivileges.empty();
    if (!wantsPrivileges) {
        return info;
    }

    info.isTrusted = isTrustedAppLocation(appDir, projectRoot);
    if (!info.isTrusted) {
        LOG_WARN("DesktopTrust: app at '%s' requested privileged shell access (%s) but is not installed in a trusted desktop location; access denied",
                 appDir.c_str(), requestedShell ? "shell: true" : "privileged: [...]");
        return info;
    }

    info.isShell = requestedShell;
    info.grantedPrivileges = requestedPrivileges;
    LOG_INFO("DesktopTrust: app at '%s' verified as trusted shell app", appDir.c_str());
    return info;
}

} // namespace bro::engine
