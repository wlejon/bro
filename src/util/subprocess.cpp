#include "util/subprocess.h"
#include "util/log.h"

#include <cstdlib>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>

namespace bro::util {

bool hasExecutableOnPath(const std::string& name) {
    if (name.empty()) return false;
    char buffer[MAX_PATH];
    DWORD res = SearchPathA(nullptr, name.c_str(), ".exe", MAX_PATH, buffer, nullptr);
    return res > 0 && res < MAX_PATH;
}

SubprocessResult runSubprocess(const std::vector<std::string>& args,
                              const std::string& inputStdin) {
    SubprocessResult result;
    if (args.empty()) return result;

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = nullptr;

    HANDLE inRd = NULL, inWr = NULL;
    HANDLE outRd = NULL, outWr = NULL;
    HANDLE errRd = NULL, errWr = NULL;

    if (!CreatePipe(&inRd, &inWr, &saAttr, 0) ||
        !CreatePipe(&outRd, &outWr, &saAttr, 0) ||
        !CreatePipe(&errRd, &errWr, &saAttr, 0)) {
        if (inRd) CloseHandle(inRd); if (inWr) CloseHandle(inWr);
        if (outRd) CloseHandle(outRd); if (outWr) CloseHandle(outWr);
        if (errRd) CloseHandle(errRd); if (errWr) CloseHandle(errWr);
        return result;
    }

    SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);

    // Build command line with proper argument quoting
    std::string cmdLine;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) cmdLine += " ";
        const std::string& a = args[i];
        bool needQuote = a.find(' ') != std::string::npos || a.find('\t') != std::string::npos || a.empty();
        if (needQuote) cmdLine += "\"";
        cmdLine += a;
        if (needQuote) cmdLine += "\"";
    }

    STARTUPINFOA si{};
    si.cb = sizeof(STARTUPINFOA);
    si.hStdInput = inRd;
    si.hStdOutput = outWr;
    si.hStdError = errWr;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    std::vector<char> cmdVec(cmdLine.begin(), cmdLine.end());
    cmdVec.push_back('\0');

    BOOL created = CreateProcessA(
        nullptr, cmdVec.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    CloseHandle(inRd);
    CloseHandle(outWr);
    CloseHandle(errWr);

    if (!created) {
        CloseHandle(inWr);
        CloseHandle(outRd);
        CloseHandle(errRd);
        return result;
    }

    if (!inputStdin.empty()) {
        DWORD written = 0;
        WriteFile(inWr, inputStdin.data(), static_cast<DWORD>(inputStdin.size()), &written, nullptr);
    }
    CloseHandle(inWr);

    char buf[4096];
    DWORD readBytes = 0;
    while (ReadFile(outRd, buf, sizeof(buf), &readBytes, nullptr) && readBytes > 0) {
        result.stdOut.insert(result.stdOut.end(), buf, buf + readBytes);
    }
    CloseHandle(outRd);

    while (ReadFile(errRd, buf, sizeof(buf), &readBytes, nullptr) && readBytes > 0) {
        result.stdErr.append(buf, readBytes);
    }
    CloseHandle(errRd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    if (GetExitCodeProcess(pi.hProcess, &exitCode)) {
        result.exitCode = static_cast<int>(exitCode);
        result.success = (result.exitCode == 0);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return result;
}

} // namespace bro::util

#else // POSIX

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

extern char** environ;

namespace bro::util {

bool hasExecutableOnPath(const std::string& name) {
    if (name.empty()) return false;
    if (name.find('/') != std::string::npos) {
        return access(name.c_str(), X_OK) == 0;
    }
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return false;
    std::stringstream ss(pathEnv);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) dir = ".";
        std::string full = dir + "/" + name;
        if (access(full.c_str(), X_OK) == 0) {
            return true;
        }
    }
    return false;
}

SubprocessResult runSubprocess(const std::vector<std::string>& args,
                              const std::string& inputStdin) {
    SubprocessResult result;
    if (args.empty()) return result;

    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};

    if (pipe(inPipe) != 0 || pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        if (inPipe[0] >= 0) { close(inPipe[0]); close(inPipe[1]); }
        if (outPipe[0] >= 0) { close(outPipe[0]); close(outPipe[1]); }
        if (errPipe[0] >= 0) { close(errPipe[0]); close(errPipe[1]); }
        return result;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);

    posix_spawn_file_actions_addclose(&actions, inPipe[0]);
    posix_spawn_file_actions_addclose(&actions, inPipe[1]);
    posix_spawn_file_actions_addclose(&actions, outPipe[0]);
    posix_spawn_file_actions_addclose(&actions, outPipe[1]);
    posix_spawn_file_actions_addclose(&actions, errPipe[0]);
    posix_spawn_file_actions_addclose(&actions, errPipe[1]);

    std::vector<char*> argv;
    for (const auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    int spawnRes = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);

    close(inPipe[0]);
    close(outPipe[1]);
    close(errPipe[1]);

    if (spawnRes != 0) {
        close(inPipe[1]);
        close(outPipe[0]);
        close(errPipe[0]);
        result.exitCode = spawnRes;
        return result;
    }

    fcntl(inPipe[1], F_SETFL, O_NONBLOCK);
    fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(errPipe[0], F_SETFL, O_NONBLOCK);

    size_t inWritten = 0;
    bool inClosed = inputStdin.empty();
    if (inClosed) {
        close(inPipe[1]);
        inPipe[1] = -1;
    }

    bool outClosed = false;
    bool errClosed = false;
    char buf[4096];

    while (!outClosed || !errClosed || !inClosed) {
        struct pollfd pfd[3];
        int nfds = 0;
        int inIdx = -1, outIdx = -1, errIdx = -1;

        if (!inClosed && inPipe[1] >= 0) {
            inIdx = nfds;
            pfd[nfds].fd = inPipe[1];
            pfd[nfds].events = POLLOUT;
            pfd[nfds].revents = 0;
            nfds++;
        }
        if (!outClosed && outPipe[0] >= 0) {
            outIdx = nfds;
            pfd[nfds].fd = outPipe[0];
            pfd[nfds].events = POLLIN;
            pfd[nfds].revents = 0;
            nfds++;
        }
        if (!errClosed && errPipe[0] >= 0) {
            errIdx = nfds;
            pfd[nfds].fd = errPipe[0];
            pfd[nfds].events = POLLIN;
            pfd[nfds].revents = 0;
            nfds++;
        }

        if (nfds == 0) break;

        int pollRes = poll(pfd, nfds, 1000);
        if (pollRes < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (inIdx >= 0 && (pfd[inIdx].revents & (POLLOUT | POLLERR | POLLHUP))) {
            if (pfd[inIdx].revents & POLLOUT) {
                ssize_t w = write(inPipe[1], inputStdin.data() + inWritten, inputStdin.size() - inWritten);
                if (w > 0) {
                    inWritten += static_cast<size_t>(w);
                    if (inWritten >= inputStdin.size()) {
                        close(inPipe[1]);
                        inPipe[1] = -1;
                        inClosed = true;
                    }
                } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    close(inPipe[1]);
                    inPipe[1] = -1;
                    inClosed = true;
                }
            } else {
                close(inPipe[1]);
                inPipe[1] = -1;
                inClosed = true;
            }
        }

        if (outIdx >= 0 && (pfd[outIdx].revents & (POLLIN | POLLERR | POLLHUP))) {
            ssize_t r = read(outPipe[0], buf, sizeof(buf));
            if (r > 0) {
                result.stdOut.insert(result.stdOut.end(), buf, buf + r);
            } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                close(outPipe[0]);
                outPipe[0] = -1;
                outClosed = true;
            }
        }

        if (errIdx >= 0 && (pfd[errIdx].revents & (POLLIN | POLLERR | POLLHUP))) {
            ssize_t r = read(errPipe[0], buf, sizeof(buf));
            if (r > 0) {
                result.stdErr.append(buf, r);
            } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                close(errPipe[0]);
                errPipe[0] = -1;
                errClosed = true;
            }
        }
    }

    if (inPipe[1] >= 0) close(inPipe[1]);
    if (outPipe[0] >= 0) close(outPipe[0]);
    if (errPipe[0] >= 0) close(errPipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }

    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
        result.success = (result.exitCode == 0);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
        result.success = false;
    }

    return result;
}

} // namespace bro::util
#endif
