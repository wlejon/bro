#include "engine/app_runtime.h"

#include "platform/desktop_single_instance.h"
#include "util/exe_dir.h"
#include "util/log.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace bro::engine {

namespace {

// Process start, as near as a library can see it: static initialisation runs
// before main().
const std::chrono::steady_clock::time_point kStart = std::chrono::steady_clock::now();
std::atomic<double> gFirstFrameMs{-1.0};
std::atomic<double> gLoadedMs{-1.0};

double sinceStartMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - kStart).count();
}

AppRuntimeInfo& info() {
    static AppRuntimeInfo s;
    return s;
}

// The hand-off message: a tag (so a stray client cannot pass for a launch),
// the launch's working directory, then its argv.
constexpr const char* kInstanceTag = "bro-app-instance/1";

struct Launch {
    std::vector<std::string> argv;
    std::string cwd;
};

// Main thread only: the desktop pump and setAppInstanceHandler both run there.
AppInstanceHandler& handler() {
    static AppInstanceHandler h;
    return h;
}

std::deque<Launch>& backlog() {
    static std::deque<Launch> q;
    return q;
}

void deliver(Launch launch) {
    if (!handler()) {
        backlog().push_back(std::move(launch));
        return;
    }
    // A copy: the handler may replace itself.
    AppInstanceHandler h = handler();
    h(launch.argv, launch.cwd);
}

void onWire(const std::vector<std::string>& msg) {
    if (msg.size() < 2 || msg[0] != kInstanceTag) {
        LOG_WARN("app: ignored a malformed single-instance message");
        return;
    }
    Launch l;
    l.cwd = msg[1];
    l.argv.assign(msg.begin() + 2, msg.end());
    LOG_INFO("app: another launch handed off %zu argument(s) from %s", l.argv.size(), l.cwd.c_str());
    deliver(std::move(l));
}

void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

}  // namespace

std::string currentWorkingDirectory() {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::current_path(ec);
    if (ec) return {};
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

void finalizeAppIdentity(EngineConfig& config) {
    config.appId = appIdFor(config.manifest, config.appDir);
    if (config.launchCwd.empty()) config.launchCwd = currentWorkingDirectory();

    AppRuntimeInfo& a = info();
    a.id = config.appId;
    a.dir = config.appDir;
    a.manifest = config.manifest;
    a.argv = config.appArgs;
    a.cwd = config.launchCwd;
    a.dirs = appDirsFor(config.appId);
    if (!config.appId.empty()) setEnv("BRO_APP_ID", config.appId);
}

const AppRuntimeInfo& currentApp() { return info(); }

void setCurrentAppLogFile(const std::string& path) { info().logFile = path; }

InstanceClaim claimSingleInstance(const EngineConfig& config) {
    std::vector<std::string> msg;
    msg.reserve(config.appArgs.size() + 2);
    msg.emplace_back(kInstanceTag);
    msg.push_back(config.launchCwd.empty() ? currentWorkingDirectory() : config.launchCwd);
    msg.insert(msg.end(), config.appArgs.begin(), config.appArgs.end());

    const std::string channel = "app-" + (config.appId.empty() ? std::string("app") : config.appId);
    const bool primary = platform::desktop::requestSingleInstance(channel, msg, onWire);
    if (!primary) return InstanceClaim::HandedOff;
    info().singleInstance = true;
    return InstanceClaim::Primary;
}

void setAppInstanceHandler(AppInstanceHandler h) {
    handler() = std::move(h);
    while (handler() && !backlog().empty()) {
        Launch l = std::move(backlog().front());
        backlog().pop_front();
        deliver(std::move(l));
    }
}

void pumpAppInstances() {
    if (info().singleInstance) platform::desktop::pumpSingleInstanceEvents();
}

void simulateAppInstance(const std::vector<std::string>& argv, const std::string& cwd) {
    deliver(Launch{argv, cwd});
}

void noteDocumentLoaded() {
    double expected = -1.0;
    gLoadedMs.compare_exchange_strong(expected, sinceStartMs());
}

void noteFramePresented() {
    if (gFirstFrameMs.load(std::memory_order_relaxed) >= 0.0) return;
    const double loaded = gLoadedMs.load(std::memory_order_relaxed);
    if (loaded < 0.0) return;  // the page has not run yet: not its frame
    const double ms = sinceStartMs();
    double expected = -1.0;
    if (!gFirstFrameMs.compare_exchange_strong(expected, ms)) return;
    const std::string& id = info().id;
    LOG_INFO("app %s: first frame %.0f ms after start (page loaded at %.0f ms)",
             id.empty() ? "(unnamed)" : id.c_str(), ms, loaded);
}

double firstFrameMs() { return gFirstFrameMs.load(std::memory_order_relaxed); }

}  // namespace bro::engine
