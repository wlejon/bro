#include "engine/app_permissions.h"

#include "engine/capabilities.h"
#include "platform/dialogs.h"
#include "util/exe_dir.h"
#include "util/log.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
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

}  // namespace

std::string canonicalPath(const std::string& path) {
    std::string s = canonicalDisplay(path);
#ifdef _WIN32
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#endif
    return s;
}

bool pathWithin(const std::string& path, const std::string& root) {
    if (root.empty()) return false;
    if (path.size() < root.size() || path.compare(0, root.size(), root) != 0) return false;
    return path.size() == root.size() || root.back() == '/' || path[root.size()] == '/';
}

AppPermissions& appPermissions() {
    static AppPermissions s;
    return s;
}

void AppPermissions::configure(const PermissionsSetup& setup) {
    const GrantStoreData store = loadGrantStore();
    std::lock_guard<std::mutex> lock(mu_);
    mainThread_ = std::this_thread::get_id();
    appDir_ = setup.appDir;
    appId_ = setup.appId;
    appName_ = setup.appName.empty() ? setup.appId : setup.appName;
    identity_ = appIdentityFor(setup.appDir);
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
    decisions_.clear();
    picked_.clear();
    devMode_ = store.devMode;
    if (auto it = store.apps.find(identity_); it != store.apps.end()) {
        decisions_ = it->second.decisions;
        for (const std::string& p : it->second.picked) addRoot(picked_, p);
    }

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
    // not its whole directory: bro's config directory also holds the config
    // directories of apps without an id (apps/<name>), which are theirs.
    storeFile_ = canonicalPath(grantStorePath());
    addRoot(protected_, storeFile_);
    for (const std::string& root : installedAppRoots()) {
        addRoot(protected_, root);
        addRoot(protected_, u8str(u8path(root).parent_path() / "bin"));
    }
    addRoot(protected_, util::engineDir());
    addRoot(protected_, util::resourceDir());
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
    if (it != decisions_.end() && it->second == Decision::Deny) return {GrantState::Denied, "it was denied"};
    return {GrantState::Undecided, "it has not been granted yet"};
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
    return s;
}

void AppPermissions::persist(std::string_view cap, Decision d) {
    const std::string capName(cap);
    std::string err;
    const bool ok = updateGrantStore([&](GrantStoreData& data) {
        AppGrantRecord& rec = data.apps[identity_];
        rec.id = appId_;
        rec.dir = appDir_;
        rec.decisions[capName] = d;
    }, &err);
    if (!ok) LOG_WARN("permissions: %s; the answer holds for this run only", err.c_str());
}

std::string AppPermissions::use(std::string_view cap, const std::string& what) {
    std::unique_lock<std::mutex> lock(mu_);
    if (!configured_) return {};   // no app (a bare engine, a tool): nothing to protect
    Judgement j = judge(cap);
    if (j.state == GrantState::Granted) return {};
    if (j.state == GrantState::Denied) return refusal(cap, what, j.reason);
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
        return j.state == GrantState::Granted ? std::string() : refusal(cap, what, j.reason);
    }
    if (!answer) {
        // Not a decision, so not kept: the next use asks again.
        return refusal(cap, what, "nobody answered the prompt for it");
    }
    const Decision d = *answer ? Decision::Allow : Decision::Deny;
    decisions_[std::string(cap)] = d;
    lock.unlock();
    persist(cap, d);
    LOG_INFO("permissions: %s %s %s", appName_.c_str(), *answer ? "was granted" : "was denied", std::string(cap).c_str());
    return *answer ? std::string() : refusal(cap, what, "it was denied");
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

std::string AppPermissions::checkFs(const std::string& path, bool write) {
    std::string p;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!configured_) return {};
        p = canonicalPath(path);
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
            if (home_.empty() || !pathWithin(p, home_)) return {};
            if (underAny(p, readRoots_) || underAny(p, picked_)) return {};
        }
    }
    const std::string what = std::string(write ? "writing " : "reading ") + canonicalDisplay(path);
    const std::string refused = use("files", what);
    if (refused.empty()) return {};
    return refused + (write ? " (without it an app writes only its own folders, the temp folder and files "
                              "picked in a dialog)"
                            : " (without it an app reads only its own folders and files picked in a dialog "
                              "in your home folder)");
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
        for (const std::string& a : added)
            if (!contains(rec.picked, a)) rec.picked.push_back(a);
    }, &err);
    if (!ok) LOG_WARN("permissions: %s; the picked files are the app's for this run only", err.c_str());
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
