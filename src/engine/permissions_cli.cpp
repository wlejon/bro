// `bro --permissions`, `--grant-app`, `--revoke-app`, `--dev-mode`: the grant
// store from a terminal (docs/apps.md, Permissions). The user's own hand on
// it, so it may decide any ask capability for any app; a shell capability is
// still the host's alone (decideAppCapability refuses it).

#include "engine/capabilities.h"
#include "engine/grant_store.h"
#include "engine/launcher.h"

#include <cstdio>
#include <filesystem>

namespace bro::engine {

namespace {

namespace fs = std::filesystem;

int permissionsUsage() {
    std::fprintf(stderr,
        "usage: bro --permissions [<id | app-dir>]\n"
        "       bro --grant-app <id | app-dir> <capability>\n"
        "       bro --revoke-app <id | app-dir> <capability>\n"
        "       bro --dev-mode on|off\n"
        "capabilities: %s\n", capabilityNameList().c_str());
    return 2;
}

void printEntry(const AppPermissionsEntry& e) {
    std::printf("%s\t%s\t%s%s\n", e.identity.c_str(), e.id.c_str(), e.dir.c_str(),
                e.installed ? " (installed)" : "");
    // Every declared capability, decided or not, then any decision the
    // manifest no longer declares (kept, but it grants nothing).
    for (const std::string& cap : e.declared) {
        auto it = e.decisions.find(cap);
        const Capability* c = findCapability(cap);
        std::printf("  %-18s %-10s %s\n", cap.c_str(),
                    it == e.decisions.end() ? "undecided" : decisionName(it->second),
                    c && c->kind == CapabilityKind::Shell ? "(host only)" : "");
    }
    for (const auto& [cap, d] : e.decisions) {
        bool declared = false;
        for (const std::string& x : e.declared) declared = declared || x == cap;
        if (!declared) std::printf("  %-18s %-10s (not declared)\n", cap.c_str(), decisionName(d));
    }
    for (const std::string& p : e.picked) std::printf("  picked %s\n", p.c_str());
}

// The identity and folder a command names, or false with a message.
bool target(const std::string& arg, std::string& identity, std::string& dir) {
    const GrantStoreData store = loadGrantStore();
    identity = resolveIdentityArg(arg, store);
    if (identity.empty()) {
        std::fprintf(stderr, "bro: no app '%s' (not a folder, an installed app or one the grant store knows)\n",
                     arg.c_str());
        return false;
    }
    std::error_code ec;
    if (fs::is_directory(fs::path(std::u8string(arg.begin(), arg.end())), ec)) dir = arg;
    else if (std::string installed = findInstalledApp(arg); !installed.empty()) dir = installed;
    return true;
}

}  // namespace

int runPermissionsCommand(const std::vector<std::string>& args) {
    const std::string& cmd = args[0];
    std::string err;
    if (cmd == "--dev-mode") {
        if (args.size() != 2 || (args[1] != "on" && args[1] != "off")) return permissionsUsage();
        if (!setGrantStoreDevMode(args[1] == "on", &err)) {
            std::fprintf(stderr, "bro: %s\n", err.c_str());
            return 1;
        }
        std::printf("dev mode %s: %s\n", args[1].c_str(),
                    args[1] == "on" ? "every app is granted every ask capability it declares"
                                    : "apps have what was decided for them");
        return 0;
    }
    if (cmd == "--permissions") {
        if (args.size() > 2) return permissionsUsage();
        std::printf("grant store %s, dev mode %s\n", grantStorePath().c_str(),
                    loadGrantStore().devMode ? "ON" : "off");
        const std::vector<AppPermissionsEntry> all = listAppPermissions();
        if (args.size() == 1) {
            for (const AppPermissionsEntry& e : all) printEntry(e);
            return 0;
        }
        std::string identity, dir;
        if (!target(args[1], identity, dir)) return 1;
        for (const AppPermissionsEntry& e : all)
            if (e.identity == identity) { printEntry(e); return 0; }
        // A folder the store has never seen: what it would ask for.
        AppPermissionsEntry e;
        e.identity = identity;
        e.dir = dir;
        AppDescriptor m;
        const fs::path json = fs::path(std::u8string(dir.begin(), dir.end())) / "bro.json";
        std::u8string u = json.generic_u8string();
        if (!dir.empty() && parseAppManifest(std::string(u.begin(), u.end()), m, &err)) {
            e.declared = m.permissions;
            e.id = appIdFor(m, dir);
        } else if (!err.empty()) {
            std::fprintf(stderr, "bro: %s\n", err.c_str());
        }
        printEntry(e);
        return 0;
    }
    // --grant-app / --revoke-app
    if (args.size() != 3) return permissionsUsage();
    std::string identity, dir;
    if (!target(args[1], identity, dir)) return 1;
    const Decision d = cmd == "--grant-app" ? Decision::Allow : Decision::Deny;
    if (!decideAppCapability(identity, dir, args[2], d, &err)) {
        std::fprintf(stderr, "bro: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s: %s %s\n", identity.c_str(), args[2].c_str(), decisionName(d));
    return 0;
}

}  // namespace bro::engine
