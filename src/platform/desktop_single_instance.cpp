#include "platform/desktop_single_instance.h"
#include "platform/desktop_platform.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace bro::platform::desktop {

namespace {

std::mutex s_instanceMutex;
std::function<void(const std::vector<std::string>&)> s_callback;
std::deque<std::vector<std::string>> s_pendingArgv;
std::atomic<bool> s_running{false};
std::thread s_serverThread;

#ifdef _WIN32
HANDLE s_pipeHandle = INVALID_HANDLE_VALUE;
std::string s_boundPipeName;
#else
int s_serverFd = -1;
std::string s_boundSocketPath;
#endif

std::string sanitizeName(const std::string& name) {
    std::string safe;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            safe += c;
        } else {
            safe += '_';
        }
    }
    return safe.empty() ? "bro_default" : safe;
}

std::vector<uint8_t> serializeArgs(const std::vector<std::string>& args) {
    std::vector<uint8_t> buf;
    for (const auto& a : args) {
        buf.insert(buf.end(), a.begin(), a.end());
        buf.push_back('\0');
    }
    buf.push_back('\0');  // Double null terminator ends argument list
    return buf;
}

std::vector<std::string> deserializeArgs(const uint8_t* data, size_t len) {
    std::vector<std::string> args;
    std::string current;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '\0') {
            if (current.empty() && !args.empty()) break;
            args.push_back(current);
            current.clear();
        } else {
            current += static_cast<char>(data[i]);
        }
    }
    return args;
}

#ifndef _WIN32
std::string getSocketPath(const std::string& name) {
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    std::string dir = (xdg && *xdg) ? xdg : "/tmp";
    return dir + "/bro_single_" + sanitizeName(name) + ".sock";
}

void serverLoopUnix() {
    while (s_running.load(std::memory_order_relaxed)) {
        if (s_serverFd < 0) break;
        struct pollfd pfd;
        pfd.fd = s_serverFd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int ret = poll(&pfd, 1, 100);
        if (ret <= 0) {
            continue;
        }
        if (!(pfd.revents & POLLIN)) {
            continue;
        }
        int clientFd = accept(s_serverFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (!s_running.load(std::memory_order_relaxed)) break;
            continue;
        }

        std::vector<uint8_t> buf;
        uint8_t chunk[512];
        ssize_t n = 0;
        while ((n = read(clientFd, chunk, sizeof(chunk))) > 0) {
            buf.insert(buf.end(), chunk, chunk + n);
            if (buf.size() >= 2 && buf[buf.size() - 1] == '\0' && buf[buf.size() - 2] == '\0') {
                break;
            }
        }

        auto args = deserializeArgs(buf.data(), buf.size());
        {
            std::lock_guard<std::mutex> lock(s_instanceMutex);
            s_pendingArgv.push_back(std::move(args));
        }

        // Send 1-byte ack
        char ack = 1;
        (void)write(clientFd, &ack, 1);
        close(clientFd);
    }
}
#else
void serverLoopWin(const std::string& pipeName) {
    while (s_running.load(std::memory_order_relaxed)) {
        HANDLE pipe = CreateNamedPipeA(
            pipeName.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 4096, 4096, 0, NULL
        );
        if (pipe == INVALID_HANDLE_VALUE) break;

        if (ConnectNamedPipe(pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
            std::vector<uint8_t> buf;
            uint8_t chunk[512];
            DWORD readBytes = 0;
            while (ReadFile(pipe, chunk, sizeof(chunk), &readBytes, NULL) && readBytes > 0) {
                buf.insert(buf.end(), chunk, chunk + readBytes);
                if (buf.size() >= 2 && buf[buf.size() - 1] == '\0' && buf[buf.size() - 2] == '\0') {
                    break;
                }
            }

            auto args = deserializeArgs(buf.data(), buf.size());
            {
                std::lock_guard<std::mutex> lock(s_instanceMutex);
                s_pendingArgv.push_back(std::move(args));
            }

            char ack = 1;
            DWORD written = 0;
            WriteFile(pipe, &ack, 1, &written, NULL);
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}
#endif

} // namespace

bool requestSingleInstance(
    const std::string& name,
    const std::vector<std::string>& currentArgs,
    std::function<void(const std::vector<std::string>& args)> onInstanceCallback
) {
    shutdownSingleInstance();

    {
        std::lock_guard<std::mutex> lock(s_instanceMutex);
        s_callback = std::move(onInstanceCallback);
    }

#ifdef _WIN32
    std::string pipeName = "\\\\.\\pipe\\bro_single_" + sanitizeName(name);
    HANDLE hPipe = CreateFileA(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hPipe != INVALID_HANDLE_VALUE) {
        auto wire = serializeArgs(currentArgs);
        DWORD written = 0;
        WriteFile(hPipe, wire.data(), static_cast<DWORD>(wire.size()), &written, NULL);
        char ack = 0;
        DWORD readBytes = 0;
        ReadFile(hPipe, &ack, 1, &readBytes, NULL);
        CloseHandle(hPipe);
        return false;
    }

    s_boundPipeName = pipeName;
    s_running.store(true, std::memory_order_relaxed);
    s_serverThread = std::thread(serverLoopWin, pipeName);
    return true;
#else
    std::string sockPath = getSocketPath(name);

    int clientFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (clientFd >= 0) {
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(clientFd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            auto wire = serializeArgs(currentArgs);
            (void)write(clientFd, wire.data(), wire.size());
            char ack = 0;
            (void)read(clientFd, &ack, 1);
            close(clientFd);
            return false;
        }
        close(clientFd);
    }

    // Connect failed: no existing active instance, bind server
    unlink(sockPath.c_str());

    s_serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s_serverFd < 0) return true;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(s_serverFd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(s_serverFd);
        s_serverFd = -1;
        return true;
    }

    if (listen(s_serverFd, 5) != 0) {
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
    if (!s_running.exchange(false, std::memory_order_relaxed)) return;

#ifdef _WIN32
    if (!s_boundPipeName.empty()) {
        HANDLE h = CreateFileA(s_boundPipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        s_boundPipeName.clear();
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
#endif

    if (s_serverThread.joinable()) {
        s_serverThread.join();
    }

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
