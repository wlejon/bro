#pragma once

// What the running app may do: the one authority every check asks
// (docs/apps.md, Permissions; docs/app-isolation.md). One app per process, so
// one AppPermissions per process, settled by the Engine at startup and read
// from every thread a realm runs on: the brokit access policy (fs,
// child_process, the network), the terminal's spawn gate, the mic capture
// gate, bro.app.spawn, the privileged bro.<namespace> stubs.
//
// A capability (capabilities.h) is granted when
//   - the host executable grants it (EngineConfig::hostGrants), or
//   - it is an ask capability the manifest declares, and the user allowed it
//     (the grant store, grant_store.h) or dev mode is on.
// A shell capability comes from the host alone. One the manifest does not
// declare is denied without asking. One declared and undecided is asked for
// on its first use on the main thread (a native OK / Cancel dialog; headless
// answers through setDialogAnswer), and the answer is kept; a Worker cannot
// ask, so there it is denied until granted.
//
// Files (checkFs): beyond what `files` grants, an app writes only to its own
// config, data, cache and log directories, the OS temp directory and what the
// user picked in bro's dialogs or dropped on the window (the powerbox:
// grantPicked); it reads anywhere outside the user's home, and under it only
// those, its own folder, bro's resources and mounts (/lib, /system), and the
// models directories. bro's own state (the grant store, the install roots and
// their executables, the engine) is never writable by an app, `files` or not.
// Every path is canonicalized (symlinks, "..") before it is judged.

#include "engine/app_manifest.h"
#include "engine/grant_store.h"

#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace bro::engine {

enum class GrantState { Granted, Denied, Undecided };

struct PermissionsSetup {
    std::string appDir;
    std::string appId;
    std::string appName;                    // for prompts and refusals; the id when empty
    std::vector<std::string> declared;      // the manifest's `permissions`
    std::vector<std::string> hostGrants;    // EngineConfig::hostGrants ("declared" expanded here)
    AppDirs dirs;
    std::vector<std::string> readRoots;     // bro's resources, the mounts, the models dirs
};

struct PermissionsReport {
    std::vector<std::string> declared, granted, denied, undecided;
};

class AppPermissions {
public:
    /// Settle the app's grants (on the main thread, before any script runs).
    void configure(const PermissionsSetup& setup);

    GrantState state(std::string_view cap) const;
    bool granted(std::string_view cap) const { return state(cap) == GrantState::Granted; }

    /// Use `cap` now, for `what` ("fetch https://example.com/"). "" when it is
    /// granted, asking first when it is declared, undecided and this is the
    /// main thread; otherwise the sentence the refusal throws with, naming
    /// the app, the capability and `what`.
    std::string use(std::string_view cap, const std::string& what);

    /// bro.app.requestPermission: ask now when undecided (main thread only);
    /// whether it is granted afterwards.
    bool request(std::string_view cap);

    /// "" when the app may read (or write) `path`, else the refusal.
    std::string checkFs(const std::string& path, bool write);

    /// The powerbox: what the user picked or dropped is the app's to read and
    /// write (a folder, all under it), now and in later runs.
    void grantPicked(const std::vector<std::string>& paths);

    PermissionsReport report() const;
    const std::string& identity() const { return identity_; }
    bool configured() const { return configured_; }
    bool onMainThread() const { return std::this_thread::get_id() == mainThread_; }

private:
    struct Judgement {
        GrantState state;
        std::string reason;   // why it is denied
    };
    Judgement judge(std::string_view cap) const;
    std::string refusal(std::string_view cap, const std::string& what, const std::string& reason) const;
    void persist(std::string_view cap, Decision d);
    bool underAny(const std::string& p, const std::vector<std::string>& roots) const;

    mutable std::mutex mu_;
    bool configured_ = false;
    std::thread::id mainThread_;
    std::string identity_, appId_, appName_, appDir_;
    std::vector<std::string> declared_, hostGrants_;
    std::map<std::string, Decision> decisions_;
    bool devMode_ = false;
    // Canonical, comparable forms (canonicalPath below).
    std::vector<std::string> writeRoots_, readRoots_, picked_, protected_;
    std::string storeFile_;  // the grant store, canonical (canonicalPath)
    std::string home_;
};

/// The process's one instance.
AppPermissions& appPermissions();

/// `path` absolute, symlinks and ".." resolved as far as it exists, in the
/// form the checks compare (forward slashes; case-folded on Windows).
std::string canonicalPath(const std::string& path);

/// Whether `path` is `root` or lies under it (both canonicalPath forms).
bool pathWithin(const std::string& path, const std::string& root);

}  // namespace bro::engine
