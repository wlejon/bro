// bro-app-stub: the executable every installed app's own is a copy of
// (engine/app_exe.h, docs/apps.md "The app's executable").
//
// It is built once with bro and shipped beside the engine library. `bro
// --install` copies it, names the copy for the app, and stamps the identity
// block below in place (and on Windows its icon and version resources). It
// links nothing of bro's: it loads the engine library the block names and
// calls bro_run_app with the block, which runs the app in this process.
//
// Loading the library at run time (rather than linking it) is what lets the
// copy live anywhere — with the app, not beside bro — and still find the one
// engine every bro process maps. When the library is gone or speaks another
// entry ABI, the stub runs the app as a folder under the stock bro the block
// names, so an app's executable never fails to start for want of the
// matching engine.
//
// Nothing here may grow a second copy of the marker: the installer finds the
// block by searching the file for it and refuses a file with two.

#include "engine/app_exe.h"
#include "runtime/bro_entry.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
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
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace {

constexpr size_t kCapacity = bro::engine::kAppExeIdentityCapacity;
constexpr size_t kMarker = bro::engine::kAppExeMarkerSize;

// The block. volatile: the compiler may not assume it still holds what it
// was compiled with, because the installer rewrites it in the file.
volatile const char kIdentity[kCapacity] = "BRO\x01" "APP-IDENTITY-BLOCK-v1\x01......";

constexpr bool sameMarker(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return false;
    return true;
}
static_assert(sameMarker("BRO\x01" "APP-IDENTITY-BLOCK-v1\x01......", bro::engine::kAppExeMarker, kMarker),
              "the stub's marker must be app_exe.h's");

using AbiFn = uint32_t (*)();
using AppMainFn = int (*)(int, char**, const char*, size_t);

std::string field(const std::string& text, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        if (text.compare(pos, k.size(), k) == 0) return text.substr(pos + k.size(), end - pos - k.size());
        pos = end + 1;
    }
    return {};
}

#ifdef _WIN32
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') out.append(bs * 2 + 1, L'\\');
        else out.append(bs, L'\\');
        bs = 0;
        out += c;
    }
    out.append(bs * 2, L'\\');
    return out + L"\"";
}

void fail(const std::string& msg) {
    std::fprintf(stderr, "%s\n", msg.c_str());
    MessageBoxW(nullptr, wide(msg).c_str(), L"bro", MB_OK | MB_ICONERROR);
}
#else
void fail(const std::string& msg) { std::fprintf(stderr, "%s\n", msg.c_str()); }
#endif

}  // namespace

int main(int argc, char** argv) {
    char block[kCapacity];
    for (size_t i = 0; i < kCapacity; ++i) block[i] = kIdentity[i];
    block[kCapacity - 1] = '\0';
    const std::string text(block + kMarker);
    if (text.empty()) {
        fail("bro-app-stub: this is the template app executables are made from; `bro --install <app>` builds one");
        return 2;
    }
    const std::string libbro = field(text, "libbro");
    const std::string bro = field(text, "bro");
    const std::string app = field(text, "app");

    std::string why;
#ifdef _WIN32
    // An absolute path with LOAD_WITH_ALTERED_SEARCH_PATH: the library's own
    // dependencies (bronze's runtime) are found beside it.
    HMODULE lib = LoadLibraryExW(wide(libbro).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (lib) {
        auto abi = reinterpret_cast<AbiFn>(GetProcAddress(lib, "bro_entry_abi"));
        auto appMain = reinterpret_cast<AppMainFn>(GetProcAddress(lib, "bro_run_app"));
        if (abi && appMain && abi() == BRO_ENTRY_ABI) return appMain(argc, argv, text.c_str(), text.size());
        why = "it speaks another entry ABI";
    } else {
        why = "it did not load (error " + std::to_string(GetLastError()) + ")";
    }
    // The folder under the stock bro, this launch's arguments after it.
    std::wstring cmd = quote(wide(bro)) + L" " + quote(wide(app));
    int n = 0;
    if (LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 1; i < n; ++i) cmd += L" " + quote(w[i]);
        LocalFree(w);
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        return static_cast<int>(code);
    }
    fail("bro: cannot use the engine " + libbro + " (" + why + ") or start " + bro +
         " (error " + std::to_string(GetLastError()) + "). Reinstall the app with `bro --install`.");
    return 127;
#else
    // RTLD_GLOBAL: the engine's symbols are the process's, as when an
    // executable links it.
    void* lib = dlopen(libbro.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (lib) {
        auto abi = reinterpret_cast<AbiFn>(dlsym(lib, "bro_entry_abi"));
        auto appMain = reinterpret_cast<AppMainFn>(dlsym(lib, "bro_run_app"));
        if (abi && appMain && abi() == BRO_ENTRY_ABI) return appMain(argc, argv, text.c_str(), text.size());
        why = "it speaks another entry ABI";
    } else {
        const char* e = dlerror();
        why = e ? e : "it did not load";
    }
    std::vector<std::string> args{bro, app};
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    std::vector<char*> av;
    for (auto& a : args) av.push_back(a.data());
    av.push_back(nullptr);
    std::fprintf(stderr, "bro: cannot use the engine %s (%s); running %s under %s\n", libbro.c_str(), why.c_str(),
                 app.c_str(), bro.c_str());
    execv(bro.c_str(), av.data());
    fail("bro: cannot start " + bro + " (" + std::strerror(errno) + "). Reinstall the app with `bro --install`.");
    return 127;
#endif
}
