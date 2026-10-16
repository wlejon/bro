#include "util/process_spawn.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace bro::util {

#ifdef _WIN32
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// One argument as CommandLineToArgvW reads it back.
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}

}  // namespace

std::wstring windowsCommandLine(const std::vector<std::string>& argv) {
    std::wstring cmd;
    for (const auto& a : argv) {
        if (!cmd.empty()) cmd += L' ';
        cmd += quoteArg(widen(a));
    }
    return cmd;
}

bool spawnDetached(const std::vector<std::string>& argv, const std::string& cwd, std::string& err) {
    std::wstring cmd = windowsCommandLine(argv);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring wcwd = widen(cwd);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT, nullptr,
                        wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi)) {
        err = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

int runAndWait(const std::vector<std::string>& argv, std::string& err, const std::string& outputFile) {
    std::wstring cmd = windowsCommandLine(argv);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    HANDLE out = INVALID_HANDLE_VALUE;
    HANDLE in = INVALID_HANDLE_VALUE;
    if (!outputFile.empty()) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        out = CreateFileW(widen(outputFile).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        if (out == INVALID_HANDLE_VALUE || in == INVALID_HANDLE_VALUE) {
            err = "cannot open " + outputFile + " (" + std::to_string(GetLastError()) + ")";
            if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
            if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
            return -1;
        }
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = out;
        si.hStdError = out;
        si.hStdInput = in;
    }
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT,
                                        nullptr, nullptr, &si, &pi);
    const DWORD startError = GetLastError();
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if (!started) {
        err = "CreateProcess failed (" + std::to_string(startError) + ")";
        return -1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}
#else
bool spawnDetached(const std::vector<std::string>& argv, const std::string& cwd, std::string& err) {
    if (argv.empty()) { err = "nothing to run"; return false; }
    // Everything the child touches is built before fork: between fork and
    // exec only async-signal-safe calls are allowed in a threaded process.
    std::vector<char*> av;
    for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    const char* dir = cwd.empty() ? nullptr : cwd.c_str();
    pid_t pid = fork();
    if (pid < 0) { err = "fork failed"; return false; }
    if (pid == 0) {
        // The grandchild is reparented to init: no zombie for us to reap, and
        // its own session so closing this app does not take it down.
        setsid();
        pid_t again = fork();
        if (again == 0) {
            if (dir && chdir(dir) != 0) { /* keep the inherited cwd */ }
            execv(av[0], av.data());
            _exit(127);
        }
        _exit(again < 0 ? 1 : 0);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { err = "fork failed"; return false; }
    return true;
}

int runAndWait(const std::vector<std::string>& argv, std::string& err, const std::string& outputFile) {
    if (argv.empty()) { err = "nothing to run"; return -1; }
    std::vector<char*> av;
    for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    // Opened before fork, for the reason spawnDetached gives.
    int out = -1, in = -1;
    if (!outputFile.empty()) {
        out = ::open(outputFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        in = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (out < 0 || in < 0) {
            err = "cannot open " + outputFile;
            if (out >= 0) ::close(out);
            if (in >= 0) ::close(in);
            return -1;
        }
    }
    pid_t pid = fork();
    if (pid < 0) {
        if (out >= 0) ::close(out);
        if (in >= 0) ::close(in);
        err = "fork failed";
        return -1;
    }
    if (pid == 0) {
        if (out >= 0) {
            ::dup2(out, 1);
            ::dup2(out, 2);
            ::dup2(in, 0);
        }
        execv(av[0], av.data());
        _exit(127);
    }
    if (out >= 0) ::close(out);
    if (in >= 0) ::close(in);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}
#endif

}  // namespace bro::util
