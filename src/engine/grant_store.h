#pragma once

// What the user decided each app may do, kept where only bro writes: the grant
// store, grants.json in bro's own config directory (grantStoreDir()). It is
// keyed by the app's identity, never by the id its bro.json claims, which any
// app can claim:
//
//   install:<id>   the app runs from an install root (app_manifest.h,
//                  installedAppRoots): its folder there is named by its id,
//                  which `bro --install` wrote and the app cannot write
//                  (no app may write an install root, app_permissions.cpp)
//   path:<dir>     anything else: the canonical path of its folder
//
// Each identity maps capability names (capabilities.h) to "allow" or "deny"
// and lists the files and folders the user picked for it (the powerbox,
// app_permissions.h). One global flag, devMode, grants every app every ask
// capability its manifest declares; shell capabilities are never in the
// store's gift, whatever it holds.
//
//   {
//     "devMode": false,
//     "apps": {
//       "install:org.example.Notes": {
//         "id": "org.example.Notes",
//         "dir": "/home/j/.local/share/bro/apps/org.example.Notes",
//         "installHash": "<what was installed when this was decided>",
//         "capabilities": { "net": "allow", "spawn": "deny" },
//         "picked": ["/home/j/Documents/notes"]
//       }
//     }
//   }
//
// Several bro processes share the file, so every change is a read, a change
// and an atomic replace (update()), never a write of a copy held since start.

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace bro::engine {

enum class Decision { Undecided, Allow, Deny };

struct AppGrantRecord {
    std::string id;    // the app's id when last run (for listings)
    std::string dir;   // its folder when last run
    // An install: the hash of what was installed when these decisions were
    // made (install_record.h). Content with another hash asks again: its
    // decisions and picked paths are forgotten at its launch.
    std::string installHash;
    std::map<std::string, Decision> decisions;
    std::vector<std::string> picked;
};

struct GrantStoreData {
    bool devMode = false;
    std::map<std::string, AppGrantRecord> apps;   // by identity
};

/// bro's own per-user config directory: $XDG_CONFIG_HOME/bro (~/.config/bro),
/// %APPDATA%\bro, ~/Library/Application Support/bro; under BRO_APP_HOME=<dir>
/// it is <dir>/bro, beside (not among) the app's four directories, so a test
/// keeps its grants with the rest of its scratch state. No app's own config
/// directory is this one: the id "bro" is reserved (isValidAppId).
std::string grantStoreDir();
std::string grantStorePath();

/// Read the store. A missing file is an empty store; an unreadable or
/// malformed one is too, with a warning, so a corrupt file denies (nothing is
/// decided) rather than granting. `readOk` (when given) is false for a store
/// that is there but could not be read or parsed, so a reader that already
/// has a good copy keeps it and tries again.
GrantStoreData loadGrantStore(bool* readOk = nullptr);

/// Read, change and atomically replace the store. False (with *error) when it
/// could not be written; the change is then lost.
bool updateGrantStore(const std::function<void(GrantStoreData&)>& change, std::string* error = nullptr);

/// The identity of the app in `appDir` (see above), and its install id when
/// it has one.
std::string appIdentityFor(const std::string& appDir);

/// What a command line names (`bro --permissions <id|dir>`): an existing
/// folder is the identity of the app there; otherwise an installed app's id,
/// else the identity of a store entry with that id. "" when nothing matches.
std::string resolveIdentityArg(const std::string& idOrDir, const GrantStoreData& store);

const char* decisionName(Decision d);   // "allow", "deny", "undecided"

/// One app as the grant listings show it (`bro --permissions`,
/// bro.permissions.list): every installed app and every identity the store
/// holds, with what its manifest declares as it is now on disk.
struct AppPermissionsEntry {
    std::string identity, id, name, dir;
    bool installed = false;
    // An install's state against its record: "current" (the decisions were
    // made for what is installed), "changed" (installed again, or altered,
    // since: its next launch asks again), "unrecorded" (no record: installed
    // before records, or not by `bro --install`), "linked" (a --link
    // install); "" for an app run from a folder.
    std::string install;
    std::vector<std::string> declared;
    std::map<std::string, Decision> decisions;
    std::vector<std::string> picked;
    std::map<std::string, double> lastUsed;   // ms since the epoch, by capability
};
std::vector<AppPermissionsEntry> listAppPermissions();

/// The hash an identity's decisions attach to when made now: an install's
/// recorded hash ("unrecorded" without a record), "" for a folder.
std::string currentInstallHash(const std::string& identity, const std::string& dir);

/// Forget every decision and picked path of `identity` (it asks again).
bool resetAppPermissions(const std::string& identity, std::string* error);

/// Take back one picked file or folder.
bool removeAppPicked(const std::string& identity, const std::string& path, std::string* error);

// ---- when each capability was last used ----------------------------------------
//
// Kept apart from the grants, in usage.json beside them, so the frequent
// writes of running apps never race the user's decisions or wake every
// running app's watch on grants.json:
//
//   { "install:org.example.Notes": { "net": 1761480000000, "files": ... } }

std::string grantUsagePath();
std::map<std::string, std::map<std::string, double>> loadGrantUsage();
/// Merge `used` into identity's entry (the later time of each wins).
bool mergeGrantUsage(const std::string& identity, const std::map<std::string, double>& used, std::string* error);

/// Decide `cap` for `identity` (Undecided forgets the decision, so the app
/// asks again). Only an ask capability can be decided here: a shell one is
/// the host's alone. False with *error otherwise or when the store could not
/// be written. `dir` fills in a record the store does not hold yet.
bool decideAppCapability(const std::string& identity, const std::string& dir, const std::string& cap,
                         Decision d, std::string* error);

bool setGrantStoreDevMode(bool on, std::string* error);

}  // namespace bro::engine
