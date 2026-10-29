#pragma once

// What the running app may do: the one authority every check asks
// (docs/apps.md, Permissions). One app per process, so
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

#include <functional>
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
    /// Granted by the host executable: nothing in the store can take it back.
    bool hostGranted(std::string_view cap) const;

    /// Use `cap` now, for `what` ("fetch https://example.com/"). "" when it is
    /// granted, asking first when it is declared, undecided, `mayAsk` and
    /// this is the main thread; otherwise the sentence the refusal throws
    /// with, naming the app, the capability and `what`. A capability granted
    /// earlier in this run and since taken back is refused with
    /// bronze::embed::kPermissionRevokedPrefix before the sentence, which
    /// makes the program's error a PermissionRevoked. Each granted use is
    /// recorded as the capability's last use (usage below).
    std::string use(std::string_view cap, const std::string& what, bool mayAsk = true);

    /// bro.app.requestPermission: ask now when undecided (main thread only);
    /// whether it is granted afterwards.
    bool request(std::string_view cap);

    /// "" when the app may read (or write) `path`, else the refusal.
    std::string checkFs(const std::string& path, bool write, bool mayAsk = true);

    /// process.env: "" when the app may read variable `name` ("" for the
    /// whole listing). The variables that name the user, the locale and the
    /// desktop, and where things are (envAlwaysVisible), need nothing; the
    /// rest (where tokens and passwords live) need `env`.
    std::string checkEnvRead(const std::string& name, bool mayAsk);
    /// Changing the real environment needs `env`, and is never asked for: a
    /// refused change stays in the app's own view of it (brokit).
    std::string checkEnvWrite(const std::string& name);

    /// The powerbox: what the user picked or dropped is the app's to read and
    /// write (a folder, all under it), now and in later runs.
    void grantPicked(const std::vector<std::string>& paths);

    /// Read the grant store again (it changed: `bro --revoke-app`, the
    /// shell's bro.permissions.set, another process's prompt). What it no
    /// longer grants is revoked from now on; true when anything this app may
    /// do changed. The caller then has what is held open revalidated
    /// (bronze::embed::accessChanged).
    bool reload();

    /// Whether the grant store's file changed since it was last read (its
    /// size and modification time, one stat): what the main loop polls.
    bool storeChanged();

    /// Record, and write out, when each capability was last used
    /// (grant_store.h, usage). Cheap to call every frame: it writes at most
    /// every `minIntervalMs`, or at once when a capability was used for the
    /// first time this run; `force` writes whatever is pending.
    void flushUsage(bool force = false, double minIntervalMs = 60000.0);

    /// The installed app's content no longer matches what `bro --install`
    /// recorded (found after launch, by the background verification): its
    /// decisions are forgotten in the store, so it asks again, and reload()
    /// revokes them now. Main thread.
    void installChanged(const std::string& actualHash);

    PermissionsReport report() const;
    const std::string& identity() const { return identity_; }
    const std::string& appName() const { return appName_; }

    /// Who asks the user about this app's undecided capabilities. Unset (or
    /// answering Unhandled), bro's native dialog asks. An app running inline
    /// in a host page (an <appview>) has its host page ask instead: Allow /
    /// Deny is the user's answer, kept like the dialog's; Pending means the
    /// host is asking in its own time, so this use is refused (not kept) and
    /// the answer arrives through decide().
    enum class AskAnswer { Unhandled, Allow, Deny, Pending };
    using Asker = std::function<AskAnswer(std::string_view cap, const std::string& question,
                                          const std::string& what)>;
    void setAsker(Asker asker);
    /// The user's answer about `cap`, from wherever it was asked: kept in the
    /// store as a prompt's is.
    void decide(std::string_view cap, bool allow);
    const std::string& installHash() const { return installHash_; }
    const std::string& appDir() const { return appDir_; }
    // Whether a bro.conf change (broconf's gate subject: "set <path>.<key>
    // in the desktop settings", or "reset ...") is to the app's own settings,
    // the section named by its id: always its to write, as its own folders are.
    bool ownsConfChange(std::string_view subject) const;
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
    // The store file's modification time and size; -1 when it is missing (or,
    // for the last read, when that read failed and must be repeated).
    struct StoreStamp {
        long long mtime = -1, size = -1;
    };
    static StoreStamp stampStore();
    void applyStore(const GrantStoreData& store, const StoreStamp& stamp);   // under mu_
    void noteUse(std::string_view cap);             // under mu_

    mutable std::mutex mu_;
    Asker asker_;
    bool configured_ = false;
    std::thread::id mainThread_;
    std::string identity_, appId_, appName_, appDir_;
    std::string installHash_;   // what grants of an install attach to (install_record.h)
    std::vector<std::string> declared_, hostGrants_;
    std::map<std::string, Decision> decisions_;
    bool devMode_ = false;
    // Capabilities granted at some point of this run: one refused now was
    // revoked, and says so.
    std::vector<std::string> everGranted_;
    // Canonical, comparable forms (canonicalPath below).
    std::vector<std::string> writeRoots_, readRoots_, picked_, protected_;
    std::string storeFile_;  // the grant store, canonical (canonicalPath)
    std::string home_;
    std::string procSelf_;   // Linux: /proc/<pid>, this process's own
    // The store file as stamped just before it was last read: what
    // storeChanged compares.
    StoreStamp store_;
    // Last use per capability (ms since the epoch), and what is not written yet.
    std::map<std::string, double> lastUsed_;
    bool usagePending_ = false, usageUrgent_ = false;
    double usageWrittenAt_ = 0.0;
};

/// Environment variables every app sees without `env`: who the user is, the
/// locale and time zone, the terminal and the desktop session, and where the
/// standard folders are. Nothing that holds a credential.
bool envAlwaysVisible(std::string_view name);

/// The grants of the code running now: the app's (processPermissions), or,
/// when a host page runs another app inline (an <appview> compartment,
/// docs/appview-api.js), that app's own. Every gate asks this, so a
/// check made while a compartment's code runs judges that compartment.
AppPermissions& appPermissions();
/// The process's own app: the one the engine was started for.
AppPermissions& processPermissions();
/// Who answers appPermissions(): the script host's current compartment. A
/// resolver answering null (another thread, no compartment) means the
/// process's.
using PermissionsResolver = AppPermissions* (*)();
void setPermissionsResolver(PermissionsResolver resolver);

/// `path` absolute, symlinks and ".." resolved as far as it exists, in the
/// form the checks compare (forward slashes; case-folded on Windows).
std::string canonicalPath(const std::string& path);

/// Whether `path` is `root` or lies under it (both canonicalPath forms).
bool pathWithin(const std::string& path, const std::string& root);

}  // namespace bro::engine
