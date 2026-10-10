#pragma once

// The running app as the runtime sees it, for the life of the process: who it
// is (id, manifest), what it was launched with (argv, working directory),
// where its state lives (AppDirs), and the single-instance hand-off. One app
// per process: bro runs one app, and an <iframe> or a Worker is part of it.
//
// The launchers (main.cpp, headless_driver.cpp, an embedder through
// launcher.h) settle the identity with finalizeAppIdentity(); bro.app
// (bronze_host/host_app.cpp) reads it from here.

#include "engine/app_manifest.h"
#include "engine/engine_config.h"

#include <functional>
#include <string>
#include <vector>

namespace bro::engine {

struct AppRuntimeInfo {
    std::string id;
    std::string dir;                 // absolute app directory
    AppDescriptor manifest;
    std::vector<std::string> argv;   // after the app directory
    std::string cwd;                 // where the app was launched from
    AppDirs dirs;
    std::string logFile;             // the file stdout/stderr go to, "" for none
    bool singleInstance = false;     // this process holds the app's instance channel
};

/// Settle `config.appId` (appIdFor: the declared id, else the folder name),
/// fill launchCwd when the caller left it empty, and publish the result as
/// currentApp() and as BRO_APP_ID in the environment. Call after the app
/// directory is resolved and absolute (publishLaunchEnv), before the Engine.
void finalizeAppIdentity(EngineConfig& config);

const AppRuntimeInfo& currentApp();

/// Record where stdout/stderr were sent (bro.app.logFile).
void setCurrentAppLogFile(const std::string& path);

/// The working directory, as UTF-8 ("" when it cannot be read).
std::string currentWorkingDirectory();

/// The single-instance channel for the app (`"singleInstance": true`).
///   Primary   this process now owns the channel: later launches hand off to it
///   HandedOff another instance is running and has this launch's argv and cwd;
///             the caller exits without starting an Engine
enum class InstanceClaim { Primary, HandedOff };
InstanceClaim claimSingleInstance(const EngineConfig& config);

/// Give the channel up on the way out (both drivers call it after the Engine
/// is gone): the socket is removed and the server thread joined, so the next
/// launch becomes the primary. A no-op when nothing holds a channel.
void releaseSingleInstance();

/// Where a handed-off launch goes. Called on the main thread from the frame
/// loop's desktop pump; launches that arrive before a handler is set are
/// queued and delivered when it is. Pass nullptr to queue again.
using AppInstanceHandler = std::function<void(const std::vector<std::string>& argv, const std::string& cwd)>;
void setAppInstanceHandler(AppInstanceHandler handler);

/// Deliver queued hand-offs (the windowed loop's desktop pump does this too;
/// the headless and DRM loops call it themselves).
void pumpAppInstances();

/// Test hook: deliver a launch as though another instance had handed it off.
void simulateAppInstance(const std::vector<std::string>& argv, const std::string& cwd);

/// The page's scripts have run and its load event fired.
void noteDocumentLoaded();

/// The window presented a frame. The first one after the page loaded logs
/// how long the launch took to show the app ("first frame N ms after start",
/// measured from the engine library's static initialisation, just before
/// main): the number docs/apps.md's startup note is about. A frame presented
/// before the page ran (the empty window) does not count. Later calls are free.
void noteFramePresented();

/// Milliseconds from start to the page's first presented frame, or -1 before it.
/// Headless has no present; its first flush after the page loaded counts.
double firstFrameMs();

/// Milliseconds from start to the page's load (its scripts run, load fired),
/// or -1 before it.
double documentLoadedMs();

/// The window's title before the page names one: bro.json's "title", else
/// the manifest name, else "Bro".
std::string initialWindowTitle(const EngineConfig& config);

/// The manifest icon as an absolute path ("" when none is declared or the
/// file is missing).
std::string appIconPath(const AppDescriptor& manifest, const std::string& appDir);

/// Load an icon file as straight-alpha RGBA8: an SVG is rasterized at
/// fit `size` x `size` (its own aspect kept), any other format decoded.
bool loadIconPixels(const std::string& path, int size, int& width, int& height, std::vector<uint8_t>& rgba);

}  // namespace bro::engine
