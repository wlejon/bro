#include "engine/launcher.h"

#include "engine/app_manifest.h"
#include "engine/app_permissions.h"
#include "engine/app_runtime.h"
#include "engine/config_loader.h"
#include "util/exe_dir.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#endif

namespace bro::engine {

namespace {

namespace fs = std::filesystem;

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string u8str(const fs::path& p) {
    std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

std::string getEnv(const char* name) {
#ifdef _WIN32
    std::wstring wname(name, name + std::char_traits<char>::length(name));
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring buf(n, L'\0');
    n = GetEnvironmentVariableW(wname.c_str(), buf.data(), n);
    buf.resize(n);
    return u8str(fs::path(buf));
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

// Ensures the user-data directory that system/projects stores projects.json in
// exists before the script runs: on Windows and macOS that folder also holds
// bro's grant store (grants.json), and checkFs refuses a script-side mkdirSync
// on an ancestor of a protected file.
void ensureProjectsUserDir() {
    fs::path dir;
#if defined(_WIN32)
    std::string base = getEnv("APPDATA");
    if (base.empty()) {
        std::string home = getEnv("USERPROFILE");
        if (!home.empty()) base = u8str(u8path(home) / "AppData" / "Roaming");
    }
    if (!base.empty()) dir = u8path(base) / "bro";
#elif defined(__APPLE__)
    std::string home = getEnv("HOME");
    if (!home.empty()) dir = u8path(home) / "Library" / "Application Support" / "bro";
#else
    std::string base = getEnv("XDG_DATA_HOME");
    if (base.empty() || !u8path(base).is_absolute()) {
        std::string home = getEnv("HOME");
        if (!home.empty()) base = u8str(u8path(home) / ".local" / "share");
    }
    if (!base.empty()) dir = u8path(base) / "bro";
#endif
    if (!dir.empty()) {
        std::error_code ec;
        fs::create_directories(dir, ec);
    }
}

bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

bool isJsonFile(const std::string& s) {
    return s.size() >= 5 && s.substr(s.size() - 5) == ".json";
}

std::string dirOf(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return (i == std::string::npos) ? std::string(".") : p.substr(0, i);
}

bool isAbsolute(const std::string& p) {
    return !p.empty() && (p[0] == '/' || p[0] == '\\' ||
                          (p.size() >= 2 && p[1] == ':'));
}

void setEnvVar(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
#endif
}

// Apply a bro.json to `config`, sorting out whether it is a project manifest
// (has default_app/lib/system) or a plain app manifest. `dir` is the
// directory the manifest lives in.
void applyManifest(const std::string& manifestPath, const std::string& dir,
                   EngineConfig& config) {
    bool isProject = false;
    parseConfig(manifestPath, config, &isProject);
    if (isProject) {
        config.projectRoot = dir;
        // A project manifest's `app`/`default_app` is relative to the project.
        if (config.appDir.empty()) config.appDir = dir;
        else if (!isAbsolute(config.appDir)) config.appDir = dir + "/" + config.appDir;
    } else {
        // A plain app manifest: its own directory is the app, whatever the
        // manifest's `"app": "."` says.
        config.appDir = dir;
    }
}

} // namespace

std::string absolutePath(const std::string& path) {
    if (path.empty()) return path;
#ifdef _WIN32
    char abs[MAX_PATH];
    return _fullpath(abs, path.c_str(), MAX_PATH) ? std::string(abs) : path;
#else
    char abs[PATH_MAX];
    return realpath(path.c_str(), abs) ? std::string(abs) : path;
#endif
}

bool resolveLaunchTarget(const std::string& target, EngineConfig& config) {
    std::string actualTarget = target;
    if (!actualTarget.empty() && !fileExists(actualTarget) &&
        !fileExists(actualTarget + "/bro.json") && !fileExists(actualTarget + "/index.html")) {
        std::string candidate = "src/bronze_host/" + actualTarget;
        if (fileExists(candidate) || fileExists(candidate + "/bro.json") ||
            fileExists(candidate + "/index.html")) {
            actualTarget = candidate;
        } else if (isValidAppId(actualTarget)) {
            // Not a path: an installed app's id (`bro org.example.Notes`),
            // looked up in the install roots (docs/apps.md).
            std::string installed = findInstalledApp(actualTarget);
            if (!installed.empty()) actualTarget = installed;
        }
    }
    if (!actualTarget.empty()) {
        if (isJsonFile(actualTarget) && fileExists(actualTarget)) {
            applyManifest(actualTarget, dirOf(actualTarget), config);
        } else {
            // A directory. Do NOT preset config.appDir first: config_loader
            // skips default_app when appDir is already set.
            std::string broJson = actualTarget + "/bro.json";
            if (fileExists(broJson)) {
                applyManifest(broJson, actualTarget, config);
            } else if (fileExists(actualTarget + "/index.html")) {
                config.appDir = actualTarget;   // bare directory of HTML
            } else {
                // Neither manifest nor index.html: this is not an app. Say so
                // rather than accepting it and failing later inside the
                // Engine — callers try candidate directories in order (a host
                // application looking for its bundled UI does exactly that),
                // and that only works if a miss actually reports a miss.
                return false;
            }
        }
    } else {
        // No argument: probe the shipped data directory — beside the
        // executable, or a macOS bundle's Contents/Resources — which is how a
        // packaged app and the bundled project manager both launch.
        std::string dir = util::resourceDir();
#ifndef _WIN32
        if (!dir.empty() && dir != ".") { if (chdir(dir.c_str()) != 0) { /* keep cwd */ } }
#endif
        std::string configPath = dir + "/bro.json";
        if (fileExists(configPath)) {
            bool isProject = false;
            parseConfig(configPath, config, &isProject);
            if (isProject) config.projectRoot = dir;
            if (config.appDir.empty()) config.appDir = dir;
            else if (!isAbsolute(config.appDir)) config.appDir = dir + "/" + config.appDir;
        } else if (fileExists(dir + "/index.html")) {
            config.appDir = dir;
        } else if (fileExists(dir + "/system/projects/index.html")) {
            config.appDir = dir + "/system/projects";
        } else {
            return false;
        }
    }

    // The project manager (system/projects) is bro's own, not an app the user
    // installed: whether reached with no argument or named directly, bro as its
    // host grants what it is for (reading and writing the user's project
    // folders and its registry, and launching projects) and ensures the
    // registry's parent directory exists.
    if (!config.appDir.empty()) {
        const std::string builtInProjects = util::resourceDir() + "/system/projects";
        if (fileExists(builtInProjects + "/index.html") &&
            canonicalPath(config.appDir) == canonicalPath(builtInProjects)) {
            for (const char* cap : {"files", "spawn"}) {
                if (std::find(config.hostGrants.begin(), config.hostGrants.end(), cap) ==
                    config.hostGrants.end()) {
                    config.hostGrants.emplace_back(cap);
                }
            }
            ensureProjectsUserDir();
        }
    }

    // The app was launched by pointing at its own directory and its bro.json
    // carries no project keys. Walk up for an ancestor project manifest so
    // /lib, /system, /std still resolve; failing that, inherit the project
    // of a parent bro process (a launcher spawning an app inside it).
    const bool launchedProject = !config.projectRoot.empty();
    if (config.projectRoot.empty() && !config.appDir.empty()) {
        config.projectRoot = findAncestorProjectRoot(config.appDir);
    }
    if (config.projectRoot.empty()) config.projectRoot = inheritedProjectRoot(config.appDir);
    // A project found either way names its own lib/system directories.
    if (!launchedProject) applyProjectDirNames(config.projectRoot, config);

    // When launched via a PROJECT manifest, the app's own bro.json still
    // supplies per-app overrides (title, size). Parse it after the project so
    // app keys win, preserving the resolved appDir against its `"app": "."`.
    if (!config.projectRoot.empty() && !config.appDir.empty()) {
        std::string appBroJson = config.appDir + "/bro.json";
        if (fileExists(appBroJson) && appBroJson != config.projectRoot + "/bro.json") {
            std::string preserved = config.appDir;
            parseConfig(appBroJson, config, nullptr);
            config.appDir = preserved;
        }
    }

    return !config.appDir.empty();
}

void publishLaunchEnv(EngineConfig& config) {
    config.appDir = absolutePath(config.appDir);
    if (!config.projectRoot.empty())
        config.projectRoot = absolutePath(config.projectRoot);

    // Apps like the launcher locate sibling executables through this.
    setEnvVar("BRO_EXE_DIR", util::executableDir());
    setEnvVar("BRO_APP_DIR", config.appDir);
    setEnvVar("BRO_PROJECT_ROOT", config.projectRoot);

    // The app's id and launch context (bro.app, BRO_APP_ID).
    finalizeAppIdentity(config);
}

} // namespace bro::engine
