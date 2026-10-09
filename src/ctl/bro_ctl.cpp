// bro-ctl: drive and observe a running bro from a shell (docs/agent-control.md).
//
//   bro-ctl [-s NAME | -S PATH] [-o OUT] <command> [args...]
//   bro-ctl batch            commands from stdin, one per line, one connection
//   bro-ctl list             the control sockets of this user's bro processes
//
// A windowed or headless bro's socket is <app>-<pid>; -s takes that whole
// name, the app alone (when one process of it runs) or the pid.
//   bro-ctl profile <secs>   native CPU profile of the process (perf)
//
// Every other command is the engine's (bro-ctl help lists them). The request
// is the argv as a JSON array of strings on one line; the reply is
// "ok <n>\n" or "error <n>\n" and n bytes, printed as they are. Exit status:
// 0 ok, 1 the command failed, 2 no bro to talk to.
//
// Files: `-o -` sends a screenshot (or anything a command writes to a path)
// to stdout — `ssh host bro-ctl -o - screenshot > shot.png` — and `-o FILE`
// names where it goes. `record` also makes video.mp4 from its frames when
// ffmpeg is on the PATH.

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
// The same AF_UNIX socket, through Winsock (Windows 10 1803 and later).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

#if defined(_WIN32)
using Sock = SOCKET;
const Sock kNoSock = INVALID_SOCKET;
void closeSock(Sock s) { ::closesocket(s); }
int sockWrite(Sock s, const char* p, size_t n) { return ::send(s, p, static_cast<int>(n), 0); }
int sockRead(Sock s, char* p, size_t n) { return ::recv(s, p, static_cast<int>(n), 0); }
long processId() { return static_cast<long>(::_getpid()); }
#else
using Sock = int;
const Sock kNoSock = -1;
void closeSock(Sock s) { ::close(s); }
int sockWrite(Sock s, const char* p, size_t n) { return static_cast<int>(::write(s, p, n)); }
int sockRead(Sock s, char* p, size_t n) { return static_cast<int>(::read(s, p, n)); }
long processId() { return static_cast<long>(::getpid()); }
#endif

// Where the engine binds its sockets (platform/control_socket.cpp says the
// same): $XDG_RUNTIME_DIR, else the temp directory, /bro-control.
std::string socketDir() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    std::error_code ec;
    std::string base = (xdg && *xdg) ? std::string(xdg) : std::filesystem::temp_directory_path(ec).string();
    if (base.empty()) base = "/tmp";
    while (base.size() > 1 && (base.back() == '/' || base.back() == '\\')) base.pop_back();
    return base + "/bro-control";
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

Sock connectTo(const std::string& path) {
    Sock fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == kNoSock) return kNoSock;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        closeSock(fd);
        return kNoSock;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSock(fd);
        return kNoSock;
    }
    return fd;
}

std::vector<std::string> listSockets() {
    std::vector<std::string> out;
    const std::string dir = socketDir();
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".sock") == 0) out.push_back(dir + "/" + n);
    }
    return out;
}

std::vector<std::string> liveSockets() {
    std::vector<std::string> live;
    for (const auto& s : listSockets()) {
        Sock fd = connectTo(s);
        if (fd != kNoSock) {
            closeSock(fd);
            live.push_back(s);
        }
    }
    return live;
}

std::string socketStem(const std::string& path) {
    std::string n = std::filesystem::path(path).filename().string();
    return n.size() > 5 ? n.substr(0, n.size() - 5) : n;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// Does a socket's stem answer to `-s NAME`? The whole name (`helmterm-4242`),
// the app (`helmterm`, matching `helmterm-<pid>`), or the pid (`4242`).
bool nameMatches(const std::string& stem, const std::string& name) {
    if (stem == name) return true;
    const size_t dash = stem.rfind('-');
    if (dash == std::string::npos) return false;
    const std::string app = stem.substr(0, dash), pid = stem.substr(dash + 1);
    if (!allDigits(pid)) return false;
    return app == name || pid == name;
}

std::string pickOne(const std::vector<std::string>& live, const std::string& what, std::string& why) {
    if (live.size() == 1) return live[0];
    if (live.empty()) why = "no running bro serves a control socket " + what + " in " + socketDir() +
                            " (bro --drm serves one; elsewhere start it with BRO_CONTROL=1)";
    else {
        why = "several bro processes answer" + (what.empty() ? std::string() : " " + what) +
              "; pick one with -s NAME (app-pid, app or pid):";
        for (const auto& s : live) why += "\n  " + socketStem(s);
    }
    return {};
}

// The socket to use: -S, -s, $BRO_CONTROL_SOCKET, display.sock, else the one live socket.
std::string resolveSocket(const std::string& path, const std::string& name, std::string& why) {
    if (!path.empty()) return path;
    if (!name.empty()) {
        // An exact name first (display, or what BRO_CONTROL=<name> chose),
        // even if it does not answer: connecting says why.
        const std::string exact = socketDir() + "/" + name + ".sock";
        std::error_code ec;
        if (std::filesystem::exists(exact, ec)) return exact;
        std::vector<std::string> match;
        for (const auto& s : liveSockets())
            if (nameMatches(socketStem(s), name)) match.push_back(s);
        return pickOne(match, "to -s " + name, why);
    }
    if (const char* env = std::getenv("BRO_CONTROL_SOCKET"); env && *env) return env;
    const std::string display = socketDir() + "/display.sock";
    std::error_code ec;
    if (std::filesystem::exists(display, ec)) return display;
    return pickOne(liveSockets(), "", why);
}

struct Conn {
    Sock fd = kNoSock;
    std::string buf;

    bool send(const std::vector<std::string>& argv) {
        std::string line = "[";
        for (size_t i = 0; i < argv.size(); ++i) line += (i ? "," : "") + quote(argv[i]);
        line += "]\n";
        size_t off = 0;
        while (off < line.size()) {
            const int w = sockWrite(fd, line.data() + off, line.size() - off);
            if (w <= 0) return false;
            off += static_cast<size_t>(w);
        }
        return true;
    }

    bool fill() {
        char tmp[65536];
        const int r = sockRead(fd, tmp, sizeof(tmp));
        if (r <= 0) return false;
        buf.append(tmp, static_cast<size_t>(r));
        return true;
    }

    // One reply: ok, payload. False when the connection died.
    bool receive(bool& ok, std::string& payload) {
        size_t nl;
        while ((nl = buf.find('\n')) == std::string::npos)
            if (!fill()) return false;
        std::string head = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        const size_t sp = head.find(' ');
        if (sp == std::string::npos) return false;
        ok = head.compare(0, sp, "ok") == 0;
        const size_t n = std::strtoull(head.c_str() + sp + 1, nullptr, 10);
        while (buf.size() < n)
            if (!fill()) return false;
        payload = buf.substr(0, n);
        buf.erase(0, n);
        return true;
    }
};

// The value of "key":"..." in a JSON reply (no unescaping beyond \" and \\).
std::string jsonString(const std::string& json, const std::string& key) {
    const std::string pat = "\"" + key + "\":\"";
    size_t i = json.find(pat);
    if (i == std::string::npos) return {};
    i += pat.size();
    std::string out;
    while (i < json.size() && json[i] != '"') {
        if (json[i] == '\\' && i + 1 < json.size()) ++i;
        out += json[i++];
    }
    return out;
}

// One argument for std::system's shell: cmd.exe on Windows, sh elsewhere.
std::string shellArg(const std::string& s) {
#if defined(_WIN32)
    // cmd.exe has no escape inside "..." and a path cannot hold '"'; '%'
    // would expand a variable, but %TEMP% paths do not carry one.
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char c : s) out += (c == '\'') ? std::string("'\\''") : std::string(1, c);
    return out + "'";
#endif
}

// ffmpeg turns a recording's frames.ffconcat into video.mp4. The concat
// demuxer reads the frames relative to the .ffconcat file, so no cd is
// needed and the command is the same on every shell but for the quoting.
std::string ffmpegCommand(const std::string& dir) {
    const std::filesystem::path d(dir);
    std::string cmd = "ffmpeg -loglevel error -y -f concat -safe 0 -i " + shellArg((d / "frames.ffconcat").string()) +
                      " -fps_mode cfr -r 60 -pix_fmt yuv420p -c:v libx264 -preset veryfast " +
                      shellArg((d / "video.mp4").string());
#if defined(_WIN32)
    // cmd /c strips the outer quotes of a line that starts with one; wrapping
    // the whole line keeps the inner ones.
    cmd = "\"" + cmd + "\"";
#endif
    return cmd;
}

bool onPath(const char* tool) {
#if defined(_WIN32)
    std::string cmd = std::string("where ") + tool + " >NUL 2>&1";
#else
    std::string cmd = std::string("command -v ") + tool + " >/dev/null 2>&1";
#endif
    return std::system(cmd.c_str()) == 0;
}

int printReply(bool ok, const std::string& payload) {
    if (ok) {
        std::fwrite(payload.data(), 1, payload.size(), stdout);
        if (!payload.empty() && payload.back() != '\n') std::fputc('\n', stdout);
        return 0;
    }
    std::fprintf(stderr, "bro-ctl: %s\n", payload.c_str());
    return 1;
}

bool catFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::cout << in.rdbuf();
    std::cout.flush();
    return true;
}

// Shell-like split of one batch line: whitespace, '...' and "..." quoting, \ escapes,
// and a line starting with '#' is a comment.
std::vector<std::string> splitLine(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool have = false;
    char q = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (q) {
            if (c == q) q = 0;
            else if (c == '\\' && q == '"' && i + 1 < line.size()) cur += line[++i];
            else cur += c;
        } else if (c == '\'' || c == '"') {
            q = c;
            have = true;
        } else if (c == '\\' && i + 1 < line.size()) {
            cur += line[++i];
            have = true;
        } else if (c == ' ' || c == '\t') {
            if (have) out.push_back(cur);
            cur.clear();
            have = false;
        } else if (c == '#' && out.empty() && !have) {
            break;  // a comment line; elsewhere '#' starts a selector (#id)
        } else {
            cur += c;
            have = true;
        }
    }
    if (have) out.push_back(cur);
    return out;
}

void usage() {
    std::fprintf(stderr,
                 "usage: bro-ctl [-s NAME | -S PATH] [-o OUT|-] <command> [args...]\n"
                 "       bro-ctl batch < commands      (one command per line)\n"
                 "       bro-ctl list | profile <seconds> [--top=40]\n"
                 "       bro-ctl help                  (the running engine's commands)\n");
}

// A command's file output: screenshot takes its path as the first argument,
// record its directory as the second. `-o -` writes to a temp file and then
// to stdout.
struct OutputPlan {
    std::string path;
    bool toStdout = false;
};

int runOne(Conn& c, std::vector<std::string> argv, const std::string& out) {
    OutputPlan plan;
    const std::string& cmd = argv[0];
    if (!out.empty() && (cmd == "screenshot" || cmd == "record")) {
        plan.toStdout = (out == "-");
        plan.path = plan.toStdout ? socketDir() + "/stdout-" + std::to_string(processId()) +
                                        (cmd == "screenshot" ? ".png" : "")
                                  : out;
        // Insert the path where the command reads it.
        std::vector<std::string> pos;
        size_t firstPos = 0, nPos = 0;
        for (size_t i = 1; i < argv.size(); ++i)
            if (argv[i].rfind("--", 0) != 0) {
                if (!nPos) firstPos = i;
                ++nPos;
            }
        if (cmd == "screenshot") {
            if (nPos) argv[firstPos] = plan.path;
            else argv.push_back(plan.path);
        } else {  // record [seconds] [dir]
            if (nPos == 0) argv.insert(argv.begin() + 1, {"3", plan.path});
            else if (nPos == 1) argv.insert(argv.begin() + firstPos + 1, plan.path);
            else argv[firstPos + 1] = plan.path;
        }
    }
    if (cmd == "eval") {
        // eval -  : the script from stdin;  eval -f FILE : from a file.
        for (size_t i = 1; i < argv.size(); ++i) {
            if (argv[i] == "-") {
                std::stringstream ss;
                ss << std::cin.rdbuf();
                argv[i] = ss.str();
            } else if (argv[i] == "-f" && i + 1 < argv.size()) {
                std::ifstream in(argv[i + 1]);
                std::stringstream ss;
                ss << in.rdbuf();
                argv[i] = ss.str();
                argv.erase(argv.begin() + i + 1);
            }
        }
    }
    bool ok = false;
    std::string payload;
    if (!c.send(argv) || !c.receive(ok, payload)) {
        std::fprintf(stderr, "bro-ctl: the connection closed before a reply\n");
        return 2;
    }
    if (ok && cmd == "record" && !argv.empty()) {
        const std::string dir = jsonString(payload, "dir");
        bool wantVideo = true;
        for (const auto& a : argv)
            if (a == "--no-video") wantVideo = false;
        if (!dir.empty() && wantVideo && onPath("ffmpeg")) {
            const std::string video = (std::filesystem::path(dir) / "video.mp4").string();
            if (std::system(ffmpegCommand(dir).c_str()) == 0 && !plan.toStdout && !payload.empty() &&
                payload.back() == '}') {
                payload.insert(payload.size() - 1, ",\"video\":" + quote(video));
            }
        }
        if (plan.toStdout) {
            const bool sent = catFile(dir + "/video.mp4") || catFile(dir + "/contact.png");
            return sent ? 0 : 1;
        }
    }
    if (ok && plan.toStdout) {
        const bool sent = catFile(plan.path);
        std::remove(plan.path.c_str());
        return sent ? 0 : 1;
    }
    return printReply(ok, payload);
}

int runProfile(Conn& c, const std::vector<std::string>& argv) {
    double secs = argv.size() > 1 ? std::atof(argv[1].c_str()) : 3.0;
    if (secs <= 0) secs = 3.0;
    int top = 40;
    for (const auto& a : argv)
        if (a.rfind("--top=", 0) == 0) top = std::atoi(a.c_str() + 6);
    bool ok = false;
    std::string info;
    if (!c.send({"info"}) || !c.receive(ok, info) || !ok) {
        std::fprintf(stderr, "bro-ctl: info failed\n");
        return 2;
    }
    const size_t p = info.find("\"pid\":");
    if (p == std::string::npos) return std::fprintf(stderr, "bro-ctl: no pid in info\n"), 1;
    const long pid = std::strtol(info.c_str() + p + 6, nullptr, 10);
    if (!onPath("perf")) {
        std::fprintf(stderr, "bro-ctl: profile needs perf (pacman -S perf / apt install linux-perf)\n");
        return 1;
    }
    const std::string data = socketDir() + "/perf-" + std::to_string(processId()) + ".data";
    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd),
                  "perf record -q -F 1999 -p %ld -o '%s' -- sleep %.3f >/dev/null 2>&1 && "
                  "perf report -i '%s' --stdio --no-children --sort dso,symbol --percent-limit 0.3 "
                  "2>/dev/null | grep -v '^#' | grep -v '^$' | sed 's/ *$//' | head -n %d; "
                  "echo; echo '--- by thread (tid:name) ---'; "
                  "perf report -i '%s' --stdio --no-children --sort pid 2>/dev/null | grep -v '^#' | "
                  "grep -v '^$' | sed 's/ *$//' | head -n 20; rm -f '%s'",
                  pid, data.c_str(), secs, data.c_str(), top, data.c_str(), data.c_str());
    return std::system(cmd) == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string name, path, out;
    int i = 1;
    for (; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "-s" || a == "--name") && i + 1 < argc) name = argv[++i];
        else if ((a == "-S" || a == "--socket") && i + 1 < argc) path = argv[++i];
        else if ((a == "-o" || a == "--out") && i + 1 < argc) out = argv[++i];
        else if (a == "-h" || a == "--help") return usage(), 0;
        else break;
    }
    if (i >= argc) return usage(), 2;
    std::vector<std::string> args(argv + i, argv + argc);

#if defined(_WIN32)
    WSADATA wsa;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return std::fprintf(stderr, "bro-ctl: Winsock did not start\n"), 2;
    // Replies (a PNG to stdout with -o -) are bytes, not text.
    ::_setmode(::_fileno(stdout), _O_BINARY);
#endif

    if (args[0] == "list") {
        for (const auto& s : listSockets()) {
            Sock fd = connectTo(s);
            std::printf("%s%s\n", s.c_str(), fd != kNoSock ? "" : "  (stale)");
            if (fd != kNoSock) closeSock(fd);
        }
        return 0;
    }

    std::string why;
    const std::string sock = resolveSocket(path, name, why);
    if (sock.empty()) return std::fprintf(stderr, "bro-ctl: %s\n", why.c_str()), 2;
    Conn c;
    c.fd = connectTo(sock);
    if (c.fd == kNoSock) return std::fprintf(stderr, "bro-ctl: cannot connect to %s: %s\n", sock.c_str(), std::strerror(errno)), 2;

    if (args[0] == "profile") return runProfile(c, args);
    if (args[0] == "batch") {
        int status = 0;
        std::string line;
        while (std::getline(std::cin, line)) {
            auto argvLine = splitLine(line);
            if (argvLine.empty()) continue;
            std::printf("$ %s\n", line.c_str());
            std::fflush(stdout);
            if (argvLine[0] == "sleep" && argvLine.size() > 1) {
                std::this_thread::sleep_for(std::chrono::duration<double>(std::atof(argvLine[1].c_str())));
                continue;
            }
            int rc = runOne(c, argvLine, {});
            std::fflush(stdout);
            if (rc == 2) return 2;
            if (rc) status = 1;
        }
        return status;
    }
    return runOne(c, args, out);
}
