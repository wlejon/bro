#pragma once

// What `bro --install` installed, recorded beside it so the grants the user
// gives an installed app attach to exactly that content (docs/apps.md,
// Permissions).
//
// The record is <install root>/<id>.install.json, in the install root no app
// may write (app_permissions.cpp protects the roots):
//
//   {
//     "id": "org.example.Notes",
//     "hash": "<sha-256 of the tree, hex>",     // "linked:<target>" for --link
//     "linked": false,
//     "installedAt": 1761480000000,
//     "files": [ { "path": "index.html", "size": 1234, "mtime": 133...,
//                  "sha256": "<hex>" }, ... ]
//   }
//
// The tree's hash is SHA-256 over each regular file under the folder, in
// path order: "<relative path>\0<size>\0<file's sha-256>\n". A --link install
// records no content (the folder is a checkout being worked on): its hash
// names the link's target, so its grants hold across edits and are lost if it
// is relinked elsewhere.
//
// At launch nothing is hashed: checkInstall compares each recorded file's
// size and modification time with the folder's (one stat each, no reads), and
// a file added, removed or changed by that measure costs a full hash then.
// What the stat cannot see (content changed with size and time put back) the
// background verification finds: verifyInstallContent hashes the content off
// the main thread after launch, and a mismatch takes the grants back while
// the app runs (AppPermissions::installChanged).

#include <cstdint>
#include <string>
#include <vector>

namespace bro::engine {

struct InstallRecordFile {
    std::string path;     // relative, forward slashes
    uint64_t size = 0;
    int64_t mtime = 0;    // the file system's own ticks
    std::string sha256;
};

struct InstallRecord {
    std::string id;
    std::string hash;
    bool linked = false;
    double installedAt = 0;
    std::vector<InstallRecordFile> files;
};

/// <root>/<id>.install.json for the installed folder <root>/<id>.
std::string installRecordPath(const std::string& installedDir);

/// Hash the folder and write its record. `linked`: a --link install (no content).
bool writeInstallRecord(const std::string& installedDir, const std::string& id, bool linked, std::string* error);

bool readInstallRecord(const std::string& installedDir, InstallRecord& out);

void removeInstallRecord(const std::string& installedDir);

struct InstallCheck {
    std::string hash;        // what grants attach to now
    bool recorded = false;   // the folder has a record
    bool matches = false;    // the folder is what the record says (by stat, or hashed)
    bool hashed = false;     // the stat differed, so the content was hashed now
    double ms = 0;           // what the check cost
};

/// The hash an installed app's grants attach to at launch. With no record it
/// is "unrecorded" (grants made then hold only while it stays unrecorded);
/// otherwise the recorded hash when every file's size and time match, else
/// the content's hash now.
InstallCheck checkInstall(const std::string& installedDir);

/// The content's hash, read in full (the files' own hashes in `files` when
/// not null).
std::string hashInstalledTree(const std::string& dir, std::vector<InstallRecordFile>* files = nullptr);

}  // namespace bro::engine
