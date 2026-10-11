#include "engine/app_runtime.h"

#include "platform/desktop_notifications.h"
#include "platform/desktop_single_instance.h"
#include "platform/window_system.h"
#include "svg/svg_renderer.h"
#include "util/exe_dir.h"
#include "util/log.h"

#include "broimage/decode.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
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

AppRuntimeInfo& info() {
    static AppRuntimeInfo s;
    return s;
}

// The hand-off message: a tag (so a stray client cannot pass for a launch),
// the launch's working directory, then its argv. /2 puts the launch's
// activation token (XDG_ACTIVATION_TOKEN, possibly empty) after the working
// directory, so the running instance raises its window with the token the
// launcher gave the second launch. /3 puts the launch's notification
// activation text (`--notification <args>`, possibly empty) after the token:
// a click on a notification that started a second launch of a running
// single-instance app (macOS, where every app is one bundle) is the running
// one's click. /2 and /1 are still accepted.
constexpr const char* kInstanceTag = "bro-app-instance/3";
constexpr const char* kInstanceTagV2 = "bro-app-instance/2";
constexpr const char* kInstanceTagV1 = "bro-app-instance/1";

struct Launch {
    std::vector<std::string> argv;
    std::string cwd;
    std::string activationToken;
    std::string notification;
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
    // The handler raises the window (host_app's onInstance); a token handed
    // over with the launch is what lets that raise take focus.
    if (!launch.activationToken.empty())
        platform::windowSystem().setActivationToken(launch.activationToken);
    // A copy: the handler may replace itself.
    AppInstanceHandler h = handler();
    h(launch.argv, launch.cwd);
}

void onWire(const std::vector<std::string>& msg) {
    const bool v3 = msg.size() >= 4 && msg[0] == kInstanceTag;
    const bool v2 = msg.size() >= 3 && msg[0] == kInstanceTagV2;
    const bool v1 = msg.size() >= 2 && msg[0] == kInstanceTagV1;
    if (!v1 && !v2 && !v3) {
        LOG_WARN("app: ignored a malformed single-instance message");
        return;
    }
    Launch l;
    l.cwd = msg[1];
    if (v2 || v3) l.activationToken = msg[2];
    if (v3) l.notification = msg[3];
    l.argv.assign(msg.begin() + (v3 ? 4 : v2 ? 3 : 2), msg.end());
    LOG_INFO("app: another launch handed off %zu argument(s) from %s%s", l.argv.size(), l.cwd.c_str(),
             l.notification.empty() ? "" : " with a notification click");
    // The click is this instance's, heard as one that ended an earlier run
    // (the page's notificationclick); it raises the window. A launch that
    // was only the click (no arguments of its own) is no other launch.
    if (!l.notification.empty()) {
        platform::desktop::noteLaunchNotification(l.notification);
        if (l.argv.empty()) return;
    }
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

InstanceClaim claimSingleInstance(const EngineConfig& config, const std::string& launchNotification) {
    std::vector<std::string> msg;
    msg.reserve(config.appArgs.size() + 4);
    msg.emplace_back(kInstanceTag);
    msg.push_back(config.launchCwd.empty() ? currentWorkingDirectory() : config.launchCwd);
    msg.push_back(platform::launchActivationToken());
    msg.push_back(launchNotification);
    msg.insert(msg.end(), config.appArgs.begin(), config.appArgs.end());

    const std::string channel = "app-" + (config.appId.empty() ? std::string("app") : config.appId);
    const bool primary = platform::desktop::requestSingleInstance(channel, msg, onWire);
    if (!primary) return InstanceClaim::HandedOff;
    info().singleInstance = true;
    return InstanceClaim::Primary;
}

void releaseSingleInstance() {
    // Unconditional: bro.window.requestSingleInstance claims a channel too.
    platform::desktop::shutdownSingleInstance();
    info().singleInstance = false;
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

double sinceStartMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - kStart).count();
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

double documentLoadedMs() { return gLoadedMs.load(std::memory_order_relaxed); }

// Written once at launch on the page thread and read there (bro.app.startup),
// so no atomics: a mutex keeps a reader on another thread honest.
namespace {
std::mutex gStartupMutex;
GraphicsStartup gGraphicsStartup;
PageStartup gPageStartup;
}  // namespace

void noteGraphicsStartup(const GraphicsStartup& g) {
    std::lock_guard<std::mutex> lock(gStartupMutex);
    gGraphicsStartup = g;
}

GraphicsStartup graphicsStartup() {
    std::lock_guard<std::mutex> lock(gStartupMutex);
    return gGraphicsStartup;
}

void notePageGlobals(double atMs, double ms) {
    std::lock_guard<std::mutex> lock(gStartupMutex);
    gPageStartup.globalsAtMs = atMs;
    gPageStartup.globalsMs = ms;
}

void notePageCompile(double startMs, double endMs, double waitMs, const std::string& codeCache) {
    std::lock_guard<std::mutex> lock(gStartupMutex);
    gPageStartup.compileStartMs = startMs;
    gPageStartup.compileEndMs = endMs;
    gPageStartup.waitMs = waitMs;
    gPageStartup.codeCache = codeCache;
}

PageStartup pageStartup() {
    std::lock_guard<std::mutex> lock(gStartupMutex);
    return gPageStartup;
}

std::string initialWindowTitle(const EngineConfig& config) {
    if (!config.title.empty()) return config.title;
    if (!config.manifest.name.empty()) return config.manifest.name;
    return "Bro";
}

std::string appIconPath(const AppDescriptor& manifest, const std::string& appDir) {
    if (manifest.icon.empty()) return {};
    namespace fs = std::filesystem;
    const std::u8string rel(manifest.icon.begin(), manifest.icon.end());
    fs::path p(rel);
    if (p.is_relative() && !appDir.empty()) p = fs::path(std::u8string(appDir.begin(), appDir.end())) / p;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        LOG_WARN("app: icon '%s' not found", manifest.icon.c_str());
        return {};
    }
    fs::path abs = fs::absolute(p, ec);
    std::u8string u = (ec ? p : abs).lexically_normal().u8string();
    return std::string(u.begin(), u.end());
}

bool loadIconPixels(const std::string& path, int size, int& width, int& height, std::vector<uint8_t>& rgba) {
    std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    if (!f) return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return false;
    if (svg::looksLikeSvg(bytes.data(), bytes.size())) {
        float sw = 0, sh = 0;
        svg::svgIntrinsicSize(bytes.data(), bytes.size(), sw, sh);
        int w = size, h = size;
        if (sw > 0 && sh > 0) {
            if (sw >= sh) h = (std::max)(1, static_cast<int>(size * sh / sw + 0.5f));
            else w = (std::max)(1, static_cast<int>(size * sw / sh + 0.5f));
        }
        return svg::rasterizeSvgMarkup(bytes.data(), bytes.size(), w, h, width, height, rgba);
    }
    broimage::Image img;
    if (!broimage::decode_memory(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), img)) return false;
    width = img.width;
    height = img.height;
    rgba = std::move(img.pixels);
    return width > 0 && height > 0;
}

}  // namespace bro::engine
