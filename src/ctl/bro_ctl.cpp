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
// is the argv, the reply ok or not and a payload printed as it is
// (platform/control_protocol.h: brolink-framed messages over brolink's local
// IPC). Exit status: 0 ok, 1 the command failed, 2 no bro to talk to.
//
// Files: `-o -` sends a screenshot (or anything a command writes to a path)
// to stdout — `ssh host bro-ctl -o - screenshot > shot.png` — and `-o FILE`
// names where it goes. `record` also makes video.mp4 from its frames when
// ffmpeg is on the PATH.

#include "platform/control_protocol.h"

#include <brolink/paths.h>
#include <brolink/stream.h>
#include <brolink/wire.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

namespace control = bro::platform::control;

long processId() { return static_cast<long>(brolink::current_pid()); }

// The files a command writes for bro-ctl to pass on (-o -) and perf's data
// go in the control channel's private directory, which the engine uses too.
std::string runtimeDir() {
    std::string dir = brolink::runtime_dir(control::kApp);
    if (dir.empty()) {
        std::error_code ec;
        dir = std::filesystem::temp_directory_path(ec).string();
    }
    return dir;
}

std::string addressOf(const std::string& name) { return brolink::local_address(control::kApp, name); }

// Where the endpoints are, for messages: the socket directory, or on
// Windows the pipe namespace.
std::string endpointsWhere() {
#if defined(_WIN32)
    return "the named pipes \\\\.\\pipe\\" + std::string(control::kApp) + "-<SID>-*";
#else
    return runtimeDir();
#endif
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

// Whether a server answers at `address` (and passes brolink's peer check).
bool answers(const std::string& address) { return brolink::connect_local(address) != nullptr; }

// The endpoint names that answer.
std::vector<std::string> liveNames() {
    std::vector<std::string> live;
    for (const auto& n : brolink::list_local(control::kApp))
        if (answers(addressOf(n))) live.push_back(n);
    return live;
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
    if (live.size() == 1) return addressOf(live[0]);
    if (live.empty()) why = "no running bro serves a control socket " + what + " in " + endpointsWhere() +
                            " (bro --drm serves one; elsewhere start it with BRO_CONTROL=1)";
    else {
        why = "several bro processes answer" + (what.empty() ? std::string() : " " + what) +
              "; pick one with -s NAME (app-pid, app or pid):";
        for (const auto& n : live) why += "\n  " + n;
    }
    return {};
}

// The address to use: -S, -s, $BRO_CONTROL_SOCKET, display, else the one live endpoint.
std::string resolveSocket(const std::string& path, const std::string& name, std::string& why) {
    if (!path.empty()) return path;
    const std::vector<std::string> names = brolink::list_local(control::kApp);
    auto have = [&](const std::string& n) { return std::find(names.begin(), names.end(), n) != names.end(); };
    if (!name.empty()) {
        // An exact name first (display, or what BRO_CONTROL=<name> chose),
        // even if it does not answer: connecting says why.
        if (have(name)) return addressOf(name);
        std::vector<std::string> match;
        for (const auto& n : liveNames())
            if (nameMatches(n, name)) match.push_back(n);
        return pickOne(match, "to -s " + name, why);
    }
    if (const char* env = std::getenv("BRO_CONTROL_SOCKET"); env && *env) return env;
    if (have("display")) return addressOf("display");
    return pickOne(liveNames(), "", why);
}

// One connection: requests out, replies back by id (control_protocol.h).
struct Conn {
    std::unique_ptr<brolink::Stream> stream;
    brolink::wire::MessageSplitter split{control::kMaxMessage};
    uint64_t nextId = 1;
    uint64_t sent = 0;

    bool send(const std::vector<std::string>& argv) {
        sent = nextId++;
        return stream->write(control::encodeRequest(sent, argv));
    }

    // The reply to the last request sent: ok, payload. False when the
    // connection died or spoke something else.
    bool receive(bool& ok, std::string& payload) {
        brolink::wire::MessageSplitter::Message m;
        for (;;) {
            while (split.next(m)) {
                uint64_t id = 0;
                if (m.type != control::kReply || !control::decodeReply(m.payload, id, ok, payload)) return false;
                if (id == sent) return true;
            }
            if (split.error()) return false;
            char tmp[65536];
            const size_t r = stream->read(tmp, sizeof(tmp));
            if (r == 0) return false;
            split.feed(tmp, r);
        }
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
        plan.path = plan.toStdout ? (std::filesystem::path(runtimeDir()) /
                                     ("stdout-" + std::to_string(processId()) + (cmd == "screenshot" ? ".png" : "")))
                                        .string()
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
    const std::string data = runtimeDir() + "/perf-" + std::to_string(processId()) + ".data";
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
    // Replies (a PNG to stdout with -o -) are bytes, not text.
    ::_setmode(::_fileno(stdout), _O_BINARY);
#endif

    if (args[0] == "list") {
        for (const auto& n : brolink::list_local(control::kApp)) {
            const std::string address = addressOf(n);
            std::printf("%s%s\n", address.c_str(), answers(address) ? "" : "  (stale)");
        }
        return 0;
    }

    std::string why;
    const std::string sock = resolveSocket(path, name, why);
    if (sock.empty()) return std::fprintf(stderr, "bro-ctl: %s\n", why.c_str()), 2;
    Conn c;
    std::string err;
    c.stream = brolink::connect_local(sock, &err);
    if (!c.stream) return std::fprintf(stderr, "bro-ctl: %s\n", err.c_str()), 2;

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
