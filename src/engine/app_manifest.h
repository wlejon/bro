#pragma once

// An app's identity and desktop integration, read from its bro.json
// (docs/apps.md). A folder holding bro.json and a page is a desktop
// application: the manifest names it, says where its state lives, whether a
// second launch hands off to the first, which files it opens, and which
// privileged namespaces it asks for. The window keys (title, width, ...) are
// config_loader.cpp's; this is everything else.

#include <string>
#include <vector>

namespace bro::engine {

/// A kind of file the app opens: what goes into its desktop entry's MimeType=
/// and what a file manager offers it for.
struct AppFileType {
    std::string name;                     // "Shell script"
    std::vector<std::string> mimeTypes;   // "application/x-shellscript"
    std::vector<std::string> extensions;  // "sh" (no dot)
};

/// A launcher action ("New Window", "New Private Tab"): a desktop entry
/// [Desktop Action] whose Exec runs the app with `args` after its directory.
struct AppAction {
    std::string id;                  // [A-Za-z0-9-]
    std::string name;
    std::vector<std::string> args;
    std::string icon;
};

/// What bro.json says about the app itself. (Not app_loader.h's AppManifest,
/// which is the page's resources: its HTML, scripts and stylesheets.)
struct AppDescriptor {
    /// The app's id, as declared (empty when bro.json names none; the
    /// effective id is then derived from the folder name, appIdFor()).
    std::string id;
    std::string name;
    std::string version;
    std::string icon;            // relative to the app dir, as declared
    std::string description;
    std::vector<std::string> categories;   // FreeDesktop main/additional categories
    std::vector<std::string> keywords;
    /// A second launch hands its argv and working directory to the running
    /// instance (an `instance` event on bro.app) and exits.
    bool singleInstance = false;
    /// Whether the app shows in launchers (false: a helper the desktop entry
    /// marks NoDisplay=true).
    bool display = true;
    std::vector<AppFileType> fileTypes;
    std::vector<AppAction> actions;
    /// Privileged namespaces asked for (`permissions`, and the older
    /// `privileged`, merged). Asking grants nothing: desktop_trust.h decides.
    std::vector<std::string> permissions;
    bool shell = false;
};

/// Read the app keys of a bro.json. Returns false (with *error) when the file
/// cannot be read or is not a JSON object; unknown keys are ignored, and a key
/// of the wrong type is skipped with a warning rather than failing the launch.
bool parseAppManifest(const std::string& path, AppDescriptor& out, std::string* error = nullptr);

/// An id is reverse-DNS (`org.example.Editor`) or a simple slug (`notes`):
/// 1-255 chars of [A-Za-z0-9._-], not starting or ending with '.', no "..".
/// It names the window's app_id / WM class, the desktop entry
/// (<id>.desktop), the single-instance channel and the per-app directories,
/// so it must be safe as a file name everywhere.
bool isValidAppId(const std::string& id);

/// The effective id: the declared one when valid, else the app folder's name
/// reduced to [A-Za-z0-9._-] (the rule util::appUserDataDir has always used).
std::string appIdFor(const AppDescriptor& manifest, const std::string& appDir);

/// Where an app keeps its state, keyed by id. Nothing is created here.
///   config  settings the user edits     $XDG_CONFIG_HOME/<id>   %APPDATA%\<id>
///   data    state the app owns          $XDG_DATA_HOME/<id>     %APPDATA%\<id>\data
///   cache   rebuildable                 $XDG_CACHE_HOME/<id>    %LOCALAPPDATA%\<id>\cache
///   log     <state>/<id>.log            $XDG_STATE_HOME/<id>    %LOCALAPPDATA%\<id>
/// (macOS: ~/Library/Application Support/<id>[/data], ~/Library/Caches/<id>,
/// ~/Library/Logs/<id>.) BRO_APP_HOME=<dir> puts all four under
/// <dir>/{config,data,cache,log} instead: how a test or a portable install
/// keeps an app away from the user's own state.
struct AppDirs {
    std::string config;
    std::string data;
    std::string cache;
    std::string logDir;
    std::string logFile;
};
AppDirs appDirsFor(const std::string& id);

/// Where installed folder apps live, user first: $XDG_DATA_HOME/bro/apps
/// (%LOCALAPPDATA%\bro\apps), then the system roots ($XDG_DATA_DIRS/bro/apps,
/// so /usr/share/bro/apps and /usr/local/share/bro/apps; %ProgramFiles%\bro\apps).
/// Each app is <root>/<id>/ (a directory, or on POSIX a symlink to one).
std::vector<std::string> installedAppRoots();

/// The installed app directory for `id`, or "" when none is installed.
std::string findInstalledApp(const std::string& id);

/// The user's own permission grants (docs/desktop-trust.md): the file
/// $XDG_CONFIG_HOME/bro/permissions.json (%APPDATA%\bro\permissions.json,
/// ~/Library/Application Support/bro/permissions.json), an object mapping app
/// ids to the privileged namespaces granted to them:
///   { "org.example.Remote": ["remote"], "my.shell": ["shell"] }
/// "*" grants every namespace the app asks for; "shell" grants shell status.
/// The id "*" grants to every app ({ "*": ["*"] }: a development machine that
/// does not want the trust check). Returns what is listed for `id` plus what
/// is listed for "*" ([] when the file or both are absent).
/// BRO_PERMISSIONS_FILE=<path> reads that file instead (tests isolate with it).
std::vector<std::string> userPermissionGrants(const std::string& id);
std::string userPermissionsFile();

}  // namespace bro::engine
