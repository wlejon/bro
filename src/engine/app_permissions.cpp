#include "engine/app_permissions.h"

#include "engine/capabilities.h"
#include "engine/install_record.h"
#include "platform/dialogs.h"
#include "util/exe_dir.h"
#include "util/log.h"

#include "embed/embed_access.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace bro::engine {

namespace fs = std::filesystem;

namespace {

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string u8str(const fs::path& p) {
    std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

std::string homeDir() {
#ifdef _WIN32
    const wchar_t* w = _wgetenv(L"USERPROFILE");
    return w && *w ? u8str(fs::path(w)) : std::string();
#else
    const char* h = std::getenv("HOME");
    return h && *h ? std::string(h) : std::string();
#endif
}

double epochMs() {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// The canonical form, unfolded: what the store keeps and messages show.
std::string canonicalDisplay(const std::string& path) {
    std::error_code ec;
    fs::path p = u8path(path);
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    fs::path c = fs::weakly_canonical(abs, ec);
    std::string s = u8str((ec ? abs : c).lexically_normal());
    while (s.size() > 1 && s.back() == '/' && !(s.size() == 3 && s[1] == ':')) s.pop_back();
    return s;
}

bool contains(const std::vector<std::string>& list, std::string_view s) {
    return std::find(list.begin(), list.end(), s) != list.end();
}

void addRoot(std::vector<std::string>& roots, const std::string& path) {
    if (path.empty()) return;
    const std::string c = canonicalPath(path);
    if (!contains(roots, c)) roots.push_back(c);
}

// Linux's /proc/<pid>/<what>: `pid` (a number, "self" or "thread-self") and
// the entry, or false.
bool procEntry(const std::string& p, std::string& pid, std::string& entry) {
#ifdef __linux__
    if (p.rfind("/proc/", 0) != 0) return false;
    const size_t slash = p.find('/', 6);
    pid = p.substr(6, slash == std::string::npos ? std::string::npos : slash - 6);
    if (pid.empty() || !(pid == "self" || pid == "thread-self" ||
                         std::all_of(pid.begin(), pid.end(), [](char c) { return c >= '0' && c <= '9'; })))
        return false;
    entry = slash == std::string::npos ? std::string() : p.substr(slash + 1);
    return true;
#else
    (void)p; (void)pid; (void)entry;
    return false;
#endif
}

}  // namespace

std::string canonicalPath(const std::string& path) {
    std::string s = canonicalDisplay(path);
#ifdef _WIN32
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#endif
    return s;
}

// canonicalPath for the check every fs call makes, cheaply: the folder is
// resolved once and cached (weakly_canonical opens the path, ~50 us a call on
// Windows), and only the leaf is looked at each time, with one lstat; a leaf
// that is a link (or a reparse point) is resolved in full. A folder replaced
// by a link after it was cached is resolved as it was: the cache is dropped
// whenever the grants change, and is no defence against an app racing its
// own checks (the open after a check can race it anyway).
std::mutex g_dirCacheMu;
std::unordered_map<std::string, std::string>& dirCache() {
    static auto* m = new std::unordered_map<std::string, std::string>();
    return *m;
}

std::string checkedPath(const std::string& path) {
    std::error_code ec;
    fs::path abs = fs::absolute(u8path(path), ec);
    if (ec) return canonicalPath(path);
    abs = abs.lexically_normal();
    if (!abs.has_filename()) return canonicalPath(path);
    // Anything but a plain file, a folder or nothing (a link, a junction, a
    // device) is resolved in full.
    const fs::file_type leaf = fs::symlink_status(abs, ec).type();
    if (leaf != fs::file_type::regular && leaf != fs::file_type::directory && leaf != fs::file_type::not_found)
        return canonicalPath(path);
    const std::string dirKey = u8str(abs.parent_path());
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(g_dirCacheMu);
        if (auto it = dirCache().find(dirKey); it != dirCache().end()) dir = it->second;
    }
    if (dir.empty()) {
        dir = canonicalPath(dirKey);
        std::lock_guard<std::mutex> lock(g_dirCacheMu);
        if (dirCache().size() > 4096) dirCache().clear();
        dirCache()[dirKey] = dir;
    }
    std::string name = u8str(abs.filename());
#ifdef _WIN32
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#endif
    return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name;
}

void clearDirCache() {
    std::lock_guard<std::mutex> lock(g_dirCacheMu);
    dirCache().clear();
}

bool pathWithin(const std::string& path, const std::string& root) {
    if (root.empty()) return false;
    if (path.size() < root.size() || path.compare(0, root.size(), root) != 0) return false;
    return path.size() == root.size() || root.back() == '/' || path[root.size()] == '/';
}

bool envAlwaysVisible(std::string_view n) {
    static constexpr std::string_view exact[] = {
        // who and where
        "HOME", "USER", "USERNAME", "LOGNAME", "USERPROFILE", "HOMEDRIVE", "HOMEPATH", "PWD",
        "TMPDIR", "TEMP", "TMP", "APPDATA", "LOCALAPPDATA", "PROGRAMDATA", "PUBLIC",
        "ProgramFiles", "ProgramFiles(x86)", "CommonProgramFiles", "SystemRoot", "SystemDrive", "windir",
        "PATH", "PATHEXT", "SHELL", "COMSPEC", "ComSpec",
        // locale and time
        "LANG", "LANGUAGE", "TZ",
        // the terminal and the desktop
        "TERM", "COLORTERM", "TERM_PROGRAM", "DISPLAY", "WAYLAND_DISPLAY", "DESKTOP_SESSION",
        "XCURSOR_THEME", "XCURSOR_SIZE",
        // the machine
        "OS", "PROCESSOR_ARCHITECTURE", "NUMBER_OF_PROCESSORS", "COMPUTERNAME", "HOSTNAME",
    };
    for (std::string_view e : exact)
#ifdef _WIN32
        if (e.size() == n.size() &&
            std::equal(e.begin(), e.end(), n.begin(), [](char a, char b) { return std::tolower(a) == std::tolower(b); }))
#else
        if (e == n)
#endif
            return true;
    // LC_ALL, LC_CTYPE, ...; XDG_CONFIG_HOME, XDG_SESSION_TYPE, XDG_CURRENT_DESKTOP, ...
    // (XDG_RUNTIME_DIR names a folder of sockets: the files rules judge what
    // is in it); BRO_*, bro's own switches (BRO_APP_HOME, BRO_MODELS), which
    // hold no credential; CI, a test harness's.
    return n.rfind("LC_", 0) == 0 || n.rfind("XDG_", 0) == 0 || n.rfind("BRO_", 0) == 0 || n == "CI";
}

AppPermissions& appPermissions() {
    static AppPermissions s;
    return s;
}

AppPermissions::StoreStamp AppPermissions::stampStore() {
    StoreStamp s;
    std::error_code ec;
    const fs::path file = u8path(grantStorePath());
    const auto t = fs::last_write_time(file, ec);
    s.mtime = ec ? -1 : static_cast<long long>(t.time_since_epoch().count());
    const auto size = fs::file_size(file, ec);
    s.size = ec ? -1 : static_cast<long long>(size);
    return s;
}

void AppPermissions::applyStore(const GrantStoreData& store, const StoreStamp& stamp) {
    decisions_.clear();
    picked_.clear();
    devMode_ = store.devMode;
    if (auto it = store.apps.find(identity_); it != store.apps.end()) {
        decisions_ = it->second.decisions;
        for (const std::string& p : it->second.picked) addRoot(picked_, p);
    }
    store_ = stamp;
}

void AppPermissions::configure(const PermissionsSetup& setup) {
    // An installed app's grants are for the content that was installed
    // (install_record.h): a stat of each file, and a hash only if one moved.
    const std::string identity = appIdentityFor(setup.appDir);
    std::string installHash;
    if (identity.rfind("install:", 0) == 0) {
        const InstallCheck check = checkInstall(setup.appDir);
        installHash = check.hash;
        LOG_INFO("permissions: install record of %s: %s%s (%.2f ms)", identity.c_str(),
                 !check.recorded ? "none" : check.matches ? "matches" : "DIFFERS",
                 check.hashed ? ", content hashed" : "", check.ms);
        std::string err;
        updateGrantStore([&](GrantStoreData& data) {
            auto it = data.apps.find(identity);
            if (it == data.apps.end() || it->second.installHash == installHash) return;
            AppGrantRecord& rec = it->second;
            if (!rec.decisions.empty() || !rec.picked.empty())
                LOG_WARN("permissions: %s is not what its permissions were decided for (installed %s, decided "
                         "for %s): it asks again",
                         identity.c_str(), installHash.c_str(),
                         rec.installHash.empty() ? "(unknown)" : rec.installHash.c_str());
            rec.decisions.clear();
            rec.picked.clear();
            rec.installHash = installHash;
        }, &err);
    }

    // Stamped before it is read: a change landing after the stamp is a change
    // the next poll sees (storeChanged).
    StoreStamp stamp = stampStore();
    bool readOk = true;
    const GrantStoreData store = loadGrantStore(&readOk);
    if (!readOk) stamp = StoreStamp{};  // read again at the first poll
    std::lock_guard<std::mutex> lock(mu_);
    mainThread_ = std::this_thread::get_id();
    appDir_ = setup.appDir;
    appId_ = setup.appId;
    appName_ = setup.appName.empty() ? setup.appId : setup.appName;
    identity_ = identity;
    installHash_ = installHash;
    declared_ = setup.declared;
    hostGrants_.clear();
    for (const std::string& g : setup.hostGrants) {
        if (g == "declared") {
            for (const std::string& d : declared_)
                if (!contains(hostGrants_, d)) hostGrants_.push_back(d);
        } else if (findCapability(g)) {
            if (!contains(hostGrants_, g)) hostGrants_.push_back(g);
        } else {
            LOG_ERROR("permissions: the host grants \"%s\", which is not a capability (they are: %s)",
                      g.c_str(), capabilityNameList().c_str());
        }
    }
    applyStore(store, stamp);
    everGranted_.clear();
    lastUsed_.clear();

    home_ = homeDir().empty() ? std::string() : canonicalPath(homeDir());
    writeRoots_.clear();
    readRoots_.clear();
    protected_.clear();
    std::error_code ec;
    const std::string temp = u8str(fs::temp_directory_path(ec));
    for (const std::string& d : {setup.dirs.config, setup.dirs.data, setup.dirs.cache, setup.dirs.logDir, temp})
        addRoot(writeRoots_, d);
    readRoots_ = writeRoots_;
    addRoot(readRoots_, setup.appDir);
    addRoot(readRoots_, util::resourceDir());
    addRoot(readRoots_, util::engineDir());
    for (const std::string& r : setup.readRoots) addRoot(readRoots_, r);
    // bro's own state: what decides every app's powers, and the code other
    // apps (with other powers) run from.
    // The grant store's file and its temporary siblings (grants.json.<pid>.tmp),
    // and the usage file beside it, not their whole directory: bro's config
    // directory also holds the config directories of apps without an id
    // (apps/<name>), which are theirs.
    storeFile_ = canonicalPath(grantStorePath());
    addRoot(protected_, storeFile_);
    addRoot(protected_, grantUsagePath());
    for (const std::string& root : installedAppRoots()) {
        addRoot(protected_, root);
        addRoot(protected_, u8str(u8path(root).parent_path() / "bin"));
    }
    addRoot(protected_, util::engineDir());
    addRoot(protected_, util::resourceDir());
#ifdef __linux__
    procSelf_ = "/proc/" + std::to_string(static_cast<long long>(getpid()));
#endif
    configured_ = true;

    // Said loudly: each is a grant nobody was asked for.
    std::string hostList;
    for (const std::string& g : hostGrants_) hostList += (hostList.empty() ? "" : ", ") + g;
    if (!hostList.empty())
        LOG_WARN("permissions: the host grants %s: %s", appName_.c_str(), hostList.c_str());
    if (devMode_)
        LOG_WARN("permissions: DEV MODE is on (%s): every app is granted every capability it declares "
                 "(`bro --dev-mode off` to end it)", grantStorePath().c_str());
    LOG_INFO("permissions: %s is %s", appName_.c_str(), identity_.c_str());
}

AppPermissions::Judgement AppPermissions::judge(std::string_view cap) const {
    const Capability* c = findCapability(cap);
    if (!c) return {GrantState::Denied, "there is no such capability"};
    if (contains(hostGrants_, cap)) return {GrantState::Granted, {}};
    if (c->kind == CapabilityKind::Shell)
        return {GrantState::Denied, "it is granted only by the host executable (the desktop shell), never asked for"};
    if (!contains(declared_, cap))
        return {GrantState::Denied, "its bro.json does not declare it in \"permissions\""};
    if (devMode_) return {GrantState::Granted, {}};
    auto it = decisions_.find(std::string(cap));
    if (it != decisions_.end() && it->second == Decision::Allow) return {GrantState::Granted, {}};
    const bool revoked = contains(everGranted_, cap);
    if (it != decisions_.end() && it->second == Decision::Deny)
        return {GrantState::Denied, revoked ? "it was revoked while the app ran" : "it was denied"};
    return {GrantState::Undecided,
            revoked ? "it was revoked while the app ran (it asks again)" : "it has not been granted yet"};
}

bool AppPermissions::hostGranted(std::string_view cap) const {
    std::lock_guard<std::mutex> lock(mu_);
    return contains(hostGrants_, cap);
}

GrantState AppPermissions::state(std::string_view cap) const {
    std::lock_guard<std::mutex> lock(mu_);
    return judge(cap).state;
}

std::string AppPermissions::refusal(std::string_view cap, const std::string& what, const std::string& reason) const {
    const Capability* c = findCapability(cap);
    std::string s = appName_ + " may not " + (c ? c->summary : "do that") + " (" + std::string(cap) +
                    "): " + reason;
    if (!what.empty()) s += ", so " + what + " is refused";
    // Granted earlier in this run: the program's error is a PermissionRevoked.
    if (contains(everGranted_, cap)) return std::string(bronze::embed::kPermissionRevokedPrefix) + s;
    return s;
}

void AppPermissions::persist(std::string_view cap, Decision d) {
    const std::string capName(cap);
    std::string err;
    const bool ok = updateGrantStore([&](GrantStoreData& data) {
        AppGrantRecord& rec = data.apps[identity_];
        rec.id = appId_;
        rec.dir = appDir_;
        rec.installHash = installHash_;
        rec.decisions[capName] = d;
    }, &err);
    if (!ok) LOG_WARN("permissions: %s; the answer holds for this run only", err.c_str());
}

void AppPermissions::noteUse(std::string_view cap) {
    const std::string key(cap);
    if (!contains(everGranted_, key)) everGranted_.push_back(key);
    // A host's grant is the host's business, not the user's to review.
    if (contains(hostGrants_, cap)) return;
    auto [it, fresh] = lastUsed_.try_emplace(key, 0.0);
    it->second = epochMs();
    usagePending_ = true;
    if (fresh) usageUrgent_ = true;
}

std::string AppPermissions::use(std::string_view cap, const std::string& what, bool mayAsk) {
    std::unique_lock<std::mutex> lock(mu_);
    if (!configured_) return {};   // no app (a bare engine, a tool): nothing to protect
    Judgement j = judge(cap);
    if (j.state == GrantState::Granted) {
        noteUse(cap);
        return {};
    }
    if (j.state == GrantState::Denied) return refusal(cap, what, j.reason);
    if (!mayAsk) return refusal(cap, what, j.reason);
    if (std::this_thread::get_id() != mainThread_)
        return refusal(cap, what,
                       "it has not been granted yet, and a Worker cannot ask: ask on the main thread first "
                       "(bro.app.requestPermission('" + std::string(cap) + "'))");
    // The dialog is modal; nothing else takes the lock meanwhile but other
    // threads' checks, which must not wait on the user.
    lock.unlock();
    const Capability* c = findCapability(cap);
    const std::string question = appName_ + " wants to " + c->summary + " (" + std::string(cap) + ").\n\n" +
                                 (what.empty() ? std::string() : "It is trying to: " + what + "\n\n") +
                                 "Allow it? Your answer is remembered for this app.";
    const std::optional<bool> answer = platform::Dialogs::askPermission(question);
    lock.lock();
    // Answered in the meantime (another prompt for the same capability).
    j = judge(cap);
    if (j.state != GrantState::Undecided) {
        if (j.state == GrantState::Granted) {
            noteUse(cap);
            return {};
        }
        return refusal(cap, what, j.reason);
    }
    if (!answer) {
        // Not a decision, so not kept: the next use asks again.
        return refusal(cap, what, "nobody answered the prompt for it");
    }
    const Decision d = *answer ? Decision::Allow : Decision::Deny;
    decisions_[std::string(cap)] = d;
    if (*answer) noteUse(cap);
    const std::string refused = *answer ? std::string() : refusal(cap, what, "it was denied");
    lock.unlock();
    persist(cap, d);
    LOG_INFO("permissions: %s %s %s", appName_.c_str(), *answer ? "was granted" : "was denied", std::string(cap).c_str());
    return refused;
}

bool AppPermissions::request(std::string_view cap) {
    if (state(cap) == GrantState::Undecided && onMainThread()) use(cap, {});
    return granted(cap);
}

bool AppPermissions::underAny(const std::string& p, const std::vector<std::string>& roots) const {
    for (const std::string& r : roots)
        if (pathWithin(p, r)) return true;
    return false;
}

std::string AppPermissions::checkFs(const std::string& path, bool write, bool mayAsk) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!configured_) return {};
    }
    // Resolved outside the lock: no filesystem call is made holding it.
    const std::string p = checkedPath(path);
    {
        std::lock_guard<std::mutex> lock(mu_);
        // Linux's view of processes as files: this process's own memory and
        // state are bro's (they hold every app's secrets in a shell, and
        // writing them is running any code); any process's environment is
        // `env`'s, the same as process.env.
        std::string pid, entry;
        if (procEntry(p, pid, entry)) {
            const bool self = pid == "self" || pid == "thread-self" || "/proc/" + pid == procSelf_;
            const bool memory = entry == "mem" || (entry.rfind("task/", 0) == 0 && entry.ends_with("/mem"));
            if (write && self)
                return appName_ + " may not write " + p + ": it is this process's own state, which no app writes";
            if (memory)
                return appName_ + " may not read " + p + ": a process's memory is never an app's to read";
        }
        if (write) {
            const bool storeSibling = !storeFile_.empty() && p.compare(0, storeFile_.size(), storeFile_) == 0;
            for (const std::string& r : protected_) {
                // Within one, or above one (removing or renaming an ancestor
                // takes it along).
                if (storeSibling || pathWithin(p, r) || pathWithin(r, p))
                    return appName_ + " may not write " + canonicalDisplay(path) +
                           ": it is bro's own state (grants, installed apps or the engine), which no app writes";
            }
            if (underAny(p, writeRoots_) || underAny(p, picked_)) return {};
        } else {
            std::string pid2, entry2;
            const bool environFile =
                procEntry(p, pid2, entry2) && (entry2 == "environ" || entry2.ends_with("/environ"));
            if (!environFile) {
                if (home_.empty() || !pathWithin(p, home_)) return {};
                if (underAny(p, readRoots_) || underAny(p, picked_)) return {};
            }
        }
    }
    {
        std::string pid, entry;
        if (!write && procEntry(p, pid, entry) && (entry == "environ" || entry.ends_with("/environ")))
            return use("env", "read " + p, mayAsk);
    }
    const std::string what = std::string(write ? "writing " : "reading ") + canonicalDisplay(path);
    const std::string refused = use("files", what, mayAsk);
    if (refused.empty()) return {};
    return refused + (write ? " (without it an app writes only its own folders, the temp folder and files "
                              "picked in a dialog)"
                            : " (without it an app reads only its own folders and files picked in a dialog "
                              "in your home folder)");
}

bool AppPermissions::ownsConfChange(std::string_view subject) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (appId_.empty()) return false;
    for (std::string_view verb : {std::string_view("set "), std::string_view("reset ")}) {
        if (subject.substr(0, verb.size()) != verb) continue;
        const std::string_view rest = subject.substr(verb.size());
        if (rest.size() > appId_.size() && rest.substr(0, appId_.size()) == appId_ && rest[appId_.size()] == '.')
            return true;
    }
    return false;
}

std::string AppPermissions::checkEnvRead(const std::string& name, bool mayAsk) {
    if (!name.empty() && envAlwaysVisible(name)) return {};
    return use("env", name.empty() ? std::string("list the environment variables") : "read $" + name, mayAsk);
}

std::string AppPermissions::checkEnvWrite(const std::string& name) {
    // Even a variable every app may read: changing where the standard folders
    // are, or PATH, changes what bro itself and every library in the process
    // find.
    return use("env", "set $" + name, /*mayAsk=*/false);
}

void AppPermissions::grantPicked(const std::vector<std::string>& paths) {
    std::vector<std::string> added;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!configured_) return;
        for (const std::string& path : paths) {
            if (path.empty()) continue;
            const std::string c = canonicalPath(path);
            if (contains(picked_, c)) continue;
            picked_.push_back(c);
            added.push_back(canonicalDisplay(path));
        }
    }
    if (added.empty()) return;
    std::string err;
    const bool ok = updateGrantStore([&](GrantStoreData& data) {
        AppGrantRecord& rec = data.apps[identity_];
        rec.id = appId_;
        rec.dir = appDir_;
        rec.installHash = installHash_;
        for (const std::string& a : added)
            if (!contains(rec.picked, a)) rec.picked.push_back(a);
    }, &err);
    // Our own write is read back at the next poll like anyone's (stamping it
    // here could pass over another process's write landing just after), and
    // changes nothing.
    if (!ok) LOG_WARN("permissions: %s; the picked files are the app's for this run only", err.c_str());
}

bool AppPermissions::storeChanged() {
    const StoreStamp now = stampStore();
    std::lock_guard<std::mutex> lock(mu_);
    return configured_ && (now.mtime != store_.mtime || now.size != store_.size);
}

bool AppPermissions::reload() {
    // Stamped before it is read (see configure); a store caught mid-replace
    // is read again at the next poll, the copy in hand kept meanwhile.
    const StoreStamp stamp = stampStore();
    bool readOk = true;
    const GrantStoreData store = loadGrantStore(&readOk);
    if (!readOk) return false;
    clearDirCache();
    std::lock_guard<std::mutex> lock(mu_);
    if (!configured_) return false;
    // What every capability and the picked paths were, to say what changed.
    std::vector<std::string> all = declared_;
    std::map<std::string, GrantState> before;
    for (const std::string& cap : all) before[cap] = judge(cap).state;
    const std::vector<std::string> pickedBefore = picked_;
    applyStore(store, stamp);
    bool changed = picked_ != pickedBefore;
    for (const std::string& cap : all) {
        const GrantState now = judge(cap).state;
        if (now == before[cap]) continue;
        changed = true;
        if (before[cap] == GrantState::Granted) {
            if (!contains(everGranted_, cap)) everGranted_.push_back(cap);
            LOG_WARN("permissions: %s's %s was revoked while it runs", appName_.c_str(), cap.c_str());
        } else if (now == GrantState::Granted) {
            LOG_INFO("permissions: %s was granted %s while it runs", appName_.c_str(), cap.c_str());
        }
    }
    return changed;
}

void AppPermissions::installChanged(const std::string& actualHash) {
    std::string identity;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!configured_ || actualHash == installHash_) return;
        LOG_WARN("permissions: %s's installed files are not what was installed (%s, recorded %s): "
                 "its permissions are taken back and it asks again",
                 appName_.c_str(), actualHash.c_str(), installHash_.c_str());
        installHash_ = actualHash;
        identity = identity_;
    }
    std::string err;
    updateGrantStore([&](GrantStoreData& data) {
        auto it = data.apps.find(identity);
        if (it == data.apps.end()) return;
        it->second.decisions.clear();
        it->second.picked.clear();
        it->second.installHash = actualHash;
    }, &err);
    reload();
}

void AppPermissions::flushUsage(bool force, double minIntervalMs) {
    std::map<std::string, double> pending;
    std::string identity;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!configured_ || !usagePending_) return;
        const double now = epochMs();
        if (!force && !usageUrgent_ && now - usageWrittenAt_ < minIntervalMs) return;
        pending = lastUsed_;
        identity = identity_;
        usagePending_ = usageUrgent_ = false;
        usageWrittenAt_ = now;
    }
    std::string err;
    if (!mergeGrantUsage(identity, pending, &err)) LOG_WARN("permissions: last use not recorded: %s", err.c_str());
}

PermissionsReport AppPermissions::report() const {
    std::lock_guard<std::mutex> lock(mu_);
    PermissionsReport r;
    r.declared = declared_;
    // Every capability the app has or could have: the declared ones and the
    // host's grants.
    std::vector<std::string> all = declared_;
    for (const std::string& g : hostGrants_)
        if (!contains(all, g)) all.push_back(g);
    for (const std::string& cap : all) {
        switch (judge(cap).state) {
            case GrantState::Granted: r.granted.push_back(cap); break;
            case GrantState::Denied: r.denied.push_back(cap); break;
            case GrantState::Undecided: r.undecided.push_back(cap); break;
        }
    }
    return r;
}

}  // namespace bro::engine
