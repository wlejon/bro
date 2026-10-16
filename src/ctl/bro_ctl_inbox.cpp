// bro-ctl's agent inbox commands: bro_ctl_inbox.h.
#include "ctl/bro_ctl_inbox.h"

#include "platform/inbox_bundle.h"
#include "util/json_in.h"
#include "util/json_out.h"

#include <brolink/paths.h>
#include <brolink/peer.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#if !defined(_WIN32)
#include <signal.h>
#include <unistd.h>
#endif

namespace broctl {

namespace {

namespace fs = std::filesystem;
namespace inbox = bro::platform::inbox;
using bro::util::JsonOut;
using bro::util::JsonValue;
using Clock = std::chrono::steady_clock;

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string pathString(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(u8path(path), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeFile(const fs::path& p, const std::string& bytes) {
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(o);
}

std::string machineName() {
#if defined(_WIN32)
    if (const char* n = std::getenv("COMPUTERNAME"); n && *n) return n;
    return "windows";
#else
    char buf[256] = {};
    if (::gethostname(buf, sizeof buf - 1) == 0 && buf[0]) return buf;
    return "localhost";
#endif
}

// Who this bro-ctl says it is: user@machine.
std::string selfOrigin() { return brolink::current_user() + "@" + machineName(); }

std::string defaultAgentName() {
    if (const char* n = std::getenv("BRO_AGENT_NAME"); n && *n) return n;
    return selfOrigin();
}

bool parseJson(const std::string& text, JsonValue& v) { return bro::util::parseJson(text, v, nullptr) && v.isObject(); }

// The options of an inbox command: --name VALUE or --name=VALUE, repeatable
// ones gathered; everything else positional.
struct Args {
    std::vector<std::string> pos;
    std::vector<std::pair<std::string, std::string>> opts;
    bool has(const std::string& k) const {
        for (auto& [n, v] : opts)
            if (n == k) return true;
        return false;
    }
    std::string get(const std::string& k, const std::string& fallback = {}) const {
        for (auto& [n, v] : opts)
            if (n == k) return v;
        return fallback;
    }
    std::vector<std::string> all(const std::string& k) const {
        std::vector<std::string> out;
        for (auto& [n, v] : opts)
            if (n == k) out.push_back(v);
        return out;
    }
};

// `valued` names the options that take a value.
Args parseArgs(const std::vector<std::string>& in, size_t from, const std::set<std::string>& valued) {
    Args a;
    for (size_t i = from; i < in.size(); ++i) {
        const std::string& s = in[i];
        if (s.size() > 2 && s[0] == '-' && s[1] == '-') {
            const size_t eq = s.find('=');
            std::string name = s.substr(2, eq == std::string::npos ? std::string::npos : eq - 2);
            if (eq != std::string::npos) a.opts.emplace_back(name, s.substr(eq + 1));
            else if (valued.count(name) && i + 1 < in.size()) a.opts.emplace_back(name, in[++i]);
            else a.opts.emplace_back(name, "true");
        } else {
            a.pos.push_back(s);
        }
    }
    return a;
}

// Where a command's files come back: the engine writes them to the
// output-path argument; remote, that is the reply's file (outArg).
struct FileOut {
    std::string arg;     // the argument to send
    std::string local;   // where to read it here (local only)
};
FileOut fileOut(Caller& c, const std::string& runtimeDir) {
    static std::atomic<uint64_t> serial{0};
    FileOut f;
    if (c.remote()) {
        f.arg = "files.bin";
        return f;
    }
    f.local = pathString(u8path(runtimeDir) / ("inbox-" + std::to_string(brolink::current_pid()) + "-" +
                                               std::to_string(++serial) + ".bin"));
    f.arg = f.local;
    return f;
}

// One command whose argument `outIndex` is a FileOut: ok, payload and the bundle.
bool callWithFiles(Caller& c, std::vector<std::string> argv, size_t outIndex, const FileOut& f, bool& ok,
                   std::string& payload, std::string& bundle, std::string& err) {
    std::string file;
    if (!c.call(argv, c.remote() ? outIndex : 0, ok, payload, file)) {
        err = "the connection closed before a reply" + (payload.empty() ? std::string() : ": " + payload);
        return false;
    }
    if (c.remote()) bundle = std::move(file);
    else if (ok) readFile(f.local, bundle);
    if (!f.local.empty()) {
        std::error_code ec;
        fs::remove(u8path(f.local), ec);
    }
    return true;
}

bool callPlain(Caller& c, const std::vector<std::string>& argv, bool& ok, std::string& payload, std::string& err) {
    std::string file;
    if (!c.call(argv, 0, ok, payload, file)) {
        err = "the connection closed before a reply" + (payload.empty() ? std::string() : ": " + payload);
        return false;
    }
    return true;
}

// The bundle of the files at `paths` (each named by its last component).
bool bundleOf(const std::vector<std::string>& paths, std::string& out, std::string& err) {
    std::vector<inbox::File> files;
    size_t total = 0;
    for (const std::string& p : paths) {
        inbox::File f;
        f.name = inbox::baseName(p);
        if (!inbox::validName(f.name)) return err = "cannot attach '" + p + "' (not a file name)", false;
        if (!readFile(p, f.bytes)) return err = "cannot read " + p, false;
        total += f.bytes.size();
        files.push_back(std::move(f));
    }
    if (total > inbox::kMaxBundle) return err = "the files are larger than 48 MB together", false;
    out = files.empty() ? std::string() : inbox::encode(files);
    return true;
}

// Writes a bundle's files into `dir` (made if any), answering their paths.
std::vector<std::string> unpack(const std::string& bundle, const fs::path& dir, std::string& err) {
    std::vector<inbox::File> files;
    std::vector<std::string> paths;
    if (!inbox::decode(bundle, files, &err)) return paths;
    if (files.empty()) return paths;
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (const auto& f : files) {
        const fs::path p = dir / u8path(f.name);
        if (!writeFile(p, f.bytes)) {
            err = "cannot write " + pathString(p);
            continue;
        }
        paths.push_back(pathString(fs::absolute(p, ec)));
    }
    return paths;
}

bool finishedState(const std::string& s) { return s == "done" || s == "failed" || s == "cancelled"; }

// Prints a finished request's answer: its text on stdout (its error on
// stderr), its files saved under `saveDir`. The exit status.
int printAnswer(const std::string& payload, const std::string& bundle, const std::string& saveDir, bool json) {
    JsonValue v;
    if (!parseJson(payload, v)) {
        std::fprintf(stderr, "bro-ctl: unexpected reply: %s\n", payload.c_str());
        return 1;
    }
    const std::string state = v.get("state").string, id = v.get("id").string;
    std::string err;
    const std::string dir = saveDir.empty() ? id : saveDir;
    const auto saved = state == "done" ? unpack(bundle, u8path(dir), err) : std::vector<std::string>();
    if (!err.empty()) std::fprintf(stderr, "bro-ctl: %s\n", err.c_str());
    if (json) {
        std::string out = payload;
        if (!saved.empty() && !out.empty() && out.back() == '}') {
            JsonOut j;
            j.beginArray();
            for (const auto& s : saved) j.string(s);
            j.endArray();
            out.insert(out.size() - 1, ",\"saved\":" + j.take());
        }
        std::printf("%s\n", out.c_str());
    } else if (state == "done") {
        const std::string& text = v.get("text").string;
        std::fwrite(text.data(), 1, text.size(), stdout);
        if (!text.empty() && text.back() != '\n') std::fputc('\n', stdout);
        std::fflush(stdout);
        for (const auto& s : saved) std::fprintf(stderr, "saved %s\n", s.c_str());
    } else if (finishedState(state)) {
        std::fprintf(stderr, "bro-ctl: %s %s: %s\n", id.c_str(), state.c_str(), v.get("error").string.c_str());
    } else {
        std::fprintf(stderr, "bro-ctl: %s is still %s: bro-ctl inbox result %s\n", id.c_str(), state.c_str(),
                     id.c_str());
        if (!json) std::printf("%s\n", id.c_str());
        return 3;
    }
    std::fflush(stdout);
    return state == "done" ? 0 : (finishedState(state) ? 1 : 3);
}

// Follows a request until it finishes or `deadline`, printing each progress
// line to stderr. False on a broken connection; `state` is the last seen.
bool follow(Caller& c, const std::string& id, Clock::time_point deadline, bool quiet, std::string& state,
            std::string& err) {
    size_t since = 0;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) return true;
        const double wait = std::min<double>(20.0, static_cast<double>(left) / 1000.0);
        bool ok = false;
        std::string payload;
        char w[32];
        std::snprintf(w, sizeof w, "--wait=%.3f", wait);
        if (!callPlain(c, {"inbox", "status", id, "--since=" + std::to_string(since), w}, ok, payload, err))
            return false;
        if (!ok) return err = payload, false;
        JsonValue v;
        if (!parseJson(payload, v)) return err = "unexpected reply: " + payload, false;
        state = v.get("state").string;
        for (const JsonValue& p : v.get("progress").array) {
            if (!quiet) std::fprintf(stderr, "[%s] %s\n", v.get("to").string.c_str(), p.get("text").string.c_str());
        }
        since = static_cast<size_t>(v.get("progressNext").number);
        if (finishedState(state)) return true;
    }
}

// ---------------------------------------------------------------------------
// ask / result / status
// ---------------------------------------------------------------------------

int runAsk(const std::vector<std::string>& in, Caller& c, const std::string& runtimeDir) {
    const Args a = parseArgs(in, 1, {"file", "from", "wait", "save"});
    if (a.pos.empty()) {
        std::fprintf(stderr, "usage: bro-ctl ask NAME TEXT... [--file PATH]... [--from NAME] [--wait SECS] "
                             "[--async] [--save DIR] [--json]   (TEXT - reads stdin)\n");
        return 2;
    }
    std::string text;
    for (size_t i = 1; i < a.pos.size(); ++i) text += (i > 1 ? " " : "") + a.pos[i];
    if (text == "-") {
        std::stringstream ss;
        ss << std::cin.rdbuf();
        text = ss.str();
    }
    std::string bundle, err;
    if (!bundleOf(a.all("file"), bundle, err)) return std::fprintf(stderr, "bro-ctl: %s\n", err.c_str()), 1;
    std::vector<std::string> argv = {"ask", a.pos[0], "--text=" + text, "--from=" + a.get("from", defaultAgentName()),
                                     "--origin=" + selfOrigin()};
    if (!bundle.empty()) argv.push_back("--files=" + bundle);
    bool ok = false;
    std::string payload;
    if (!callPlain(c, argv, ok, payload, err)) return std::fprintf(stderr, "bro-ctl: %s\n", err.c_str()), 2;
    if (!ok) return std::fprintf(stderr, "bro-ctl: %s\n", payload.c_str()), 1;
    JsonValue v;
    if (!parseJson(payload, v)) return std::fprintf(stderr, "bro-ctl: unexpected reply: %s\n", payload.c_str()), 1;
    const std::string id = v.get("id").string;
    if (a.has("async")) {
        if (a.has("json")) std::printf("%s\n", payload.c_str());
        else std::printf("%s\n", id.c_str());
        return 0;
    }
    const double waitS = std::atof(a.get("wait", "600").c_str());
    const auto deadline = Clock::now() + std::chrono::milliseconds(static_cast<int64_t>(waitS * 1000));
    std::string state;
    if (!follow(c, id, deadline, a.has("quiet"), state, err))
        return std::fprintf(stderr, "bro-ctl: %s\n", err.c_str()), 2;
    const FileOut f = fileOut(c, runtimeDir);
    std::string bundleBack;
    if (!callWithFiles(c, {"inbox", "result", id, f.arg}, 3, f, ok, payload, bundleBack, err))
        return std::fprintf(stderr, "bro-ctl: %s\n", err.c_str()), 2;
    if (!ok) return std::fprintf(stderr, "bro-ctl: %s\n", payload.c_str()), 1;
    return printAnswer(payload, bundleBack, a.get("save"), a.has("json"));
}

int runResult(const std::vector<std::string>& in, Caller& c, const std::string& runtimeDir) {
    const Args a = parseArgs(in, 2, {"wait", "save"});
    if (a.pos.empty()) return std::fprintf(stderr, "usage: bro-ctl inbox result ID [--wait SECS] [--save DIR]\n"), 2;
    const std::string id = a.pos[0];
    std::string err;
    if (a.has("wait")) {
        std::string state;
        const auto deadline =
            Clock::now() + std::chrono::milliseconds(static_cast<int64_t>(std::atof(a.get("wait").c_str()) * 1000));
        if (!follow(c, id, deadline, a.has("quiet"), state, err))
            return std::fprintf(stderr, "bro-ctl: %s\n", err.c_str()), 1;
    }
    const FileOut f = fileOut(c, runtimeDir);
    bool ok = false;
    std::string payload, bundle;
    if (!callWithFiles(c, {"inbox", "result", id, f.arg}, 3, f, ok, payload, bundle, err))
        return std::fprintf(stderr, "bro-ctl: %s\n", err.c_str()), 2;
    if (!ok) return std::fprintf(stderr, "bro-ctl: %s\n", payload.c_str()), 1;
    return printAnswer(payload, bundle, a.get("save"), a.has("json"));
}

int runStatus(const std::vector<std::string>& in, Caller& c) {
    const Args a = parseArgs(in, 2, {});
    if (a.pos.empty()) return std::fprintf(stderr, "usage: bro-ctl inbox status ID [--follow]\n"), 2;
    std::string err;
    if (a.has("follow")) {
        std::string state;
        if (!follow(c, a.pos[0], Clock::now() + std::chrono::hours(24 * 365), false, state, err))
            return std::fprintf(stderr, "bro-ctl: %s\n", err.c_str()), 1;
        std::printf("%s %s\n", a.pos[0].c_str(), state.c_str());
        return state == "done" ? 0 : 1;
    }
    bool ok = false;
    std::string payload;
    if (!callPlain(c, {"inbox", "status", a.pos[0]}, ok, payload, err))
        return std::fprintf(stderr, "bro-ctl: %s\n", err.c_str()), 2;
    if (!ok) return std::fprintf(stderr, "bro-ctl: %s\n", payload.c_str()), 1;
    std::printf("%s\n", payload.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// log
// ---------------------------------------------------------------------------

std::string localTime(double unixMs) {
    const std::time_t t = static_cast<std::time_t>(unixMs / 1000.0);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::string oneLine(std::string s, size_t max = 100) {
    for (char& ch : s)
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    if (s.size() > max) s = s.substr(0, max) + "...";
    return s;
}

// "tester (me@here; remote j from 10.1.0.4)"
std::string describeSender(const JsonValue& f) {
    std::string out = f.get("agent").string;
    std::string how = f.get("via").string;
    if (!f.get("user").string.empty()) how += " " + f.get("user").string;
    if (!f.get("sshClient").string.empty()) how += " from " + f.get("sshClient").string;
    const std::string& origin = f.get("origin").string;
    return out + " (" + (origin.empty() || origin == out ? "" : origin + "; ") + how + ")";
}

int runLog(const std::vector<std::string>& in, Caller& c) {
    const Args a = parseArgs(in, 2, {"limit", "id"});
    std::vector<std::string> argv = {"inbox", "log", "--limit=" + a.get("limit", "50")};
    if (a.has("id")) argv.push_back("--id=" + a.get("id"));
    bool ok = false;
    std::string payload, err;
    if (!callPlain(c, argv, ok, payload, err)) return std::fprintf(stderr, "bro-ctl: %s\n", err.c_str()), 2;
    if (!ok) return std::fprintf(stderr, "bro-ctl: %s\n", payload.c_str()), 1;
    if (a.has("json")) {
        std::fwrite(payload.data(), 1, payload.size(), stdout);
        return 0;
    }
    std::istringstream lines(payload);
    std::string line;
    while (std::getline(lines, line)) {
        JsonValue v;
        if (!parseJson(line, v)) continue;
        const std::string ev = v.get("event").string;
        std::string what;
        if (ev == "ask") {
            what = v.get("id").string + "  " + describeSender(v.get("from")) + " -> " + v.get("to").string + ": " +
                   oneLine(v.get("text").string);
        } else if (ev == "register") {
            const JsonValue& by = v.get("by");
            what = v.get("agent").string + " registered" +
                   (v.get("about").string.empty() ? "" : " (" + oneLine(v.get("about").string, 60) + ")") + " via " +
                   by.get("via").string + (by.find("pid") ? " pid " + std::to_string(static_cast<long long>(by.get("pid").number)) : "");
        } else if (ev == "leave") {
            what = v.get("agent").string + ": " + oneLine(v.get("text").string);
        } else {
            what = v.get("id").string + "  " + v.get("to").string;
            if (!v.get("text").string.empty()) what += ": " + oneLine(v.get("text").string);
        }
        if (v.find("files")) {
            std::string names;
            for (const auto& f : v.get("files").array) names += (names.empty() ? "" : ", ") + f.get("name").string;
            if (!names.empty()) what += "  [" + names + "]";
        }
        char evbuf[16];
        std::snprintf(evbuf, sizeof evbuf, "%-9s", ev.c_str());
        std::printf("%s  %s %s\n", localTime(v.get("t").number).c_str(), evbuf, what.c_str());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// serve
// ---------------------------------------------------------------------------

std::atomic<bool> g_stop{false};
extern "C" void onStopSignal(int) { g_stop = true; }

// SIGINT / SIGTERM stop serving: without SA_RESTART on POSIX, so a read of
// stdin blocked in getline returns and the loop sees the flag.
void catchStopSignals() {
#if defined(_WIN32)
    std::signal(SIGINT, onStopSignal);
    std::signal(SIGTERM, onStopSignal);
#else
    struct sigaction sa {};
    sa.sa_handler = onStopSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif
}

class Server {
public:
    Server(const CallerFactory& open, std::unique_ptr<Caller> answers, std::string name, std::string about,
           bool replace, std::string dir, std::string exec, std::string runtimeDir)
        : open_(open), answers_(std::move(answers)), name_(std::move(name)), about_(std::move(about)),
          replace_(replace), dir_(std::move(dir)), exec_(std::move(exec)), runtimeDir_(std::move(runtimeDir)) {}

    int run() {
        std::string err;
        if (!registerName(err)) return std::fprintf(stderr, "bro-ctl: %s\n", err.c_str()), 1;
        std::fprintf(stderr, "bro-ctl: serving '%s' (requests in %s)%s\n", name_.c_str(), dir_.c_str(),
                     exec_.empty() ? "; one JSON request per line on stdout, replies on stdin" : "");
        catchStopSignals();
        std::thread poller([this] { pollLoop(); });
        if (exec_.empty()) {
            std::string line;
            while (!g_stop && !pollFailed_ && std::getline(std::cin, line)) handleLine(line);
        } else {
            while (!g_stop && !pollFailed_) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        g_stop = true;
        {
            std::lock_guard<std::mutex> lk(answerMu_);
            bool ok = false;
            std::string payload, e;
            callPlain(*answers_, {"inbox", "unregister", token()}, ok, payload, e);
        }
        // The poller is in a held poll; unregistering answered it ("gone").
        poller.join();
        for (auto& t : workers_) t.join();
        return pollFailed_ ? 2 : 0;
    }

private:
    std::string token() {
        std::lock_guard<std::mutex> lk(tokenMu_);
        return token_;
    }

    bool registerName(std::string& err) {
        std::vector<std::string> argv = {"inbox", "register", name_, "--about=" + about_};
        if (replace_) argv.push_back("--replace");
        bool ok = false;
        std::string payload;
        std::lock_guard<std::mutex> lk(answerMu_);
        if (!callPlain(*answers_, argv, ok, payload, err)) return false;
        if (!ok) return err = payload, false;
        JsonValue v;
        if (!parseJson(payload, v)) return err = "unexpected reply: " + payload, false;
        std::lock_guard<std::mutex> tk(tokenMu_);
        token_ = v.get("token").string;
        return true;
    }

    void emit(const std::string& line) {
        std::lock_guard<std::mutex> lk(outMu_);
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    void pollLoop() {
        std::string err;
        std::unique_ptr<Caller> c = open_(err);
        if (!c) {
            std::fprintf(stderr, "bro-ctl: %s\n", err.c_str());
            pollFailed_ = true;
            return;
        }
        while (!g_stop) {
            const FileOut f = fileOut(*c, runtimeDir_);
            bool ok = false;
            std::string payload, bundle;
            if (!callWithFiles(*c, {"inbox", "next", token(), f.arg, "--timeout=20000"}, 3, f, ok, payload, bundle,
                               err)) {
                if (!g_stop) std::fprintf(stderr, "bro-ctl: %s\n", err.c_str());
                pollFailed_ = true;
                return;
            }
            if (g_stop) return;
            if (!ok) {
                // The registration lapsed (the machine slept, the session was
                // busy): take the name again, if it is free.
                std::fprintf(stderr, "bro-ctl: %s; registering again\n", payload.c_str());
                std::string e;
                if (!registerName(e)) {
                    std::fprintf(stderr, "bro-ctl: %s\n", e.c_str());
                    pollFailed_ = true;
                    return;
                }
                continue;
            }
            JsonValue v;
            if (!parseJson(payload, v)) continue;
            const std::string type = v.get("type").string;
            if (type == "cancel") {
                emit(payload);
                continue;
            }
            if (type == "gone") return;
            if (type != "request") continue;
            deliver(v, bundle);
        }
    }

    void deliver(const JsonValue& v, const std::string& bundle) {
        const std::string id = v.get("id").string;
        const fs::path dir = u8path(dir_) / u8path(id);
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::string err;
        const auto paths = unpack(bundle, dir / "files", err);
        if (!err.empty()) std::fprintf(stderr, "bro-ctl: %s: %s\n", id.c_str(), err.c_str());
        // The request as the agent sees it: the engine's, with the files' paths here.
        JsonOut j;
        j.beginObject();
        j.key("type").string("request");
        j.key("id").string(id);
        j.key("text").string(v.get("text").string);
        j.key("from").beginObject();
        for (const auto& [k, val] : v.get("from").object) {
            j.key(k);
            if (val.isNumber()) j.integer(static_cast<int64_t>(val.number));
            else j.string(val.string);
        }
        j.endObject();
        j.key("files").beginArray();
        for (const auto& p : paths) j.string(p);
        j.endArray();
        j.key("dir").string(pathString(fs::absolute(dir, ec)));
        j.key("asked").integer(static_cast<int64_t>(v.get("asked").number));
        j.endObject();
        const std::string line = j.take();
        writeFile(dir / "request.json", line + "\n");
        if (exec_.empty()) return emit(line);
        writeFile(dir / "request.txt", v.get("text").string);
        std::lock_guard<std::mutex> lk(workersMu_);
        workers_.emplace_back([this, id, dir] { runExec(id, dir); });
    }

    // One line from the agent on stdin.
    void handleLine(const std::string& line) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) return;
        JsonValue v;
        if (!parseJson(line, v) || v.get("id").string.empty()) {
            std::fprintf(stderr, "bro-ctl: a reply is one JSON object with an \"id\" per line: %s\n", line.c_str());
            return;
        }
        const std::string id = v.get("id").string;
        if (const JsonValue* p = v.find("progress")) return progress(id, p->string);
        std::vector<std::string> files;
        for (const auto& f : v.get("files").array) files.push_back(f.string);
        const JsonValue* error = v.find("error");
        const bool failed = (error && error->isString()) || (v.find("ok") && !v.get("ok").boolean);
        answer(id, error && error->isString() ? error->string : v.get("text").string, files, failed);
    }

    void progress(const std::string& id, const std::string& text) {
        bool ok = false;
        std::string payload, err;
        std::lock_guard<std::mutex> lk(answerMu_);
        if (!callPlain(*answers_, {"inbox", "progress", token(), id, "--text=" + text}, ok, payload, err) || !ok)
            std::fprintf(stderr, "bro-ctl: progress %s: %s\n", id.c_str(), (ok ? err : payload).c_str());
    }

    void answer(const std::string& id, const std::string& text, const std::vector<std::string>& files, bool failed) {
        std::string bundle, err;
        if (!bundleOf(files, bundle, err)) {
            std::fprintf(stderr, "bro-ctl: %s: %s (answered as failed)\n", id.c_str(), err.c_str());
            bundle.clear();
            failed = true;
        }
        std::vector<std::string> argv = {"inbox", "reply", token(), id,
                                         "--text=" + (err.empty() ? text : text + "\n(" + err + ")")};
        if (!bundle.empty()) argv.push_back("--files=" + bundle);
        if (failed) argv.push_back("--failed");
        bool ok = false;
        std::string payload;
        std::lock_guard<std::mutex> lk(answerMu_);
        if (!callPlain(*answers_, argv, ok, payload, err) || !ok)
            std::fprintf(stderr, "bro-ctl: reply %s: %s\n", id.c_str(), (ok ? err : payload).c_str());
    }

    static std::string shellQuote(const std::string& s) {
#if defined(_WIN32)
        return "\"" + s + "\"";
#else
        std::string out = "'";
        for (char ch : s) out += ch == '\'' ? std::string("'\\''") : std::string(1, ch);
        return out + "'";
#endif
    }

    // --exec: the command runs in the request's directory with the text on
    // stdin; its stdout is the reply, each stderr line a progress line, the
    // files it leaves in ./out the reply's files; a nonzero exit fails it.
    void runExec(const std::string& id, const fs::path& dir) {
        std::error_code ec;
        fs::create_directories(dir / "out", ec);
        const std::string d = pathString(fs::absolute(dir, ec));
#if defined(_WIN32)
        const std::string cmd = "\"cd /d " + shellQuote(d) + " && (" + exec_ +
                                ") < request.txt > reply.txt 2> progress.log\"";
#else
        const std::string cmd = "cd " + shellQuote(d) + " && BRO_INBOX_ID=" + shellQuote(id) + " sh -c " +
                                shellQuote(exec_) + " < request.txt > reply.txt 2> progress.log";
#endif
        std::atomic<bool> done{false};
        std::thread tail([&] {
            // Each complete stderr line becomes a progress line, as it is written.
            size_t at = 0;
            std::string pending;
            auto pump = [&] {
                std::string all;
                if (!readFile(pathString(dir / "progress.log"), all) || all.size() <= at) return;
                pending += all.substr(at);
                at = all.size();
                for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                    std::string l = pending.substr(0, nl);
                    pending.erase(0, nl + 1);
                    if (!l.empty() && l.back() == '\r') l.pop_back();
                    if (!l.empty()) progress(id, l);
                }
            };
            while (!done) {
                pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            pump();
        });
        const int rc = std::system(cmd.c_str());
        done = true;
        tail.join();
        std::string reply, log;
        readFile(pathString(dir / "reply.txt"), reply);
        while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) reply.pop_back();
        std::vector<std::string> files;
        for (const auto& e : fs::directory_iterator(dir / "out", ec))
            if (e.is_regular_file()) files.push_back(pathString(e.path()));
        if (rc == 0) return answer(id, reply, files, false);
        readFile(pathString(dir / "progress.log"), log);
        if (log.size() > 2000) log = log.substr(log.size() - 2000);
        answer(id, (reply.empty() ? log : reply) + "\n(exit status " + std::to_string(rc) + ")", {}, true);
    }

    const CallerFactory& open_;
    std::unique_ptr<Caller> answers_;
    std::string name_, about_;
    bool replace_ = false;
    std::string dir_, exec_, runtimeDir_;
    std::mutex tokenMu_, answerMu_, outMu_, workersMu_;
    std::string token_;
    std::vector<std::thread> workers_;
    std::atomic<bool> pollFailed_{false};
};

int runServe(const std::vector<std::string>& in, Caller& c, const CallerFactory& open, const std::string& runtimeDir) {
    const Args a = parseArgs(in, 2, {"about", "exec", "dir"});
    if (a.pos.empty()) {
        std::fprintf(stderr, "usage: bro-ctl inbox serve NAME [--about TEXT] [--exec CMD] [--replace] [--dir DIR]\n");
        return 2;
    }
    std::string err;
    std::unique_ptr<Caller> answers = open(err);
    if (!answers) return std::fprintf(stderr, "bro-ctl: %s\n", err.c_str()), 2;
    (void)c;
    const std::string dir =
        a.get("dir", pathString(u8path(runtimeDir) / "inbox" / u8path(a.pos[0])));
    Server s(open, std::move(answers), a.pos[0], a.get("about"), a.has("replace"), dir, a.get("exec"), runtimeDir);
    return s.run();
}

}  // namespace

bool isInboxCommand(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    if (args[0] == "ask") return true;
    if (args[0] != "inbox" || args.size() < 2) return false;
    const std::string& sub = args[1];
    return sub == "serve" || sub == "result" || sub == "status" || sub == "log";
}

int runInbox(const std::vector<std::string>& args, Caller& c, const CallerFactory& open,
             const std::string& runtimeDir) {
    if (args[0] == "ask") return runAsk(args, c, runtimeDir);
    const std::string& sub = args[1];
    if (sub == "serve") return runServe(args, c, open, runtimeDir);
    if (sub == "result") return runResult(args, c, runtimeDir);
    if (sub == "status") return runStatus(args, c);
    return runLog(args, c);
}

}  // namespace broctl
