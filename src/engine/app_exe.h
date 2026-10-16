#pragma once

// An installed app's own executable (docs/apps.md, "The app's executable").
//
// Every installed folder app gets a small executable named for it
// (helm.calculator -> helm-calculator), so the process, its window and its
// taskbar entry are the app's rather than "bro". It holds no engine: it is
// bro's prebuilt stub (bro-app-stub, built with bro and shipped beside the
// engine library), copied and stamped on the machine the app is installed
// on, with no compiler involved:
//
//   * an identity block, found by its marker and overwritten in place: the
//     app's id, name, version and folder, the engine build and the content
//     hash the executable was made against, and where the engine library and
//     the stock bro are. The stub loads the library and hands the block to
//     bro_run_app (runtime/bro_entry.h).
//   * on Windows, an icon and a version resource, written with the system's
//     own resource updater (BeginUpdateResource), so Explorer, the taskbar
//     and Task Manager show the app's icon and name.
//
// The executable is cached by appContentHash() and the engine build
// (engineVersion()): `bro --install` and `bro --build-exe` rebuild it only
// when either changed, and an executable that finds itself out of date runs
// the app as a folder under the stock bro while a fresh one is built.
//
// Seams for what the executable will carry later (an AOT-compiled app module,
// a startup heap snapshot, packed assets, the list of natives modules the app
// uses): each is another input to appContentHash() and another key in the
// identity block or another section appended to the stub copy; the stub and
// bro_run_app already treat unknown keys as data for the engine to read.

#include "engine/app_manifest.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace bro::engine {

/// What an app executable's identity block says.
struct AppExeIdentity {
    int format = 0;        // 1; 0 when the block was never stamped
    std::string id;
    std::string name;
    std::string version;
    std::string app;       // the app folder, absolute (by reference)
    std::string engine;    // engineVersion() it was built against
    std::string hash;      // appContentHash() it was built from
    std::string libbro;    // the engine library the stub loads
    std::string bro;       // the stock bro, for when the library cannot be used
};

/// The block in the stub: kAppExeMarkerSize bytes of marker, then the
/// identity as `key=value\n` lines, NUL-padded to kAppExeIdentityCapacity.
/// The stub's own source (runtime/app_stub.cpp) spells the same constants.
inline constexpr size_t kAppExeIdentityCapacity = 8192;
inline constexpr size_t kAppExeMarkerSize = 32;
inline constexpr char kAppExeMarker[kAppExeMarkerSize + 1] = "BRO\x01" "APP-IDENTITY-BLOCK-v1\x01......";

std::string serializeAppExeIdentity(const AppExeIdentity& id);
bool parseAppExeIdentity(std::string_view text, AppExeIdentity& out);

/// The engine build this process runs: `<bro version>+abi<N>.<size>.<mtime>`
/// of the file holding the engine code (libbro, or the executable when the
/// engine is linked in statically). Any relink of the library changes it.
const std::string& engineVersion();

/// The file holding the engine code, absolute.
std::string engineImagePath();

/// The stock bro and the app stub, beside the engine library ("" for the
/// stub when this build ships none).
std::string stockBroPath();
std::string appExeStubPath();

/// helm.calculator -> helm-calculator (+ ".exe" on Windows): the id with its
/// dots made dashes, which is what a process list shows.
std::string appExeName(const std::string& id);

/// Where installed apps' executables live: `bin/` beside the install root
/// (app_manifest.h installRoot()): ~/.local/share/bro/bin,
/// %LOCALAPPDATA%\bro\bin, ~/Library/Application Support/bro/bin.
std::string appExeDir(bool system);
std::string appExePath(const std::string& id, bool system);

/// The hash an executable is cached by: what the executable is built from in
/// the app folder (bro.json and the icon today), never the page's sources,
/// which the executable only refers to.
std::string appContentHash(const std::string& appDir, const AppDescriptor& manifest);

/// Read the identity stamped into an executable. False when the file is
/// missing or holds no stamped block.
bool readAppExeIdentity(const std::string& exePath, AppExeIdentity& out);

enum class AppExeState {
    Missing,      // no executable there
    Current,      // built from this app folder against this engine
    StaleEngine,  // built against another engine build
    StaleApp,     // the app's bro.json or icon changed since
    Foreign,      // an executable for another folder, or not one of ours
};
const char* appExeStateName(AppExeState s);
AppExeState appExeState(const std::string& exePath, const std::string& appDir, AppExeIdentity* stamped = nullptr);

/// Build (or keep) the executable for `appDir` at `exePath`. Unless `force`,
/// a Current executable is left alone (*rebuilt = false). The new file is
/// written beside the old and moved over it, so a running copy is never
/// overwritten in place (on Windows the running file is renamed aside).
/// `stampEngine`, when not empty, is stamped as the engine build instead of
/// this one's: how a test makes an executable "built against another
/// engine" without building one (`bro --build-exe --stamp-engine <v>`).
bool buildAppExe(const std::string& appDir, const AppDescriptor& manifest, const std::string& exePath,
                 bool force, bool* rebuilt, std::string& err, const std::string& stampEngine = {});

/// What `bro --exe-info <exe>` prints: the identity stamped in an executable
/// and whether it is current for its folder; on Windows also what the system
/// reads from the file (its version strings and icon sizes, the AppUserModelID
/// of the app's Start menu shortcut) and, with `iconPng`, the shell's own
/// rendering of the executable's icon written there as a PNG.
std::string describeAppExe(const std::string& exePath, const std::string& iconPng);

/// The Windows half of describeAppExe (app_exe_win.cpp); "" elsewhere.
std::string describeWindowsExe(const std::string& exePath, const AppExeIdentity& id, const std::string& iconPng);

/// Remove an executable this machine built (and anything it left beside it).
void removeAppExe(const std::string& exePath);

/// The executable to start installed app `id` in folder `dir` with: its own,
/// when one is installed for that folder and Current; else "".
std::string currentAppExecutable(const std::string& id, const std::string& dir);

/// Windows: the Start menu shortcut that launches `exePath` under the app's
/// AppUserModelID (`id`), so the shortcut, the running window and a pinned
/// taskbar button are one group. A no-op returning true elsewhere.
bool writeStartMenuShortcut(const std::string& id, const std::string& name, const std::string& description,
                            const std::string& exePath, bool system, std::string& err);
void removeStartMenuShortcut(const std::string& name, bool system);
std::string startMenuShortcutPath(const std::string& name, bool system);

/// Windows: stamp the icon (from `iconPath`, any format loadIconPixels reads;
/// "" for none) and a version resource naming the app into `exePath`.
/// Implemented in app_exe_win.cpp; true and a no-op elsewhere.
bool stampWindowsResources(const std::string& exePath, const AppDescriptor& manifest, const std::string& id,
                           const std::string& iconPath, std::string& err);

}  // namespace bro::engine
