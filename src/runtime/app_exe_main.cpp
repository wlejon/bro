// bro_run_app: an installed app's own executable (engine/app_exe.h) calls in
// here with the identity `bro --install` stamped into it.
//
// An executable built from this app folder against this engine build runs
// the app in this process, as itself: the process is named for the app, and
// bro.app.executable is the file. One that is out of date (the engine was
// rebuilt, or the app's bro.json or icon changed) starts a rebuild of itself
// in the background and hands the launch to the stock bro beside the engine,
// as a folder, so the app always starts; the next launch runs the fresh one.

#include "runtime/bro_entry.h"

#include "engine/app_exe.h"
#include "engine/app_manifest.h"
#include "runtime/runtime_mains.h"
#include "util/process_spawn.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <climits>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string selfPath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    const std::u8string u = fs::path(std::wstring(buf, n)).u8string();
    return std::string(u.begin(), u.end());
#else
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return {};
    buf[len] = '\0';
    return buf;
#endif
}

// The app's arguments, UTF-8: everything after the program name.
std::vector<std::string> appArguments(int argc, char** argv) {
    std::vector<std::string> out;
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (w) {
        for (int i = 1; i < n; ++i) {
            int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(size_t(len > 0 ? len - 1 : 0), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
            out.push_back(std::move(s));
        }
        LocalFree(w);
    }
#else
    for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
#endif
    return out;
}

void report(const std::string& msg) {
    std::fprintf(stderr, "%s\n", msg.c_str());
#ifdef _WIN32
    OutputDebugStringA((msg + "\n").c_str());
#endif
}

}  // namespace

extern "C" BRO_API int bro_run_app(int argc, char** argv, const char* identity, size_t identityLen) {
    using namespace bro::engine;
    AppExeIdentity id;
    if (!identity || !parseAppExeIdentity(std::string_view(identity, identityLen), id)) {
        report("bro: this executable carries no app identity (`bro --install <app>` builds one)");
        return 2;
    }
    const std::string self = selfPath();
    std::error_code ec;
    const bool haveApp = fs::exists(u8path(id.app) / "bro.json", ec) || fs::exists(u8path(id.app) / "index.html", ec);
    if (!haveApp) {
        report("bro: " + self + ": the app folder " + id.app + " is gone (reinstall " + id.id + ")");
        return 1;
    }

    // Up to date: this engine build, and the folder's bro.json and icon as
    // they were when the executable was made.
    AppDescriptor manifest;
    if (fs::exists(u8path(id.app) / "bro.json", ec)) {
        const std::u8string json = (u8path(id.app) / "bro.json").u8string();
        parseAppManifest(std::string(json.begin(), json.end()), manifest, nullptr);
    }
    const char* stale = nullptr;
    if (id.engine != engineVersion()) stale = "built against another engine build";
    else if (id.hash != appContentHash(id.app, manifest)) stale = "the app's bro.json or icon changed";

    if (!stale) {
        bro::runtime::AppExeLaunch launch{id.app, self};
        return bro::runtime::runWindowed(argc, argv, &launch);
    }

    // Out of date: rebuild in the background, run the folder under bro now.
    const std::string bro = stockBroPath();
    report("bro: " + self + " is out of date (" + stale + "); running " + id.app + " under " + bro +
           " while it is rebuilt");
    std::string err;
    if (!bro::util::spawnDetached({bro, "--build-exe", id.app, "--out", self, "--quiet"}, {}, err))
        report("bro: cannot rebuild " + self + ": " + err);

    std::vector<std::string> cmd{bro, id.app};
    // A toast's activator names the folder after --notification-activated;
    // the folder is bro's target here already, so that copy is dropped.
    bool dropFolder = false;
    for (const std::string& a : appArguments(argc, argv)) {
        if (dropFolder) {
            dropFolder = false;
            if (!a.empty() && a[0] != '-') continue;
        }
        if (a == "--notification-activated") dropFolder = true;
        cmd.push_back(a);
    }
#ifdef _WIN32
    const int status = bro::util::runAndWait(cmd, err);
    if (status >= 0) return status;
#else
    std::vector<char*> av;
    for (auto& a : cmd) av.push_back(a.data());
    av.push_back(nullptr);
    execv(bro.c_str(), av.data());
    err = std::strerror(errno);
#endif
    // No stock bro to hand to: run the folder here, rather than not at all.
    report("bro: cannot start " + bro + " (" + err + "); running " + id.app + " in this process");
    bro::runtime::AppExeLaunch launch{id.app, {}};
    return bro::runtime::runWindowed(argc, argv, &launch);
}
