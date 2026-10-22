#include "engine/control.h"

#include "engine/agent_inbox.h"
#include "engine/control_helpers.h"
#include "engine/engine.h"
#include "platform/control_socket.h"
#include "util/log.h"
#include "util/time.h"

#include <brolink/paths.h>
#include <brolink/peer.h>

#include <atomic>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace bro::engine {

// ---------------------------------------------------------------------------
// ControlCall
// ---------------------------------------------------------------------------

std::vector<std::string> ControlCall::positional() const {
    std::vector<std::string> out;
    for (size_t i = 1; i < argv_.size(); ++i) {
        const std::string& a = argv_[i];
        if (a.size() > 2 && a[0] == '-' && a[1] == '-') continue;
        out.push_back(a);
    }
    return out;
}

std::string ControlCall::arg(size_t i, const std::string& fallback) const {
    auto p = positional();
    return i < p.size() ? p[i] : fallback;
}

std::string ControlCall::option(const std::string& name, const std::string& fallback) const {
    const std::string key = "--" + name;
    for (size_t i = 1; i < argv_.size(); ++i) {
        const std::string& a = argv_[i];
        if (a == key) return "true";
        if (a.size() > key.size() && a.compare(0, key.size(), key) == 0 && a[key.size()] == '=')
            return a.substr(key.size() + 1);
    }
    return fallback;
}

bool ControlCall::flag(const std::string& name) const {
    std::string v = option(name);
    return !v.empty() && v != "false" && v != "0" && v != "no";
}

double ControlCall::number(const std::string& name, double fallback) const {
    std::string v = option(name);
    if (v.empty()) return fallback;
    char* end = nullptr;
    double d = std::strtod(v.c_str(), &end);
    return (end && end != v.c_str()) ? d : fallback;
}

// ---------------------------------------------------------------------------
// ControlServer
// ---------------------------------------------------------------------------

ControlServer::ControlServer(Engine& engine)
    : engine_(engine), socket_(std::make_shared<platform::ControlSocket>()),
      inbox_(std::make_unique<AgentInbox>([this] { return clockMs(); }, AgentInbox::defaultLogPath(engine))) {
    registerCommand("help", "[command]  list the commands, or one command's usage",
                    [this](const ControlCallPtr& call) {
                        const std::string which = call->arg(0);
                        if (which.empty()) return call->ok(helpText());
                        auto it = commands_.find(which);
                        if (it == commands_.end()) return call->fail("no command '" + which + "'");
                        call->ok(which + " " + it->second.usage + "\n");
                    });
}

ControlServer::~ControlServer() {
    stop();
    releaseControlView(this);
}

bool ControlServer::start(const std::string& name, std::string* why) {
    if (!socket_->start(name, why)) return false;
    LOG_INFO("Engine: agent control on %s (bro-ctl help)", socket_->path().c_str());
    return true;
}

void ControlServer::stop() { socket_->stop(); }

double ControlServer::clockMs() const {
    if (engine_.displayMode() == DisplayMode::Headless) return engine_.virtualTime();
    return util::currentTimeMs();
}
bool ControlServer::running() const { return socket_->running(); }
std::string ControlServer::socketPath() const { return socket_->path(); }
int ControlServer::pollFd() const { return socket_->running() ? socket_->readyFd() : -1; }

void ControlServer::registerCommand(const std::string& name, const std::string& usage, Handler handler) {
    commands_[name] = Command{usage, std::move(handler)};
}

ControlServer::Handler ControlServer::handler(const std::string& name, std::string* usage) const {
    auto it = commands_.find(name);
    if (it == commands_.end()) return {};
    if (usage) *usage = it->second.usage;
    return it->second.handler;
}

std::string ControlServer::helpText() const {
    std::string out;
    for (const auto& [name, cmd] : commands_) out += name + " " + cmd.usage + "\n";
    return out;
}

ControlOrigin ControlServer::inProcess() {
    ControlOrigin o;
    o.via = "app";
    o.pid = brolink::current_pid();
    o.user = brolink::current_user();
    return o;
}

void ControlServer::dispatch(std::vector<std::string> argv, std::function<void(bool, std::string)> sink,
                             ControlOrigin origin) {
    if (argv.empty()) {
        sink(false, "empty command (try: help)");
        return;
    }
    auto it = commands_.find(argv[0]);
    if (it == commands_.end()) {
        sink(false, "no command '" + argv[0] + "' (try: help)");
        return;
    }
    auto call = std::make_shared<ControlCall>(std::move(argv), std::move(sink), std::move(origin));
    try {
        it->second.handler(call);
    } catch (const std::exception& e) {
        call->fail(std::string("command threw: ") + e.what());
    }
}

namespace {

std::filesystem::path fsPath(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// The file's extension (".png"), or none: the part after the last '.' of the
// last path component.
std::string extensionOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    const std::string ext = path.substr(dot);
    return ext.size() <= 16 ? ext : std::string();
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
}

// A string as it appears inside a JSON string literal (a reply's "path").
std::string jsonEscaped(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

}  // namespace

void ControlServer::dispatchWithFile(std::vector<std::string> argv, size_t outArg, FileSink sink,
                                     ControlOrigin origin) {
    if (outArg == 0 || outArg >= argv.size()) {
        return dispatch(
            std::move(argv), [sink = std::move(sink)](bool ok, std::string payload) { sink(ok, std::move(payload), {}); },
            std::move(origin));
    }
    static std::atomic<uint64_t> serial{0};
    const std::string asked = argv[outArg];
    const std::string path = controlRuntimePath("out-" + std::to_string(brolink::current_pid()) + "-" +
                                                std::to_string(++serial) + extensionOf(asked));
    argv[outArg] = path;
    dispatch(std::move(argv), [sink = std::move(sink), path, asked](bool ok, std::string payload) {
        std::string file;
        std::error_code ec;
        if (ok) {
            std::ifstream in(fsPath(path), std::ios::binary);
            if (in) file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            else ok = false, payload = "the command wrote no file at " + path + " (" + payload + ")";
        }
        std::filesystem::remove(fsPath(path), ec);
        if (ok) {
            replaceAll(payload, jsonEscaped(path), jsonEscaped(asked));
            replaceAll(payload, path, asked);
        }
        sink(ok, std::move(payload), std::move(file));
    }, std::move(origin));
}

void ControlServer::post(std::vector<std::string> argv, size_t outArg, FileSink sink, ControlOrigin origin) {
    {
        std::lock_guard<std::mutex> lk(postedMutex_);
        posted_.push_back({std::move(argv), outArg, std::move(sink), std::move(origin)});
    }
    socket_->poke();
}

void ControlServer::pump() {
    if (socket_->running()) {
        std::weak_ptr<platform::ControlSocket> weakSocket = socket_;
        for (auto& req : socket_->take()) {
            const uint64_t conn = req.conn, id = req.id;
            auto reply = [weakSocket, conn, id](bool ok, std::string payload, std::string file) {
                if (auto sock = weakSocket.lock()) {
                    sock->reply(conn, id, ok, std::move(payload), std::move(file));
                }
            };
            if (!req.parseError.empty()) {
                reply(false, req.parseError, {});
                continue;
            }
            ControlOrigin origin;
            origin.via = "local";
            origin.pid = req.peerPid;
            origin.user = std::move(req.peerUser);
            origin.sshClient = std::move(req.peerSshClient);
            dispatchWithFile(std::move(req.argv), static_cast<size_t>(req.outArg), reply, std::move(origin));
        }
    }
    std::vector<Posted> posted;
    {
        std::lock_guard<std::mutex> lk(postedMutex_);
        posted.swap(posted_);
    }
    for (Posted& p : posted) dispatchWithFile(std::move(p.argv), p.outArg, std::move(p.sink), std::move(p.origin));
    inbox_->tick();
    if (tickers_.empty()) return;
    // A ticker may add tickers (a command built from others): run the ones
    // present at the start, keep the unfinished and the new.
    std::vector<Ticker> running;
    running.swap(tickers_);
    std::vector<Ticker> keep;
    for (auto& t : running)
        if (!t()) keep.push_back(std::move(t));
    for (auto& t : tickers_) keep.push_back(std::move(t));
    tickers_.swap(keep);
}

}  // namespace bro::engine
