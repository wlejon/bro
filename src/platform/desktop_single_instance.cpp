#include "platform/desktop_single_instance.h"
#include "platform/desktop_platform.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

// One channel per name. The first process to claim it OWNS it (a named mutex
// on Windows, an flock'd lock file on POSIX, held for the life of the process)
// and serves it (a named pipe / a Unix socket); a later process finds the
// owner and writes its arguments to the server instead. Ownership is the lock,
// not the socket: a second launch racing the first's startup waits for the
// server to come up rather than taking the name over, and a stale socket left
// by a crash is replaced only by whoever holds the lock.
//
// The wire is length-prefixed so an empty argument survives:
//   u32 count, then per argument u32 length + bytes (little-endian);
// the server answers one byte once the arguments are queued.

namespace bro::platform::desktop {

namespace {

std::mutex s_instanceMutex;
std::function<void(const std::vector<std::string>&)> s_callback;

#ifndef _WIN32
// A Unix stream socket closed across exec. macOS has no SOCK_CLOEXEC.
int cloexecUnixSocket() {
#ifdef SOCK_CLOEXEC
    return socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
#endif
}
#endif
std::deque<std::vector<std::string>> s_pendingArgv;
std::atomic<bool> s_running{false};
std::thread s_serverThread;

#ifdef _WIN32
HANDLE s_ownerMutex = nullptr;
std::wstring s_boundPipeName;
#else
int s_serverFd = -1;
int s_lockFd = -1;
std::string s_boundSocketPath;
#endif

constexpr uint32_t kMaxArgs = 4096;
constexpr uint32_t kMaxWire = 4u << 20;

std::string sanitizeName(const std::string& name) {
    std::string safe;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') {
            safe += c;
        } else {
            safe += '_';
        }
    }
    return safe.empty() ? "bro_default" : safe;
}

void putU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

std::vector<uint8_t> serializeArgs(const std::vector<std::string>& args) {
    std::vector<uint8_t> buf;
    putU32(buf, uint32_t(args.size()));
    for (const auto& a : args) {
        putU32(buf, uint32_t(a.size()));
        buf.insert(buf.end(), a.begin(), a.end());
    }
    return buf;
}

uint32_t getU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// How many bytes the message in `buf` needs in all, or 0 while that is not
// yet known (or -1 for a malformed one).
long long wireLength(const std::vector<uint8_t>& buf) {
    if (buf.size() < 4) return 0;
    uint32_t count = getU32(buf.data());
    if (count > kMaxArgs) return -1;
    size_t pos = 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (buf.size() < pos + 4) return 0;
        uint32_t len = getU32(buf.data() + pos);
        if (len > kMaxWire) return -1;
        pos += 4 + size_t(len);
        if (pos > kMaxWire) return -1;
    }
    return static_cast<long long>(pos);
}

std::vector<std::string> deserializeArgs(const std::vector<uint8_t>& buf) {
    std::vector<std::string> args;
    if (buf.size() < 4) return args;
    uint32_t count = getU32(buf.data());
    size_t pos = 4;
    for (uint32_t i = 0; i < count && pos + 4 <= buf.size(); ++i) {
        uint32_t len = getU32(buf.data() + pos);
        pos += 4;
        if (pos + len > buf.size()) break;
        args.emplace_back(reinterpret_cast<const char*>(buf.data() + pos), len);
        pos += len;
    }
    return args;
}

void queueArgs(std::vector<std::string> args) {
    std::lock_guard<std::mutex> lock(s_instanceMutex);
    s_pendingArgv.push_back(std::move(args));
}

#ifndef _WIN32
std::string runtimeDir() {
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) return xdg;
    return "/tmp";
}

std::string channelBase(const std::string& name) {
    // /tmp is shared between users; XDG_RUNTIME_DIR is not, but the uid in
    // the name costs nothing.
    return runtimeDir() + "/bro_single_" + std::to_string(getuid()) + "_" + sanitizeName(name);
}

bool sendTo(const std::string& sockPath, const std::vector<std::string>& args) {
    int fd = cloexecUnixSocket();
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return false;
    }
    auto wire = serializeArgs(args);
    size_t off = 0;
    while (off < wire.size()) {
        ssize_t n = write(fd, wire.data() + off, wire.size() - off);
        if (n <= 0) { close(fd); return false; }
        off += size_t(n);
    }
    char ack = 0;
    pollfd pfd{fd, POLLIN, 0};
    bool acked = poll(&pfd, 1, 5000) > 0 && read(fd, &ack, 1) == 1;
    close(fd);
    return acked;
}

void serverLoopUnix() {
    while (s_running.load(std::memory_order_relaxed)) {
        if (s_serverFd < 0) break;
        pollfd pfd{s_serverFd, POLLIN, 0};
        if (poll(&pfd, 1, 100) <= 0 || !(pfd.revents & POLLIN)) continue;
        int clientFd = accept(s_serverFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (!s_running.load(std::memory_order_relaxed)) break;
            continue;
        }
        std::vector<uint8_t> buf;
        uint8_t chunk[4096];
        long long need = 0;
        for (;;) {
            pollfd cp{clientFd, POLLIN, 0};
            if (poll(&cp, 1, 2000) <= 0) break;
            ssize_t n = read(clientFd, chunk, sizeof(chunk));
            if (n <= 0) break;
            buf.insert(buf.end(), chunk, chunk + n);
            need = wireLength(buf);
            if (need < 0 || (need > 0 && buf.size() >= size_t(need))) break;
        }
        if (need > 0 && buf.size() >= size_t(need)) {
            queueArgs(deserializeArgs(buf));
            char ack = 1;
            (void)!write(clientFd, &ack, 1);
        }
        close(clientFd);
    }
}
#else
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::wstring channelBase(const std::string& name) {
    // Pipe names are machine-wide: the user's name keeps two users' copies of
    // one app apart.
    wchar_t user[256] = L"";
    DWORD n = 256;
    if (!GetUserNameW(user, &n)) user[0] = L'\0';
    std::wstring u;
    for (const wchar_t* p = user; *p; ++p) u += (iswalnum(*p) ? *p : L'_');
    return L"bro_single_" + u + L"_" + widen(sanitizeName(name));
}

bool sendTo(const std::wstring& pipeName, const std::vector<std::string>& args) {
    HANDLE h = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    auto wire = serializeArgs(args);
    DWORD written = 0;
    bool ok = WriteFile(h, wire.data(), DWORD(wire.size()), &written, nullptr) && written == wire.size();
    char ack = 0;
    DWORD got = 0;
    ok = ok && ReadFile(h, &ack, 1, &got, nullptr) && got == 1;
    CloseHandle(h);
    return ok;
}

HANDLE makePipe(const std::wstring& pipeName) {
    return CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
}

void serverLoopWin(std::wstring pipeName, HANDLE first) {
    HANDLE pipe = first;
    while (s_running.load(std::memory_order_relaxed)) {
        if (pipe == INVALID_HANDLE_VALUE) pipe = makePipe(pipeName);
        if (pipe == INVALID_HANDLE_VALUE) break;
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            // The next instance is listening before this one is read, so a
            // launch never finds no pipe at all.
            HANDLE next = s_running.load(std::memory_order_relaxed) ? makePipe(pipeName) : INVALID_HANDLE_VALUE;
            std::vector<uint8_t> buf;
            uint8_t chunk[4096];
            DWORD got = 0;
            long long need = 0;
            while (ReadFile(pipe, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
                buf.insert(buf.end(), chunk, chunk + got);
                need = wireLength(buf);
                if (need < 0 || (need > 0 && buf.size() >= size_t(need))) break;
            }
            if (s_running.load(std::memory_order_relaxed) && need > 0 && buf.size() >= size_t(need)) {
                queueArgs(deserializeArgs(buf));
                char ack = 1;
                DWORD written = 0;
                WriteFile(pipe, &ack, 1, &written, nullptr);
                FlushFileBuffers(pipe);
            }
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            pipe = next;
        } else {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
    }
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
}
#endif

} // namespace

bool requestSingleInstance(
    const std::string& name,
    const std::vector<std::string>& currentArgs,
    std::function<void(const std::vector<std::string>& args)> onInstanceCallback
) {
    shutdownSingleInstance();

    // A process that leaves through exit() rather than its driver's return
    // (Xlib's fatal IO handler when the display goes away, for one) still
    // gives the channel up: removes the socket, and joins the server thread
    // before its static std::thread is destroyed joinable (terminate()).
    // Registered after the statics here are built, so it runs before they go.
    static const bool releaseAtExit = [] {
        std::atexit([] { shutdownSingleInstance(); });
        return true;
    }();
    (void)releaseAtExit;

    {
        std::lock_guard<std::mutex> lock(s_instanceMutex);
        s_callback = std::move(onInstanceCallback);
    }

#ifdef _WIN32
    const std::wstring base = channelBase(name);
    const std::wstring pipeName = L"\\\\.\\pipe\\" + base;
    const std::wstring mutexName = L"Local\\" + base;

    HANDLE owner = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    const bool exists = owner && GetLastError() == ERROR_ALREADY_EXISTS;
    if (exists) {
        CloseHandle(owner);
        // Someone owns the name: hand the arguments over, waiting up to a few
        // seconds for its server (it may still be starting).
        for (int attempt = 0; attempt < 60; ++attempt) {
            if (sendTo(pipeName, currentArgs)) return false;
            if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(pipeName.c_str(), 100);
            else Sleep(50);
        }
        // An owner that never answers: run as a separate instance rather than
        // not at all.
        return true;
    }
    if (!owner) return true;
    s_ownerMutex = owner;

    HANDLE first = makePipe(pipeName);
    s_boundPipeName = pipeName;
    s_running.store(true, std::memory_order_relaxed);
    s_serverThread = std::thread(serverLoopWin, pipeName, first);
    return true;
#else
    const std::string base = channelBase(name);
    const std::string sockPath = base + ".sock";
    const std::string lockPath = base + ".lock";

    int lockFd = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lockFd >= 0 && flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
        close(lockFd);
        for (int attempt = 0; attempt < 60; ++attempt) {
            if (sendTo(sockPath, currentArgs)) return false;
            usleep(50 * 1000);
        }
        return true;
    }
    s_lockFd = lockFd;  // -1 when the lock file cannot be made: serve unguarded

    // Ours: whatever socket is there was left by a process that is gone.
    unlink(sockPath.c_str());
    s_serverFd = cloexecUnixSocket();
    if (s_serverFd < 0) return true;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(s_serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s_serverFd, 16) != 0) {
        close(s_serverFd);
        s_serverFd = -1;
        unlink(sockPath.c_str());
        return true;
    }

    s_boundSocketPath = sockPath;
    s_running.store(true, std::memory_order_relaxed);
    s_serverThread = std::thread(serverLoopUnix);
    return true;
#endif
}

void shutdownSingleInstance() {
    const bool wasRunning = s_running.exchange(false, std::memory_order_relaxed);

#ifdef _WIN32
    if (wasRunning && !s_boundPipeName.empty()) {
        // Wake the server out of ConnectNamedPipe.
        HANDLE h = CreateFileW(s_boundPipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
#endif
    if (s_serverThread.joinable()) {
        s_serverThread.join();
    }
#ifdef _WIN32
    s_boundPipeName.clear();
    if (s_ownerMutex) {
        CloseHandle(s_ownerMutex);
        s_ownerMutex = nullptr;
    }
#else
    if (s_serverFd >= 0) {
        close(s_serverFd);
        s_serverFd = -1;
    }
    if (!s_boundSocketPath.empty()) {
        unlink(s_boundSocketPath.c_str());
        s_boundSocketPath.clear();
    }
    if (s_lockFd >= 0) {
        close(s_lockFd);  // releases the flock
        s_lockFd = -1;
    }
#endif

    std::lock_guard<std::mutex> lock(s_instanceMutex);
    s_pendingArgv.clear();
}

void pumpSingleInstanceEvents() {
    std::vector<std::vector<std::string>> toDispatch;
    std::function<void(const std::vector<std::string>&)> cb;
    {
        std::lock_guard<std::mutex> lock(s_instanceMutex);
        cb = s_callback;
        while (!s_pendingArgv.empty()) {
            toDispatch.push_back(std::move(s_pendingArgv.front()));
            s_pendingArgv.pop_front();
        }
    }
    if (cb) {
        for (const auto& args : toDispatch) {
            cb(args);
        }
    }
}

bool simulateSingleInstanceMessage(const std::string& /*name*/, const std::vector<std::string>& args) {
    std::lock_guard<std::mutex> lock(s_instanceMutex);
    s_pendingArgv.push_back(args);
    return true;
}

} // namespace bro::platform::desktop
